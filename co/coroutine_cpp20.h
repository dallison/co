// Copyright 2023-2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

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
#include <fcntl.h>
#include <functional>
#include <memory>
#include <string>
#include <sys/poll.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if CO_POLL_MODE == CO_POLL_EPOLL
#include <sys/epoll.h>
#endif

namespace co20 {

class Coroutine;
class Scheduler;

struct AbortException {};

struct BaseAwaitable;
struct YieldAwaitable;
struct WaitAwaitable;
struct SleepAwaitable;

// Represents a single C++20 stackless coroutine managed by a Scheduler.
class Coroutine {
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
  friend struct SleepAwaitable;
  friend struct Task;

  template<typename Func>
  Coroutine(Scheduler& scheduler, Func&& func, const std::string& name = "");

  ~Coroutine() {
    if (handle_) {
      handle_.destroy();
    }
  }

  Coroutine(const Coroutine&) = delete;
  Coroutine& operator=(const Coroutine&) = delete;
  Coroutine(Coroutine&&) = delete;
  Coroutine& operator=(Coroutine&&) = delete;

  YieldAwaitable Yield();
  WaitAwaitable Wait(int fd, uint32_t event_mask = POLLIN, uint64_t timeout_ns = 0);
  SleepAwaitable Sleep(uint64_t nanoseconds);

  template <typename Rep, typename Period>
  SleepAwaitable Sleep(std::chrono::duration<Rep, Period> duration);

  void Abort();

  bool IsAborted() const { return abort_pending_; }
  const std::string& Name() const { return name_; }
  State GetState() const { return state_; }
  Scheduler& GetScheduler() const { return scheduler_; }

private:
  void SetHandle(std::coroutine_handle<> handle) { handle_ = handle; }
  void SetState(State state) { state_ = state; }
  void SetWaitResult(int result) { wait_result_ = result; }
  int GetWaitResult() const { return wait_result_; }

  inline void Resume(int value = 0);

  std::coroutine_handle<> handle_;
  Scheduler& scheduler_;
  State state_ = State::kNew;
  std::string name_;
  bool abort_pending_ = false;
  int id_;
  int wait_result_ = -1;
};

// Manages a set of coroutines, scheduling them cooperatively using
// epoll (Linux) or poll as the I/O readiness backend.
class Scheduler {
public:
  Scheduler();
  ~Scheduler();

  void Run();
  void Stop() { running_ = false; }

  template<typename Func>
  Coroutine* Spawn(Func&& func, const std::string& name = "") {
    auto coroutine = std::make_unique<Coroutine>(*this, std::forward<Func>(func), name);
    Coroutine* ptr = coroutine.get();
    coroutines_.push_back(std::move(coroutine));
    ptr->SetState(Coroutine::State::kReady);
    ready_queue_.push_back(ptr);
    TriggerInterrupt();
    return ptr;
  }

  void ScheduleCoroutine(Coroutine* coroutine);
  void WaitForFd(Coroutine* coroutine, int fd, uint32_t event_mask, uint64_t timeout_ns);
  void SleepFor(Coroutine* coroutine, uint64_t nanoseconds);
  void ResumeCoroutine(Coroutine* coroutine, int value);
  int PollFd(int fd, uint32_t event_mask);
  int AllocateId() { return next_id_++; }

private:
  void ProcessReadyCoroutines();
  void ProcessEvents();
  void CleanupCoroutine(Coroutine* coroutine);
  void TriggerInterrupt();
#if CO_POLL_MODE == CO_POLL_EPOLL
  void DispatchEpollEvents(struct epoll_event* events, int count);
#endif

  bool running_ = false;
  std::vector<std::unique_ptr<Coroutine>> coroutines_;
  std::deque<Coroutine*> ready_queue_;
  std::unordered_map<int, std::vector<Coroutine*>> waiting_fds_;
  std::unordered_map<Coroutine*, int> coroutine_fds_;
  std::unordered_set<int> timerfds_;

#if CO_POLL_MODE == CO_POLL_EPOLL
  int epoll_fd_ = -1;
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

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) const {
    if (coroutine_) {
      coroutine_->SetHandle(handle);
    }
  }
};

struct YieldAwaitable : BaseAwaitable {
  using BaseAwaitable::BaseAwaitable;

  bool await_ready() const noexcept { return false; }

  void await_resume() const {
    if (coroutine_ && coroutine_->IsAborted()) {
      throw AbortException();
    }
  }

  void await_suspend(std::coroutine_handle<> handle) const {
    BaseAwaitable::await_suspend(handle);
    if (coroutine_ && scheduler_) {
      coroutine_->SetState(Coroutine::State::kYielded);
      scheduler_->ScheduleCoroutine(coroutine_);
    }
  }
};

