// Copyright 2023-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "co/coroutine.h"

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

// Timer implementation selection:
// 1. CO_TIMER_TIMERFD - Linux timerfd (Linux only)
// 2. CO_TIMER_EVENT - macOS kqueue event (macOS only)
// 3. CO_TIMER_POSIX - POSIX timer_create with pipe (portable)
#define CO_TIMER_TIMERFD 1
#define CO_TIMER_EVENT 2
#define CO_TIMER_POSIX 3

// Event-fd backend selection (used by co::EventFd):
// 1. CO_EVENT_EVENTFD - Linux eventfd (Linux only)
// 2. CO_EVENT_KQUEUE  - macOS kqueue with EVFILT_USER (macOS only)
// 3. CO_EVENT_PIPE    - non-blocking pipe pair (portable)
//
// You can override the default backend by defining CO_EVENT_MODE before
// including this header (e.g. -DCO_EVENT_MODE=CO_EVENT_PIPE) so you can
// exercise the portable pipe-based path on Linux/macOS.
#define CO_EVENT_EVENTFD 1
#define CO_EVENT_KQUEUE 2
#define CO_EVENT_PIPE 3

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
#define CO_TIMER_MODE CO_TIMER_EVENT
#ifndef CO_EVENT_MODE
#define CO_EVENT_MODE CO_EVENT_KQUEUE
#endif

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

#define CO_TIMER_MODE CO_TIMER_TIMERFD // Change this line to use POSIX timer instead
#ifndef CO_EVENT_MODE
#define CO_EVENT_MODE CO_EVENT_EVENTFD
#endif

#include <sys/epoll.h>
#include <ucontext.h>

#elif defined(__QNX__) || defined(__QNXNTO__)

// QNX configuration
#if defined(__x86_64__) || defined(__aarch64__)
// On QNX, use custom context switcher if available for the architecture.
#define CO_CTX_MODE CO_CTX_CUSTOM
#else
// Custom context switcher is not available for this architecture.  Use setjmp/longjmp.
#define CO_CTX_MODE CO_CTX_SETJMP
#endif
#define CO_TIMER_MODE CO_TIMER_POSIX
#ifndef CO_EVENT_MODE
#define CO_EVENT_MODE CO_EVENT_PIPE
#endif

// Nothing to include.

#else

// Other OS, use the custom context switcher if available
// or setjmp/longjmp if not.  The custom context switcher is only available
#if defined(__x86_64__) || defined(__aarch64__)
#define CO_CTX_MODE CO_CTX_CUSTOM
#else
// Portable version is setjmp/longjmp
#define CO_CTX_MODE CO_CTX_SETJMP
#endif

#define CO_TIMER_MODE CO_TIMER_POSIX // Use POSIX timer for other OSes
#ifndef CO_EVENT_MODE
#define CO_EVENT_MODE CO_EVENT_PIPE
#endif

#include <csetjmp>

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

#if __has_include(<valgrind/valgrind.h>)
#define CO_HAVE_VALGRIND 1
#else
#define CO_HAVE_VALGRIND 0
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

#if CO_TIMER_MODE == CO_TIMER_POSIX
#include <time.h>  // For timer_t
#endif

#include "bitset.h"
#include "detect_sanitizers.h"

#if defined(CO_ADDRESS_SANITIZER)
extern "C" {
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom,
                                    size_t size);

void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                     const void **bottom_old, size_t *size_old);
}
#endif

#if defined(CO_THREAD_SANITIZER)
extern "C" {
// Fiber switching API exposed by the ThreadSanitizer runtime.  We forward
// declare the entry points we use rather than including <sanitizer/tsan_interface.h>
// because that header is not always available on every toolchain.
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void *fiber);
void __tsan_switch_to_fiber(void *fiber, unsigned flags);
void __tsan_set_fiber_name(void *fiber, const char *name);
}
#endif

namespace co {

template <typename T>
using GeneratorFunction = std::function<void(Generator<T> *)>;

template <typename T>
using GeneratorFunctionRef = std::function<void(const Generator<T> &)>;

extern "C" {
// This is needed here because it's a friend with C linkage.
void __co_Invoke(class ScheduledCoroutine *c);
}

class ScheduledCoroutine;

struct YieldedCoroutine {
  YieldedCoroutine() = default;
  YieldedCoroutine(const ScheduledCoroutine *c, int f, uint32_t e = 0)
      : co(c), fd(f), events(e) {}
  const ScheduledCoroutine *co = nullptr;
  int fd = -1;
  uint32_t events = 0;
};

// EventFd is a portable representation of a triggerable file descriptor used
// to wake up coroutines.  On Linux it wraps an eventfd; on macOS it wraps a
// kqueue (with an EVFILT_USER filter); on other systems it is implemented as
// a non-blocking pipe pair.  In all cases:
//
//   - poll_fd    is the file descriptor that should be added to a poll/epoll
//                set.  It becomes readable when the event is triggered.
//   - trigger_fd is the file descriptor that should be written to in order to
//                signal the event.  On Linux/macOS it is the same fd as
//                poll_fd; on systems backed by a pipe it is the write end of
//                the pipe.
//
// This is functionally equivalent to toolbelt::TriggerFd but is self-contained
// to avoid a circular dependency on cpp_toolbelt.
struct EventFd {
  int poll_fd = -1;
  int trigger_fd = -1;

  bool IsValid() const { return poll_fd != -1; }

  // Trigger the event so that anything polling poll_fd will wake up.
  void Trigger() const;

  // Drain a previously triggered event (no-op if not triggered).
  void Clear() const;

  // Close any owned file descriptors and reset to the invalid state.
  void Close();

  // Reset to the invalid state without closing any file descriptors.
  void Reset() {
    poll_fd = -1;
    trigger_fd = -1;
  }

  // Allocate a new event fd suitable for general signalling.  Returns an
  // invalid EventFd (IsValid() == false) on failure.
  static EventFd Create();

