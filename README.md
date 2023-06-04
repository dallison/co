# Coroutine library in C++

This a cool library that implements coroutines in C++17.  It has absolutely nothing
to do with the upcoming (as of writing) C++20 coroutines facility.

Coroutines are an attractive alternative to threads.  They give you the ability to
perform multiple tasks in parallel in a program without the danger of threads sharing
memory and bugs causing random memory overwrites.

This coroutine library uses a combination of multiplexed I/O (the poll function) and
a fair scheduler to allow a set of coroutines to cooperate in their use of the
CPU.  Each coroutine is a single function that executes on its own stack.  Coroutines
can *yield* control back to the scheduler or can *wait* for a set of events to
occur.

Using coroutines, you can write complex pieces of software, like network servers,
using blocking I/O calls without a thread in sight.  The coroutines yield control
back to the scheduler when they are waiting for input or output.  It allows you
to write safe code where the program state is held on the stack of a set
of coroutines instead of in complex finite state machines.  This results a design
that is much easier to understand and debug.  Oh, did I mention that you don't need
any threads which makes your code completely thread safe.

This is similar to the *Go* language's, *goroutines*, but unfortunately, goroutines
are distributed among a set of threads, so they aren't really coroutines and you
still need thread synchronization when any data is shared among goroutines.

## Performance and safety
The use of coroutines in a program can improve the performance of the program
if the program spends time waiting for I/O.  Since the program will
be single-threaded, coroutines will not enable the execution to be split
across multiple cores.  The latency of I/O handling can be improved though, if you
arrange your program to handle each I/O session in a different coroutine.

The use of threads in a program to speed it up is specious and rarely improves
performance unless the program has been designed to allow the threads to
work well over multiple cores.  The context switch of one thread to another is
a substantial CPU burden and threads themselves consume a process in the OS.

Coroutines, however do not suffer from a heavy context switch - it's just swapping
the machine registers to a different context.  The judicious use of coroutines,
combined with multiple processes and an IPC system can improve both latency and
throughput in your system as well as safety.

As for program safety, threads introduce a huge cognitive burden on the programmer
to make sure that every piece of data in the program is protected against
an errant thread overwriting it.  It is almost impossible for a programmer
to cover every possible situation and thus there is no guarantee that a
write to memory followed by a read of the same address in the same thread
will result in a known value - another thread might have pre-empted you and
clobbered your memory.

Coroutines completely eliminate the need to for locking shared data in a program.
Since only one coroutine can be executing at once, the access to memory is,
by definition, serial.  This completely eliminates the need for locks unless
you really need to share memory between processes and there are good
solutions for that.

However, coroutines are not a panacea and, since the program executes multiple
coroutines in what appears to be parallel, you still have to deal with timing
issues where a coroutine voluntarily suspends itself and another one affects
some shared state.  When writing asynchronous applications, there is always
the dimension of time to take into account, but a well designed system with
multiplexed I/O and coroutines can be much safer than one that uses
threads to achieve the same purpose.

I've tested with library with thousands of coroutines all ready at the
same time (see the costress.cc file).  As you increase the number of
coroutines the system call to *::poll* to perform the context switch begins to
take more time.  When you get to about 10000 coroutines, when all of them are
ready to run, things get slow.  This would be a highly unusual situation.  A
well designed program using coroutines can have many thousands of coroutines
but they certainly won't all be ready to run at the same time.  Nothing is
for free.

## The API
There are two C++ classes in the library:

1. The main *Coroutine* class.  This is the object that represents a single coroutine
1. The scheduler class, called *CoroutineScheduler*.  This provides the multiplexed I/O
and schedules coroutines to run when they are ready.

To create a coroutine, allocate an instance of *Coroutine*, passing it a reference
to the *CoroutineScheduler* and a function to invoke that contains the body of the
coroutine.  The function is an instance of *std::function* so this allows you to
use anything that *std::function* supports, inluding free functions or lambdas with
local captures.

The coroutine can call the *Wait* functions to wait for a file descriptor to
become ready to read or write.  

The API for *Coroutine* and *CoroutineScheduler* is:

```c++
class Coroutine {
public:
  Coroutine(CoroutineScheduler &scheduler, CoroutineFunctor functor,
            const char *name = nullptr, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  ~Coroutine();

  // Start a coroutine running if it is not already running,
  void Start();

  // Yield control to another coroutine.
  void Yield();

  // Call another coroutine and store the result.
  template <typename T>
  T Call(Coroutine &callee);

  // Yield control and store value.
  template <typename T>
  void YieldValue(const T& value);

  // For all Wait functions, the timeout is optional and if greater than zero
  // specifies a nanosecond timeout.  If the timeout occurs before the fd (or
  // one of the fds) becomes ready, Wait will return -1. If an fd is ready, Wait
  // will return the fd that terminated the wait.

  // Wait for a file descriptor to become ready.  Returns the fd if it
  // was triggered or -1 for timeout.
  int Wait(int fd, short event_mask = POLLIN, int64_t timeout_ns = 0);

  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int Wait(struct pollfd &fd, int64_t timeout_ns = 0);

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int Wait(const std::vector<struct pollfd> &fds, int64_t timeout_ns = 0);

  void Exit();

  // Sleeping functions.
  void Nanosleep(uint64_t ns);
  void Millisleep(time_t msecs) { Nanosleep(msecs * 1000000LL); }
  void Sleep(time_t secs) { Nanosleep(secs * 1000000000LL); }

  // Set and get the name.  You can change the name at any time.  It's
  // only for debug really.
  void SetName(const std::string &name) { name_ = name; }
  const std::string &Name() const { return name_; }

  // Set and get the user data (not owned by the coroutine).  It's up
  // to you what this contains and you are responsible for its
  // management.
  void SetUserData(void *user_data) { user_data_ = user_data; }
  void *UserData() const { return user_data_; }

  // Is the given coroutine alive?
  bool IsAlive();

  uint64_t LastTick() const { return last_tick_; }
  CoroutineScheduler &Scheduler() const { return scheduler_; }

  void Show();

  // Each coroutine has a unique id.
  int64_t Id() const { return id_; }
};

class CoroutineScheduler {
public:
  CoroutineScheduler();
  ~CoroutineScheduler();

  // Run the scheduler until all coroutines have terminated or
  // told to stop.
  void Run();

  // Stop the scheduler.  Running coroutines will not be terminated.
  void Stop();

  void AddCoroutine(Coroutine *c);
  void RemoveCoroutine(Coroutine *c);
  void StartCoroutine(Coroutine *c);

  // When you don't want to use the Run function, these
  // functions allow you to incorporate the multiplexed
  // IO into your own poll loop.
  void GetPollState(PollState *poll_state);
  void ProcessPoll(PollState *poll_state);

  // Print the state of all the coroutines to stderr.
  void Show();

  // Call the given function when a coroutine exits.
  // You can use this to delete the coroutine.
  void SetCompletionCallback(CompletionCallback callback) {
    completion_callback_ = callback;
  }
};

```

In addition to the function to be called, a coroutine has:

1. A unique integer id
1. A settable name
1. Its own fixed size stack
1. Optional user data that is not owned by the coroutine

Coroutines run until they yield control back to the scheduler using the *Yield*
or *Wait* functions.  Since they all run in a single thread, there
is never any need to synchronize shared data.  When the coroutine function
returns, control is yieled back to the scheduler.

When all coroutine functions have returned the scheduler exits its *Run* function.
If all coroutines are blocked waiting for I/O, the scheduler is blocked in
a call to *::poll*.

You can stop the scheduler by calling its *Stop* function.  This will just
leave all the coroutines in their current state and the *Run* function will
return.  The two practical places to call this from is within a coroutine
or from a signal handler.

You can get a dump of the current coroutines by calling the *Show*
function.  This could be done from a signal handler (*SIGQUIT*, say)
like this:

```c++
static co::CoroutineScheduler* g_scheduler;
void Signal(int sig) {
  printf("\nAll coroutines:\n");
  g_scheduler->Show();
  signal(sig, SIG_DFL);
  raise(sig);
}

int main(int argc, char **argv) {
  co::CoroutineScheduler scheduler;

  g_scheduler = &scheduler;     // For signal handler.
  signal(SIGPIPE, SIG_IGN);
  signal(SIGQUIT, Signal);
  
  // Program logic here.
}

```


## Coroutine Lifecycle management
The *CoroutineScheduler* object does not own the *Coroutine* instances.  The
lifetime of the coroutines is managed by the program that's using the
*CoroutineScheduler*.  This means that coroutines can be allocated anywhere
and their lifecycle managed using techniques such as *std::unique_ptr*.

To aid in this, the *CoroutineScheduler* class has a way to register a function
that will be called when a coroutine finishes.   For example, say you decide
to hold you coroutines in a set of *std::unique_ptr* instances:

```c++
absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;
```

You can remove coroutines from this set when they complete using:

```c++
  co_scheduler_.SetCompletionCallback(
      [this](co::Coroutine *c) { coroutines_.erase(c); });
```


## Yielding and Generators
If a coroutine has a long-running task to perform it should be nice to other
coroutines by calling *Yield* to give others a chance to run.  It is actually
unusual for a coroutine to need to do this, but not impossible.  It should be
obvious that yielding control often will make that coroutine run slower as it
will not get as much CPU time.  So don't yield in the performance critical path
of a function (like, don't yield inside an inner loop).

Another way to yield is to yield a value using the Generator's *YieldValue* function.
This is combined with a *Call* function to implement *generators*.  A *Generator* is
a typed *Coroutine* that provides the *YieldValue* function that yields the
appropriate type.  You must *Call* a generator that yields the correct type otherwise
compilation errors will result.

The *Generator* template is derived from *Coroutine* and adds a *YieldValue*
function that copies a value to the calling coroutine and yields control.

For example, here's a coroutine that prints the numbers generated by
another coroutine once a second.

```c++
void Co1(Coroutine *c) {
  Generator<int> generator(c->Scheduler(), [](Generator<int> *c) {
    for (int i = 1; i < 5; i++) {
      c->YieldValue(i);
    }
  });

  while (generator.IsAlive()) {
    int value = c->Call(generator);
    if (generator.IsAlive()) {
      printf("Value: %d\n", value);
      c->Millisleep(1000);
    }
  }
}
```

## Waiting
The most common way for a coroutine to yield is to use one of the *Wait*
functions to wait for a set of file descriptors to become ready.  The
most general *Wait* function is:

```c++
// Wait for a set of pollfds.  Each needs to specify an fd and an event.
// Returns the fd that was triggered, or -1 for a timeout.
int Wait(const std::vector<struct pollfd> &fds, int64_t timeout_ns = 0);

```

This takes a set of *struct pollfd* instances and an optional timeout.  It will
return the first of the file descriptors to become ready, or -1 if a timeout
occurred.  If there is no timeout and no file descriptors become ready the
coroutine will not be resumed, but others can still run.  If more than one
file descriptor is ready, the scheduler will choose one and the others
will remain ready for the next resumption of the coroutine.  It will
most likely be the first one added to the wait call, but that
is not guaranteed.  Don't rely on it.

Most of the time a coroutine will wait for a single file descriptor using
the function:

```c++
// Wait for a file descriptor to become ready.  Returns the fd if it
// was triggered or -1 for timeout.
int Wait(int fd, short event_mask = POLLIN, int64_t timeout_ns = 0);
```

A call to any *Wait* function will yield control to the scheduler and thus
a system call to *::poll* even if there are no other coroutines or no
other coroutines are ready to run.

## Example

For example, say we have a server that listens for incoming connections on a
Unix domain socket and handles each of them in a coroutine.  We could create 
a listener coroutine as follows.  This is from an IPC server I am writing
so there are references to undefined classes (like *UnixSocket*), but they
are not important to this discussion.  It also uses Google's *Abseil* library.


```c++
void Server::ListenerCoroutine(UnixSocket listen_socket, co::Coroutine *c) {
  for (;;) {
    absl::Status status = HandleIncomingConnection(listen_socket, c);
    if (!status.ok()) {
      fprintf(stderr, "Unable to make incoming connection: %s\n",
              status.ToString().c_str());
    }
  }
}

absl::Status Server::Run() {
  UnixSocket listen_socket;
  absl::Status status = listen_socket.Bind(socket_name_, true);
  if (!status.ok()) {
    return status;
  }

  // Register a callback to be called when a coroutine completes.  The
  // server keeps track of all coroutines created for handling commands.
  // This deletes them when they are done.
  co_scheduler_.SetCompletionCallback(
      [this](co::Coroutine *c) { coroutines_.erase(c); });

  // Start the listener coroutine.
  co::Coroutine listener(co_scheduler_, [this, &listen_socket](co::Coroutine *c) {
    ListenerCoroutine(std::move(listen_socket), c);
  });

  // Other coroutines here...

  // Run the coroutine main loop.
  co_scheduler_.Run();
  return absl::OkStatus();
}

```

The *HandleIncomingConnection* function could be implemented as follows.  It waits
for an incoming connection by calling *Accept* and then spawns a new coroutine
to handle data on that connection.

```c++
absl::Status Server::HandleIncomingConnection(UnixSocket &listen_socket,
                                              co::Coroutine *c) {
  absl::StatusOr<UnixSocket> s = listen_socket.Accept(c);
  if (!s.ok()) {
    return s.status();
  }
  client_handlers_.push_back(
      std::make_unique<ClientHandler>(this, std::move(*s)));
  ClientHandler *handler_ptr = client_handlers_.back().get();

  coroutines_.insert(std::make_unique<co::Coroutine>(
      co_scheduler_, [handler_ptr](co::Coroutine *c) { handler_ptr->Run(c); }));

  return absl::OkStatus();
}
```

The *Accept* function on the *UnixSocket* class would wait for an the socket
to be ready to read and could be implemented as:

```c++
absl::StatusOr<UnixSocket> UnixSocket::Accept(co::Coroutine *c) {
  if (!fd_.Valid()) {
    return absl::InternalError("UnixSocket is not valid");
  }
  if (c != nullptr) {
    c->Wait(fd_.Fd(), POLLIN);
  }
  struct sockaddr_un sender;
  socklen_t sock_len = sizeof(sender);
  int new_fd =
      ::accept(fd_.Fd(), reinterpret_cast<struct sockaddr *>(&sender), &sock_len);
  if (new_fd == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to accept unix socket connection on fd %d: %s",
                        fd_.Fd(), strerror(errno)));
  }
  return UnixSocket(new_fd, /*connected=*/true);
}
```

The call to *c->Wait()* will suspend the coroutine until the given file descriptor is ready
to read and then we call *accept* to get a new *UnixSocket*.


# Building and Dependencies

It is built using [Google's Bazel](https://github.com/bazelbuild/bazel).  The following
WORKSPACE entry can be used to import it into another Bazel-built system:

```
http_archive(
  name = "coroutines",
  urls = ["https://github.com/dallison/co/archive/refs/tags/A.B.C.tar.gz"],
  strip_prefix = "co-A.B.C",
)

Where "A.B.C" is replaced by the actual version you want, something like
*1.2.1*.

You can add a *sha256* entry to make sure you get the right version.  Bazel
will tell you what it is.
```

# Portability
This has been tested on MacOS (Apple Silicon) and Linux (x86_64).  I haven't tried
it on Windows but there's no reason why it shouldn't be capable of running, maybe
after a few tweaks.

It uses setjmp and longjmp to switch between the coroutines so that should be
fairly portable.

There is some necessary assembly language magic involved in switching stacks and that
supports ARM64 (Aarch64) and x86_64 only.  32-bit ports would be pretty easy and can
be done if requested.

# Example code
I've provided two example programs for your enjoyment:

1. An HTTP server
1. An HTTP client

Run the server and it will open TCP port 80 on localhost and allow you to
send HTTP (not HTTPS) requests to it to get a file from the local file
system.

You can use something like *curl* or *wget* to get exercise the server, or
you can use the HTTP client program.  The advantage of the HTTP client
program is that is allows you to run multiple jobs at the same time, something
that is reasonably difficult with the other tools.

Both the client and server are single threaded coroutine based programs that
can handle many requests at the same time.  The only limit is the number of
open files resource limit.

The server could be used as the basis for a simple HTTP server for an embedded
system (although lack of SSL support is a big issue).  The client is pretty
functional and supports chunked data.

## Running the server
To run the server:

```bash
$ bazel-bin/http_server/http_server
```

## Runnng the client
You can run the client with the following args:

1. Hostname - the hostname of the server
2. Filename - the filename you want to get
3. -j # - the number of jobs to run at once (default 1)

For example, to get */etc/hosts* 100 times from the server:

```bash
$ bazel-bin/http_client/http_client localhost /etc/hosts -j 100
```

If you try too many jobs, the server will be unable to accept new
connections due to the open file limits.

If you want to slightly stress out the Google servers (be nice, Google
used to be)

```bash
$ bazel-bin/http_client/http_client www.google.com / -j 100
```


# Licensing
This is licensed under the Apache License Version 2.0.  Please see the LICENSE
file for details.

If you would like to use this library in commercial or non-commercial applications
and the license is not suitable for you, please contact me and I'll see what
I can do.