struct WaitAwaitable : BaseAwaitable {
  int fd_;
  uint32_t event_mask_;
  uint64_t timeout_ns_;
  mutable int result_ = -1;

  WaitAwaitable(Coroutine* coroutine, Scheduler* scheduler, int fd,
                 uint32_t event_mask, uint64_t timeout_ns)
    : BaseAwaitable(coroutine, scheduler), fd_(fd),
      event_mask_(event_mask), timeout_ns_(timeout_ns) {}

  bool await_ready() const noexcept {
    if (coroutine_ && scheduler_) {
      int result = scheduler_->PollFd(fd_, event_mask_);
      if (result == fd_) {
        result_ = fd_;
        return true;
      }
    }
    return false;
  }

  void await_suspend(std::coroutine_handle<> handle) const {
    BaseAwaitable::await_suspend(handle);
    if (coroutine_ && scheduler_) {
      scheduler_->WaitForFd(coroutine_, fd_, event_mask_, timeout_ns_);
    }
  }

  int await_resume() const {
    if (coroutine_ && coroutine_->IsAborted()) {
      throw AbortException();
    }
    if (result_ != -1) {
      return result_;
    }
    if (coroutine_) {
      return coroutine_->GetWaitResult();
    }
    return -1;
  }
};

struct SleepAwaitable : BaseAwaitable {
  uint64_t timeout_ns_;

  SleepAwaitable(Coroutine* coroutine, Scheduler* scheduler, uint64_t timeout_ns)
    : BaseAwaitable(coroutine, scheduler), timeout_ns_(timeout_ns) {}

  bool await_ready() const noexcept {
    return timeout_ns_ == 0;
  }

  void await_suspend(std::coroutine_handle<> handle) const {
    BaseAwaitable::await_suspend(handle);
    if (coroutine_ && scheduler_) {
      scheduler_->SleepFor(coroutine_, timeout_ns_);
    }
  }

  void await_resume() const {
    if (coroutine_ && coroutine_->IsAborted()) {
      throw AbortException();
    }
  }
};

// --- Coroutine inline implementations (need full Scheduler definition) ---

template<typename Func>
inline Coroutine::Coroutine(Scheduler& scheduler, Func&& func, const std::string& name)
  : handle_(),
    scheduler_(scheduler),
    state_(State::kNew),
    name_(),
    abort_pending_(false),
    id_(scheduler.AllocateId()),
    wait_result_(-1) {
  name_ = name.empty() ? "co-" + std::to_string(id_) : name;

  auto task = [&]() {
    if constexpr (std::is_invocable_v<Func, Coroutine&>) {
      return func(*this);
    } else {
      return func();
    }
  }();
  if constexpr (requires { task.handle_; }) {
    handle_ = std::coroutine_handle<>(task.handle_);
    if constexpr (requires { task.handle_.promise().coroutine; }) {
      task.handle_.promise().coroutine = this;
    }
    task.handle_ = {};
  }
}

inline void Coroutine::Abort() {
  abort_pending_ = true;
  scheduler_.ResumeCoroutine(this, -1);
}

inline YieldAwaitable Coroutine::Yield() {
  return YieldAwaitable(this, &scheduler_);
}

inline WaitAwaitable Coroutine::Wait(int fd, uint32_t event_mask, uint64_t timeout_ns) {
  return WaitAwaitable(this, &scheduler_, fd, event_mask, timeout_ns);
}

inline SleepAwaitable Coroutine::Sleep(uint64_t nanoseconds) {
  return SleepAwaitable(this, &scheduler_, nanoseconds);
}

template <typename Rep, typename Period>
inline SleepAwaitable Coroutine::Sleep(std::chrono::duration<Rep, Period> duration) {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  return Sleep(static_cast<uint64_t>(ns));
}

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

    void unhandled_exception() {
      // Storing the exception prevents it from escaping handle_.resume(),
      // which would corrupt scheduler state. The coroutine body should
      // catch all expected exceptions (including AbortException).
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

// Thread-local pointers to the currently running coroutine and its scheduler.
// These are set automatically by the scheduler before each coroutine is resumed,
// enabling the free functions below. Each thread running a scheduler gets its
// own copy.
extern thread_local Coroutine* self;
extern thread_local Scheduler* scheduler;

// --- Coroutine::Resume (needs thread_local declarations above) ---

inline void Coroutine::Resume(int value) {
  if (handle_ && !handle_.done()) {
    wait_result_ = value;
    state_ = State::kRunning;
    co20::self = this;
    co20::scheduler = &scheduler_;
    handle_.resume();
    if (handle_.done()) {
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
