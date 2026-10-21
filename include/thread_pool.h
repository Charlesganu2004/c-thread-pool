#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct thread_pool thread_pool_t;
typedef struct thread_pool_future thread_pool_future_t;
typedef void *(*thread_pool_task_fn)(void *arg);

/*
 * Returns NULL with errno set (EINVAL for a zero argument, ENOMEM otherwise).
 */
thread_pool_t *thread_pool_create(size_t thread_count, size_t queue_capacity);

/*
 * Queues function(arg) and blocks while the queue is full. out_future is
 * required: the call fails with EINVAL if it is NULL. On success the caller
 * must pass the returned future to thread_pool_future_destroy.
 *
 * Returns 0 on success, or -1 with errno set to EINVAL, ENOMEM, or EBUSY when
 * thread_pool_shutdown has been called on the pool.
 *
 * A task must not call thread_pool_submit on its own pool: if the queue is
 * full and every worker does this, no worker is left to free a slot.
 */
int thread_pool_submit(
    thread_pool_t *pool,
    thread_pool_task_fn function,
    void *arg,
    thread_pool_future_t **out_future
);

/*
 * Blocks until the queue is empty and no task is running. A task must not call
 * it on its own pool, because the calling task counts as running.
 */
void thread_pool_wait(thread_pool_t *pool);

/*
 * Stops accepting work. Every thread blocked inside thread_pool_submit is woken
 * and returns EBUSY, and every later call to thread_pool_submit returns EBUSY.
 * Tasks that are already queued still run. Nothing is freed and no thread is
 * joined, so the pool stays valid until thread_pool_destroy.
 *
 * Safe to call from any thread, from a task, and more than once, while other
 * threads call thread_pool_submit or thread_pool_wait on the pool.
 */
void thread_pool_shutdown(thread_pool_t *pool);

/*
 * Stops accepting work as thread_pool_shutdown does, runs every queued task,
 * joins the workers and frees the pool.
 *
 * No other call on the pool may be running when destroy starts or may start
 * while it runs, and the pool must not be used after it returns. To stop
 * producers first, call thread_pool_shutdown, join every thread that submits,
 * then call destroy. A task must not call it on its own pool.
 */
void thread_pool_destroy(thread_pool_t *pool);

/* Blocks until the task has finished and returns its result. */
void *thread_pool_future_get(thread_pool_future_t *future);

/* Waits for the task to finish, then frees the future. Valid after the pool is destroyed. */
void thread_pool_future_destroy(thread_pool_future_t *future);

size_t thread_pool_thread_count(const thread_pool_t *pool);
size_t thread_pool_queue_capacity(const thread_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif
