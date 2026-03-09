# Coroutine library in C++

This library provides two coroutine implementations:

1. **C++17 Stackful Coroutines** (`co::` namespace) -- The original implementation using manual stack switching with a custom context switcher. Full-featured with generators, multiple wait modes, and broad platform support.

2. **C++20 Stackless Coroutines** (`co20::` namespace) -- A standalone, lightweight implementation using C++20 compiler-generated coroutines with `co_await`. Uses an epoll/poll-based scheduler with no dependency on the C++17 library.

Both implementations support cooperative multitasking with file-descriptor-based I/O scheduling, sleeping, yielding, and coroutine abort.

## Why Coroutines?

Coroutines are an attractive alternative to threads. They give you the ability to
perform multiple tasks in parallel in a program without the danger of threads sharing
memory and bugs causing random memory overwrites.

This coroutine library uses a combination of multiplexed I/O (the poll function) and
a fair scheduler to allow a set of coroutines to cooperate in their use of the
CPU. Coroutines can *yield* control back to the scheduler or can *wait* for a set of events to
occur.

Using coroutines, you can write complex pieces of software, like network servers,
using blocking I/O calls without a thread in sight. The coroutines yield control
back to the scheduler when they are waiting for input or output. It allows you
to write safe code where the program state is held on the stack of a set
of coroutines instead of in complex finite state machines. This results in a design
that is much easier to understand and debug.

This is similar to the *Go* language's *goroutines*, but unfortunately, goroutines
are distributed among a set of threads, so they aren't really coroutines and you
still need thread synchronization when any data is shared among goroutines.

## Performance and Safety

The use of coroutines in a program can improve the performance of the program
if the program spends time waiting for I/O. Since the program will
be single-threaded, coroutines will not enable the execution to be split
across multiple cores. The latency of I/O handling can be improved though, if you
arrange your program to handle each I/O session in a different coroutine.

Coroutines do not suffer from a heavy context switch -- it's just swapping
the machine registers to a different context (C++17 mode) or a compiler-generated
state machine transition (C++20 mode). The judicious use of coroutines,
combined with multiple processes and an IPC system can improve both latency and
throughput in your system as well as safety.

Coroutines completely eliminate the need for locking shared data in a program.
Since only one coroutine can be executing at once, the access to memory is,
by definition, serial. This completely eliminates the need for locks unless
you really need to share memory between processes and there are good
solutions for that.

## Quick Start

### C++17 Stackful Coroutines

```cpp
#include "co/coroutine.h"

co::CoroutineScheduler scheduler;

scheduler.Spawn([]() {
  co::Sleep(std::chrono::milliseconds(100));
  int fd = co::Wait(some_fd, POLLIN);
  // handle I/O...
});

scheduler.Run();
```

### C++20 Stackless Coroutines

```cpp
#include "co/coroutine_cpp20.h"

co20::Scheduler scheduler;

// Using free functions (no Coroutine& parameter needed):
scheduler.Spawn([]() -> co20::Task {
  co_await co20::Sleep(std::chrono::milliseconds(100));
  int fd = co_await co20::Wait(some_fd, POLLIN);
  // handle I/O...
  co_return;
});

// Or with explicit Coroutine& parameter:
scheduler.Spawn([](co20::Coroutine& co) -> co20::Task {
  co_await co.Sleep(std::chrono::milliseconds(100));
  int fd = co_await co.Wait(some_fd, POLLIN);
  co_return;
});

scheduler.Run();
```

See the [User Guide](USER_GUIDE.md) for full API documentation and examples.

## The C++17 API

There are two C++ classes in the C++17 library:

1. The main *Coroutine* class. This is the object that represents a single coroutine.
1. The scheduler class, called *CoroutineScheduler*. This provides the multiplexed I/O
and schedules coroutines to run when they are ready.

To create a coroutine, use `Spawn` on the scheduler passing a function (typically a lambda)
that contains the body of the coroutine. Inside the coroutine you can use the
free functions in the `co::` namespace:

- `co::Yield()` -- yield control to other coroutines
- `co::Wait(fd, events, timeout)` -- wait for a file descriptor to become ready
- `co::Sleep(duration)` -- sleep for a duration
- `co::Millisleep(ms)` -- sleep for milliseconds
- `co::Nanosleep(ns)` -- sleep for nanoseconds
- `co::self` -- pointer to the currently running coroutine
- `co::scheduler` -- pointer to the current scheduler

## The C++20 API

The C++20 library lives in the `co20::` namespace and is completely standalone
(no dependency on the C++17 library). It requires a C++20 compiler and uses
Abseil's `flat_hash_map`/`flat_hash_set` for fast container lookups.

Key differences from the C++17 API:
- All suspend operations use `co_await`
- Coroutine functions return `co20::Task`
- Uses `co_return` instead of a normal return

Free functions in the `co20::` namespace:

- `co20::Yield()` -- returns an awaitable (use with `co_await`)
- `co20::Wait(fd, events, timeout)` -- wait for FD readiness
- `co20::Sleep(nanoseconds)` -- sleep for nanoseconds
- `co20::Sleep(std::chrono::duration)` -- sleep for a chrono duration
- `co20::Millisleep(ms)` -- sleep for milliseconds
- `co20::Nanosleep(ns)` -- sleep for nanoseconds
- `co20::self` -- pointer to the currently running coroutine
- `co20::scheduler` -- pointer to the current scheduler

## Building

### Bazel

The project is built using [Google's Bazel](https://github.com/bazelbuild/bazel).

```
# C++17 library and tests
bazel build //co:co
bazel test //co:coroutines_test

# C++20 library and tests
bazel build //co:co_cpp20
bazel test //co:test_cpp20
```

To import into another Bazel project:

```
http_archive(
  name = "coroutines",
  urls = ["https://github.com/dallison/co/archive/refs/tags/A.B.C.tar.gz"],
  strip_prefix = "co-A.B.C",
)
```

Where `A.B.C` is replaced by the version you want. You can add a `sha256` entry
to make sure you get the right version.

### CMake

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest
```

CMake targets:
- `co` -- C++17 stackful coroutine library
- `co_cpp20` -- C++20 stackless coroutine library
- `coroutines_test` -- C++17 tests
- `test_cpp20` -- C++20 tests

## Portability

This has been tested on macOS (Apple Silicon), Linux (x86_64), and QNX.

The C++17 library has a custom context switcher implemented in assembly language
for Linux, macOS, and QNX on x86_64 and ARM64 (Aarch64) architectures. A fallback
using setjmp/longjmp is also available.

The C++20 library uses standard C++20 coroutines and should work on any platform
with a C++20-compatible compiler (GCC 10+, Clang 14+, MSVC 2019+). On Linux it
uses epoll for I/O readiness; on other platforms it falls back to poll.

### Timer Implementations (C++17 library)

The C++17 library supports three timer implementations, selected automatically:

1. **Linux timerfd** (`CO_TIMER_TIMERFD`): Uses `timerfd_create()`. Default on Linux.
2. **macOS kqueue** (`CO_TIMER_EVENT`): Uses kqueue. Default on macOS.
3. **POSIX timers** (`CO_TIMER_POSIX`): Uses `timer_create()`. Default on QNX and other POSIX systems.

The C++20 library uses timerfd on Linux and falls back to immediate scheduling on
other platforms.

## Example Code

Two example programs are provided:

1. An HTTP server
1. An HTTP client

Run the server and it will open TCP port 80 on localhost and allow you to
send HTTP requests to it to get a file from the local file system.

```bash
$ bazel-bin/http_server/http_server
$ bazel-bin/http_client/http_client localhost /etc/hosts -j 100
```

Both the client and server are single threaded coroutine based programs that
can handle many requests at the same time.

## Licensing

This is licensed under the Apache License Version 2.0. Please see the LICENSE
file for details.
