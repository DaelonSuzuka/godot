/*************************************************************************/
/*  worker_thread_pool.cpp                                               */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "worker_thread_pool.h"

#include "core/os/os.h"

WorkerThreadPool *WorkerThreadPool::singleton = nullptr;

void WorkerThreadPool::_process_task_queue() {
	task_mutex.lock();
	Task *task = task_queue.first()->self();
	task_queue.remove(task_queue.first());
	task_mutex.unlock();
	_process_task(task);
}

void WorkerThreadPool::_process_task(Task *p_task) {
	bool low_priority = p_task->low_priority;

	if (p_task->group) {
		// Handling a group
		bool do_post = false;
		if (p_task->native_group_func) {
			while (true) {
				uint32_t work_index = p_task->group->index.postincrement();
				if (work_index >= p_task->group->max) {
					do_post = work_index == p_task->group->max; // First one reaching max handles semaphore and clean-up.
					break;
				}
				p_task->native_group_func(p_task->native_func_userdata, work_index);
			}

		} else {
			Callable::CallError ce;
			Variant ret;
			Variant arg;
			Variant *argptr = &arg;
			while (true) {
				uint32_t work_index = p_task->group->index.postincrement();
				if (work_index >= p_task->group->max) {
					do_post = work_index == p_task->group->max; // First one reaching max handles semaphore and clean-up.
					break;
				}
				arg = work_index;
				p_task->callable.call((const Variant **)&argptr, 1, ret, ce);
			}
		}

		if (low_priority && use_native_low_priority_threads) {
			p_task->completed = true;
			p_task->done_semaphore.post();
			if (do_post) {
				p_task->group->completed.set_to(true);
			}
		} else {
			if (do_post) {
				p_task->group->done_semaphore.post();
				p_task->group->completed.set_to(true);
			}
			uint32_t max_users = p_task->group->tasks_used + 1; // Add 1 because the thread waiting for it is also user. Read before to avoid another thread freeing task after increment.
			uint32_t finished_users = p_task->group->finished.increment();

			if (finished_users == max_users) {
				// Get rid of the group, because nobody else is using it.
				task_mutex.lock();
				group_allocator.free(p_task->group);
				task_mutex.unlock();
			}

			// For groups, tasks get rid of themselves.

			task_mutex.lock();
			task_allocator.free(p_task);
			task_mutex.unlock();
		}
	} else {
		if (p_task->native_func) {
			p_task->native_func(p_task->native_func_userdata);
		} else {
			Callable::CallError ce;
			Variant ret;
			p_task->callable.call(nullptr, 0, ret, ce);
		}

		p_task->completed = true;
		p_task->done_semaphore.post();
	}

	if (!use_native_low_priority_threads && low_priority) {
		// A low prioriry task was freed, so see if we can move a pending one to the high priority queue.
		bool post = false;
		task_mutex.lock();
		if (low_priority_task_queue.first()) {
			Task *low_prio_task = low_priority_task_queue.first()->self();
			low_priority_task_queue.remove(low_priority_task_queue.first());
			task_queue.add_last(&low_prio_task->task_elem);
			post = true;
		} else {
			low_priority_threads_used.decrement();
		}
		task_mutex.lock();
		if (post) {
			task_available_semaphore.post();
		}
	}
}

void WorkerThreadPool::_thread_function(void *p_user) {
	while (true) {
		singleton->task_available_semaphore.wait();
		if (singleton->exit_threads.is_set()) {
			break;
		}
		singleton->_process_task_queue();
	}
}

void WorkerThreadPool::_native_low_priority_thread_function(void *p_user) {
	Task *task = (Task *)p_user;
	singleton->_process_task(task);
}

void WorkerThreadPool::_post_task(Task *p_task, bool p_high_priority) {
	task_mutex.lock();
	p_task->low_priority = !p_high_priority;
	if (!p_high_priority && use_native_low_priority_threads) {
		task_mutex.unlock();
		p_task->low_priority_thread = native_thread_allocator.alloc();
		p_task->low_priority_thread->start(_native_low_priority_thread_function, p_task); // Pask task directly to thread.

	} else if (p_high_priority || low_priority_threads_used.get() < max_low_priority_threads) {
		task_queue.add_last(&p_task->task_elem);
		if (!p_high_priority) {
			low_priority_threads_used.increment();
		}
		task_mutex.unlock();
		task_available_semaphore.post();
	} else {
		// Too many threads using low priority, must go to queue.
		low_priority_task_queue.add_last(&p_task->task_elem);
		task_mutex.unlock();
	}
}

WorkerThreadPool::TaskID WorkerThreadPool::add_native_task(void (*p_func)(void *), void *p_userdata, bool p_high_priority, const String &p_description) {
	task_mutex.lock();
	// Get a free task
	Task *task = task_allocator.alloc();
	TaskID id = last_task++;
	task->native_func = p_func;
	task->native_func_userdata = p_userdata;
	task->description = p_description;
	tasks.insert(id, task);
	task_mutex.unlock();

	_post_task(task, p_high_priority);

	return id;
}

