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

## The API
There are two C++ classes in the library:

1. The main *Coroutine* class.  This is the object that represents a single coroutine
1. The scheduler class, called *CoroutineMachine*.  This provides the multiplexed I/O
and schedules coroutines to run when they are ready.


To create a coroutine, allocate an instance of *Coroutine*, passing it a reference
to the *CoroutineMachine* and a function to invoke that contains the body of the
coroutine.  The function is an instance of *std::function* so this allows you to
use anything that *std::function* supports, inluding free functions or lambdas with
local captures.

The coroutine can call the *Wait* functions to wait for a file descriptor to
become ready to read or write.  

The API for *Coroutine* and *CoroutineMachine* is:

```c++
class Coroutine {
public:
  Coroutine(CoroutineMachine &machine, CoroutineFunctor functor,
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
  CoroutineMachine &Machine() const { return machine_; }

  void Show();

  // Each coroutine has a unique id.
  int64_t Id() const { return id_; }
};

class CoroutineMachine {
public:
  CoroutineMachine();
  ~CoroutineMachine();

  // Run the machine until all coroutines have terminated or
  // told to stop.
  void Run();

  // Stop the machine.  Running coroutines will not be terminated.
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

Coroutines run until they yield control back to the scheduler using the *Yield*, 
*YieldValue* or *Wait* functions.  Since they all run in a single thread, there
is never any need to synchronize shared data.

## Coroutine Lifecycle management
The *CoroutineMachine* object does not own the *Coroutine* instances.  The
lifetime of the coroutines is managed by the program that's using the
*CoroutineMachine*.  This means that coroutines can be allocated anywhere
and their lifecycle managed using techniques such as *std::unique_ptr*.

To aid in this, the *CoroutineMachine* class has a way to register a function
that wil be called when a coroutine finishes.   For example, say you decide
to hold you coroutines in a set of *std::unique_ptr* instances:

```c++
absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;
```

You can remove coroutines from this set when they complete using:

```c++
  co_machine_.SetCompletionCallback(
      [this](co::Coroutine *c) { coroutines_.erase(c); });
```


## Yielding
If a coroutine has a long-running task to perform it should be nice to other
coroutines by calling *Yield* to give others a chance to run.  It is actually
unusual for a coroutine to need to do this, but not impossible.

Another way to yield is to yield a value using the *YieldValue* function.  This
is combined with a *Call* function to implement *generators*.

For example, here's a generator coroutine that prints the numbers generated by
another coroutine once a second.

```c++
void Co1(Coroutine *c) {
  Coroutine generator(c->Machine(), [](Coroutine *c) {
    for (int i = 1; i < 5; i++) {
      c->YieldValue(i);
    }
  });

  while (generator.IsAlive()) {
    int value = c->Call<int>(generator);
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
coroutine will not be resumed, but others can still run.

Most of the time a coroutine will wait for a single file descriptor using
the function:

```c++
// Wait for a file descriptor to become ready.  Returns the fd if it
// was triggered or -1 for timeout.
int Wait(int fd, short event_mask = POLLIN, int64_t timeout_ns = 0);
```


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
  co_machine_.SetCompletionCallback(
      [this](co::Coroutine *c) { coroutines_.erase(c); });

  // Start the listener coroutine.
  co::Coroutine listener(co_machine_, [this, &listen_socket](co::Coroutine *c) {
    ListenerCoroutine(std::move(listen_socket), c);
  });

  // Other coroutines here...

  // Run the coroutine main loop.
  co_machine_.Run();
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
      co_machine_, [handler_ptr](co::Coroutine *c) { handler_ptr->Run(c); }));

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
  urls = ["https://github.com/dallison/cocpp/archive/refs/tags/1.2.0.tar.gz"],
  strip_prefix = "cocpp-1.2.0",
  sha256 = "831c8c9e844822f58da2b3263b42f162b5efef88a4b85f08bf3a02861d264a63"
)

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

