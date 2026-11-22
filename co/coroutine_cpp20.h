// Copyright 2023-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

// Copyright 2026 Cruise LLC.

#pragma once

// Poll mode constants - must be defined before use in conditionals.
#define CO_POLL_EPOLL 1
#define CO_POLL_POLL 2

#ifndef CO_POLL_MODE
#ifdef __linux__
#define CO_POLL_MODE CO_POLL_EPOLL
#else
#define CO_POLL_MODE CO_POLL_POLL
#endif
#endif

// Timer mode constants.
// 1. CO_TIMER_TIMERFD - Linux timerfd (Linux only)
// 2. CO_TIMER_EVENT   - macOS kqueue EVFILT_TIMER (macOS only)
// 3. CO_TIMER_POSIX   - POSIX timer_create with pipe (portable)
#define CO_TIMER_TIMERFD 1
#define CO_TIMER_EVENT 2
#define CO_TIMER_POSIX 3

#ifndef CO_TIMER_MODE
#if defined(__APPLE__)
#define CO_TIMER_MODE CO_TIMER_EVENT
#elif defined(__linux__)
#define CO_TIMER_MODE CO_TIMER_TIMERFD
#else
#define CO_TIMER_MODE CO_TIMER_POSIX
#endif
#endif

// Check for C++20 coroutine support
#if __cplusplus >= 202002L
  #if __has_include(<coroutine>)
    #include <coroutine>
    #if defined(__cpp_coroutines) && __cpp_coroutines >= 201902L
      #define CO20_HAVE_COROUTINES 1
    #elif defined(_MSC_VER) && _MSC_VER >= 1928
      #define CO20_HAVE_COROUTINES 1
    #elif defined(__GNUC__) && __GNUC__ >= 10
      #define CO20_HAVE_COROUTINES 1
    #elif defined(__clang__) && __clang_major__ >= 14
      #define CO20_HAVE_COROUTINES 1
    #else
      #define CO20_HAVE_COROUTINES 0
    #endif
  #else
    #define CO20_HAVE_COROUTINES 0
  #endif
#else
  #define CO20_HAVE_COROUTINES 0
#endif

#if CO20_HAVE_COROUTINES

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <sys/poll.h>
#include <type_traits>
#include <unistd.h>
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "toolbelt/async_waiter.h"
#include <vector>

#if CO_POLL_MODE == CO_POLL_EPOLL
#include <sys/epoll.h>
#endif

#if CO_TIMER_MODE == CO_TIMER_EVENT
#include <sys/event.h>
#endif

#if CO_TIMER_MODE == CO_TIMER_POSIX
#include <signal.h>
#include <time.h>
#endif

namespace co20 {

class Coroutine;
class Scheduler;

// Thrown at the next co_await of a coroutine that has been aborted.  A
// coroutine that lets it escape simply finishes.
struct AbortException {};

// A file descriptor and the poll events (POLLIN, POLLOUT) to wait for.  When
// returned by WaitAll, events holds the events that are ready.
struct WaitFd {
  int fd = -1;
  uint32_t events = POLLIN;
};

using WaitFdList = absl::InlinedVector<WaitFd, 4>;

struct BaseAwaitable;
struct YieldAwaitable;
struct WaitAwaitable;
struct WaitAllAwaitable;
struct SleepAwaitable;
struct Task;
template <typename T> struct ValueTask;

// Represents a single C++20 stackless coroutine managed by a Scheduler.  The
// scheduler owns it and destroys it when its function finishes.
//
// Like co::Coroutine it is a toolbelt::AsyncWaiter, so code written against
// that interface can use it for the non-blocking Poll functions.  A stackless
// coroutine can't suspend from inside an ordinary function call, though, so the
// suspending operations (Yield, Wait, Sleep) are declared here as functions that
// return awaitables to be used with co_await.  They hide the AsyncWaiter
// versions, which are only reached through a toolbelt::AsyncWaiter reference,
// and there they block the calling thread, and so every coroutine on the
// scheduler, instead of suspending.
class Coroutine : public toolbelt::AsyncWaiter {
public:
  enum class State {
    kNew,
    kReady,
    kRunning,
    kYielded,
    kWaiting,
    kDead
  };

