// Copyright 2023-2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef coroutine_h
#define coroutine_h

// We have two modes of context switches available.  The most
// portable is using setjmp/longjmp with a little assembly
// language to switch stacks for the first call.  There is
// also user contexts which is a System V facility that is
// available on Linux and other operating systems.
//
// TODO: maybe I need to write my own context switching functions
// if the OS providers are going to remove features.  They seem
// to be forcing everything into threads, which is the antithesis
// of coroutines.
#define CO_CTX_SETJMP 1
#define CO_CTX_UCONTEXT 2

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
#if defined(__APPLE__)
#define CO_CTX_MODE CO_CTX_SETJMP
#define CO_POLL_MODE CO_POLL_POLL
#include <csetjmp>
#elif defined(__linux__)
// Linux supports user contexts.  Let's use them so that tsan works.
#define CO_CTX_MODE CO_CTX_UCONTEXT
#include <sys/epoll.h>
#include <ucontext.h>
#define CO_POLL_MODE CO_POLL_EPOLL // Change this line to disable epoll
#else
// Portable version is setjmp/longjmp
#define CO_CTX_MODE CO_CTX_SETJMP
#include <csetjmp>
#define CO_POLL_MODE CO_POLL_POLL
#endif

#include <poll.h>

#if CO_POLL_MODE == CO_POLL_EPOLL
#include "absl/container/flat_hash_map.h"
#endif

#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include "bitset.h"

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

struct CoroutineFd {
  CoroutineFd() = default;
  CoroutineFd(const Coroutine *c, int f, uint32_t e = 0) : co(c), fd(f), events(e) {}
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
class Coroutine {
public:
  // Important note: when using an interrupt_fd, you need to be careful
  // to duplicate it by calling dup(2) for each coroutine.  The coroutine
  // will add it to the poll set and that is racy if you use the same
  // fd in two coroutines.  In fact, when using epoll, it won't be allowed.
  Coroutine(CoroutineScheduler &machine, CoroutineFunction function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  Coroutine(CoroutineScheduler &machine, CoroutineFunction function,
            std::string name, size_t stack_size)
      : Coroutine(machine, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  // Options based constructor.
  Coroutine(CoroutineScheduler &machine, CoroutineFunction function,
            CoroutineOptions opts)
      : Coroutine(machine, function, opts.name, opts.interrupt_fd,
                  opts.autostart,
                  opts.stack_size == 0 ? kCoDefaultStackSize : opts.stack_size,
                  opts.user_data) {}

  Coroutine(CoroutineScheduler &machine, CoroutineFunctionRef function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  Coroutine(CoroutineScheduler &machine, CoroutineFunctionRef function,
            std::string name, size_t stack_size)
      : Coroutine(machine, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  // Options based constructor.
  Coroutine(CoroutineScheduler &machine, CoroutineFunctionRef function,
            CoroutineOptions opts)
      : Coroutine(machine, function, opts.name, opts.interrupt_fd,
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

#if CO_POLL_MODE == CO_POLL_EPOLL
  int Wait(const std::vector<WaitFd> &fds, uint64_t timeout_ns = 0) const;
#else
  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int Wait(struct pollfd &fd, uint64_t timeout_ns = 0) const;

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int Wait(const std::vector<struct pollfd> &fds,
           uint64_t timeout_ns = 0) const;
#endif

  void Exit();

  // Sleeping functions.
  void Nanosleep(uint64_t ns) const;
  void Millisleep(time_t msecs) const {
    Nanosleep(static_cast<uint64_t>(msecs) * 1000000LL);
  }
  void Sleep(time_t secs) const {
    Nanosleep(static_cast<uint64_t>(secs) * 1000000000LL);
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

  void GetAllFds(std::vector<int>& fds) const;

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
#if CO_CTX_MODE == CO_CTX_SETJMP
  mutable jmp_buf resume_; // Program environemnt for resuming.
  mutable jmp_buf exit_;   // Program environemt to exit.
#else
  mutable ucontext_t resume_;
  mutable ucontext_t exit_;
#endif
  mutable int wait_result_;
  mutable bool first_resume_ = true;

#if CO_POLL_MODE == CO_POLL_EPOLL
  mutable CoroutineFd yield_fd_;
  mutable std::vector<CoroutineFd> wait_fds_;
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
  Generator(CoroutineScheduler &machine, GeneratorFunction<T> function,
            std::string name = "", int interrupt_fd = -1,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr)
      : Coroutine(machine,
                  [function = std::move(function)](const Coroutine &c) {
                    function(reinterpret_cast<Generator<T> *>(
                        const_cast<Coroutine *>(&c)));
                  },
                  name, interrupt_fd, /*autostart=*/false, stack_size,
                  user_data) {}

  Generator(CoroutineScheduler &machine, GeneratorFunctionRef<T> function,
            std::string name = "", int interrupt_fd = -1,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr)
      : Coroutine(machine,
                  [this](const Coroutine &c) {
                    gen_function_(reinterpret_cast<const Generator<T> &>(c));
                  },
                  name, interrupt_fd, /*autostart=*/false, stack_size,
                  user_data),
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
  void Stop();

  void AddCoroutine(Coroutine *c);
  void RemoveCoroutine(const Coroutine *c);
  void StartCoroutine(Coroutine *c);

#if CO_POLL_MODE == CO_POLL_POLL
  // When you don't want to use the Run function, these
  // functions allow you to incorporate the multiplexed
  // IO into your own poll loop.
  void GetPollState(PollState *poll_state);
  void ProcessPoll(PollState *poll_state);
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

private:
  friend class Coroutine;
  template <typename T> friend class Generator;

#if CO_POLL_MODE == CO_POLL_EPOLL
  CoroutineFd *ChooseRunnable(const std::vector<struct epoll_event> &events,
                              int num_ready);
  CoroutineFd *
  GetRunnableCoroutine(const std::vector<struct epoll_event> &events,
                       int num_ready);
  void AddEpollFd(int fd, uint32_t events);
  void AddEpollFd(CoroutineFd *cfd, uint32_t events);
  void RemoveEpollFd(CoroutineFd *cfd);
#else
  void BuildPollFds(PollState *poll_state);
  CoroutineFd ChooseRunnable(PollState *poll_state, int num_ready);
  CoroutineFd GetRunnableCoroutine(PollState *poll_state, int num_ready);
#endif
  uint32_t AllocateId();
  uint64_t TickCount() const { return tick_count_; }
  bool IdExists(uint32_t id) const { return coroutine_ids_.Contains(id); }
#if CO_CTX_MODE == CO_CTX_SETJMP
  jmp_buf &YieldBuf() { return yield_; }
#else
  ucontext_t *YieldCtx() { return &yield_; }
#endif

  std::list<Coroutine *> coroutines_;
  BitSet coroutine_ids_;
  uint32_t last_freed_coroutine_id_ = -1U;
#if CO_CTX_MODE == CO_CTX_SETJMP
  jmp_buf yield_;
#else
  ucontext_t yield_;
#endif
  bool running_ = false;
#if CO_POLL_MODE == CO_POLL_EPOLL
  std::vector<struct epoll_event> events_;
  int epoll_fd_ = -1;
  int interrupt_fd_ = -1;
  size_t num_epoll_events_ = 0;
  absl::flat_hash_map<int, CoroutineFd *> coroutine_fds_;
#else
  PollState poll_state_;
  struct pollfd interrupt_fd_;
#endif
  uint64_t tick_count_ = 0;
  CompletionCallback completion_callback_;
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

} // namespace co
#endif /* coroutine_h */
