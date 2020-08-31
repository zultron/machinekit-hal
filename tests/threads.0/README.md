Test thread preemption and execution order

This test runs two threads, which must run on a single CPU, executing
the three `threadtest` comp functions:

- Fast thread:  Should never be preempted
  - Run `threadtest.increment`: Set mutex; increment counter
  - Run `sampler`:  Record `threadtest` pins
  - Run `threadtest.release`: Check and release mutex
- Slow thread:
  - Run `threadtest.reset`:  Check mutex and reset counter

Additionally, all functions record their start and stop time.

The `checkresult` function checks several things:
- The mutex is not set upon entry to `increment`
  - The `release` function has already released the mutex
- The mutex can be otained by `increment`
  - The `release` function has already released the mutex
- The reset function (on the slow thread) start and end times are
  outside the increment and reset function start and end times,
  respectively
  - The slow thread has not preempted the fast thread (on a single
    CPU)

It also may fail if any barriers are failing to ensure write/read
consistency.

If the fast and slow threads are running on separate CPUs, their
interaction will be different, and the slow thread may execute during
the fast thread without preempting it.
