/*
 * Regression test for shutting a pool down while producers are blocked in
 * thread_pool_submit on a full queue.
 *
 * Each round fills a pool completely (every worker busy behind a closed gate,
 * the queue full) and starts a group of submitters that block on backpressure.
 * The main thread then follows the order the header requires: it calls
 * thread_pool_shutdown, joins every submitter, and only then calls
 * thread_pool_destroy. No sleep is needed. A submitter that reaches
 * thread_pool_submit after shutdown simply returns EBUSY.
 *
 * Two kinds of round alternate. In the first the gate stays closed until every
 * submitter has been joined, so each submit must return EBUSY, and a shutdown
 * that fails to wake a blocked submitter hangs the join until the watchdog
 * fails the test. In the second the gate opens and at least one blocked submit
 * is queued and run before shutdown, so each remaining submit either queues a
 * task or returns EBUSY, and destroy must run every task that was queued. Run
 * under AddressSanitizer or ThreadSanitizer, a pool freed while another thread
 * is still using it shows up as a use-after-free.
 *
 * Uses pthreads directly, so it builds on POSIX systems only.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "thread_pool.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    worker_count = 2,
    queue_capacity = 1,
    gated_task_count = worker_count + queue_capacity,
    rounds_per_kind = 100,
    round_watchdog_seconds = 60
};

typedef struct round_state {
    thread_pool_t *pool;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int gate_open;
    unsigned int ready;
    unsigned int executed;
} round_state_t;

typedef struct submitter {
    round_state_t *state;
    pthread_t thread;
    int rc;
    int err;
} submitter_t;

static void on_watchdog(int signal_number)
{
    static const char message[] =
        "destroy_race: FAILED, a round timed out, a blocked submit was never woken\n";
    ssize_t written;

    (void)signal_number;
    written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(EXIT_FAILURE);
}

static void *gated_task(void *arg)
{
    round_state_t *state;

    state = (round_state_t *)arg;

    pthread_mutex_lock(&state->mutex);
    while (!state->gate_open) {
        pthread_cond_wait(&state->changed, &state->mutex);
    }
    state->executed += 1;
    pthread_cond_broadcast(&state->changed);
    pthread_mutex_unlock(&state->mutex);

    return state;
}

static void *counting_task(void *arg)
{
    round_state_t *state;

    state = (round_state_t *)arg;

    pthread_mutex_lock(&state->mutex);
    state->executed += 1;
    pthread_cond_broadcast(&state->changed);
    pthread_mutex_unlock(&state->mutex);

    return state;
}

static void *submitter_main(void *arg)
{
    submitter_t *self;
    thread_pool_future_t *future;

    self = (submitter_t *)arg;
    future = NULL;

    pthread_mutex_lock(&self->state->mutex);
    self->state->ready += 1;
    pthread_cond_broadcast(&self->state->changed);
    pthread_mutex_unlock(&self->state->mutex);

    errno = 0;
    self->rc = thread_pool_submit(self->state->pool, counting_task, self->state, &future);
    self->err = errno;

    if (self->rc == 0) {
        thread_pool_future_destroy(future);
    }

    return NULL;
}

static void *opener_main(void *arg)
{
    round_state_t *state;

    state = (round_state_t *)arg;

    pthread_mutex_lock(&state->mutex);
    state->gate_open = 1;
    pthread_cond_broadcast(&state->changed);
    pthread_mutex_unlock(&state->mutex);

    return NULL;
}

/*
 * Returns 0 on success. Adds to *accepted and *rejected the submitter outcomes.
 * With open_gate_early the gate opens before shutdown, otherwise only after every
 * submitter has been joined.
 */