  friend class Scheduler;
  friend struct BaseAwaitable;
  friend struct YieldAwaitable;
  friend struct WaitAwaitable;
  friend struct WaitAllAwaitable;
  friend struct SleepAwaitable;
  friend struct Task;
  template <typename T> friend struct ValueTask;

  template<typename Func>
  Coroutine(Scheduler& scheduler, Func&& func, const std::string& name = "",
            int interrupt_fd = -1);

  ~Coroutine() override;

  Coroutine(const Coroutine&) = delete;
  Coroutine& operator=(const Coroutine&) = delete;
  Coroutine(Coroutine&&) = delete;
  Coroutine& operator=(Coroutine&&) = delete;

  // Poll (immediately, without blocking) is inherited from
  // toolbelt::AsyncWaiter.  It also checks the coroutine's interrupt fd.

  // Give other ready coroutines a turn.  Equivalent to Sleep(0): does not
  // create a timer.  The scheduler runs Yield-ers in fair passes and parks
  // on I/O or the interrupt fd between passes, so a Yield-only loop cannot
  // starve Wait/Sleep or ignore Stop().
  YieldAwaitable Yield();

  // Wait for one of a set of fds to become ready.  The result is the fd that
  // is ready, the coroutine's interrupt fd if that was triggered, or -1 if
  // timeout_ns (when non-zero) expired first.
  WaitAwaitable Wait(int fd, uint32_t event_mask = POLLIN, uint64_t timeout_ns = 0);
  WaitAwaitable Wait(std::initializer_list<int> fds, uint32_t event_mask = POLLIN,
                     uint64_t timeout_ns = 0);
  WaitAwaitable Wait(const std::vector<int>& fds, uint32_t event_mask = POLLIN,
                     uint64_t timeout_ns = 0);
  WaitAwaitable Wait(const std::vector<WaitFd>& fds, uint64_t timeout_ns = 0);

  // Wait for at least one of a set of fds to become ready and return all of
  // the ones that are ready, with WaitFd::events set to the ready events.  The
  // result is empty on timeout or interrupt.
  WaitAllAwaitable WaitAll(const std::vector<WaitFd>& fds, uint64_t timeout_ns = 0);

  SleepAwaitable Sleep(uint64_t nanoseconds);

  template <typename Rep, typename Period>
  SleepAwaitable Sleep(std::chrono::duration<Rep, Period> duration);

  SleepAwaitable Millisleep(uint64_t milliseconds);

  // The toolbelt::AsyncWaiter interface.  These can't suspend the coroutine,
  // so they block the calling thread instead.  In a coroutine function use the
  // awaitable versions above with co_await.
  void Yield() const override;
  void Nanosleep(uint64_t ns) const override;

  // Make the coroutine throw AbortException at its current or next co_await.
  void Abort();

  bool IsAborted() const { return abort_pending_; }
  const std::string& Name() const { return name_; }
  void SetName(const std::string& name) { name_ = name; }
  int Id() const { return id_; }
  State GetState() const { return state_; }
  Scheduler& GetScheduler() const { return scheduler_; }
  int GetInterruptFd() const { return interrupt_fd_; }
  // Scheduler tick when this coroutine last parked (Yield/Wait/Sleep).
  // Used to prefer longer-waiting coroutines, matching the stackful scheduler.
  uint64_t LastTick() const { return last_tick_; }

  // A one-line description of the coroutine for debugging.
  std::string ToString() const;
  void Show() const;

protected:
  // toolbelt::AsyncWaiter implementation.  The wait is a blocking poll of the
  // fds added, plus the interrupt fd.
  void AddToUserWaitFds(int fd, uint32_t event_mask) const override;
  int WaitOnUserWaitFds(uint64_t timeout_ns) const override;
  int PollWithMutableFds(std::vector<struct pollfd>& fds) const override;

private:
  void SetHandle(std::coroutine_handle<> handle) { handle_ = handle; }
  void SetState(State state) { state_ = state; }
  void SetWaitResult(int result) { wait_result_ = result; }
  int GetWaitResult() const { return wait_result_; }
  void SetLastTick(uint64_t tick) { last_tick_ = tick; }

