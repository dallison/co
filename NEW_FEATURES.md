# Evolution of coroutines: new features

The original coroutine library was written over 2 years ago and has changed substantially since
then. New features have been added and things have been simplified:

## Linux epoll support
On Linux this library will use the `epoll` interface by default. This is a higher performance
version of poll.

## Custom context switch routines
The original version used `setjmp/longjmp` for context switching between coroutines. In addition to the
somewhat deprecated `setcontext/swapcontext` functions, the latest versions of this library use
a custom context switcher for higher performance.

## General spawn function and thread local contexts
A common pattern emerged when writing coroutine based code of using a set of `std::unique_ptr<co::Coroutine>`
in the object that owns the scheduler and using the completion callback to remove finished coroutines.

This pattern has been encapsulated in a `Spawn` function that does it all for you.

Most of the time you will run a single coroutine scheduler in a thread (or the main thread). The
`co::self` thread-local variable always points to the current coroutine. This enables us to omit
the `co::Coroutine* c` argument from the coroutine lambda and access it using `co::self`.

A set of free functions have been made available to enable access to the current coroutine without
having to explicitly reference `co::self`. These are all in the `co::` namespace and all of the
standard Coroutine member functions (like Wait, Sleep, etc.) are available.

For example, to spawn a coroutine and run a function you can now do this:

```c++
scheduler.Spawn([this]() {
  // ...
  co::Sleep(std::chrono::microseconds(100));
  // ...
});
```

The scheduler owns the coroutine and will clean it up when it exits. The `co::Sleep` function is
simply:

```c++
void Sleep(...) {
  co::self->Sleep(...);
}
```

If you want access to the scheduler while inside the coroutine function you can get it via `co::scheduler`.

## C++20 Stackless Coroutine Library

A new standalone C++20 coroutine library has been added in the `co20::` namespace
(`co/coroutine_cpp20.h` and `co/coroutine_cpp20.cc`). This is a completely separate
implementation that uses C++20 compiler coroutines (`co_await`, `co_return`) with an
epoll/poll-based scheduler. It has no dependency on the C++17 library or Abseil.

The C++20 library provides the same thread-local pattern as the C++17 library:
- `co20::self` points to the currently running coroutine
- `co20::scheduler` points to the current scheduler
- Free functions like `co20::Yield()`, `co20::Wait()`, `co20::Sleep()`, `co20::Millisleep()` etc.

The key difference is that all free functions return awaitables and must be used with `co_await`:

```c++
scheduler.Spawn([]() -> co20::Task {
  co_await co20::Sleep(std::chrono::milliseconds(100));
  int fd = co_await co20::Wait(my_fd, POLLIN);
  co_await co20::Yield();
  co_return;
});
```

The `Spawn` function accepts lambdas with or without a `Coroutine&` parameter:

```c++
// No parameter -- use free functions
scheduler.Spawn([]() -> co20::Task {
  co_await co20::Yield();
  co_return;
});

// With parameter -- call methods directly
scheduler.Spawn([](co20::Coroutine& co) -> co20::Task {
  co_await co.Yield();
  co_return;
});
```

Coroutines can be aborted by calling `Abort()` on the coroutine object. The aborted
coroutine receives an `AbortException` at its next `co_await` point.

See the [User Guide](USER_GUIDE.md) for full documentation and examples.
