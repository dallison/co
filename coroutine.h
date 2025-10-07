// Copyright 2023-2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

// We have three modes of context switches available.
// 1. using setjmp/longjmp with a little assembly
//   language to switch stacks for the first call.
// 2. user contexts which is a System V facility that is
//     available on Linux and other operating systems.
// 3. A custom context switcher written in assembly language for
//    x86_64 and aarch64.
//
//
// Which one to use?  The custom context switcher is the fastest but may
// not work on your architecture.  The setjmp/longjmp is the most portable
// but might cause issues if the system library intercepts longjmp.
// The user contexts are disabled on MacOS and also cause issues with
// ASAN.
//
// I think the custom context switcher is the best choice for portability
// and performance.  However, since it's custom, it might cause issues
// if tools are coded to understand the ucontext stuff in libc.
#define CO_CTX_SETJMP 1
#define CO_CTX_UCONTEXT 2
#define CO_CTX_CUSTOM 3

// Do we use ::poll or ::epoll?  The epoll system call is Linux only and
// can improve performance.
//
// NOTE: there is a difference in behavior when using epoll vs poll.  In epoll
// mode you can't add the same fd to the poll set more than once.  There is
// no such restriction for poll.  This means that two coroutines can't wait
// for the same fd at the same time.  This is usually an error anyway
// but is not enforced with poll.
//
// The main effect of this is when passing an interrrupt_fd to the coroutines.
// You will need to dup(2) it before passing to more than one coroutine.  This
// is normally what you need anyway.
//
// By default POLL_EPOLL is used on Linux and POLL_POLL on all other OSes.
// If you don't want to use POLL_EPOLL on Linux, modify the setting of
// POLL_MODE inside the defined(__linux__) below.
#define CO_POLL_EPOLL 1
#define CO_POLL_POLL 2

// Apple has deprecated user contexts so we can't use them
// on MacOS.  Linux still has them and there's an issue with
// using setjmp/longjmp on Linux when running with LLVM
// TSAN.  It assumes that a longjmp is always to the same
// stack as the setjmp used.  That's kind of the point of
// coroutines.  It's also not possible to suppress the
// longjmp interception in TSAN, so if you want to make
// use of TSAN in something that uses coroutines, you have to
// use user contexts.
//
// Another alternative is to use the custom context switcher that is
// provided in context.h.  This is the fastest and most portable but
// is only available for x86_64 and aarch64 (at the moment).
//
// Modify the CO_CTX_MODE macro value to change the context switcher.
#if defined(__APPLE__)
#if defined(__x86_64__) || defined(__aarch64__)
// On Apple, we can use the custom context switcher for x86_64 and aarch64.
#define CO_CTX_MODE CO_CTX_CUSTOM
#else
// Is there another Apple architecture?  Maybe, but the custome contxt switcher
// is not available for it.  Use the setjmp/longjmp context switcher.
#define CO_CTX_MODE CO_CTX_SETJMP
#endif
#define CO_POLL_MODE CO_POLL_POLL
#include <csetjmp>

#elif defined(__linux__)

// On Linux, let's use custom context if it's available for the architecture.
#if defined(__x86_64__) || defined(__aarch64__)
#define CO_CTX_MODE CO_CTX_CUSTOM
#else
// Custom context switcher is not available for this architecture.  Use the
// linux user context switcher.
#define CO_CTX_MODE CO_CTX_UCONTEXT
#endif

#include <sys/epoll.h>
#include <ucontext.h>
#define CO_POLL_MODE CO_POLL_EPOLL // Change this line to disable epoll
#else
// Other OS, use the custom context switcher if available
// or setjmp/longjmp if not.  The custom context switcher is only available
#if defined(__x86_64__) || defined(__aarch64__)
#define CO_CTX_MODE CO_CTX_CUSTOM
#else
// Portable version is setjmp/longjmp
#define CO_CTX_MODE CO_CTX_SETJMP
#endif

#include <csetjmp>
#define CO_POLL_MODE CO_POLL_POLL
#endif

#include <poll.h>