  // The poll events this coroutine is waiting for on fd, or 0 if it isn't
  // waiting on it.
  uint32_t WaitMaskFor(int fd) const;

  inline void Resume(int value = 0);

  // The frame of the coroutine function.  Destroying it destroys the frames
  // of any ValueTasks it is suspended in.
  std::coroutine_handle<> root_;
  // The innermost frame, where the coroutine resumes.
  std::coroutine_handle<> handle_;
  Scheduler& scheduler_;
  State state_ = State::kNew;
  std::string name_;
  bool abort_pending_ = false;
  int id_;
  int wait_result_ = -1;
  int interrupt_fd_ = -1;
  int timeout_fd_ = -1;
  // Tick count at the last Yield/Wait/Sleep.  Lower means waited longer.
  uint64_t last_tick_ = 0;
  // The fds (other than the interrupt and timeout fds) passed to the wait the
  // coroutine is suspended in.
  WaitFdList wait_fds_;
  // Copy of wait_fds_ kept after DetachWaits so a readiness re-check can
  // re-park the coroutine if another waiter already drained the fd.
  WaitFdList parked_wait_fds_;
  uint64_t parked_timeout_ns_ = 0;
  // The fds added by AsyncWaiter::Wait and not yet waited on.
  mutable std::vector<struct pollfd> user_wait_fds_;
  // An exception other than AbortException that escaped the coroutine
  // function.  Scheduler::Run rethrows it.
  std::exception_ptr exception_;
  // Holds the callable (e.g. lambda) that produced the coroutine, keeping
  // its captures alive for the lifetime of the coroutine frame.
  std::shared_ptr<void> callable_storage_;
};

// Manages a set of coroutines, scheduling them cooperatively using
// epoll (Linux) or poll as the I/O readiness backend.
class Scheduler {
public:
  using CompletionCallback = std::function<void(Coroutine*)>;

  Scheduler();
  ~Scheduler();

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // Run until every coroutine has finished or Stop is called.  If a coroutine
  // function lets an exception other than AbortException escape, the
  // coroutine is destroyed and Run rethrows the exception.
  void Run();

  // Make Run return at its next opportunity.  A Stop before Run makes the next
  // Run return immediately.
  void Stop() {
    stop_requested_ = true;
    TriggerInterrupt();
  }

  bool IsRunning() const { return running_ && !stop_requested_; }

  template<typename Func>
  Coroutine* Spawn(Func&& func, const std::string& name = "",
                   int interrupt_fd = -1) {
    auto coroutine = std::make_unique<Coroutine>(*this, std::forward<Func>(func), name,
                                                 interrupt_fd);
    Coroutine* ptr = coroutine.get();
    coroutines_.emplace(ptr, std::move(coroutine));
    MakeReady(ptr, 0);
    return ptr;
  }

  // Called with each coroutine just before it is destroyed after its function
  // finishes.
  void SetCompletionCallback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
  }

  size_t NumCoroutines() const { return coroutines_.size(); }

  // Monotonic resume counter used for wait-time fairness (see LastTick).
  uint64_t TickCount() const { return tick_count_; }

  // Print every coroutine to stderr.
  void Show() const;
  std::vector<std::string> AllCoroutineStrings() const;

  // Every fd the scheduler and its coroutines are using, including the fds
  // coroutines are waiting on.  A child process can close them after fork.
  std::vector<int> GetAllFds() const;

  // Coroutine::Resume advances the tick counter.
  friend class Coroutine;

