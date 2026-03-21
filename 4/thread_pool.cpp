#include "thread_pool.h"

#include <pthread.h>
#include <assert.h>

enum thread_task_state {
	CREATED,
	PUSHED,
	RUNNING,
	EXECUTED,
	JOINED,
};

struct thread_task {
	thread_task_f function;
	enum thread_task_state state;
	bool detached;
	bool ready_to_join;
	// bool running;
	// bool executed;
	// bool joined;

	pthread_cond_t cond;
	pthread_mutex_t lock;

	/* PUT HERE OTHER MEMBERS */
};



struct thread_pool {
	std::vector<pthread_t> threads;
	std::vector<struct thread_task *> task_queue;

	pthread_mutex_t threads_lock;

	pthread_mutex_t task_queue_lock;
	pthread_cond_t task_queue_cond;

	int running_tasks;
	int max_threads;

	bool stop;

	/* PUT HERE OTHER MEMBERS */
};

static void *
thread_pool_worker(void *arg)
{
	struct thread_pool *pool = (struct thread_pool *)arg;
	bool task_finished = false;

	for (;;) {
		pthread_mutex_lock(&pool->task_queue_lock);
		if (task_finished) {
			pool->running_tasks--;
		}

		if (pool->task_queue.empty()) {
			pthread_cond_wait(&pool->task_queue_cond, &pool->task_queue_lock);
		}

		if (pool->stop) {
			pthread_mutex_unlock(&pool->task_queue_lock);
			break;
		}

		thread_task *task = pool->task_queue.back();
		pool->task_queue.pop_back();

		pool->running_tasks++;
		pthread_mutex_unlock(&pool->task_queue_lock);

		// TODO: memorder.
		__atomic_store_n(&task->state, RUNNING, __ATOMIC_RELEASE);
		task->function();

		if (task->detached) {
			__atomic_store_n(&task->state, JOINED, __ATOMIC_RELEASE);
			continue;
		}
		__atomic_store_n(&task->state, EXECUTED, __ATOMIC_RELEASE);

		pthread_mutex_lock(&task->lock);
		task->ready_to_join = true;
		pthread_cond_signal(&task->cond);
		pthread_mutex_unlock(&task->lock);

		task_finished = true;
	}
	return NULL;
}

static int 
thread_pool_add_worker(struct thread_pool *pool)
{
	pthread_t thread;
	int err = pthread_create(&thread, NULL, thread_pool_worker, pool);
	if (err != 0) {
		return err;
	}

	pool->threads.push_back(thread);

	return 0;
}


int
thread_pool_new(int thread_count, struct thread_pool **pool)
{
	if (thread_count <= 0 || thread_count > TPOOL_MAX_THREADS)
		return TPOOL_ERR_INVALID_ARGUMENT;

	*pool = new thread_pool;
	(*pool)->max_threads = thread_count;
	(*pool)->running_tasks = 0;

	pthread_mutex_init(&(*pool)->threads_lock, NULL);
	pthread_mutex_init(&(*pool)->task_queue_lock, NULL);
	pthread_cond_init(&(*pool)->task_queue_cond, NULL);
	
	return 0;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	pthread_mutex_lock(&pool->task_queue_lock);
	if (!pool->task_queue.empty() || pool->running_tasks != 0) {
		pthread_mutex_unlock(&pool->task_queue_lock);
		return TPOOL_ERR_HAS_TASKS;
	}
	pool->stop = true;
	pthread_cond_broadcast(&pool->task_queue_cond);
	pthread_mutex_unlock(&pool->task_queue_lock);

	pthread_mutex_lock(&pool->threads_lock);
	for (pthread_t thread : pool->threads) {
		pthread_join(thread, NULL);
	}
	pthread_mutex_unlock(&pool->threads_lock);

	delete pool;
	return 0;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	pthread_mutex_lock(&pool->threads_lock);
	if (pool->threads.size() < pool->max_threads) {
		int err = thread_pool_add_worker(pool);
		assert (err == 0);
	}
	pthread_mutex_unlock(&pool->threads_lock);


	pthread_mutex_lock(&pool->task_queue_lock);
	if (pool->task_queue.size() + pool->running_tasks > TPOOL_MAX_TASKS) {
		pthread_mutex_unlock(&pool->task_queue_lock);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	pool->task_queue.push_back(task);
	pthread_cond_signal(&pool->task_queue_cond);
	pthread_mutex_unlock(&pool->task_queue_lock);

	__atomic_store_n(&task->state, PUSHED, __ATOMIC_RELEASE);
	return 0;
}

int
thread_task_new(struct thread_task **task, const thread_task_f &function)
{
	*task = new thread_task;
	(*task)->function = function;
	(*task)->state = CREATED;
	(*task)->detached = false;
	(*task)->ready_to_join = false;

	pthread_mutex_init(&(*task)->lock, NULL);
	pthread_cond_init(&(*task)->cond, NULL);

	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	return  __atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == JOINED;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	return  __atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == RUNNING;
}

int
thread_task_join(struct thread_task *task)
{
	if (__atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == CREATED) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	pthread_mutex_lock(&task->lock);
	if (!task->ready_to_join) {
		pthread_cond_wait(&task->cond, &task->lock);
	}
	pthread_mutex_unlock(&task->lock);
	
	__atomic_store_n(&task->state, JOINED, __ATOMIC_RELEASE);
	return 0;
}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	(void)timeout;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	enum thread_task_state state = __atomic_load_n(&task->state, __ATOMIC_ACQUIRE);
	if (state != CREATED && state != JOINED) {
		return TPOOL_ERR_TASK_IN_POOL;
	}
	delete task;
	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif
