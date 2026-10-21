# C Thread Pool

**Timeline:** April 2026

A reusable thread pool in C11 with a fixed set of worker threads, a bounded task
queue that applies backpressure, futures for collecting each task's result, and a
graceful shutdown that finishes queued work before the workers exit. One source file
runs on Windows (Win32 threads, critical sections and condition variables) and on
POSIX systems (pthreads).

## How it works

- **Workers.** `thread_pool_create(thread_count, queue_capacity)` starts the workers
  up front. Each one sleeps on a condition variable until a task is queued.
- **Bounded queue with backpressure.** The queue holds at most `queue_capacity`
  tasks. When it is full, `thread_pool_submit` blocks until a worker frees a slot,
  so a fast producer cannot grow memory without limit.
- **Futures.** Every submit hands back a `thread_pool_future_t` through
  `out_future`. The argument is required: `thread_pool_submit` fails with `EINVAL`
  if it is `NULL`, so fire-and-forget submission is not possible. The caller must
  pass every future to `thread_pool_future_destroy`, or it leaks.
  `thread_pool_future_get` blocks until that task finishes and returns its
  `void *` result. The caller owns any heap memory the task returns.
- **Shutdown.** `thread_pool_wait` blocks until the queue is empty and every worker
  is idle. `thread_pool_shutdown` stops accepting work: every thread blocked in
  `thread_pool_submit` wakes and returns `EBUSY`, every later submit returns `EBUSY`,
  and the tasks already queued still run. It frees nothing. `thread_pool_destroy`
  lets the workers drain what is already queued, joins them and frees the pool, so it
  is called after the threads that submit have been joined.
- **Portability.** A thin layer maps mutex, condition-variable and thread calls to
  Win32 or pthreads at compile time, so the pool logic is written once.

## API

```c
thread_pool_t *thread_pool_create(size_t thread_count, size_t queue_capacity);
int  thread_pool_submit(thread_pool_t *pool, thread_pool_task_fn function,
                        void *arg, thread_pool_future_t **out_future);
void *thread_pool_future_get(thread_pool_future_t *future);
void thread_pool_future_destroy(thread_pool_future_t *future);
void thread_pool_wait(thread_pool_t *pool);
void thread_pool_shutdown(thread_pool_t *pool);
void thread_pool_destroy(thread_pool_t *pool);
size_t thread_pool_thread_count(const thread_pool_t *pool);
size_t thread_pool_queue_capacity(const thread_pool_t *pool);
```

A task is any `void *(*)(void *)` function.

`thread_pool_create` returns `NULL` with `errno` set to `EINVAL` when either argument
is zero and `ENOMEM` when it cannot allocate or start the threads.
`thread_pool_submit` returns 0 on success, or -1 with `errno` set to `EINVAL`,
`ENOMEM`, or `EBUSY` once `thread_pool_shutdown` has been called.

## Rules for callers

- A task must not call `thread_pool_wait` or `thread_pool_destroy` on its own pool.
  Both wait for the running-task count to reach zero, and the calling task is part of
  that count, so the call never returns.
- A task must not call `thread_pool_submit` on its own pool. If the queue is full and
  every worker is blocked this way, no worker is left to free a slot.
- `thread_pool_destroy` frees the pool, so no other call on that pool may be running
  when it starts or may start while it runs. That includes a `thread_pool_submit`
  that has been entered but has not yet blocked. To stop while other threads still
  submit, call `thread_pool_shutdown`, join every thread that calls
  `thread_pool_submit`, and only then call `thread_pool_destroy`.
- `thread_pool_shutdown` can be called from any thread, from a task, and more than
  once, while other threads submit or wait.
- Do not use a pool after `thread_pool_destroy` returns. Futures stay valid and can
  still be read and destroyed.

## Demo

`src/main.c` creates a pool of 4 workers with a queue capacity of 4, then submits 12
jobs. The pool holds 8 jobs at once (4 running and 4 queued), so the submits for
jobs 9 to 12 block until a worker frees a slot. The demo times each submit and prints
a line when one waited 10 ms or more. Each job sleeps 120 to 330 ms and computes a sum
of squares; the results come back through the futures in submission order:

```text
Submitted job 8 with input=4750 and delay=330 ms.
Submitted job 9 with input=5000 and delay=120 ms.
  Queue was full: submit waited 120 ms for a free slot.
Submitted job 10 with input=5250 and delay=190 ms.
  Queue was full: submit waited 70 ms for a free slot.
...
Collecting results from futures:
Job 01 complete: sum_of_squares(1..3000) = 9004500500
...
Job 12 complete: sum_of_squares(1..5750) = 63386323875

All jobs completed successfully.
```

In 20 runs on WSL 2, jobs 9 to 12 each reported a wait in every run, about 120, 70, 50
and 20 ms, and no earlier job did. The wait lines depend on timing. In 20 runs on
Windows, the waits of jobs 9 to 11 ranged from 42 to 139 ms, and job 12 printed a
wait line in 14 of the 20 runs because its wait was sometimes under the 10 ms
threshold.

## Build and run

Linux (or WSL):

```bash
make
./thread_pool_demo
```

Windows with MinGW: `make` (GNU Make), or `./build.ps1` if make is not installed,
then `./thread_pool_demo.exe`.

## Tests

`tests/destroy_race.c` is a regression test for shutting a pool down while producers
are blocked on a full queue. Each round uses 2 workers and a queue of 1, keeps both
workers busy behind a gate so the queue is full, and starts a group of submitters
that block on backpressure. The main thread then calls `thread_pool_shutdown`, joins
every submitter and calls `thread_pool_destroy`, the order the rules above require.
The test has no sleeps: a submitter that reaches `thread_pool_submit` after the
shutdown simply gets `EBUSY`.

Two kinds of round alternate. In the first the gate stays closed until every
submitter has been joined, so every submit must return `EBUSY`; a shutdown that fails
to wake a blocked submitter hangs the join, and a 60 second watchdog on each round
then fails the test. In the second the gate opens before the shutdown and at least one blocked submit
is queued and run first, so each remaining submit either queues a task or returns
`EBUSY`, and every queued task must run before `thread_pool_destroy` returns. The test
runs 100 rounds of each kind with 1, 2, 4, 8 and 64 submitters (1000 rounds). It uses
pthreads directly, so it builds on POSIX systems only.

```bash
make test
make test-asan
make test-tsan TSAN_RUN="setarch $(uname -m) -R"
```

The ThreadSanitizer runtime refuses to start when the kernel's address-space
randomization is more random than it supports, and exits with `FATAL: ThreadSanitizer:
unexpected memory mapping`. GCC 13.3 on WSL 2 does this, so the `TSAN_RUN` prefix is
needed there; it turns randomization off for the test process. If you see that message
on another system, use the same prefix or lower the randomization with
`sudo sysctl vm.mmap_rnd_bits=28`. Where the runtime starts without help, as Clang 18
does on the same WSL 2 machine, run `make test-tsan` with no prefix, or
`make test-tsan CC=clang` to use Clang.

## Verification

Ubuntu 24.04 (WSL 2, 8 cores), GCC 13.3.0 and Clang 18.1.3:

- `make` with `-std=c11 -Wall -Wextra -Wpedantic`: no warnings.
- Demo under AddressSanitizer and UndefinedBehaviorSanitizer: exit code 0, no reports.
- Demo under ThreadSanitizer: exit code 0, no reports.
- `make test`: 100 of 100 runs passed.
- `make test-asan` (GCC): 100 of 100 runs passed.
- `make test-tsan` (GCC, with the `setarch` prefix): 30 of 30 runs passed.
- `make test-tsan CC=clang` (no prefix): 20 of 20 runs passed.
- With the wake-up call removed from `thread_pool_shutdown`, one run of `make test`
  failed after 60 seconds with `a round timed out, a blocked submit was never woken`.

The two demo checks use these commands:

```bash
gcc -std=c11 -O1 -g -fsanitize=address,undefined -Iinclude src/main.c src/thread_pool.c -o tp_asan -pthread
./tp_asan
gcc -std=c11 -O1 -g -fsanitize=thread -Iinclude src/main.c src/thread_pool.c -o tp_tsan -pthread
setarch $(uname -m) -R ./tp_tsan
```

Windows 11, MinGW.org GCC 6.3.0:

- `./build.ps1` and `make` (GNU Make 3.81 from MSYS) both build, `make` with no
  warnings, and the demo exits with code 0 and prints the same job results as the
  Linux run.
- `./build.ps1` exits with the gcc exit code and prints no success line when gcc fails.
- The tests were not run on Windows.

## License

MIT. See `LICENSE`.