  void ScheduleCoroutine(Coroutine* coroutine);
  void WaitForFds(Coroutine* coroutine, const WaitFd* fds, size_t num_fds,
                  uint64_t timeout_ns);
  void WaitForFd(Coroutine* coroutine, int fd, uint32_t event_mask, uint64_t timeout_ns) {
    WaitFd wait_fd{fd, event_mask};
    WaitForFds(coroutine, &wait_fd, 1, timeout_ns);
  }
  void SleepFor(Coroutine* coroutine, uint64_t nanoseconds);
  // Wake a coroutine that is waiting, making its wait return value.
  void ResumeCoroutine(Coroutine* coroutine, int value);
  int PollFd(int fd, uint32_t event_mask);
  int AllocateId() { return next_id_++; }

private:
  void MakeReady(Coroutine* coroutine, int value);
  // Run at most one fair pass over the coroutines that were ready when the
  // call started.  Yield re-queues, so draining until empty would never
  // return and would pin the CPU.
  void ProcessReadyCoroutines();
  // Drain ready I/O without blocking (timeout_ms == 0) or block until
  // something is ready (timeout_ms < 0).  Called after every resume so a
  // Yield-only coroutine cannot starve waiters on the poll() backend.
  void PollWaitingFds(int timeout_ms);
  // Block on the scheduler interrupt fd only (used when nothing is waiting
  // on user fds but Yield-ers remain).  timeout_ms < 0 waits forever.
  void WaitOnInterrupt(int timeout_ms);
  void ProcessEvents();
  // Wake the coroutines waiting on fd for the ready events.
  void DispatchFdEvents(int fd, uint32_t revents);
  // Register coroutine as a waiter on fd.  Returns false if fd can't be
  // waited on.
  bool AddWaiter(Coroutine* coroutine, int fd);
  void RemoveWaiter(Coroutine* coroutine, int fd);
  // Make the epoll registration of fd match what its waiters want.
  bool UpdateEpoll(int fd);
  // Remove the coroutine from every fd it is waiting on and close its
  // timeout timer.
  void DetachWaits(Coroutine* coroutine);
  // Create a timer that fires after nanoseconds.  Returns -1 on failure.
  int CreateTimer(uint64_t nanoseconds);
  void CloseTimer(int fd);
  // Called when a coroutine's function has finished.  Destroys the coroutine.
  void FinishCoroutine(Coroutine* coroutine);
  void TriggerInterrupt();

  bool running_ = false;
  bool stop_requested_ = false;
  // Destroyed last so that coroutine frames are destroyed while the wait
  // tables are still valid.  ~Scheduler destroys them explicitly first.
  absl::flat_hash_map<Coroutine*, std::unique_ptr<Coroutine>> coroutines_;
  std::deque<Coroutine*> ready_queue_;
  absl::flat_hash_map<int, std::vector<Coroutine*>> waiting_fds_;
  absl::flat_hash_set<int> timerfds_;
  CompletionCallback completion_callback_;
  // Advanced on every Resume; stamped into Coroutine::last_tick_ when parking.
  uint64_t tick_count_ = 0;

#if CO_TIMER_MODE == CO_TIMER_POSIX
  struct PosixTimerInfo {
    timer_t timer_id;
    int write_fd;
  };
  absl::flat_hash_map<int, PosixTimerInfo> posix_timers_;
#endif

#if CO_POLL_MODE == CO_POLL_EPOLL
  int epoll_fd_ = -1;
  // The epoll events each fd is registered for.
  absl::flat_hash_map<int, uint32_t> epoll_events_;
#endif

  int interrupt_fd_ = -1;
#if !defined(__linux__)
  int interrupt_write_fd_ = -1;
#endif
  int next_id_ = 1;
};

// --- Awaitable types ---

struct BaseAwaitable {
  Coroutine* coroutine_ = nullptr;
  Scheduler* scheduler_ = nullptr;

  BaseAwaitable(Coroutine* coroutine, Scheduler* scheduler)
    : coroutine_(coroutine), scheduler_(scheduler) {}

  bool Aborted() const { return coroutine_ != nullptr && coroutine_->IsAborted(); }

  void ThrowIfAborted() const {
    if (Aborted()) {
      throw AbortException();
    }
  }

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) const {
    if (coroutine_) {
      coroutine_->SetHandle(handle);
    }
  }
};

struct [[nodiscard]] YieldAwaitable : BaseAwaitable {
  using BaseAwaitable::BaseAwaitable;

  bool await_ready() const noexcept { return Aborted(); }

  void await_resume() const { ThrowIfAborted(); }

