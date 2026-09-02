// Copyright 2023-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

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

#if defined(__APPLE__)
#define CO_POLL_MODE CO_POLL_POLL

#elif defined(__linux__)
#define CO_POLL_MODE CO_POLL_EPOLL // Change this line to disable epoll

#elif defined(__QNX__) || defined(__QNXNTO__)
#define CO_POLL_MODE CO_POLL_POLL

#else
#define CO_POLL_MODE CO_POLL_POLL

#endif

#include <poll.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include <utility>

#include "toolbelt/poller.h"

namespace co {

class CoroutineScheduler;
class Coroutine;
class ScheduledCoroutine;
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

constexpr size_t kCoDefaultStackSize = 64 * 1024;

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
using WaitFd = toolbelt::WaitFd;
#endif

// This is a Coroutine.  It executes its function (pointer to a function
// or a lambda).
//
// This abstract base class provides all the functions needed by
// any code running inside of a coroutine. To create and schedule
// a coroutine, refer to ScheduledCoroutine and CoroutineScheduler
// in coroutine_scheduler.h.
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
class Coroutine : public toolbelt::Poller {
public:

  // Start a coroutine running if it is not already running,
  virtual void Start() = 0;

  // Yield control to another coroutine.
  // virtual void Poller::Yield() const = 0;
  virtual void YieldToScheduler() const = 0;

  // Call another coroutine and store the result.
  template <typename T> T Call(Generator<T> &callee) const;

  // returns -1 for no fd ready, fd if one is ready.
  int Poll(const std::vector<int> &fds, short event_mask = POLLIN) const {
    std::vector<struct pollfd> pfds;
    pfds.reserve(fds.size() + 1);
    for (auto &fd : fds) {
      pfds.push_back({.fd = fd, .events = short(event_mask), .revents = 0});
    }
    return PollWithMutableFds(pfds);
  }
  int Poll(const std::vector<struct pollfd> &fds) const {
    std::vector<struct pollfd> pfds = fds;
    return PollWithMutableFds(pfds);
  }
  // Note that the interrupt Fd will be appended to this set of pollfds.
  int Poll(std::vector<struct pollfd> &fds) const {
    return PollWithMutableFds(fds);
  }

  // For all Wait functions, the timeout is optional and if greater than zero
  // specifies a nanosecond timeout.  If the timeout occurs before the fd (or
  // one of the fds) becomes ready, Wait will return -1. If an fd is ready, Wait
  // will return the fd that terminated the wait.

  // Wait for a file descriptor to become ready.  Returns the fd if it
  // was triggered or -1 for timeout.
  int Wait(int fd, uint32_t event_mask = POLLIN, uint64_t timeout_ns = 0) const {
    AddToUserWaitFds(fd, event_mask);
    return WaitOnUserWaitFds(timeout_ns);
  }

  // Wait for a set of fds, all with the same event mask.
  int Wait(const std::vector<int> &fds, uint32_t event_mask = POLLIN,
           uint64_t timeout_ns = 0) const {
    for (auto &fd : fds) {
      AddToUserWaitFds(fd, event_mask);
    }
    return WaitOnUserWaitFds(timeout_ns);
  }

  // Poll first and if the fd is not ready, wait for it.
  int PollAndWait(int fd, uint32_t event_mask = POLLIN,
                  uint64_t timeout_ns = 0) const {
    int n = Poll({fd}, event_mask);
    if (n != -1) {
      return n;
    }
    return Wait(fd, event_mask, timeout_ns);
  }

  // Wait for a set of fds, all with the same event mask.
  int PollAndWait(const std::vector<int> &fds, uint32_t event_mask = POLLIN,
                  uint64_t timeout_ns = 0) const {
    int n = Poll(fds, event_mask);
    if (n != -1) {
      return n;
    }
    return Wait(fds, event_mask, timeout_ns);
  }

#if CO_POLL_MODE == CO_POLL_EPOLL
  // Wait for a WaitFd.   Returns the fd if it was triggered or -1 for timeout.
  int Wait(WaitFd fd, uint64_t timeout_ns = 0) const {
    AddToUserWaitFds(fd.fd, fd.events);
    return WaitOnUserWaitFds(timeout_ns);
  }

  // Wait for a set of WaitFds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int Wait(const std::vector<WaitFd> &fds, uint64_t timeout_ns = 0) const {
    for (auto &fd : fds) {
      AddToUserWaitFds(fd.fd, fd.events);
    }
    return WaitOnUserWaitFds(timeout_ns);
  }

  int PollAndWait(WaitFd fd, uint64_t timeout_ns = 0) const {
    int n = Poll({(struct pollfd){.fd = fd.fd, .events = short(fd.events)}});
    if (n != -1) {
      return fd.fd;
    }
    return Wait(fd, timeout_ns);
  }