WorkerThreadPool::TaskID WorkerThreadPool::add_task(const Callable &p_action, bool p_high_priority, const String &p_description) {
	task_mutex.lock();
	// Get a free task
	Task *task = task_allocator.alloc();
	TaskID id = last_task++;
	task->callable = p_action;
	task->description = p_description;
	tasks.insert(id, task);
	task_mutex.unlock();

	_post_task(task, p_high_priority);

	return id;
}

bool WorkerThreadPool::is_task_completed(TaskID p_task_id) const {
	task_mutex.lock();
	const Task *const *taskp = tasks.getptr(p_task_id);
	if (!taskp) {
		task_mutex.unlock();
		ERR_FAIL_V_MSG(false, "Invalid Task ID"); // Invalid task
	}

	bool completed = (*taskp)->completed;
	task_mutex.unlock();

	return completed;
}

void WorkerThreadPool::wait_for_task_completion(TaskID p_task_id) {
	task_mutex.lock();
	Task **taskp = tasks.getptr(p_task_id);
	if (!taskp) {
		task_mutex.unlock();
		ERR_FAIL_MSG("Invalid Task ID"); // Invalid task
	}
	Task *task = *taskp;

	if (task->waiting) {
		String description = task->description;
		task_mutex.unlock();
		if (description.is_empty()) {
			ERR_FAIL_MSG("Another thread is waiting on this task: " + itos(p_task_id)); // Invalid task
		} else {
			ERR_FAIL_MSG("Another thread is waiting on this task: " + description + " (" + itos(p_task_id) + ")"); // Invalid task
		}
	}

	task->waiting = true;

	task_mutex.unlock();

	if (use_native_low_priority_threads && task->low_priority) {
		task->low_priority_thread->wait_to_finish();
		native_thread_allocator.free(task->low_priority_thread);
	} else {
		int *index = thread_ids.getptr(Thread::get_caller_id());

		if (index) {
			// We are an actual process thread, we must not be blocked so continue processing stuff if available.
			while (true) {
				if (task->done_semaphore.try_wait()) {
					// If done, exit
					break;
				}
				if (task_available_semaphore.try_wait()) {
					// Solve tasks while they are around.
					_process_task_queue();
					continue;
				}
				OS::get_singleton()->delay_usec(1); // Microsleep, this could be converted to waiting for multiple objects in supported platforms for a bit more performance.
			}
		} else {
			task->done_semaphore.wait();
		}
	}

	task_mutex.lock();
	tasks.erase(p_task_id);
	task_allocator.free(task);
	task_mutex.unlock();
}

WorkerThreadPool::GroupID WorkerThreadPool::add_native_group_task(void (*p_func)(void *, uint32_t), void *p_userdata, int p_elements, int p_tasks, bool p_high_priority, const String &p_description) {
	ERR_FAIL_COND_V(p_elements <= 0, INVALID_TASK_ID);
	if (p_tasks < 0) {
		p_tasks = threads.size();
	}

	task_mutex.lock();
	Group *group = group_allocator.alloc();
	GroupID id = last_task++;
	group->max = p_elements;
	group->self = id;
	group->tasks_used = p_tasks;
	Task **tasks_posted = (Task **)alloca(sizeof(Task *) * p_tasks);
	for (int i = 0; i < p_tasks; i++) {
		Task *task = task_allocator.alloc();
		task->native_group_func = p_func;
		task->native_func_userdata = p_userdata;
		task->description = p_description;
		task->group = group;
		tasks_posted[i] = task;
		// No task ID is used.
	}
	groups[id] = group;
	task_mutex.unlock();

	if (!p_high_priority && use_native_low_priority_threads) {
		group->low_priority_native_tasks.resize(p_tasks);
	}

	for (int i = 0; i < p_tasks; i++) {
		_post_task(tasks_posted[i], p_high_priority);
		if (!p_high_priority && use_native_low_priority_threads) {
			group->low_priority_native_tasks[i] = tasks_posted[i];
		}
	}

	return id;
}

WorkerThreadPool::GroupID WorkerThreadPool::add_group_task(const Callable &p_action, int p_elements, int p_tasks, bool p_high_priority, const String &p_description) {
	ERR_FAIL_COND_V(p_elements <= 0, INVALID_TASK_ID);
	if (p_tasks < 0) {
		p_tasks = threads.size();
	}

	task_mutex.lock();
	Group *group = group_allocator.alloc();
	GroupID id = last_task++;
	group->max = p_elements;
	group->self = id;
	group->tasks_used = p_tasks;
	Task **tasks_posted = (Task **)alloca(sizeof(Task *) * p_tasks);
	for (int i = 0; i < p_tasks; i++) {
		Task *task = task_allocator.alloc();
		task->callable = p_action;
		task->description = p_description;
		task->group = group;
		tasks_posted[i] = task;
		// No task ID is used.
	}
	groups[id] = group;
	task_mutex.unlock();

	if (!p_high_priority && use_native_low_priority_threads) {
		group->low_priority_native_tasks.resize(p_tasks);
	}

	for (int i = 0; i < p_tasks; i++) {
		_post_task(tasks_posted[i], p_high_priority);
		if (!p_high_priority && use_native_low_priority_threads) {
			group->low_priority_native_tasks[i] = tasks_posted[i];
		}
	}
	return id;
}