  void await_suspend(std::coroutine_handle<> handle) const {
    BaseAwaitable::await_suspend(handle);
    if (coroutine_ && scheduler_) {
      coroutine_->SetState(Coroutine::State::kYielded);
      scheduler_->ScheduleCoroutine(coroutine_);
    }
  }
};

// Poll fds without blocking.  Returns the index of the first ready fd in fds
// or -1 if none is ready.  If ready is not null, every ready fd is appended to
// it with its ready events.
int PollFds(const WaitFd* fds, size_t num_fds, std::vector<WaitFd>* ready = nullptr);

struct [[nodiscard]] WaitAwaitable : BaseAwaitable {
  WaitFdList fds_;
  uint64_t timeout_ns_;
  mutable int result_ = -1;
  mutable bool ready_ = false;

  WaitAwaitable(Coroutine* coroutine, Scheduler* scheduler, WaitFdList fds,
                uint64_t timeout_ns)
    : BaseAwaitable(coroutine, scheduler), fds_(std::move(fds)), timeout_ns_(timeout_ns) {}

  bool await_ready() const noexcept {
    if (Aborted()) {
      return true;
    }
    int index = PollFds(fds_.data(), fds_.size());
    if (index >= 0) {
      result_ = fds_[static_cast<size_t>(index)].fd;
      ready_ = true;
      return true;
    }
    return false;
  }

  void await_suspend(std::coroutine_handle<> handle) const {
    BaseAwaitable::await_suspend(handle);
    if (coroutine_ && scheduler_) {
      scheduler_->WaitForFds(coroutine_, fds_.data(), fds_.size(), timeout_ns_);
    }
  }

  int await_resume() const {
    ThrowIfAborted();
    if (ready_) {
      return result_;
    }
    if (coroutine_) {
      return coroutine_->GetWaitResult();
    }
    return -1;
  }
};

struct [[nodiscard]] WaitAllAwaitable : BaseAwaitable {
  WaitFdList fds_;
  uint64_t timeout_ns_;
  mutable std::vector<WaitFd> ready_fds_;
  mutable bool ready_ = false;

  WaitAllAwaitable(Coroutine* coroutine, Scheduler* scheduler, WaitFdList fds,
                   uint64_t timeout_ns)
    : BaseAwaitable(coroutine, scheduler), fds_(std::move(fds)), timeout_ns_(timeout_ns) {}

  bool await_ready() const noexcept {
    if (Aborted()) {
      return true;
    }
    ready_ = PollFds(fds_.data(), fds_.size(), &ready_fds_) >= 0;
    return ready_;
  }

  void await_suspend(std::coroutine_handle<> handle) const {
    BaseAwaitable::await_suspend(handle);
    if (coroutine_ && scheduler_) {
      scheduler_->WaitForFds(coroutine_, fds_.data(), fds_.size(), timeout_ns_);
    }
  }

  std::vector<WaitFd> await_resume() const {
    ThrowIfAborted();
    if (!ready_ && coroutine_ != nullptr) {
      // Woken by one of the fds: collect every fd that is ready now.  A
      // timeout or interrupt leaves the result empty.
      const int woken_by = coroutine_->GetWaitResult();
      for (const WaitFd& fd : fds_) {
        if (fd.fd == woken_by) {
          PollFds(fds_.data(), fds_.size(), &ready_fds_);
          break;
        }
      }
    }
    return std::move(ready_fds_);
  }
};

struct [[nodiscard]] SleepAwaitable : BaseAwaitable {
  uint64_t timeout_ns_;

  SleepAwaitable(Coroutine* coroutine, Scheduler* scheduler, uint64_t timeout_ns)
    : BaseAwaitable(coroutine, scheduler), timeout_ns_(timeout_ns) {}

  bool await_ready() const noexcept {
    return timeout_ns_ == 0 || Aborted();
  }

  void await_suspend(std::coroutine_handle<> handle) const {
    BaseAwaitable::await_suspend(handle);
    if (coroutine_ && scheduler_) {
      scheduler_->SleepFor(coroutine_, timeout_ns_);
    }
  }

  void await_resume() const { ThrowIfAborted(); }
};