// Uncomment this if you want to see which context switcher is being used.
// This is useful for debugging and understanding which context switcher
// is being used in your code.  It will print a message at compile time
// indicating which context switcher is being used.
#if 0
#if CO_CTX_MODE == CO_CTX_CUSTOM
#pragma message("Using custom context switcher for coroutines.")
#elif CO_CTX_MODE == CO_CTX_UCONTEXT
#pragma message("Using ucontext for coroutines.")
#elif CO_CTX_MODE == CO_CTX_SETJMP
#pragma message("Using setjmp/longjmp for coroutines.")
#else
#error                                                                         \
    "Unknown context switcher mode.  Please define CO_CTX_MODE to one of CO_CTX_SETJMP, CO_CTX_UCONTEXT, or CO_CTX_CUSTOM."
#endif
#endif

#if CO_POLL_MODE == CO_POLL_EPOLL
#include "absl/container/flat_hash_map.h"
#endif
#include "absl/container/flat_hash_set.h"

// Define the alias 'Context' for the context structure.
#if CO_CTX_MODE == CO_CTX_SETJMP
using Context = jmp_buf;
#elif CO_CTX_MODE == CO_CTX_UCONTEXT
using Context = ucontext_t;
#else
#include "context.h"
using Context = co::CoroutineContext;
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <list>
#include <set>
#include <string>
#include <vector>

#include "bitset.h"
#include "detect_sanitizers.h"

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
extern "C" {
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom,
                                    size_t size);

void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                     const void **bottom_old, size_t *size_old);
}
#endif

