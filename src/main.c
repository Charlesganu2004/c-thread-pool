#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

typedef struct job_payload {
    int job_id;
    int input;
    unsigned int delay_ms;
} job_payload_t;

typedef struct job_result {
    int job_id;
    int input;
    long long output;
} job_result_t;

static void demo_sleep(unsigned int delay_ms)
{
#ifdef _WIN32
    Sleep(delay_ms);
#else
    struct timespec delay;

    delay.tv_sec = (time_t)(delay_ms / 1000U);
    delay.tv_nsec = (long)(delay_ms % 1000U) * 1000000L;
    nanosleep(&delay, NULL);
#endif
}

static double now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
#endif
}

static long long sum_of_squares(int input)
{
    int i;
    long long total;

    total = 0;
    for (i = 1; i <= input; ++i) {
        total += (long long)i * (long long)i;
    }

    return total;
}

static void *run_job(void *arg)
{
    job_payload_t *payload;
    job_result_t *result;

    payload = (job_payload_t *)arg;
    result = (job_result_t *)malloc(sizeof(*result));
    if (result == NULL) {
        free(payload);
        return NULL;
    }

    demo_sleep(payload->delay_ms);
    result->job_id = payload->job_id;
    result->input = payload->input;
    result->output = sum_of_squares(payload->input);

    free(payload);
    return result;
}

static void cleanup_futures(thread_pool_future_t **futures, int submitted_count)
{
    int i;

    for (i = 0; i < submitted_count; ++i) {
        job_result_t *result;

        if (futures[i] == NULL) {
            continue;
        }

        result = (job_result_t *)thread_pool_future_get(futures[i]);
        free(result);
        thread_pool_future_destroy(futures[i]);
        futures[i] = NULL;
    }
}

int main(void)
{
    enum {
        worker_count = 4,
        queue_capacity = 4,
        job_count = 12,
        backpressure_report_ms = 10
    };

    thread_pool_t *pool;
    thread_pool_future_t *futures[job_count];
    int i;
    int submitted_count;

    for (i = 0; i < job_count; ++i) {
        futures[i] = NULL;
    }

    submitted_count = 0;

    pool = thread_pool_create(worker_count, queue_capacity);
    if (pool == NULL) {
        fprintf(stderr, "Failed to create thread pool.\n");
        return EXIT_FAILURE;
    }

    printf(
        "Created thread pool with %zu worker threads and queue capacity %zu.\n",
        thread_pool_thread_count(pool),
        thread_pool_queue_capacity(pool)
    );

    for (i = 0; i < job_count; ++i) {
        job_payload_t *payload;
        int job_id;
        int input;
        unsigned int delay_ms;
        double submit_start_ms;
        double submit_wait_ms;

        payload = (job_payload_t *)malloc(sizeof(*payload));
        if (payload == NULL) {
            fprintf(stderr, "Failed to allocate payload for job %d.\n", i + 1);
            thread_pool_destroy(pool);
            cleanup_futures(futures, submitted_count);
            return EXIT_FAILURE;
        }

        job_id = i + 1;
        input = 3000 + (i * 250);
        delay_ms = 120U + (unsigned int)((i % worker_count) * 70U);

        payload->job_id = job_id;
        payload->input = input;
        payload->delay_ms = delay_ms;

        submit_start_ms = now_ms();
        if (thread_pool_submit(pool, run_job, payload, &futures[i]) != 0) {
            fprintf(stderr, "Failed to submit job %d.\n", job_id);
            free(payload);
            thread_pool_destroy(pool);
            cleanup_futures(futures, submitted_count);
            return EXIT_FAILURE;
        }
        submit_wait_ms = now_ms() - submit_start_ms;

        submitted_count += 1;

        printf(
            "Submitted job %d with input=%d and delay=%u ms.\n",
            job_id,
            input,
            delay_ms
        );
        if (submit_wait_ms >= backpressure_report_ms) {
            printf("  Queue was full: submit waited %.0f ms for a free slot.\n", submit_wait_ms);
        }
    }

    puts("\nCollecting results from futures:");

    for (i = 0; i < job_count; ++i) {
        job_result_t *result;

        result = (job_result_t *)thread_pool_future_get(futures[i]);
        if (result == NULL) {
            fprintf(stderr, "Job %d returned no result.\n", i + 1);
            thread_pool_future_destroy(futures[i]);
            thread_pool_destroy(pool);
            cleanup_futures(&futures[i + 1], job_count - i - 1);
            return EXIT_FAILURE;
        }

        printf(
            "Job %02d complete: sum_of_squares(1..%d) = %lld\n",
            result->job_id,
            result->input,
            result->output
        );

        free(result);
        thread_pool_future_destroy(futures[i]);
    }

    thread_pool_wait(pool);
    thread_pool_destroy(pool);
    puts("\nAll jobs completed successfully.");

    return EXIT_SUCCESS;
}