// --- Coroutine inline implementations (need full Scheduler definition) ---

template<typename Func>
inline Coroutine::Coroutine(Scheduler& scheduler, Func&& func, const std::string& name,
                            int interrupt_fd)
  : root_(),
    handle_(),
    scheduler_(scheduler),
    state_(State::kNew),
    name_(),
    abort_pending_(false),
    id_(scheduler.AllocateId()),
    wait_result_(-1),
    interrupt_fd_(interrupt_fd == -1 ? -1 : fcntl(interrupt_fd, F_DUPFD_CLOEXEC, 0)) {
  name_ = name.empty() ? "co-" + std::to_string(id_) : name;

  // Store the callable so its captures (e.g. lambda captures) remain alive
  // for the lifetime of the coroutine. C++20 coroutine frames hold a pointer
  // to the callable's captures, so the callable must outlive the frame.
  using FuncType = std::decay_t<Func>;
  auto stored = std::make_shared<FuncType>(std::forward<Func>(func));
  callable_storage_ = stored;

  auto task = [&]() {
    if constexpr (std::is_invocable_v<FuncType, Coroutine&>) {
      return (*stored)(*this);
    } else {
      return (*stored)();
    }
  }();
  if constexpr (requires { task.handle_; }) {
    root_ = std::coroutine_handle<>(task.handle_);
    handle_ = root_;
    if constexpr (requires { task.handle_.promise().coroutine; }) {
      task.handle_.promise().coroutine = this;
    }
    task.handle_ = {};
  }
}

inline Coroutine::~Coroutine() {
  // Destroy the frame before the callable whose captures it refers to.
  if (root_) {
    root_.destroy();
  }
  if (interrupt_fd_ != -1) {
    close(interrupt_fd_);
  }
}

inline void Coroutine::Abort() {
  abort_pending_ = true;
  if (state_ == State::kWaiting) {
    scheduler_.ResumeCoroutine(this, -1);
  }
}

inline YieldAwaitable Coroutine::Yield() {
  return YieldAwaitable(this, &scheduler_);
}

inline WaitAwaitable Coroutine::Wait(int fd, uint32_t event_mask, uint64_t timeout_ns) {
  return WaitAwaitable(this, &scheduler_, WaitFdList{{fd, event_mask}}, timeout_ns);
}

inline WaitAwaitable Coroutine::Wait(std::initializer_list<int> fds, uint32_t event_mask,
                                     uint64_t timeout_ns) {
  WaitFdList list;
  for (int fd : fds) {
    list.push_back({fd, event_mask});
  }
  return WaitAwaitable(this, &scheduler_, std::move(list), timeout_ns);
}

inline WaitAwaitable Coroutine::Wait(const std::vector<int>& fds, uint32_t event_mask,
                                     uint64_t timeout_ns) {
  WaitFdList list;
  for (int fd : fds) {
    list.push_back({fd, event_mask});
  }
  return WaitAwaitable(this, &scheduler_, std::move(list), timeout_ns);
}

inline WaitAwaitable Coroutine::Wait(const std::vector<WaitFd>& fds, uint64_t timeout_ns) {
  return WaitAwaitable(this, &scheduler_, WaitFdList(fds.begin(), fds.end()), timeout_ns);
}

inline WaitAllAwaitable Coroutine::WaitAll(const std::vector<WaitFd>& fds,
                                           uint64_t timeout_ns) {
  return WaitAllAwaitable(this, &scheduler_, WaitFdList(fds.begin(), fds.end()), timeout_ns);
}

inline SleepAwaitable Coroutine::Sleep(uint64_t nanoseconds) {
  return SleepAwaitable(this, &scheduler_, nanoseconds);
}

template <typename Rep, typename Period>
inline SleepAwaitable Coroutine::Sleep(std::chrono::duration<Rep, Period> duration) {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  return Sleep(ns > 0 ? static_cast<uint64_t>(ns) : 0);
}

inline SleepAwaitable Coroutine::Millisleep(uint64_t milliseconds) {
  return Sleep(milliseconds * 1000000ULL);
}

// Thread-local pointers to the currently running coroutine and its scheduler.
// These are set automatically by the scheduler before each coroutine is resumed,
// enabling the free functions below. Each thread running a scheduler gets its
// own copy. Declared here (before Task/ValueTask) so that ValueTask::await_resume
// can reference co20::self to restore the Coroutine's handle tracking.
extern thread_local Coroutine* self;
extern thread_local Scheduler* scheduler;

// Task type returned by coroutine functions: [](Coroutine& co) -> Task { ... }
struct Task {
  struct promise_type {
    Coroutine* coroutine = nullptr;

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalSuspendAwaiter {
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
        if (handle.promise().coroutine) {
          handle.promise().coroutine->SetState(Coroutine::State::kDead);
        }
      }
      void await_resume() const noexcept {}
    };

    auto final_suspend() noexcept { return FinalSuspendAwaiter{}; }

    // An escaping AbortException just ends the coroutine.  Anything else is
    // kept so that Scheduler::Run can rethrow it once the coroutine is gone;
    // letting it escape handle_.resume() would corrupt the scheduler.
    void unhandled_exception() {
      try {
        throw;
      } catch (const AbortException&) {
      } catch (...) {
        if (coroutine) {
          coroutine->exception_ = std::current_exception();
        }
      }
    }

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    void return_void() {}
  };

  std::coroutine_handle<promise_type> handle_;

  Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
  ~Task() { if (handle_) handle_.destroy(); }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = {}; }
};

namespace internal {

// Final awaiter shared by ValueTask<T> promises: resume the awaiting caller.
template <typename Promise>
struct ValueTaskFinalAwaiter {
  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) const noexcept {
    if (auto cont = h.promise().continuation_; cont) {
      return cont;
    }
    return std::noop_coroutine();
  }
  void await_resume() const noexcept {}
};

}  // namespace internal

// A sub-coroutine that produces a value of type T. Unlike Task (which is
// managed by the Scheduler), ValueTask<T> is designed to be co_await'ed from
// within a Task or another ValueTask. It uses symmetric transfer to run inline
// with the caller, sharing the same Scheduler and Coroutine context.  An
// exception that escapes it, including AbortException, is rethrown in the
// caller.
//
// Usage:
//   ValueTask<int> ComputeAsync(Coroutine& co) {
//     co_await co.Wait(fd, POLLIN);
//     co_return 42;
//   }
//
//   // From within a Task:
//   int result = co_await ComputeAsync(co);
template <typename T>
struct [[nodiscard]] ValueTask {
  struct promise_type {
    std::optional<T> value_;
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_{};

    std::suspend_always initial_suspend() noexcept { return {}; }
    internal::ValueTaskFinalAwaiter<promise_type> final_suspend() noexcept { return {}; }
    void unhandled_exception() { exception_ = std::current_exception(); }

    ValueTask get_return_object() {
      return ValueTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    template <typename U = T>
    void return_value(U&& v) { value_.emplace(std::forward<U>(v)); }
  };

  std::coroutine_handle<promise_type> handle_;

  explicit ValueTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
  ~ValueTask() { if (handle_) handle_.destroy(); }

  ValueTask(const ValueTask&) = delete;
  ValueTask& operator=(const ValueTask&) = delete;
  ValueTask(ValueTask&& o) noexcept : handle_(o.handle_) { o.handle_ = {}; }
  ValueTask& operator=(ValueTask&& o) noexcept {
    if (this != &o) {
      if (handle_) handle_.destroy();
      handle_ = o.handle_;
      o.handle_ = {};
    }
    return *this;
  }

  // Awaitable interface for co_await.
  bool await_ready() const noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) const noexcept {
    handle_.promise().continuation_ = caller;
    return handle_;
  }

  T await_resume() {
    // Restore the Coroutine's handle to point to the caller (continuation)
    // before this ValueTask is destroyed, preventing a dangling handle.
    if (co20::self && handle_.promise().continuation_) {
      co20::self->SetHandle(handle_.promise().continuation_);
    }
    if (handle_.promise().exception_) {
      std::rethrow_exception(handle_.promise().exception_);
    }
    return std::move(*handle_.promise().value_);
  }
};