namespace co {

class CoroutineScheduler;
class Coroutine;
template <typename T> class Generator;

// Older versions of this library pass a raw pointer to a Coroutine
// to the functor.  This is because it was first written in C
// and I made a mistake not to change it to a reference in the C++
// version.  It's really the same thing, but modern C++ style prefers
// references to pointers.
//
// This is corrected now and in this version we provide a functor
// that is passed a const reference to the coroutine.  The pointer
// version will continue to work but new code should use the reference version.
using CoroutineFunction = std::function<void(Coroutine *)>;
using CompletionCallback = std::function<void(Coroutine *)>;
using CoroutineFunctionRef = std::function<void(const Coroutine &)>;
using CompletionCallbackRef = std::function<void(const Coroutine &)>;

template <typename T>
using GeneratorFunction = std::function<void(Generator<T> *)>;

template <typename T>
using GeneratorFunctionRef = std::function<void(const Generator<T> &)>;

constexpr size_t kCoDefaultStackSize = 32 * 1024;

extern "C" {
// This is needed here because it's a friend with C linkage.
void __co_Invoke(class Coroutine *c);
}

template <typename T> class Generator;

struct YieldedCoroutine {
  YieldedCoroutine() = default;
  YieldedCoroutine(const Coroutine *c, int f, uint32_t e = 0)
      : co(c), fd(f), events(e) {}
  const Coroutine *co = nullptr;
  int fd = -1;
  uint32_t events = 0;
};

struct CoroutineOptions {
  std::string name;
  int interrupt_fd = -1;
  bool autostart = true;
  size_t stack_size = kCoDefaultStackSize;
  void *user_data = nullptr;
};

#if CO_POLL_MODE == CO_POLL_EPOLL
// This is to provide the epoll equivalent of waiting for a set
// of pollfds
struct WaitFd {
  int fd;
  uint32_t events;
};
#endif

// This is a Coroutine.  It executes its function (pointer to a function
// or a lambda).
//
// It has its own stack with default size kCoDefaultStackSize.
// By default, the coroutine will be given a unique name and will
// be started automatically.  It can have some user data which is
// not owned by the coroutine.
//
// Due to stack switching, AddressSanitizer will report false-positive
// errors (use-after-return). The principal function of a coroutine is likely
// to need to be prefixed with CO_DISABLE_ADDRESS_SANITIZER
// (detect_sanitizers.h) to disable diagnostics related to its stack frame.
class Coroutine {
public:
  // Important note: when using an interrupt_fd, you need to be careful
  // to duplicate it by calling dup(2) for each coroutine.  The coroutine
  // will add it to the poll set and that is racy if you use the same
  // fd in two coroutines.  In fact, when using epoll, it won't be allowed.
  Coroutine(CoroutineScheduler &scheduler, CoroutineFunction function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  Coroutine(CoroutineScheduler &scheduler, CoroutineFunction function,
            std::string name, size_t stack_size)
      : Coroutine(scheduler, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  // Options based constructor.
  Coroutine(CoroutineScheduler &scheduler, CoroutineFunction function,
            CoroutineOptions opts)
      : Coroutine(scheduler, std::move(function), opts.name, opts.interrupt_fd,
                  opts.autostart,
                  opts.stack_size == 0 ? kCoDefaultStackSize : opts.stack_size,
                  opts.user_data) {}

  Coroutine(CoroutineScheduler &scheduler, CoroutineFunctionRef function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  Coroutine(CoroutineScheduler &scheduler, CoroutineFunctionRef function,
            std::string name, size_t stack_size)
      : Coroutine(scheduler, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  Coroutine(CoroutineScheduler &scheduler, std::function<void()> function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  Coroutine(CoroutineScheduler &scheduler, std::function<void()> function,
            std::string name, size_t stack_size)
      : Coroutine(scheduler, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  // Options based constructor.
  Coroutine(CoroutineScheduler &scheduler, CoroutineFunctionRef function,
            CoroutineOptions opts)
      : Coroutine(scheduler, std::move(function), opts.name, opts.interrupt_fd,
                  opts.autostart,
                  opts.stack_size == 0 ? kCoDefaultStackSize : opts.stack_size,
                  opts.user_data) {}

  Coroutine(CoroutineScheduler &scheduler, std::function<void()> function,
            CoroutineOptions opts)
      : Coroutine(scheduler, std::move(function), opts.name, opts.interrupt_fd,
                  opts.autostart,
                  opts.stack_size == 0 ? kCoDefaultStackSize : opts.stack_size,
                  opts.user_data) {}
  ~Coroutine();

  // Start a coroutine running if it is not already running,
  void Start();

  // Yield control to another coroutine.
  void Yield() const;
  void YieldToScheduler() const;

  // Call another coroutine and store the result.
  template <typename T> T Call(Generator<T> &callee) const;

  // returns -1 for no fd ready, fd if one is ready.
  int Poll(const std::vector<int> &fds, short event_mask = POLLIN) const;
  int Poll(const std::vector<struct pollfd> &fds) const;

  // For all Wait functions, the timeout is optional and if greater than zero
  // specifies a nanosecond timeout.  If the timeout occurs before the fd (or
  // one of the fds) becomes ready, Wait will return -1. If an fd is ready, Wait
  // will return the fd that terminated the wait.

  // Wait for a file descriptor to become ready.  Returns the fd if it
  // was triggered or -1 for timeout.
  int Wait(int fd, uint32_t event_mask = POLLIN, uint64_t timeout_ns = 0) const;

  // Wait for a set of fds, all with the same event mask.
  int Wait(const std::vector<int> &fd, uint32_t event_mask = POLLIN,
           uint64_t timeout_ns = 0) const;

  // Poll first and if the fd is not ready, wait for it.
  int PollAndWait(int fd, uint32_t event_mask = POLLIN,
                  uint64_t timeout_ns = 0) const;

  // Wait for a set of fds, all with the same event mask.
  int PollAndWait(const std::vector<int> &fd, uint32_t event_mask = POLLIN,
                  uint64_t timeout_ns = 0) const;

#if CO_POLL_MODE == CO_POLL_EPOLL
  int Wait(const std::vector<WaitFd> &fds, uint64_t timeout_ns = 0) const;
  int PollAndWait(const std::vector<WaitFd> &fds,
                  uint64_t timeout_ns = 0) const;
#else
  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int Wait(struct pollfd &fd, uint64_t timeout_ns = 0) const;

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int Wait(const std::vector<struct pollfd> &fds,
           uint64_t timeout_ns = 0) const;
  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int PollAndWait(struct pollfd &fd, uint64_t timeout_ns = 0) const;

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int PollAndWait(const std::vector<struct pollfd> &fds,
                  uint64_t timeout_ns = 0) const;
#endif

  // Templated waits with chrono timeouts.
  template <class T, class Rep, class Period>
  int Wait(const T &fd, uint32_t events,
           std::chrono::duration<Rep, Period> duration) const {
    return Wait(
        fd, events,
        std::chrono::duration_cast<std::chrono::duration<Rep, std::nano>>(
            duration)
            .count());
  }

  template <class T, class Rep, class Period>
  int Wait(const T &fd, std::chrono::duration<Rep, Period> duration) const {
    return Wait(
        fd, POLLIN,
        std::chrono::duration_cast<std::chrono::duration<Rep, std::nano>>(
            duration)
            .count());
  }

  template <class T, class Rep, class Period>
  int PollAndWait(const T &fd, uint32_t events,
                  std::chrono::duration<Rep, Period> duration) const {
    return PollAndWait(
        fd, events,
        std::chrono::duration_cast<std::chrono::duration<Rep, std::nano>>(
            duration)
            .count());
  }

  template <class T, class Rep, class Period>
  int PollAndWait(const T &fd,
                  std::chrono::duration<Rep, Period> duration) const {
    return PollAndWait(
        fd, POLLIN,
        std::chrono::duration_cast<std::chrono::duration<Rep, std::nano>>(
            duration)
            .count());
  }

  // Note this can cause memory leaks as destructors in the coroutine function
  // will not be called.  Use sparingly.  You should really use an interrupt fd
  // to cause the function to exit cleanly, but this can get you out of stick
  // situations if you need it.
  void Exit() const;

  // Sleeping functions.
  void Nanosleep(uint64_t ns) const;
  void Millisleep(time_t msecs) const {
    Nanosleep(static_cast<uint64_t>(msecs) * 1000000LL);
  }
  void Sleep(time_t secs) const {
    Nanosleep(static_cast<uint64_t>(secs) * 1000000000LL);
  }

  template <class Rep, class Period>
  void Sleep(std::chrono::duration<Rep, Period> duration) const {
    Nanosleep(std::chrono::duration_cast<std::chrono::duration<Rep, std::nano>>(
                  duration)
                  .count());
  }

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
  bool IsAlive() const;

  uint64_t LastTick() const { return last_tick_; }
  CoroutineScheduler &Scheduler() const { return scheduler_; }

  void Show() const;

  // Each coroutine has a unique id.
  uint32_t Id() const { return id_; }

  void SeToStringCallback(std::function<std::string()> cb) {
    to_string_callback_ = std::move(cb);
  }

  // Make a string describing information about this coroutine.  By default
  // this will be the same as that printed by Show().
  std::string ToString() const;

  void GetAllFds(std::vector<int> &fds) const;

private:
  enum class State {
    kCoNew,
    kCoReady,
    kCoRunning,
    kCoYielded,
    kCoWaiting,
    kCoDead,
  };

  friend class CoroutineScheduler;
  template <typename T> friend class Generator;

  friend void __co_Invoke(Coroutine *c);
  static const char *StateName(State state);

  void InvokeFunction();
  int EndOfWait(int timer_fd) const;
  int AddTimeout(uint64_t timeout_ns) const;
  State GetState() const { return state_; }
#if CO_POLL_MODE == CO_POLL_POLL
  void AddPollFds(std::vector<struct pollfd> &pollfds,
                  std::vector<Coroutine *> &covec);
#endif
  void Resume(int value) const;
  void TriggerEvent() const;
  void ClearEvent() const;
  void CallNonTemplate(Coroutine &c) const;
  void YieldNonTemplate() const;
  void SetState(State state) const;

  std::string MakeDefaultString() const;

  CoroutineScheduler &scheduler_;
  uint32_t id_;                   // Coroutine ID.
  CoroutineFunctionRef function_; // Coroutine body.
  std::string name_;              // Optional name.
  int interrupt_fd_;
  mutable State state_ = State::kCoNew;
  std::vector<char> stack_;                 // Stack, allocated from malloc.
  mutable void *yielded_address_ = nullptr; // Address at which we've yielded.
  mutable Context resume_;
  mutable Context exit_;
  mutable int wait_result_;
  mutable bool first_resume_ = true;

#if CO_POLL_MODE == CO_POLL_EPOLL
  mutable YieldedCoroutine yield_fd_;
  mutable std::vector<YieldedCoroutine> wait_fds_;
  mutable int num_epoll_events_ = 0;
#else
  mutable struct pollfd event_fd_; // Pollfd for event.
  mutable std::vector<struct pollfd>
      wait_fds_; // Pollfds for waiting for an fd.
#endif
  mutable const Coroutine *caller_ =
      nullptr;                     // If being called, who is calling us.
  void *user_data_;                // User data, not owned by this.
  mutable uint64_t last_tick_ = 0; // Tick count of last resume.

  // Function used to create a string for this coroutine.
  std::function<std::string()> to_string_callback_;
};

// A Generator is a coroutine that generates values.  The magic lamda line
// noise is because you can't cast an std::function<void(B*)> to an
// std::function<void(A*)> even though B is derived from A.
//
// A generator doesn't start automatically.  It's started on the
// first call.
template <typename T> class Generator : public Coroutine {
public:
  Generator(CoroutineScheduler &scheduler, GeneratorFunction<T> function,
            std::string name = "", int interrupt_fd = -1,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr)
      : Coroutine(
            scheduler,
            [function = std::move(function)](const Coroutine &c) {
              function(reinterpret_cast<Generator<T> *>(
                  const_cast<Coroutine *>(&c)));
            },
            name, interrupt_fd, /*autostart=*/false, stack_size, user_data) {}

  Generator(CoroutineScheduler &scheduler, GeneratorFunctionRef<T> function,
            std::string name = "", int interrupt_fd = -1,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr)
      : Coroutine(
            scheduler,
            [this](const Coroutine &c) {
              gen_function_(reinterpret_cast<const Generator<T> &>(c));
            },
            name, interrupt_fd, /*autostart=*/false, stack_size, user_data),
        gen_function_(function) {}

  // Yield control and store value.
  void YieldValue(const T &value) const;

private:
  friend class Coroutine;
  GeneratorFunctionRef<T> gen_function_;
  mutable T *result_ = nullptr; // Where to put result in YieldValue.
};

struct PollState {
  std::vector<struct pollfd> pollfds;
  std::vector<Coroutine *> coroutines;
};

class CoroutineScheduler {
public:
  CoroutineScheduler();
  ~CoroutineScheduler();

  // Run the scheduler until all coroutines have terminated or
  // told to stop.
  void Run();

  // Stop the scheduler.  Running coroutines will not be terminated.
  // This function is thread-safe since a common pattern is to "Run()"
  // the coroutines in a background thread.
  void Stop();

  void AddCoroutine(Coroutine *c);
  void RemoveCoroutine(const Coroutine *c);
  void StartCoroutine(Coroutine *c);

  int GetInterruptFd() const {
#if CO_POLL_MODE == CO_POLL_EPOLL
    return interrupt_fd_;
#else
    return interrupt_fd_.fd;
#endif
  }

  void TriggerInterrupt() const {
    uint64_t one = 1;

#if CO_POLL_MODE == CO_POLL_EPOLL
    (void)write(interrupt_fd_, &one, sizeof(one));
#else
    (void)write(interrupt_fd_.fd, &one, sizeof(one));
#endif
  }

#if CO_POLL_MODE == CO_POLL_POLL
  // When you don't want to use the Run function, these
  // functions allow you to incorporate the multiplexed
  // IO into your own poll loop.
  void GetPollState(PollState *poll_state);
#endif

  // Print the state of all the coroutines to stderr.
  void Show();

  // Call the given function when a coroutine exits.
  // You can use this to delete the coroutine.
  void SetCompletionCallback(CompletionCallback callback) {
    completion_callback_ = callback;
  }

  // Get a vector containing all the strings generated by the
  // coroutines.
  std::vector<std::string> AllCoroutineStrings() const;

  std::vector<int> GetAllFds() const;

  co::Coroutine *Spawn(std::function<void(co::Coroutine *)> f,
                       CoroutineOptions opts = {});
  co::Coroutine *Spawn(std::function<void()> f, CoroutineOptions opts = {});

private:
  friend class Coroutine;
  template <typename T> friend class Generator;

#if CO_POLL_MODE == CO_POLL_EPOLL
  void AddEpollFd(int fd, uint32_t events);
  void AddEpollFd(YieldedCoroutine *cfd, uint32_t events);
  void RemoveEpollFd(YieldedCoroutine *cfd);
#else
  void BuildPollFds(PollState *poll_state);
#endif
  uint32_t AllocateId();
  uint64_t TickCount() const { return tick_count_; }
  bool IdExists(uint32_t id) const { return coroutine_ids_.Contains(id); }
  Context &YieldCtx() { return yield_; }
  void CommitDeletions();

  std::list<Coroutine *> coroutines_;
  // These are coroutines owned by the scheduler (created using Spawn).
  absl::flat_hash_set<std::unique_ptr<Coroutine>> owned_coroutines_;

  BitSet coroutine_ids_;
  uint32_t last_freed_coroutine_id_ = -1U;
  Context yield_;
  std::atomic<bool> running_ = false;
#if CO_POLL_MODE == CO_POLL_EPOLL
  absl::flat_hash_map<int, absl::flat_hash_set<YieldedCoroutine *>>
      waiting_coroutines_;
  int epoll_fd_ = -1;
  int interrupt_fd_ = -1;
  size_t num_epoll_events_ = 0;
#else
  PollState poll_state_;
  struct pollfd interrupt_fd_;
#endif

  uint64_t tick_count_ = 0;
  CompletionCallback completion_callback_;
  absl::flat_hash_set<const Coroutine *> deletions_;
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
  void *fake_stack_ = nullptr;
#endif
};

template <typename T>
inline void Generator<T>::YieldValue(const T &value) const {
  // Copy value.
  if (result_ != nullptr) {
    *result_ = value;
  }
  YieldNonTemplate();
}

template <typename T> inline T Coroutine::Call(Generator<T> &callee) const {
  T result;
  // Tell the callee that it's being called and where to store the value.
  callee.caller_ = this;
  callee.result_ = &result;
  CallNonTemplate(callee);
  // Call done.  No result now.
  callee.result_ = nullptr;
  return result;
}

// Non-invasive coroutine functions.
// The 'co::self' variable holds a pointer to the currently running coroutine
// The 'co::sheduler' variable holds a pointer to the a coroutine's scheduler
//   object.  This is available inside a coroutine as a convenience to
//   get the scheduler.  It can also be obtained using 'self->Scheduler()'
//
// These are both thread local so each scheduler will have its own copy,
// assuming that you are running a scheduler in a thread.  I can't think of a
// reason or way to run multiple schedulers in the same thread.
extern thread_local const co::Coroutine *self;
extern thread_local co::CoroutineScheduler *scheduler;

void Yield();

int Poll(const std::vector<int> &fds, short event_mask = POLLIN);
int Poll(const std::vector<struct pollfd> &fds);

int Wait(int fd, uint32_t event_mask = POLLIN, uint64_t timeout_ns = 0);

// Wait for a set of fds, all with the same event mask.
int Wait(const std::vector<int> &fd, uint32_t event_mask = POLLIN,
         uint64_t timeout_ns = 0);

// Poll first and if the fd is not ready, wait for it.
int PollAndWait(int fd, uint32_t event_mask = POLLIN, uint64_t timeout_ns = 0);

// Wait for a set of fds, all with the same event mask.
int PollAndWait(const std::vector<int> &fd, uint32_t event_mask = POLLIN,
                uint64_t timeout_ns = 0);

#if CO_POLL_MODE == CO_POLL_EPOLL
int Wait(const std::vector<WaitFd> &fds, uint64_t timeout_ns = 0);
int PollAndWait(const std::vector<WaitFd> &fds, uint64_t timeout_ns = 0);
#else
// Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
int Wait(struct pollfd &fd, uint64_t timeout_ns = 0);

// Wait for a set of pollfds.  Each needs to specify an fd and an event.
// Returns the fd that was triggered, or -1 for a timeout.
int Wait(const std::vector<struct pollfd> &fds, uint64_t timeout_ns = 0);
// Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
int PollAndWait(struct pollfd &fd, uint64_t timeout_ns = 0);

// Wait for a set of pollfds.  Each needs to specify an fd and an event.
// Returns the fd that was triggered, or -1 for a timeout.
int PollAndWait(const std::vector<struct pollfd> &fds, uint64_t timeout_ns = 0);
#endif

template <class T, class Rep, class Period>
int Wait(const T &fd, uint32_t events,
         std::chrono::duration<Rep, Period> duration) {
  return self->Wait(fd, events, duration);
}

template <class T, class Rep, class Period>
int Wait(const T &fd, std::chrono::duration<Rep, Period> duration) {
  return self->Wait<T, Rep, Period>(fd, duration);
}

template <class T, class Rep, class Period>
int PollAndWait(const T &fd, uint32_t events,
                std::chrono::duration<Rep, Period> duration) {
  return self->PollAndWait(fd, events, duration);
}

template <class T, class Rep, class Period>
int PollAndWait(const T &fd, std::chrono::duration<Rep, Period> duration) {
  return self->PollAndWait(fd, duration);
}

// Note this can cause memory leaks as destructors in the coroutine function
// will not be called.  Use sparingly.  You should really use an interrupt fd
// to cause the function to exit cleanly, but this can get you out of stick
// situations if you need it.
void Exit();

// Sleeping functions.
void Nanosleep(uint64_t ns);
void Millisleep(time_t msecs);
void Sleep(time_t secs);

template <class Rep, class Period>
void Sleep(std::chrono::duration<Rep, Period> duration) {
  return self->Sleep(duration);
}

} // namespace co
