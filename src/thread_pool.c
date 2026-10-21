#include "thread_pool.h"

#include <errno.h>
#include <stdlib.h>

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>

typedef HANDLE tp_thread_t;
typedef CRITICAL_SECTION tp_mutex_t;
typedef CONDITION_VARIABLE tp_cond_t;
typedef DWORD tp_thread_return_t;

#define TP_THREAD_CALL WINAPI

static int tp_mutex_init(tp_mutex_t *mutex)
{
    InitializeCriticalSection(mutex);
    return 0;
}

static void tp_mutex_destroy(tp_mutex_t *mutex)
{
    DeleteCriticalSection(mutex);
}

static void tp_mutex_lock(tp_mutex_t *mutex)
{
    EnterCriticalSection(mutex);
}

static void tp_mutex_unlock(tp_mutex_t *mutex)
{
    LeaveCriticalSection(mutex);
}

static int tp_cond_init(tp_cond_t *cond)
{
    InitializeConditionVariable(cond);
    return 0;
}

static void tp_cond_destroy(tp_cond_t *cond)
{
    (void)cond;
}

static void tp_cond_wait(tp_cond_t *cond, tp_mutex_t *mutex)
{
    SleepConditionVariableCS(cond, mutex, INFINITE);
}

static void tp_cond_signal(tp_cond_t *cond)
{
    WakeConditionVariable(cond);
}

static void tp_cond_broadcast(tp_cond_t *cond)
{
    WakeAllConditionVariable(cond);
}

static int tp_thread_create(tp_thread_t *thread, LPTHREAD_START_ROUTINE start_routine, void *arg)
{
    *thread = CreateThread(NULL, 0, start_routine, arg, 0, NULL);
    return *thread == NULL ? -1 : 0;
}