// Void specialization of ValueTask for sub-coroutines that don't return a
// value but still need to co_await internally.
template <>
struct [[nodiscard]] ValueTask<void> {
  struct promise_type {
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_{};

    std::suspend_always initial_suspend() noexcept { return {}; }
    internal::ValueTaskFinalAwaiter<promise_type> final_suspend() noexcept { return {}; }
    void unhandled_exception() { exception_ = std::current_exception(); }

    ValueTask get_return_object() {
      return ValueTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    void return_void() {}
  };

  std::coroutine_handle<promise_type> handle_;

  explicit ValueTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
  ~ValueTask() { if (handle_) handle_.destroy(); }

  ValueTask(const ValueTask&) = delete;
  ValueTask& operator=(const ValueTask&) = delete;
  ValueTask(ValueTask&& o) noexcept : handle_(o.handle_) { o.handle_ = {}; }
  ValueTask& operator=(ValueTask&& o) noexcept {
    if (this != &o) {
      if (handle_) handle_.destroy();
      handle_ = o.handle_;
      o.handle_ = {};
    }
    return *this;
  }

  bool await_ready() const noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) const noexcept {
    handle_.promise().continuation_ = caller;
    return handle_;
  }

  void await_resume() const {
    if (co20::self && handle_.promise().continuation_) {
      co20::self->SetHandle(handle_.promise().continuation_);
    }
    if (handle_.promise().exception_) {
      std::rethrow_exception(handle_.promise().exception_);
    }
  }
};

// --- Coroutine::Resume (needs thread_local declarations above) ---

inline void Coroutine::Resume(int value) {
  if (handle_ && !root_.done()) {
    ++scheduler_.tick_count_;
    wait_result_ = value;
    state_ = State::kRunning;
    co20::self = this;
    co20::scheduler = &scheduler_;
    handle_.resume();
    co20::self = nullptr;
    if (root_.done()) {
      state_ = State::kDead;
    }
  }
}

// --- Free functions for non-invasive coroutine access ---
// These return awaitables and must be used with co_await, e.g.:
//   co_await co20::Yield();
//   int fd = co_await co20::Wait(my_fd, POLLIN);
//   co_await co20::Sleep(std::chrono::milliseconds(100));

inline YieldAwaitable Yield() { return self->Yield(); }

inline WaitAwaitable Wait(int fd, uint32_t event_mask = POLLIN,
                          uint64_t timeout_ns = 0) {
  return self->Wait(fd, event_mask, timeout_ns);
}

inline WaitAwaitable Wait(std::initializer_list<int> fds, uint32_t event_mask = POLLIN,
                          uint64_t timeout_ns = 0) {
  return self->Wait(fds, event_mask, timeout_ns);
}

inline WaitAwaitable Wait(const std::vector<int>& fds, uint32_t event_mask = POLLIN,
                          uint64_t timeout_ns = 0) {
  return self->Wait(fds, event_mask, timeout_ns);
}

inline WaitAwaitable Wait(const std::vector<WaitFd>& fds, uint64_t timeout_ns = 0) {
  return self->Wait(fds, timeout_ns);
}

// Poll without blocking.  See toolbelt::AsyncWaiter.
template <typename... Args>
inline int Poll(Args&&... args) {
  return self->Poll(std::forward<Args>(args)...);
}

inline WaitAllAwaitable WaitAll(const std::vector<WaitFd>& fds, uint64_t timeout_ns = 0) {
  return self->WaitAll(fds, timeout_ns);
}

inline SleepAwaitable Sleep(uint64_t nanoseconds) {
  return self->Sleep(nanoseconds);
}

template <typename Rep, typename Period>
inline SleepAwaitable Sleep(std::chrono::duration<Rep, Period> duration) {
  return self->Sleep(duration);
}

inline SleepAwaitable Nanosleep(uint64_t ns) {
  return self->Sleep(ns);
}

inline SleepAwaitable Millisleep(uint64_t ms) {
  return self->Sleep(ms * 1000000ULL);
}

} // namespace co20

#endif // CO20_HAVE_COROUTINES