  int PollAndWait(const std::vector<WaitFd> &fds,
                  uint64_t timeout_ns = 0) const {
    std::vector<struct pollfd> pfds;
    pfds.reserve(fds.size());
    for (auto &fd : fds) {
      pfds.push_back({.fd = fd.fd, .events = short(fd.events), .revents = 0});
    }
    int n = Poll(pfds);
    if (n != -1) {
      return n;
    }
    for (auto &fd : fds) {
      AddToUserWaitFds(fd.fd, fd.events);
    }
    return WaitOnUserWaitFds(timeout_ns);
  }
#else
  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int Wait(struct pollfd fd, uint64_t timeout_ns = 0) const {
    AddToUserWaitFds(fd.fd, fd.events);
    return WaitOnUserWaitFds(timeout_ns);
  }

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int Wait(const std::vector<struct pollfd> &fds,
           uint64_t timeout_ns = 0) const {
    for (auto &fd : fds) {
      AddToUserWaitFds(fd.fd, fd.events);
    }
    return WaitOnUserWaitFds(timeout_ns);
  }

  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int PollAndWait(struct pollfd fd, uint64_t timeout_ns = 0) const {
    int n = Poll({fd});
    if (n != -1) {
      return fd.fd;
    }
    return Wait(fd, timeout_ns);
  }

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int PollAndWait(const std::vector<struct pollfd> &fds,
                  uint64_t timeout_ns = 0) const {
    int n = Poll(fds);
    if (n != -1) {
      return n;
    }
    return Wait(fds, timeout_ns);
  }
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
  virtual void Exit() const = 0;

  // Sleeping functions.
  // virtual void Poller::Nanosleep(uint64_t ns) const = 0;
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

  // Abort the coroutine.  It will cause the current wait or sleep to throw an
  // internal exception which will unwind the stack.  It will also set the
  // aborted flag which can be checked inside the coroutine function.
  virtual void Abort() const = 0;

  // Has this coroutine been aborted?
  virtual bool IsAborted() const = 0;

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
  virtual bool IsAlive() const = 0;

  uint64_t LastTick() const { return last_tick_; }
  CoroutineScheduler &Scheduler() const { return scheduler_; }

  virtual void Show() const = 0;

  // Each coroutine has a unique id.
  uint32_t Id() const { return id_; }

  void SetToStringCallback(std::function<std::string()> cb) {
    to_string_callback_ = std::move(cb);
  }

  // Make a string describing information about this coroutine.  By default
  // this will be the same as that printed by Show().
  virtual std::string ToString() const = 0;

  virtual void GetAllFds(std::vector<int> &fds) const = 0;

  int GetInterruptFd() const { return interrupt_fd_; }

  size_t GetStackSize() const { return stack_.size(); }

protected:
  Coroutine(CoroutineScheduler &sched,
                     CoroutineFunctionRef functor, std::string name,
                     int interrupt_fd, size_t stack_size,
                     void *user_data)
    : scheduler_(sched), id_(0), function_(std::move(functor)),
      name_(std::move(name)),
      interrupt_fd_(interrupt_fd), stack_(stack_size), user_data_(user_data) {}

  // virtual void Poller::AddToUserWaitFds(int fd, uint32_t event_mask) const = 0;
  // virtual int Poller::WaitOnUserWaitFds(uint64_t timeout_ns) const = 0;
  // virtual int Poller::PollWithMutableFds(std::vector<struct pollfd> &fds) const = 0;
  virtual void CallNonTemplate(ScheduledCoroutine &c) const = 0;

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

  CoroutineScheduler &scheduler_;
  uint32_t id_;                   // Coroutine ID.
  CoroutineFunctionRef function_; // Coroutine body.
  std::string name_;              // Optional name.
  int interrupt_fd_;
  mutable State state_ = State::kCoNew;
  std::vector<char> stack_;                 // Stack, allocated from malloc.
  void *user_data_;                // User data, not owned by this.
  mutable uint64_t last_tick_ = 0; // Tick count of last resume.
  // Function used to create a string for this coroutine.
  std::function<std::string()> to_string_callback_;
};

template <typename T> inline T Coroutine::Call(Generator<T> &callee) const {
  T result;
  // Tell the callee where to store the value.
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

inline void Yield() { self->Yield(); }

template <typename... Args>
auto Poll(Args&&... args) { return self->Poll(std::forward<Args>(args)...); }

template <typename... Args>
auto Wait(Args&&... args) { return self->Wait(std::forward<Args>(args)...); }

template <typename... Args>
auto PollAndWait(Args&&... args) { return self->PollAndWait(std::forward<Args>(args)...); }

// Note this can cause memory leaks as destructors in the coroutine function
// will not be called.  Use sparingly.  You should really use an interrupt fd
// to cause the function to exit cleanly, but this can get you out of stick
// situations if you need it.
inline void Exit() { return self->Exit(); }

// Sleeping functions.
inline void Nanosleep(uint64_t ns) { return self->Nanosleep(ns); }
inline void Millisleep(time_t msecs) {
  Nanosleep(static_cast<uint64_t>(msecs) * 1000000LL);
}
inline void Sleep(time_t secs) {
  Nanosleep(static_cast<uint64_t>(secs) * 1000000000LL);
}

template <class Rep, class Period>
void Sleep(std::chrono::duration<Rep, Period> duration) {
  return self->Sleep(duration);
}

} // namespace co
