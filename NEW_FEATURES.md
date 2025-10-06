# Evolution of coroutines: new features

The original coroutine library was written over 2 years ago and has changed substantially since
then.  New features have been added and things have been simplified:

## Linux epoll support
On Linux this library will use the `epoll` interface by default.  This is a higher performance
version of poll.

## Custom context switch routines
The original version used `setjmp/longjmp` for context switching between coroutines. In addition to the
somewhat deprecated `setcontext/swapcontext` functions, the latest versions of this library use
a custom context switcher for higher performance.

## General spawn function and thread local contexts
A common pattern emerged when writing coroutine based code of using a set of `std::unique_ptr<co::Coroutine>`
in the object that owns the scheduler and using the completion callback to remove finished coroutines.

This pattern has been encapsulated in a `Spawn` function that does it all for you.

Most of the time you will run a single coroutine scheduler in a thread (or the main thread).  The
latest version has a new `co::self` thread-local varaible that always points to the current
coroutine.  This enables us to omit the `co::Coroutine* c` argument from the coroutine lambda
and access it using `co::self`. 

A set of free functions have been made available to enable access to the current coroutine without
having to explicitly reference `co::self`.  These are all in the `co::` namespace and all of the
standard Coroutine member functions (like Wait, Sleep, etc.) are available.

For exaample, to spawn a coroutine and run a function you can now do this:

```c++
scheduler.Spawn([this]() {
  // ...
  co::Sleep(std::chrono::microseconds(100));
  // ...
```

The scheduler owns the coroutine and will clean it up when it exits.  The `co::Sleep` functions is
simply:

```c++
void Sleep(...) {
  co::self->Sleep(...);
}
```

If you want access to the scheduler while inside the coroutine function you can get it via `co::scheduler`.