bool WorkerThreadPool::is_group_task_completed(GroupID p_group) const {
	task_mutex.lock();
	const Group *const *groupp = groups.getptr(p_group);
	if (!groupp) {
		task_mutex.unlock();
		ERR_FAIL_V_MSG(false, "Invalid Group ID");
	}
	bool completed = (*groupp)->completed.is_set();
	task_mutex.unlock();
	return completed;
}

void WorkerThreadPool::wait_for_group_task_completion(GroupID p_group) {
	task_mutex.lock();
	Group **groupp = groups.getptr(p_group);
	task_mutex.unlock();
	if (!groupp) {
		ERR_FAIL_MSG("Invalid Group ID");
	}
	Group *group = *groupp;

	if (group->low_priority_native_tasks.size() > 0) {
		for (uint32_t i = 0; i < group->low_priority_native_tasks.size(); i++) {
			group->low_priority_native_tasks[i]->low_priority_thread->wait_to_finish();
			native_thread_allocator.free(group->low_priority_native_tasks[i]->low_priority_thread);
			task_mutex.lock();
			task_allocator.free(group->low_priority_native_tasks[i]);
			task_mutex.unlock();
		}

		task_mutex.lock();
		group_allocator.free(group);
		task_mutex.unlock();
	} else {
		group->done_semaphore.wait();

		uint32_t max_users = group->tasks_used + 1; // Add 1 because the thread waiting for it is also user. Read before to avoid another thread freeing task after increment.
		uint32_t finished_users = group->finished.increment(); // fetch happens before inc, so increment later.

		if (finished_users == max_users) {
			// All tasks using this group are gone (finished before the group), so clear the gorup too.
			task_mutex.lock();
			group_allocator.free(group);
			task_mutex.unlock();
		}
	}

	groups.erase(p_group); // Threads do not access this, so safe to erase here.
}

void WorkerThreadPool::init(int p_thread_count, bool p_use_native_threads_low_priority, float p_low_priority_task_ratio) {
	ERR_FAIL_COND(threads.size() > 0);
	if (p_thread_count < 0) {
		p_thread_count = OS::get_singleton()->get_default_thread_pool_size();
	}

	if (p_use_native_threads_low_priority) {
		max_low_priority_threads = 0;
	} else {
		max_low_priority_threads = CLAMP(p_thread_count * p_low_priority_task_ratio, 1, p_thread_count);
	}

	use_native_low_priority_threads = p_use_native_threads_low_priority;

	threads.resize(p_thread_count);

	for (uint32_t i = 0; i < threads.size(); i++) {
		threads[i].index = i;
		threads[i].thread.start(&WorkerThreadPool::_thread_function, &threads[i]);
		thread_ids.insert(threads[i].thread.get_id(), i);
	}
}

void WorkerThreadPool::finish() {
	if (threads.size() == 0) {
		return;
	}

	task_mutex.lock();
	SelfList<Task> *E = low_priority_task_queue.first();
	while (E) {
		print_error("Task waiting was never re-claimed: " + E->self()->description);
		E = E->next();
	}
	task_mutex.unlock();

	exit_threads.set_to(true);

	for (uint32_t i = 0; i < threads.size(); i++) {
		task_available_semaphore.post();
	}

	for (uint32_t i = 0; i < threads.size(); i++) {
		threads[i].thread.wait_to_finish();
	}

	threads.clear();
}

void WorkerThreadPool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_task", "action", "high_priority", "description"), &WorkerThreadPool::add_task, DEFVAL(false), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("is_task_completed", "task_id"), &WorkerThreadPool::is_task_completed);
	ClassDB::bind_method(D_METHOD("wait_for_task_completion", "task_id"), &WorkerThreadPool::wait_for_task_completion);

	ClassDB::bind_method(D_METHOD("add_group_task", "action", "elements", "tasks_needed", "high_priority", "description"), &WorkerThreadPool::add_group_task, DEFVAL(-1), DEFVAL(false), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("is_group_task_completed", "group_id"), &WorkerThreadPool::is_group_task_completed);
	ClassDB::bind_method(D_METHOD("wait_for_group_task_completion", "group_id"), &WorkerThreadPool::wait_for_group_task_completion);
}

WorkerThreadPool::WorkerThreadPool() {
	singleton = this;
}

WorkerThreadPool::~WorkerThreadPool() {
	finish();
}