static void tp_thread_join(tp_thread_t thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

#else

#include <pthread.h>

typedef pthread_t tp_thread_t;
typedef pthread_mutex_t tp_mutex_t;
typedef pthread_cond_t tp_cond_t;
typedef void *tp_thread_return_t;

#define TP_THREAD_CALL

static int tp_mutex_init(tp_mutex_t *mutex)
{
    return pthread_mutex_init(mutex, NULL);
}

static void tp_mutex_destroy(tp_mutex_t *mutex)
{
    pthread_mutex_destroy(mutex);
}

static void tp_mutex_lock(tp_mutex_t *mutex)
{
    pthread_mutex_lock(mutex);
}

static void tp_mutex_unlock(tp_mutex_t *mutex)
{
    pthread_mutex_unlock(mutex);
}

static int tp_cond_init(tp_cond_t *cond)
{
    return pthread_cond_init(cond, NULL);
}

static void tp_cond_destroy(tp_cond_t *cond)
{
    pthread_cond_destroy(cond);
}

static void tp_cond_wait(tp_cond_t *cond, tp_mutex_t *mutex)
{
    pthread_cond_wait(cond, mutex);
}

static void tp_cond_signal(tp_cond_t *cond)
{
    pthread_cond_signal(cond);
}

static void tp_cond_broadcast(tp_cond_t *cond)
{
    pthread_cond_broadcast(cond);
}

static int tp_thread_create(tp_thread_t *thread, void *(*start_routine)(void *), void *arg)
{
    return pthread_create(thread, NULL, start_routine, arg);
}

static void tp_thread_join(tp_thread_t thread)
{
    pthread_join(thread, NULL);
}

#endif

typedef struct thread_pool_task {
    thread_pool_task_fn function;
    void *arg;
    thread_pool_future_t *future;
    struct thread_pool_task *next;
} thread_pool_task_t;

struct thread_pool_future {
    tp_mutex_t mutex;
    tp_cond_t ready;
    int completed;
    void *result;
};

struct thread_pool {
    tp_thread_t *workers;
    size_t worker_count;
    size_t queue_capacity;
    size_t queue_size;
    size_t active_tasks;
    int accepting;
    int stop;
    thread_pool_task_t *head;
    thread_pool_task_t *tail;
    tp_mutex_t mutex;
    tp_cond_t has_work;
    tp_cond_t has_space;
    tp_cond_t idle;
};

static void thread_pool_future_dispose(thread_pool_future_t *future)
{
    if (future == NULL) {
        return;
    }

    tp_cond_destroy(&future->ready);
    tp_mutex_destroy(&future->mutex);
    free(future);
}

static thread_pool_future_t *thread_pool_future_create(void)
{
    thread_pool_future_t *future;

    future = (thread_pool_future_t *)calloc(1, sizeof(*future));
    if (future == NULL) {
        return NULL;
    }

    if (tp_mutex_init(&future->mutex) != 0) {
        free(future);
        return NULL;
    }

    if (tp_cond_init(&future->ready) != 0) {
        tp_mutex_destroy(&future->mutex);
        free(future);
        return NULL;
    }

    return future;
}

static void thread_pool_future_complete(thread_pool_future_t *future, void *result)
{
    tp_mutex_lock(&future->mutex);
    future->result = result;
    future->completed = 1;
    tp_cond_broadcast(&future->ready);
    tp_mutex_unlock(&future->mutex);
}

static void thread_pool_free(thread_pool_t *pool, size_t threads_to_join)
{
    size_t i;

    if (pool == NULL) {
        return;
    }

    for (i = 0; i < threads_to_join; ++i) {
        tp_thread_join(pool->workers[i]);
    }

    tp_cond_destroy(&pool->idle);
    tp_cond_destroy(&pool->has_space);
    tp_cond_destroy(&pool->has_work);
    tp_mutex_destroy(&pool->mutex);
    free(pool->workers);
    free(pool);
}

static tp_thread_return_t TP_THREAD_CALL thread_pool_worker(void *arg)
{
    thread_pool_t *pool;

    pool = (thread_pool_t *)arg;

    for (;;) {
        thread_pool_task_t *task;
        void *result;

        tp_mutex_lock(&pool->mutex);
        while (pool->queue_size == 0 && !pool->stop) {
            tp_cond_wait(&pool->has_work, &pool->mutex);
        }

        if (pool->queue_size == 0 && pool->stop) {
            tp_mutex_unlock(&pool->mutex);
            break;
        }

        task = pool->head;
        pool->head = task->next;
        if (pool->head == NULL) {
            pool->tail = NULL;
        }

        pool->queue_size -= 1;
        pool->active_tasks += 1;
        tp_cond_signal(&pool->has_space);
        tp_mutex_unlock(&pool->mutex);

        result = task->function(task->arg);
        thread_pool_future_complete(task->future, result);
        free(task);

        tp_mutex_lock(&pool->mutex);
        pool->active_tasks -= 1;
        if (pool->queue_size == 0 && pool->active_tasks == 0) {
            tp_cond_broadcast(&pool->idle);
        }
        tp_mutex_unlock(&pool->mutex);
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

thread_pool_t *thread_pool_create(size_t thread_count, size_t queue_capacity)
{
    thread_pool_t *pool;
    size_t i;

    if (thread_count == 0 || queue_capacity == 0) {
        errno = EINVAL;
        return NULL;
    }

    pool = (thread_pool_t *)calloc(1, sizeof(*pool));
    if (pool == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    pool->worker_count = thread_count;
    pool->queue_capacity = queue_capacity;
    pool->accepting = 1;

    if (tp_mutex_init(&pool->mutex) != 0) {
        free(pool);
        errno = ENOMEM;
        return NULL;
    }

    if (tp_cond_init(&pool->has_work) != 0) {
        tp_mutex_destroy(&pool->mutex);
        free(pool);
        errno = ENOMEM;
        return NULL;
    }

    if (tp_cond_init(&pool->has_space) != 0) {
        tp_cond_destroy(&pool->has_work);
        tp_mutex_destroy(&pool->mutex);
        free(pool);
        errno = ENOMEM;
        return NULL;
    }

    if (tp_cond_init(&pool->idle) != 0) {
        tp_cond_destroy(&pool->has_space);
        tp_cond_destroy(&pool->has_work);
        tp_mutex_destroy(&pool->mutex);
        free(pool);
        errno = ENOMEM;
        return NULL;
    }

    pool->workers = (tp_thread_t *)calloc(thread_count, sizeof(*pool->workers));
    if (pool->workers == NULL) {
        tp_cond_destroy(&pool->idle);
        tp_cond_destroy(&pool->has_space);
        tp_cond_destroy(&pool->has_work);
        tp_mutex_destroy(&pool->mutex);
        free(pool);
        errno = ENOMEM;
        return NULL;
    }

    for (i = 0; i < thread_count; ++i) {
        if (tp_thread_create(&pool->workers[i], thread_pool_worker, pool) != 0) {
            tp_mutex_lock(&pool->mutex);
            pool->accepting = 0;
            pool->stop = 1;
            tp_cond_broadcast(&pool->has_work);
            tp_mutex_unlock(&pool->mutex);
            thread_pool_free(pool, i);
            errno = ENOMEM;
            return NULL;
        }
    }

    return pool;
}

int thread_pool_submit(
    thread_pool_t *pool,
    thread_pool_task_fn function,
    void *arg,
    thread_pool_future_t **out_future
)
{
    thread_pool_task_t *task;
    thread_pool_future_t *future;

    if (pool == NULL || function == NULL || out_future == NULL) {
        errno = EINVAL;
        return -1;
    }

    future = thread_pool_future_create();
    if (future == NULL) {
        errno = ENOMEM;
        return -1;
    }

    task = (thread_pool_task_t *)calloc(1, sizeof(*task));
    if (task == NULL) {
        thread_pool_future_dispose(future);
        errno = ENOMEM;
        return -1;
    }

    task->function = function;
    task->arg = arg;
    task->future = future;

    tp_mutex_lock(&pool->mutex);
    while (pool->queue_size >= pool->queue_capacity && pool->accepting) {
        tp_cond_wait(&pool->has_space, &pool->mutex);
    }

    if (!pool->accepting) {
        tp_mutex_unlock(&pool->mutex);
        free(task);
        thread_pool_future_dispose(future);
        errno = EBUSY;
        return -1;
    }

    if (pool->tail == NULL) {
        pool->head = task;
        pool->tail = task;
    } else {
        pool->tail->next = task;
        pool->tail = task;
    }

    pool->queue_size += 1;
    *out_future = future;
    tp_cond_signal(&pool->has_work);
    tp_mutex_unlock(&pool->mutex);

    return 0;
}

void thread_pool_wait(thread_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }

    tp_mutex_lock(&pool->mutex);
    while (pool->queue_size > 0 || pool->active_tasks > 0) {
        tp_cond_wait(&pool->idle, &pool->mutex);
    }
    tp_mutex_unlock(&pool->mutex);
}

void thread_pool_shutdown(thread_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }

    tp_mutex_lock(&pool->mutex);
    pool->accepting = 0;
    tp_cond_broadcast(&pool->has_space);
    tp_mutex_unlock(&pool->mutex);
}

void thread_pool_destroy(thread_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }

    /* Workers exit only once the queue is empty, so every queued task still runs. */
    tp_mutex_lock(&pool->mutex);
    pool->accepting = 0;
    pool->stop = 1;
    tp_cond_broadcast(&pool->has_work);
    tp_mutex_unlock(&pool->mutex);

    thread_pool_free(pool, pool->worker_count);
}

void *thread_pool_future_get(thread_pool_future_t *future)
{
    void *result;

    if (future == NULL) {
        return NULL;
    }

    tp_mutex_lock(&future->mutex);
    while (!future->completed) {
        tp_cond_wait(&future->ready, &future->mutex);
    }
    result = future->result;
    tp_mutex_unlock(&future->mutex);

    return result;
}

void thread_pool_future_destroy(thread_pool_future_t *future)
{
    if (future == NULL) {
        return;
    }

    tp_mutex_lock(&future->mutex);
    while (!future->completed) {
        tp_cond_wait(&future->ready, &future->mutex);
    }
    tp_mutex_unlock(&future->mutex);

    thread_pool_future_dispose(future);
}

size_t thread_pool_thread_count(const thread_pool_t *pool)
{
    if (pool == NULL) {
        return 0;
    }

    return pool->worker_count;
}

size_t thread_pool_queue_capacity(const thread_pool_t *pool)
{
    if (pool == NULL) {
        return 0;
    }

    return pool->queue_capacity;
}
