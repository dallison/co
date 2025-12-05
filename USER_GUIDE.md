# User Guide: Coroutine and CoroutineScheduler APIs

This guide provides an introduction to using the `co::Coroutine` and `co::CoroutineScheduler` APIs for writing asynchronous, cooperative multitasking code in C++.

## Table of Contents

1. [Introduction](#introduction)
2. [Basic Concepts](#basic-concepts)
3. [Creating a Scheduler](#creating-a-scheduler)
4. [Creating Coroutines](#creating-coroutines)
   - [Using Spawn](#using-spawn)
   - [Using Coroutine Constructor](#using-coroutine-constructor)
5. [Waiting for Events](#waiting-for-events)
   - [Wait](#wait)
   - [Poll](#poll)
   - [PollAndWait](#pollandwait)
   - [Timeouts](#timeouts)
6. [Yielding Control](#yielding-control)
7. [Sleeping](#sleeping)
8. [Generators](#generators)
9. [Complete Examples](#complete-examples)
10. [Best Practices](#best-practices)

## Introduction

The coroutine library provides a lightweight, stackful coroutine implementation for C++. Coroutines allow you to write asynchronous code that looks synchronous, making it easier to handle I/O operations, timers, and concurrent tasks without the complexity of threads or callbacks.

Key features:
- **Cooperative multitasking**: Coroutines yield control explicitly, allowing fine-grained control over execution
- **Event-driven I/O**: Efficiently wait for file descriptors to become ready
- **Lightweight**: Each coroutine has its own stack (default 64KB)
- **Portable**: Works on Linux, macOS, and other Unix-like systems

## Basic Concepts

- **CoroutineScheduler**: Manages and runs multiple coroutines. You typically create one scheduler per application or per thread.
- **Coroutine**: A single execution context with its own stack. Coroutines are created with a function that defines their behavior.
- **Yield**: Voluntarily give up control to allow other coroutines to run.
- **Wait**: Block until a file descriptor becomes ready for I/O.

## Creating a Scheduler

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

## Best Practices

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

6. **Interrupt FDs**: For coroutines that need to be cancelled, use an interrupt file descriptor:
   ```cpp
   CoroutineOptions opts;
   opts.interrupt_fd = interrupt_pipe[0];  // Monitor this fd
   
   // In another coroutine, write to interrupt_pipe[1] to wake up the coroutine
   ```

7. **Stack size**: Adjust stack size for coroutines that need more stack space:
   ```cpp
   CoroutineOptions opts;
   opts.stack_size = 256 * 1024;  // 256KB
   ```

8. **Non-invasive API**: When using `Spawn`, prefer the non-invasive API (`co::Wait`, `co::Yield`, etc.) as it's cleaner and doesn't require passing a `Coroutine*` parameter.

## API Reference Summary

### CoroutineScheduler

- `Run()`: Run the scheduler until all coroutines complete
- `Stop()`: Stop the scheduler (thread-safe)
- `Spawn(function, options)`: Create and start a coroutine
- `Show()`: Print state of all coroutines (for debugging)

### Coroutine (when using constructor)

- `Wait(fd, events, timeout)`: Wait for fd to become ready
- `Poll(fds, events)`: Check if fds are ready (non-blocking)
- `PollAndWait(fd, events, timeout)`: Poll then wait if needed
- `Yield()`: Yield control to other coroutines
- `Sleep(duration)`: Sleep for specified duration
- `Millisleep(ms)`: Sleep for milliseconds
- `Nanosleep(ns)`: Sleep for nanoseconds

### Non-invasive API (when using Spawn)

- `co::Wait(fd, events, timeout)`: Wait for fd
- `co::Poll(fds, events)`: Poll fds
- `co::PollAndWait(fd, events, timeout)`: Poll then wait
- `co::Yield()`: Yield control
- `co::Sleep(duration)`: Sleep
- `co::Millisleep(ms)`: Sleep milliseconds
- `co::Nanosleep(ns)`: Sleep nanoseconds

For more details, see the header file `co/coroutine.h`.