static int run_round(
    unsigned int submitter_count,
    int open_gate_early,
    unsigned long *accepted,
    unsigned long *rejected
)
{
    round_state_t state;
    thread_pool_future_t *futures[gated_task_count];
    submitter_t *submitters;
    pthread_t opener;
    unsigned int accepted_here;
    unsigned int i;
    int failed;

    failed = 0;
    accepted_here = 0;

    /* Restarts the watchdog, so each round gets the full time however slow the sanitizer is. */
    alarm(round_watchdog_seconds);

    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.changed, NULL);

    submitters = (submitter_t *)calloc(submitter_count, sizeof(*submitters));
    state.pool = thread_pool_create(worker_count, queue_capacity);
    if (submitters == NULL || state.pool == NULL) {
        fprintf(stderr, "setup failed: could not allocate the round\n");
        return 1;
    }

    /*
     * The third submit can only return once a worker has taken each of the
     * first two tasks, so the pool is now full: both workers wait on the gate
     * and the queue holds the third task.
     */
    for (i = 0; i < gated_task_count; ++i) {
        if (thread_pool_submit(state.pool, gated_task, &state, &futures[i]) != 0) {
            fprintf(stderr, "setup failed: submit of gated task %u returned an error\n", i);
            return 1;
        }
    }

    for (i = 0; i < submitter_count; ++i) {
        submitters[i].state = &state;
        if (pthread_create(&submitters[i].thread, NULL, submitter_main, &submitters[i]) != 0) {
            fprintf(stderr, "setup failed: could not start submitter %u\n", i);
            return 1;
        }
    }

    pthread_mutex_lock(&state.mutex);
    while (state.ready < submitter_count) {
        pthread_cond_wait(&state.changed, &state.mutex);
    }
    pthread_mutex_unlock(&state.mutex);

    if (open_gate_early) {
        if (pthread_create(&opener, NULL, opener_main, &state) != 0) {
            fprintf(stderr, "setup failed: could not start the gate opener\n");
            return 1;
        }

        /*
         * Once the gate opens, a blocked submitter gets the first free slot.
         * Wait until its task has run, so every such round accepts at least
         * one submit before shutdown.
         */
        pthread_mutex_lock(&state.mutex);
        while (state.executed <= gated_task_count) {
            pthread_cond_wait(&state.changed, &state.mutex);
        }
        pthread_mutex_unlock(&state.mutex);
    }

    thread_pool_shutdown(state.pool);

    for (i = 0; i < submitter_count; ++i) {
        pthread_join(submitters[i].thread, NULL);

        if (submitters[i].rc == 0) {
            accepted_here += 1;
        } else if (submitters[i].rc == -1 && submitters[i].err == EBUSY) {
            *rejected += 1;
        } else {
            fprintf(
                stderr,
                "submitter %u: rc=%d errno=%d, expected 0 or -1 with EBUSY\n",
                i,
                submitters[i].rc,
                submitters[i].err
            );
            failed = 1;
        }
    }
    *accepted += accepted_here;

    if (!open_gate_early && pthread_create(&opener, NULL, opener_main, &state) != 0) {
        fprintf(stderr, "setup failed: could not start the gate opener\n");
        return 1;
    }
    pthread_join(opener, NULL);

    if (!open_gate_early && accepted_here != 0) {
        fprintf(
            stderr,
            "%u submits succeeded although the queue was full and the gate was closed\n",
            accepted_here
        );
        failed = 1;
    }

    thread_pool_destroy(state.pool);

    for (i = 0; i < gated_task_count; ++i) {
        thread_pool_future_destroy(futures[i]);
    }

    if (state.executed != gated_task_count + accepted_here) {
        fprintf(
            stderr,
            "destroy dropped work: %u tasks ran, %u were accepted\n",
            state.executed,
            gated_task_count + accepted_here
        );
        failed = 1;
    }

    free(submitters);
    pthread_cond_destroy(&state.changed);
    pthread_mutex_destroy(&state.mutex);

    return failed;
}

int main(void)
{
    static const unsigned int sizes[] = { 1, 2, 4, 8, 64 };

    unsigned long accepted;
    unsigned long rejected;
    unsigned long rounds;
    size_t s;
    int kind;
    int round;

    accepted = 0;
    rejected = 0;
    rounds = 0;

    signal(SIGALRM, on_watchdog);

    for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        for (kind = 0; kind < 2; ++kind) {
            for (round = 0; round < rounds_per_kind; ++round) {
                if (run_round(sizes[s], kind, &accepted, &rejected) != 0) {
                    fprintf(
                        stderr,
                        "destroy_race: FAILED (%u submitters, gate opened %s shutdown, round %d)\n",
                        sizes[s],
                        kind ? "before" : "after",
                        round
                    );
                    return EXIT_FAILURE;
                }
                rounds += 1;
            }
        }
    }

    alarm(0);

    if (rejected == 0) {
        fprintf(stderr, "destroy_race: FAILED, no submit ever saw EBUSY, so the shutdown path was not exercised\n");
        return EXIT_FAILURE;
    }

    printf(
        "destroy_race: %lu rounds passed, %lu submits returned EBUSY, %lu queued before shutdown\n",
        rounds,
        rejected,
        accepted
    );

    return EXIT_SUCCESS;
}
