# User Guide: Coroutine APIs

This guide covers both the C++17 stackful coroutine API (`co::` namespace) and
the C++20 stackless coroutine API (`co20::` namespace).

## Table of Contents

1. [Introduction](#introduction)
2. [Basic Concepts](#basic-concepts)
3. [C++17 API](#c17-api)
   - [Creating a Scheduler](#creating-a-scheduler)
   - [Creating Coroutines](#creating-coroutines)
   - [Waiting for Events](#waiting-for-events)
   - [Yielding Control](#yielding-control)
   - [Sleeping](#sleeping)
   - [Generators](#generators)
   - [Complete Examples](#complete-examples)
4. [C++20 API](#c20-api)
   - [Getting Started](#getting-started)
   - [Free Functions vs Coroutine Reference](#free-functions-vs-coroutine-reference)
   - [Waiting for Events (C++20)](#waiting-for-events-c20)
   - [Sleeping (C++20)](#sleeping-c20)
   - [Aborting Coroutines](#aborting-coroutines)
   - [Accessing Self and Scheduler](#accessing-self-and-scheduler)
   - [Complete Examples (C++20)](#complete-examples-c20)
5. [Best Practices](#best-practices)

## Introduction

This library provides two coroutine implementations:

- **C++17 Stackful Coroutines** (`co::` namespace) -- Uses manual stack switching with a custom context switcher. Full-featured with generators, multiple wait modes, and broad platform support.
- **C++20 Stackless Coroutines** (`co20::` namespace) -- A standalone library using C++20 `co_await`/`co_return`. Lightweight, no dependency on the C++17 library.

Key features:
- **Cooperative multitasking**: Coroutines yield control explicitly
- **Event-driven I/O**: Efficiently wait for file descriptors via epoll (Linux) or poll
- **Lightweight**: C++17 coroutines use fixed-size stacks (default 64KB); C++20 coroutines use compiler-managed frames
- **Portable**: Works on Linux, macOS, and other Unix-like systems
- **Free-function API**: Both libraries provide namespace-scoped free functions so coroutine bodies don't need explicit parameter passing

## Basic Concepts

- **Scheduler**: Manages and runs multiple coroutines. You typically create one scheduler per application or per thread.
- **Coroutine**: A single execution context. C++17 coroutines have their own stack; C++20 coroutines use compiler-generated frames.
- **Yield**: Voluntarily give up control to allow other coroutines to run.
- **Wait**: Suspend until a file descriptor becomes ready for I/O.
- **Task** (C++20 only): The return type for C++20 coroutine functions.

---

## C++17 API

The C++17 API uses the `co::` namespace. Coroutines are stackful -- each one gets its own stack -- and suspend operations are regular function calls.

### Creating a Scheduler

Start by creating a `CoroutineScheduler`:

```cpp
#include "co/coroutine.h"

using namespace co;

CoroutineScheduler scheduler;
```

The scheduler manages all coroutines and coordinates their execution. You'll call `scheduler.Run()` to start executing coroutines.

## Creating Coroutines

There are two main ways to create coroutines:

### Using Spawn

The `Spawn` method is the simplest way to create a coroutine. It's a convenience method on the scheduler that creates and manages the coroutine for you:

```cpp
CoroutineScheduler scheduler;

// Spawn a simple coroutine
scheduler.Spawn([]() {
    std::cout << "Hello from coroutine!" << std::endl;
});

// Spawn with options (name, stack size, etc.)
scheduler.Spawn([]() {
    std::cout << "Named coroutine" << std::endl;
}, {
    .name = "my_coroutine",
    .stack_size = 128 * 1024  // 128KB stack
});

scheduler.Run();  // Run until all coroutines complete
```

**Note**: When using `Spawn`, you can use the non-invasive API functions like `co::Wait()`, `co::Yield()`, etc., without needing a `Coroutine*` parameter.

### Using Coroutine Constructor

For more control, you can create coroutines explicitly:

```cpp
CoroutineScheduler scheduler;

// Using a lambda with Coroutine* parameter
Coroutine co1(scheduler, [](Coroutine *c) {
    std::cout << "Coroutine 1 running" << std::endl;
    c->Yield();  // Yield control
    std::cout << "Coroutine 1 resumed" << std::endl;
});

// Using a lambda with const Coroutine& parameter (preferred)
Coroutine co2(scheduler, [](const Coroutine &c) {
    std::cout << "Coroutine 2 running" << std::endl;
    c.Yield();
    std::cout << "Coroutine 2 resumed" << std::endl;
});

// Using CoroutineOptions for more control
CoroutineOptions opts;
opts.name = "my_coroutine";
opts.stack_size = 128 * 1024;
opts.autostart = true;  // Start automatically (default)

Coroutine co3(scheduler, [](const Coroutine &c) {
    // Coroutine body
}, opts);

scheduler.Run();
```

## Waiting for Events

One of the most powerful features of coroutines is the ability to wait for file descriptors (sockets, pipes, etc.) to become ready for I/O without blocking the entire process.

### Wait

The `Wait` function blocks the coroutine until a file descriptor becomes ready:

```cpp
CoroutineScheduler scheduler;

int pipes[2];
pipe(pipes);

scheduler.Spawn([pipes]() {
    // Wait for data to be available for reading
    int fd = co::Wait(pipes[0], POLLIN);
    if (fd == pipes[0]) {
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf));
        // Process data...
    }
});

scheduler.Spawn([pipes]() {
    // Wait for write buffer to be available
    int fd = co::Wait(pipes[1], POLLOUT);
    if (fd == pipes[1]) {
        const char *msg = "Hello!";
        write(fd, msg, strlen(msg));
    }
});

scheduler.Run();
```

**Event masks**: Use `POLLIN` (data available for reading), `POLLOUT` (ready for writing), or `POLLERR` (error condition).

### Poll

`Poll` checks if file descriptors are ready without blocking. It returns immediately:

```cpp
scheduler.Spawn([pipes]() {
    std::vector<int> fds = {pipes[0], pipes[1]};
    
    // Check if any fd is ready (non-blocking)
    int ready_fd = co::Poll(fds, POLLIN);
    if (ready_fd != -1) {
        // At least one fd is ready
        // Process it...
    } else {
        // No fds are ready yet
    }
});
```

### PollAndWait

`PollAndWait` combines `Poll` and `Wait`: it first checks if the fd is ready, and if not, waits for it:

```cpp
scheduler.Spawn([pipes]() {
    // Efficient: check first, wait only if needed
    int fd = co::PollAndWait(pipes[0], POLLIN);
    if (fd == pipes[0]) {
        // Data is ready
        char buf[256];
        read(fd, buf, sizeof(buf));
    }
});
```

### Timeouts

All `Wait` functions support optional timeouts. If the timeout expires before the fd becomes ready, the function returns `-1`:

```cpp
scheduler.Spawn([pipes]() {
    // Wait up to 1 second (nanoseconds)
    int fd = co::Wait(pipes[0], POLLIN, 1000000000ULL);
    
    if (fd == -1) {
        std::cout << "Timeout!" << std::endl;
    } else {
        // Data is ready
    }
});

// Using std::chrono for timeouts (more readable)
scheduler.Spawn([pipes]() {
    int fd = co::Wait(pipes[0], POLLIN, std::chrono::seconds(1));
    // or
    int fd = co::Wait(pipes[0], POLLIN, std::chrono::milliseconds(500));
});
```

### Waiting for Multiple File Descriptors

You can wait for multiple file descriptors at once:

```cpp
scheduler.Spawn([pipes1, pipes2]() {
    std::vector<int> fds = {pipes1[0], pipes2[0]};
    
    // Wait for any of these fds to become ready
    int ready_fd = co::Wait(fds, POLLIN, std::chrono::seconds(5));
    
    if (ready_fd == -1) {
        std::cout << "Timeout waiting for fds" << std::endl;
    } else if (ready_fd == pipes1[0]) {
        // pipes1[0] is ready
    } else if (ready_fd == pipes2[0]) {
        // pipes2[0] is ready
    }
});
```

## Yielding Control

Use `Yield()` to voluntarily give up control to other coroutines:

```cpp
scheduler.Spawn([]() {
    for (int i = 0; i < 10; i++) {
        std::cout << "Coroutine 1: " << i << std::endl;
        co::Yield();  // Let other coroutines run
    }
});

scheduler.Spawn([]() {
    for (int i = 0; i < 10; i++) {
        std::cout << "Coroutine 2: " << i << std::endl;
        co::Yield();
    }
});

scheduler.Run();
```

Output will interleave between the two coroutines, demonstrating cooperative multitasking.

## Sleeping

Coroutines can sleep for a specified duration:

```cpp
scheduler.Spawn([]() {
    // Sleep for 1 second
    co::Sleep(std::chrono::seconds(1));
    
    // Sleep for 100 milliseconds
    co::Millisleep(100);
    
    // Sleep for nanoseconds
    co::Nanosleep(500000000ULL);  // 0.5 seconds
});
```

## Generators

Generators are coroutines that produce a sequence of values. They're useful for creating iterable sequences:

```cpp
CoroutineScheduler scheduler;

// Create a generator that produces numbers 1-5
Generator<int> generator(scheduler, [](Generator<int> *gen) {
    for (int i = 1; i <= 5; i++) {
        gen->YieldValue(i);
    }
});

// Use the generator in a coroutine
Coroutine consumer(scheduler, [&generator](Coroutine *c) {
    while (generator.IsAlive()) {
        int value = c->Call(generator);
        if (generator.IsAlive()) {
            std::cout << "Got value: " << value << std::endl;
        }
    }
});

scheduler.Run();
```

## Complete Examples

### Example 1: Simple Producer-Consumer with Pipes

```cpp
#include "co/coroutine.h"
#include <iostream>
#include <unistd.h>

using namespace co;

int main() {
    CoroutineScheduler scheduler;
    int pipes[2];
    pipe(pipes);

    // Producer: writes data
    scheduler.Spawn([pipes]() {
        for (int i = 0; i < 10; i++) {
            // Wait until pipe is ready for writing
            int fd = co::Wait(pipes[1], POLLOUT);
            if (fd == pipes[1]) {
                char buf[32];
                int n = snprintf(buf, sizeof(buf), "Message %d\n", i);
                write(fd, buf, n);
            }
            co::Yield();  // Give consumer a chance to run
        }
        close(pipes[1]);
    });

    // Consumer: reads data
    scheduler.Spawn([pipes]() {
        char buf[256];
        for (;;) {
            // Wait until data is available
            int fd = co::Wait(pipes[0], POLLIN);
            if (fd == pipes[0]) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n == 0) {
                    // EOF
                    break;
                }
                std::cout.write(buf, n);
            }
        }
        close(pipes[0]);
    });

    scheduler.Run();
    return 0;
}
```

### Example 2: Waiting with Timeout

```cpp
#include "co/coroutine.h"
#include <iostream>
#include <unistd.h>
#include <chrono>

using namespace co;

int main() {
    CoroutineScheduler scheduler;
    int pipes[2];
    pipe(pipes);

    scheduler.Spawn([pipes]() {
        std::cout << "Waiting for data (with 2 second timeout)..." << std::endl;
        
        // Wait up to 2 seconds
        int fd = co::Wait(pipes[0], POLLIN, std::chrono::seconds(2));
        
        if (fd == -1) {
            std::cout << "Timeout! No data received." << std::endl;
        } else {
            char buf[256];
            ssize_t n = read(fd, buf, sizeof(buf));
            std::cout << "Received: ";
            std::cout.write(buf, n);
        }
        close(pipes[0]);
    });

    // This coroutine will trigger after 3 seconds (after timeout)
    scheduler.Spawn([pipes]() {
        co::Sleep(std::chrono::seconds(3));
        const char *msg = "Too late!";
        write(pipes[1], msg, strlen(msg));
        close(pipes[1]);
    });

    scheduler.Run();
    return 0;
}
```

### Example 3: Multiple File Descriptors

```cpp
#include "co/coroutine.h"
#include <iostream>
#include <unistd.h>
#include <vector>

using namespace co;

int main() {
    CoroutineScheduler scheduler;
    
    int pipes1[2], pipes2[2], pipes3[2];
    pipe(pipes1);
    pipe(pipes2);
    pipe(pipes3);

    scheduler.Spawn([pipes1, pipes2, pipes3]() {
        std::vector<int> read_fds = {pipes1[0], pipes2[0], pipes3[0]};
        
        for (int i = 0; i < 3; i++) {
            // Wait for any of the three pipes to have data
            int ready_fd = co::Wait(read_fds, POLLIN);
            
            if (ready_fd != -1) {
                char buf[256];
                ssize_t n = read(ready_fd, buf, sizeof(buf));
                std::cout << "Received from fd " << ready_fd << ": ";
                std::cout.write(buf, n);
            }
        }
        
        close(pipes1[0]);
        close(pipes2[0]);
        close(pipes3[0]);
    });

    // Write to different pipes at different times
    scheduler.Spawn([pipes1]() {
        co::Sleep(std::chrono::milliseconds(100));
        write(pipes1[1], "From pipe 1\n", 12);
        close(pipes1[1]);
    });

    scheduler.Spawn([pipes2]() {
        co::Sleep(std::chrono::milliseconds(200));
        write(pipes2[1], "From pipe 2\n", 12);
        close(pipes2[1]);
    });

    scheduler.Spawn([pipes3]() {
        co::Sleep(std::chrono::milliseconds(300));
        write(pipes3[1], "From pipe 3\n", 12);
        close(pipes3[1]);
    });

    scheduler.Run();
    return 0;
}
```

## Best Practices (C++17)

1. **Use Spawn for simple coroutines**: The `Spawn` method is cleaner and automatically manages coroutine lifetime.

2. **Always check return values**: `Wait` returns `-1` on timeout, so always check the return value:

```cpp
int fd = co::Wait(pipe_fd, POLLIN, timeout);
if (fd == -1) {
    // Handle timeout
} else if (fd == pipe_fd) {
    // Handle ready fd
}
```

3. **Close file descriptors**: Always close file descriptors when done to avoid resource leaks.

4. **Use timeouts**: Always use timeouts for `Wait` operations in production code to avoid indefinite blocking.

5. **Yield periodically**: In long-running coroutines without I/O, call `Yield()` periodically to allow other coroutines to run.

6. **Non-invasive API**: Prefer the free functions (`co::Wait`, `co::Yield`, etc.) over passing a `Coroutine*` parameter.

### C++17 API Reference Summary

**CoroutineScheduler:**
- `Run()` -- run the scheduler until all coroutines complete
- `Stop()` -- stop the scheduler
- `Spawn(function, options)` -- create and start a coroutine

**Free functions (co:: namespace):**
- `co::self` -- pointer to the currently running coroutine
- `co::scheduler` -- pointer to the current scheduler
- `co::Wait(fd, events, timeout)` -- wait for FD readiness
- `co::Poll(fds, events)` -- non-blocking FD check
- `co::PollAndWait(fd, events, timeout)` -- poll then wait if needed
- `co::Yield()` -- yield control
- `co::Sleep(duration)` -- sleep (chrono duration)
- `co::Millisleep(ms)` -- sleep for milliseconds
- `co::Nanosleep(ns)` -- sleep for nanoseconds

For full details, see `co/coroutine.h`.

---

## C++20 API

The C++20 library is a separate, standalone implementation in the `co20::` namespace. It has no dependency on the C++17 library. It uses C++20 compiler coroutines (`co_await`, `co_return`), an epoll/poll-based scheduler, and Abseil's `flat_hash_map`/`flat_hash_set` for fast container lookups.

Include `co/coroutine_cpp20.h` to use it. You must compile with `-std=c++20`.

### Getting Started

```cpp
#include "co/coroutine_cpp20.h"

co20::Scheduler scheduler;

scheduler.Spawn([]() -> co20::Task {
  co_await co20::Yield();
  co_return;
}, "my_coroutine");

scheduler.Run();
```

Key differences from the C++17 API:
- Coroutine functions must return `co20::Task`
- All suspend operations (`Yield`, `Wait`, `Sleep`) return awaitables and must be used with `co_await`
- Use `co_return` instead of a normal `return`

### Free Functions vs Coroutine Reference

There are two styles for writing coroutine bodies. Both are fully supported.

**Free-function style** -- the coroutine lambda takes no parameters and uses
`co20::Yield()`, `co20::Wait()`, `co20::Sleep()`, etc.:

```cpp
scheduler.Spawn([]() -> co20::Task {
  co_await co20::Sleep(std::chrono::milliseconds(100));
  int fd = co_await co20::Wait(my_fd, POLLIN);
  co_await co20::Yield();
  co_return;
}, "my_coroutine");
```

**Coroutine-reference style** -- the lambda receives a `co20::Coroutine&` and
calls methods on it:

```cpp
scheduler.Spawn([](co20::Coroutine& co) -> co20::Task {
  co_await co.Sleep(std::chrono::milliseconds(100));
  int fd = co_await co.Wait(my_fd, POLLIN);
  co_await co.Yield();
  co_return;
}, "my_coroutine");
```

The free-function style is generally preferred as it's cleaner. Both styles
can be mixed in the same program.

### Waiting for Events (C++20)

`Wait` suspends the coroutine until a file descriptor becomes ready:

```cpp
scheduler.Spawn([pipes]() -> co20::Task {
  // Wait for data to be available for reading
  int fd = co_await co20::Wait(pipes[0], POLLIN);
  if (fd == pipes[0]) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    // Process data...
  }
  co_return;
});
```

`Wait` returns the file descriptor if it became ready, or `-1` on timeout/error.
Event masks are the standard poll flags: `POLLIN`, `POLLOUT`, etc.

### Sleeping (C++20)

```cpp
scheduler.Spawn([]() -> co20::Task {
  // Sleep using std::chrono (recommended)
  co_await co20::Sleep(std::chrono::milliseconds(100));
  co_await co20::Sleep(std::chrono::seconds(1));

  // Sleep for raw nanoseconds
  co_await co20::Sleep(1000000ULL);  // 1ms

  // Convenience functions
  co_await co20::Millisleep(100);
  co_await co20::Nanosleep(500000000ULL);  // 0.5s

  co_return;
});
```

### Aborting Coroutines

A coroutine can be aborted from another coroutine. The aborted coroutine will
receive an `AbortException` the next time it hits a `co_await` on `Yield`, `Wait`,
or `Sleep`. Catch the exception to perform cleanup:

```cpp
co20::Scheduler scheduler;

struct State {
  bool aborted = false;
  co20::Coroutine* target = nullptr;
} state;

scheduler.Spawn([&state]() -> co20::Task {
  state.target = co20::self;
  try {
    for (;;) {
      co_await co20::Sleep(std::chrono::seconds(10));
    }
  } catch (...) {
    state.aborted = true;
  }
  co_return;
}, "worker");

scheduler.Spawn([&state]() -> co20::Task {
  co_await co20::Sleep(std::chrono::milliseconds(100));
  if (state.target) {
    state.target->Abort();
  }
  co_return;
}, "controller");

scheduler.Run();
// state.aborted is now true
```

### Accessing Self and Scheduler

Inside a coroutine, you can access the current coroutine and scheduler via
thread-local pointers, just like the C++17 API:

```cpp
scheduler.Spawn([]() -> co20::Task {
  // Get the current coroutine
  co20::Coroutine* me = co20::self;
  std::cout << "My name is: " << me->Name() << std::endl;

  // Get the scheduler
  co20::Scheduler* sched = co20::scheduler;

  // Or via the coroutine
  co20::Scheduler& sched2 = me->GetScheduler();

  co_return;
}, "example");
```

### Complete Examples (C++20)

#### Producer-Consumer with Pipes

```cpp
#include "co/coroutine_cpp20.h"
#include <unistd.h>

int main() {
  co20::Scheduler scheduler;
  int pipes[2];
  pipe(pipes);

  // Producer
  scheduler.Spawn([pipes]() -> co20::Task {
    for (int i = 0; i < 10; i++) {
      int fd = co_await co20::Wait(pipes[1], POLLOUT);
      if (fd == pipes[1]) {
        char c = 'A' + i;
        write(fd, &c, 1);
      }
    }
    close(pipes[1]);
    co_return;
  }, "producer");

  // Consumer
  scheduler.Spawn([pipes]() -> co20::Task {
    for (;;) {
      int fd = co_await co20::Wait(pipes[0], POLLIN);
      if (fd != pipes[0]) break;

      char c;
      ssize_t n = read(fd, &c, 1);
      if (n == 0) break;
      printf("Received: %c\n", c);
    }
    close(pipes[0]);
    co_return;
  }, "consumer");

  scheduler.Run();
  return 0;
}
```

#### Multiple Coroutines Yielding

```cpp
#include "co/coroutine_cpp20.h"
#include <cstdio>

int main() {
  co20::Scheduler scheduler;

  for (int i = 0; i < 5; i++) {
    scheduler.Spawn([i]() -> co20::Task {
      for (int j = 0; j < 3; j++) {
        printf("Coroutine %d, iteration %d\n", i, j);
        co_await co20::Yield();
      }
      co_return;
    }, "co_" + std::to_string(i));
  }

  scheduler.Run();
  return 0;
}
```

### C++20 API Reference Summary

**co20::Scheduler:**
- `Run()` -- run the scheduler until all coroutines complete
- `Stop()` -- stop the scheduler
- `Spawn(function, name)` -- create and start a coroutine (accepts lambdas with or without `Coroutine&` parameter)

**co20::Coroutine:**
- `Yield()` -- returns a `YieldAwaitable`
- `Wait(fd, event_mask, timeout_ns)` -- returns a `WaitAwaitable`
- `Sleep(nanoseconds)` -- returns a `SleepAwaitable`
- `Sleep(std::chrono::duration)` -- returns a `SleepAwaitable`
- `Abort()` -- request abort (throws `AbortException` at next suspend point)
- `IsAborted()` -- check if abort has been requested
- `Name()` -- get the coroutine's name
- `GetState()` -- get current state
- `GetScheduler()` -- get the owning scheduler

**Free functions (co20:: namespace):**
- `co20::self` -- pointer to the currently running coroutine
- `co20::scheduler` -- pointer to the current scheduler
- `co20::Yield()` -- yield control (returns awaitable)
- `co20::Wait(fd, event_mask, timeout_ns)` -- wait for FD (returns awaitable)
- `co20::Sleep(nanoseconds)` -- sleep (returns awaitable)
- `co20::Sleep(std::chrono::duration)` -- sleep with chrono (returns awaitable)
- `co20::Millisleep(ms)` -- sleep for milliseconds (returns awaitable)
- `co20::Nanosleep(ns)` -- sleep for nanoseconds (returns awaitable)

For full details, see `co/coroutine_cpp20.h`.

---

## Best Practices

1. **Always check Wait return values**: `Wait` returns `-1` on timeout/error. Always check.

2. **Close file descriptors**: Always close FDs when done to avoid resource leaks.

3. **Yield periodically**: In long-running coroutines without I/O, call `Yield()` to allow other coroutines to run.

4. **Prefer the free-function API**: Using `co::Yield()` / `co20::Yield()` is cleaner than passing coroutine pointers or references.

5. **Use `co_return`**: In C++20 coroutines, always end with `co_return` and never use a bare `return` statement.

6. **Avoid `ASSERT_*` / `FAIL()` in C++20 coroutines**: Google Test macros that expand to `return` statements won't compile inside coroutines. Use `EXPECT_*` or store results in variables and assert after `Run()`.

7. **Use struct captures for shared state in C++20 tests**: When multiple C++20 coroutines need to share variables, put them in a struct and capture a reference to the struct to avoid lambda capture issues with coroutine frames.