  // Allocate a new event fd suitable for use as an abort signal.  On Linux
  // this is an eventfd with EFD_CLOEXEC set; on other platforms it behaves
  // identically to Create().
  static EventFd CreateAbort();
};

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
//
// ThreadSanitizer is also informed about every coroutine context switch via the
// __tsan_switch_to_fiber API, so cooperative yields between coroutines and the
// scheduler do not trigger spurious data-race reports.
class ScheduledCoroutine : public Coroutine {
public:
  // Important note: when using an interrupt_fd, you need to be careful
  // to duplicate it by calling dup(2) for each coroutine.  The coroutine
  // will add it to the poll set and that is racy if you use the same
  // fd in two coroutines.  In fact, when using epoll, it won't be allowed.
  ScheduledCoroutine(CoroutineScheduler &parent, CoroutineFunction function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  ScheduledCoroutine(CoroutineScheduler &parent, CoroutineFunction function,
            std::string name, size_t stack_size)
      : ScheduledCoroutine(parent, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  // Options based constructor.
  ScheduledCoroutine(CoroutineScheduler &parent, CoroutineFunction function,
            CoroutineOptions opts)
      : ScheduledCoroutine(parent, std::move(function), opts.name, opts.interrupt_fd,
                  opts.autostart,
                  opts.stack_size == 0 ? kCoDefaultStackSize : opts.stack_size,
                  opts.user_data) {}

  ScheduledCoroutine(CoroutineScheduler &parent, CoroutineFunctionRef function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  ScheduledCoroutine(CoroutineScheduler &parent, CoroutineFunctionRef function,
            std::string name, size_t stack_size)
      : ScheduledCoroutine(parent, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  ScheduledCoroutine(CoroutineScheduler &parent, std::function<void()> function,
            std::string name = "", int interrupt_fd = -1, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  ScheduledCoroutine(CoroutineScheduler &parent, std::function<void()> function,
            std::string name, size_t stack_size)
      : ScheduledCoroutine(parent, function, name, -1, true,
                  stack_size == 0 ? kCoDefaultStackSize : stack_size, nullptr) {
  }

  // Options based constructor.
  ScheduledCoroutine(CoroutineScheduler &parent, CoroutineFunctionRef function,
            CoroutineOptions opts)
      : ScheduledCoroutine(parent, std::move(function), opts.name, opts.interrupt_fd,
                  opts.autostart,
                  opts.stack_size == 0 ? kCoDefaultStackSize : opts.stack_size,
                  opts.user_data) {}

  ScheduledCoroutine(CoroutineScheduler &parent, std::function<void()> function,
            CoroutineOptions opts)
      : ScheduledCoroutine(parent, std::move(function), opts.name, opts.interrupt_fd,
                  opts.autostart,
                  opts.stack_size == 0 ? kCoDefaultStackSize : opts.stack_size,
                  opts.user_data) {}
  ~ScheduledCoroutine() override;

  // Start a coroutine running if it is not already running,
  void Start() override;

  // Yield control to another coroutine.
  void Yield() const override;
  void YieldToScheduler() const override;

  // Call another coroutine and store the result.
  template <typename T> T Call(Generator<T> &callee) const;

  // Note this can cause memory leaks as destructors in the coroutine function
  // will not be called.  Use sparingly.  You should really use an interrupt fd
  // to cause the function to exit cleanly, but this can get you out of stick
  // situations if you need it.
  void Exit() const override;

  // Sleeping functions.
  void Nanosleep(uint64_t ns) const override;

  // Abort the coroutine.  It will cause the current wait or sleep to throw an
  // internal exception which will unwind the stack.  It will also set the
  // aborted flag which can be checked inside the coroutine function.
  void Abort() const override;

  // Has this coroutine been aborted?
  bool IsAborted() const override { return aborted_; }

  // Is the given coroutine alive?
  bool IsAlive() const override;

  void Show() const override;

  // Make a string describing information about this coroutine.  By default
  // this will be the same as that printed by Show().
  std::string ToString() const override;

  void GetAllFds(std::vector<int> &fds) const override;

protected:
  void AddToUserWaitFds(int fd, uint32_t event_mask) const override;
  int WaitOnUserWaitFds(uint64_t timeout_ns) const override;
  int PollWithMutableFds(std::vector<struct pollfd> &fds) const override;

  friend class CoroutineScheduler;
  template <typename T> friend class Generator;

  friend void __co_Invoke(ScheduledCoroutine *c);
  friend int MakeTimer(const Coroutine *coroutine, uint64_t ns);
  static const char *StateName(State state);

  void InvokeFunction();
  int EndOfWait(int timer_fd) const;
  int AddTimeout(uint64_t timeout_ns) const;
  void AddAbortFd() const;
#if CO_TIMER_MODE == CO_TIMER_POSIX
  void CleanupPosixTimer() const;
#endif

  State GetState() const { return state_; }
#if CO_POLL_MODE == CO_POLL_POLL
  void AddPollFds(std::vector<struct pollfd> &pollfds,
                  std::vector<ScheduledCoroutine *> &covec);
#endif
  void Resume(int value) const;
  void TriggerEvent() const;
  void ClearEvent() const;
  void CallNonTemplate(ScheduledCoroutine &c) const override;
  void YieldNonTemplate() const;
  void SetState(State state) const;

  std::string MakeDefaultString() const;

  mutable void *yielded_address_ = nullptr; // Address at which we've yielded.
  mutable Context resume_;
  mutable Context exit_;
  mutable int wait_result_ = -1;  // Initialize to -1 to avoid garbage values
  mutable bool first_resume_ = true;

  // Abort handling.  If the scheduler has been configured to abort coroutines on stop.
  // we allocate an abort fd.
  //
  // Aborting a coroutine causes it to correctly terminate its execution function, unwinding
  // the stack and calling destructors.  It is a clean way to terminate a coroutine without
  // having to use an interrupt fd and check for termination in the coroutine function.
  mutable EventFd abort_fd_;
  mutable bool abort_pending_ = false;    // Coroutine::Abort called.
  mutable bool aborted_ = false;      // Abort has been processed.

  // Event fd used to wake this coroutine up.  The scheduler waits on
  // event_fd_.poll_fd; TriggerEvent()/ClearEvent() write/read via the
  // EventFd's trigger_fd/poll_fd as appropriate.
  mutable EventFd event_fd_;

#if CO_POLL_MODE == CO_POLL_EPOLL
  mutable YieldedCoroutine yield_fd_;
  mutable std::vector<YieldedCoroutine> wait_fds_;
  mutable int num_epoll_events_ = 0;
#else
  mutable std::vector<struct pollfd>
      wait_fds_; // Pollfds for waiting for an fd.
#endif
  mutable const ScheduledCoroutine *caller_ =
      nullptr;                     // If being called, who is calling us.

#if CO_TIMER_MODE == CO_TIMER_POSIX
  mutable int posix_timer_write_fd_ = -1; // Write end of pipe for POSIX timer
  mutable timer_t posix_timer_id_ = {}; // POSIX timer ID
  mutable bool posix_timer_active_ = false;
  mutable int posix_timer_read_fd_ = -1; // Read end of pipe (timer fd)
#endif

#if CO_HAVE_VALGRIND
  int valgrind_stack_id_ = -1;
#endif
#if defined(CO_THREAD_SANITIZER)
  // Opaque per-coroutine fiber context owned by the TSan runtime.  Created in
  // the constructor and destroyed in the destructor.  Switched to whenever the
  // scheduler resumes this coroutine.
  void *tsan_fiber_ = nullptr;
#endif
};

// A Generator is a coroutine that generates values.  The magic lamda line
// noise is because you can't cast an std::function<void(B*)> to an
// std::function<void(A*)> even though B is derived from A.
//
// A generator doesn't start automatically.  It's started on the
// first call.
template <typename T> class Generator : public ScheduledCoroutine {
public:
  Generator(CoroutineScheduler &parent, GeneratorFunction<T> function,
            std::string name = "", int interrupt_fd = -1,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr)
      : ScheduledCoroutine(parent,
                  [function = std::move(function)](const Coroutine &c) {
                    function(reinterpret_cast<Generator<T> *>(
                        const_cast<Coroutine *>(&c)));
                  },
                  name, interrupt_fd, /*autostart=*/false, stack_size,
                  user_data) {}

  Generator(CoroutineScheduler &parent, GeneratorFunctionRef<T> function,
            std::string name = "", int interrupt_fd = -1,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr)
      : ScheduledCoroutine(parent,
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
  std::vector<ScheduledCoroutine *> coroutines;
};

class CoroutineScheduler {
public:
  CoroutineScheduler();
  virtual ~CoroutineScheduler();

  // Run the scheduler until all coroutines have terminated or
  // told to stop.
  void Run();

  // Stop the scheduler.  Running coroutines will not be terminated.
  // This function is thread-safe since a common pattern is to "Run()"
  // the coroutines in a background thread.
  void Stop();

  int GetInterruptFd() const { return co_interrupt_fd_.poll_fd; }

  void TriggerInterrupt() const;

  // Enable the abort functionality for coroutines.  When enabled, you
  // can call Abort on a coroutine and the scheduler will abort all running
  // coroutines when it is stopped.
  void EnableAborts(bool enabled = true) { aborts_enabled_ = enabled; }
  void SetAbortOnStop(bool abort_on_stop = false) { abort_on_stop_ = abort_on_stop; }

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

  // Subclass can override this to provide custom behavior on coroutine
  // resumption.
  virtual void OnResume(const Coroutine *) {}

  int GetEpollFd() const {
#if CO_POLL_MODE == CO_POLL_EPOLL
    return epoll_fd_;
    ;
#else
    return -1;
#endif
  }

  // Get a vector containing all the strings generated by the
  // coroutines.
  std::vector<std::string> AllCoroutineStrings() const;

  std::vector<int> GetAllFds() const;

  co::Coroutine *Spawn(std::function<void(co::Coroutine *)> f,
                       CoroutineOptions opts = {});
  co::Coroutine *Spawn(std::function<void()> f, CoroutineOptions opts = {});

  bool IsRunning() const { return running_; }

protected:
  friend class Coroutine;
  friend class ScheduledCoroutine;
  template <typename T> friend class Generator;
  friend int MakeTimer(const Coroutine *coroutine, uint64_t ns);

  void AddCoroutine(ScheduledCoroutine *c);
  void RemoveCoroutine(const ScheduledCoroutine *c);

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

  std::list<ScheduledCoroutine *> coroutines_;
  // These are coroutines owned by the scheduler (created using Spawn).
  absl::flat_hash_set<std::unique_ptr<ScheduledCoroutine>> owned_coroutines_;

  BitSet coroutine_ids_;
  uint32_t last_freed_coroutine_id_ = -1U;
  Context yield_;
  std::atomic<bool> running_ = false;
#if CO_POLL_MODE == CO_POLL_EPOLL
  absl::flat_hash_map<int, absl::flat_hash_set<YieldedCoroutine *>>
      waiting_coroutines_;
  int epoll_fd_ = -1;
  size_t num_epoll_events_ = 0;
#else
  PollState poll_state_;
#endif
  EventFd interrupt_fd_;
  EventFd co_interrupt_fd_;

  uint64_t tick_count_ = 0;
  CompletionCallback completion_callback_;
  absl::flat_hash_set<const ScheduledCoroutine *> deletions_;
#if defined(CO_ADDRESS_SANITIZER)
  void *fake_stack_ = nullptr;
#endif
#if defined(CO_THREAD_SANITIZER)
  // TSan fiber identifying the thread that runs the scheduler loop.  Captured
  // lazily on the first call to Run() (which is when we know which thread the
  // scheduler is executing on) and switched to whenever a coroutine yields
  // back to the scheduler.
  void *tsan_fiber_ = nullptr;
#endif
  std::atomic<bool> aborts_enabled_ = true;
  std::atomic<bool> abort_on_stop_ = false;
};

template <typename T>
inline void Generator<T>::YieldValue(const T &value) const {
  // Copy value.
  if (result_ != nullptr) {
    *result_ = value;
  }
  YieldNonTemplate();
}

} // namespace co
