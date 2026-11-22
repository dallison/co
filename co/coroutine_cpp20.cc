// Copyright 2023-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

// Copyright 2026 Cruise LLC.

#include "coroutine_cpp20.h"

#if CO20_HAVE_COROUTINES

#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#if CO_TIMER_MODE == CO_TIMER_TIMERFD
#include <sys/timerfd.h>
#endif
#if CO_TIMER_MODE == CO_TIMER_POSIX
#include <signal.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <chrono>
#include <limits>
#include <sched.h>
#include <time.h>
#include <unistd.h>

namespace co20 {

namespace {

const char* StateName(Coroutine::State state) {
  switch (state) {
    case Coroutine::State::kNew:
      return "new";
    case Coroutine::State::kReady:
      return "ready";
    case Coroutine::State::kRunning:
      return "running";
    case Coroutine::State::kYielded:
      return "yielded";
    case Coroutine::State::kWaiting:
      return "waiting";
    case Coroutine::State::kDead:
      return "dead";
  }
  return "unknown";
}

#if CO_POLL_MODE == CO_POLL_EPOLL
uint32_t PollToEpollEvents(uint32_t poll_events) {
  uint32_t events = 0;
  if (poll_events & POLLIN) events |= EPOLLIN | EPOLLRDHUP;
  if (poll_events & POLLOUT) events |= EPOLLOUT;
  return events;
}

uint32_t EpollToPollEvents(uint32_t epoll_events) {
  uint32_t events = 0;
  if (epoll_events & (EPOLLIN | EPOLLRDHUP)) events |= POLLIN;
  if (epoll_events & EPOLLOUT) events |= POLLOUT;
  if (epoll_events & EPOLLERR) events |= POLLERR;
  if (epoll_events & EPOLLHUP) events |= POLLHUP;
  return events;
}
#endif

}  // namespace

int PollFds(const WaitFd* fds, size_t num_fds, std::vector<WaitFd>* ready) {
  if (num_fds == 0) {
    return -1;
  }
  absl::InlinedVector<struct pollfd, 4> pfds;
  pfds.reserve(num_fds);
  for (size_t i = 0; i < num_fds; i++) {
    struct pollfd pfd = {};
    pfd.fd = fds[i].fd;
    pfd.events = static_cast<short>(fds[i].events);
    pfds.push_back(pfd);
  }
  if (poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 0) <= 0) {
    return -1;
  }
  int first = -1;
  for (size_t i = 0; i < num_fds; i++) {
    const uint32_t revents =
        static_cast<uint32_t>(static_cast<unsigned short>(pfds[i].revents));
    // Errors and hangups make an fd ready whatever was asked for, so that the
    // caller can see them when it reads or writes.  An invalid fd isn't ready.
    const uint32_t wanted = fds[i].events | POLLERR | POLLHUP;
    if ((revents & wanted) == 0) {
      continue;
    }
    if (first == -1) {
      first = static_cast<int>(i);
    }
    if (ready == nullptr) {
      break;
    }
    ready->push_back({fds[i].fd, revents});
  }
  return first;
}

std::string Coroutine::ToString() const {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "Coroutine %d: %s: state: %s", id_, name_.c_str(),
           StateName(state_));
  return buffer;
}

void Coroutine::Show() const { fprintf(stderr, "%s\n", ToString().c_str()); }

// --- toolbelt::AsyncWaiter implementation ---
//
// A stackless coroutine can only suspend at a co_await, so these can't give
// other coroutines a chance to run.  They are the blocking equivalents.

void Coroutine::Yield() const { sched_yield(); }

void Coroutine::Nanosleep(uint64_t ns) const {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(
      std::min<uint64_t>(ns / 1000000000ULL, std::numeric_limits<time_t>::max()));
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  // nanosleep fills in the time remaining if it is interrupted by a signal.
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

int Coroutine::PollWithMutableFds(std::vector<struct pollfd>& fds) const {
  if (interrupt_fd_ != -1) {
    fds.push_back({.fd = interrupt_fd_, .events = POLLIN, .revents = 0});
  }
  if (fds.empty()) {
    return -1;
  }
  int ret;
  do {
    ret = poll(fds.data(), static_cast<nfds_t>(fds.size()), 0);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    return -2;
  }
  if (ret == 0) {
    return -1;
  }
  for (const struct pollfd& pfd : fds) {
    // As in PollFds, errors and hangups make an fd ready.
    if (pfd.revents & (pfd.events | POLLERR | POLLHUP)) {
      return pfd.fd;
    }
  }
  return -1;
}

void Coroutine::AddToUserWaitFds(int fd, uint32_t event_mask) const {
  user_wait_fds_.push_back(
      {.fd = fd, .events = static_cast<short>(event_mask), .revents = 0});
}

int Coroutine::WaitOnUserWaitFds(uint64_t timeout_ns) const {
  // Take the fds so that they are never left over for the next wait, however
  // this one ends.
  std::vector<struct pollfd> fds = std::move(user_wait_fds_);
  if (interrupt_fd_ != -1) {
    fds.push_back({.fd = interrupt_fd_, .events = POLLIN, .revents = 0});
  }
  if (fds.empty()) {
    return -1;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(std::min<uint64_t>(
                            timeout_ns, std::numeric_limits<int64_t>::max() / 2));
  for (;;) {
    int timeout_ms = -1;
    if (timeout_ns != 0) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::nanoseconds::zero()) {
        return -1;
      }
      // Round up so that we don't spin for the last fraction of a millisecond.
      const int64_t ms =
          std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
      timeout_ms = static_cast<int>(
          std::min<int64_t>(ms, std::numeric_limits<int>::max()));
    }
    for (struct pollfd& pfd : fds) {
      pfd.revents = 0;
    }
    int ret = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -2;
    }
    if (ret == 0) {
      continue;  // Timed out; the loop decides whether the deadline has passed.
    }
    for (const struct pollfd& pfd : fds) {
      if (pfd.revents & POLLNVAL) {
        return -2;
      }
      if (pfd.revents & (pfd.events | POLLERR | POLLHUP)) {
        return pfd.fd;
      }
    }
  }
}

uint32_t Coroutine::WaitMaskFor(int fd) const {
  if (fd == interrupt_fd_ || fd == timeout_fd_) {
    return POLLIN;
  }
  uint32_t mask = 0;
  for (const WaitFd& wait_fd : wait_fds_) {
    if (wait_fd.fd == fd) {
      mask |= wait_fd.events;
    }
  }
  return mask;
}

Scheduler::Scheduler() {
#if CO_POLL_MODE == CO_POLL_EPOLL
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
#endif

#if defined(__linux__)
  interrupt_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
  int pipes[2];
  if (pipe(pipes) == 0) {
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    fcntl(pipes[1], F_SETFL, O_NONBLOCK);
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    interrupt_fd_ = pipes[0];
    interrupt_write_fd_ = pipes[1];
  }
#endif

#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ != -1 && interrupt_fd_ != -1) {
    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = interrupt_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, interrupt_fd_, &ev);
  }
#endif
}

Scheduler::~Scheduler() {
  // Destroy the coroutines first, while the wait tables they are in are still
  // valid.  Destroying a suspended coroutine runs the destructors of its
  // locals.
  while (!coroutines_.empty()) {
    auto it = coroutines_.begin();
    DetachWaits(it->first);
    std::unique_ptr<Coroutine> coroutine = std::move(it->second);
    coroutines_.erase(it);
  }
  ready_queue_.clear();

  for (int fd : std::vector<int>(timerfds_.begin(), timerfds_.end())) {
    CloseTimer(fd);
  }

  if (interrupt_fd_ != -1) {
    close(interrupt_fd_);
  }
#if !defined(__linux__)
  if (interrupt_write_fd_ != -1) {
    close(interrupt_write_fd_);
  }
#endif
#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ != -1) {
    close(epoll_fd_);
  }
#endif
}

void Scheduler::TriggerInterrupt() {
#if defined(__linux__)
  if (interrupt_fd_ != -1) {
    uint64_t val = 1;
    (void)write(interrupt_fd_, &val, sizeof(val));
  }
#else
  if (interrupt_write_fd_ != -1) {
    char val = 1;
    (void)write(interrupt_write_fd_, &val, 1);
  }
#endif
}

void Scheduler::MakeReady(Coroutine* coroutine, int value) {
  coroutine->SetWaitResult(value);
  if (coroutine->GetState() == Coroutine::State::kReady) {
    // Already in the ready queue.
    return;
  }
  coroutine->SetState(Coroutine::State::kReady);
  ready_queue_.push_back(coroutine);
  TriggerInterrupt();
}

void Scheduler::ScheduleCoroutine(Coroutine* coroutine) {
  if (!coroutine) return;
  if (coroutine->GetState() == Coroutine::State::kYielded) {
    // Stamp wait age before re-queuing so a Yield competes fairly with
    // Wait/Sleep by how long each has been parked (stackful LastTick rule).
    coroutine->SetLastTick(tick_count_);
    MakeReady(coroutine, 0);
  }
}

int Scheduler::PollFd(int fd, uint32_t event_mask) {
  WaitFd wait_fd{fd, event_mask};
  return PollFds(&wait_fd, 1) == 0 ? fd : -1;
}

bool Scheduler::UpdateEpoll(int fd) {
#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ == -1) {
    return true;
  }
  uint32_t wanted = 0;
  auto waiters = waiting_fds_.find(fd);
  if (waiters != waiting_fds_.end()) {
    for (Coroutine* c : waiters->second) {
      wanted |= c->WaitMaskFor(fd);
    }
  }
  const uint32_t events = PollToEpollEvents(wanted);
  auto registered = epoll_events_.find(fd);
  if (events == 0) {
    if (registered != epoll_events_.end()) {
      epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
      epoll_events_.erase(registered);
    }
    return true;
  }
  if (registered != epoll_events_.end() && registered->second == events) {
    return true;
  }
  struct epoll_event ev = {};
  ev.events = events;
  ev.data.fd = fd;
  const int op = registered == epoll_events_.end() ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(epoll_fd_, op, fd, &ev) == -1) {
    // EEXIST: registered outside this table.  ENOENT: the kernel dropped the
    // registration when the fd was closed and the number was reused.
    const int retry_op = op == EPOLL_CTL_ADD ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    const bool retry = (op == EPOLL_CTL_ADD && errno == EEXIST) ||
                       (op == EPOLL_CTL_MOD && errno == ENOENT);
    if (!retry || epoll_ctl(epoll_fd_, retry_op, fd, &ev) == -1) {
      if (registered != epoll_events_.end()) {
        epoll_events_.erase(registered);
      }
      return false;
    }
  }
  epoll_events_[fd] = events;
#else
  (void)fd;
#endif
  return true;
}

bool Scheduler::AddWaiter(Coroutine* coroutine, int fd) {
  auto& waiters = waiting_fds_[fd];
  if (std::find(waiters.begin(), waiters.end(), coroutine) == waiters.end()) {
    waiters.push_back(coroutine);
  }
  if (UpdateEpoll(fd)) {
    return true;
  }
  const int saved_errno = errno;
  RemoveWaiter(coroutine, fd);
  errno = saved_errno;
  return false;
}

void Scheduler::RemoveWaiter(Coroutine* coroutine, int fd) {
  auto it = waiting_fds_.find(fd);
  if (it == waiting_fds_.end()) {
    return;
  }
  auto& waiters = it->second;
  waiters.erase(std::remove(waiters.begin(), waiters.end(), coroutine), waiters.end());
  if (waiters.empty()) {
    waiting_fds_.erase(it);
  }
  UpdateEpoll(fd);
}

void Scheduler::DetachWaits(Coroutine* coroutine) {
  // Clear the coroutine's wait fds before removing it so that the epoll
  // registrations are recomputed without them.
  WaitFdList fds = std::move(coroutine->wait_fds_);
  coroutine->wait_fds_.clear();
  for (const WaitFd& wait_fd : fds) {
    RemoveWaiter(coroutine, wait_fd.fd);
  }
  if (coroutine->interrupt_fd_ != -1) {
    RemoveWaiter(coroutine, coroutine->interrupt_fd_);
  }
  const int timeout_fd = coroutine->timeout_fd_;
  if (timeout_fd != -1) {
    coroutine->timeout_fd_ = -1;
    RemoveWaiter(coroutine, timeout_fd);
    CloseTimer(timeout_fd);
  }
}

void Scheduler::WaitForFds(Coroutine* coroutine, const WaitFd* fds, size_t num_fds,
                           uint64_t timeout_ns) {
  if (!coroutine) return;
  DetachWaits(coroutine);
  coroutine->SetLastTick(tick_count_);
  coroutine->SetState(Coroutine::State::kWaiting);

  coroutine->wait_fds_.assign(fds, fds + num_fds);
  for (size_t i = 0; i < num_fds; i++) {
    if (!AddWaiter(coroutine, fds[i].fd)) {
      // epoll refuses regular files (EPERM), which are always ready, and
      // closed fds (EBADF), which never will be.
      const int fd = fds[i].fd;
      const int result = errno == EPERM ? fd : -1;
      DetachWaits(coroutine);
      MakeReady(coroutine, result);
      return;
    }
  }

  if (coroutine->interrupt_fd_ != -1) {
    AddWaiter(coroutine, coroutine->interrupt_fd_);
  }

  if (timeout_ns > 0) {
    const int tfd = CreateTimer(timeout_ns);
    if (tfd == -1) {
      // Without a timer the wait would never time out.  Report the timeout
      // now instead.
      DetachWaits(coroutine);
      MakeReady(coroutine, -1);
      return;
    }
    coroutine->timeout_fd_ = tfd;
    AddWaiter(coroutine, tfd);
  }
}

void Scheduler::SleepFor(Coroutine* coroutine, uint64_t nanoseconds) {
  if (!coroutine) return;
  // Sleep(0) is Yield: re-queue with no timer.  A non-zero sleep is needed
  // to actually park on the event loop.
  if (nanoseconds == 0) {
    coroutine->SetState(Coroutine::State::kYielded);
    ScheduleCoroutine(coroutine);
    return;
  }
  // A sleep is a wait for nothing but the timeout.
  WaitForFds(coroutine, nullptr, 0, nanoseconds);
}

int Scheduler::CreateTimer(uint64_t nanoseconds) {
#if CO_TIMER_MODE == CO_TIMER_TIMERFD || CO_TIMER_MODE == CO_TIMER_POSIX
  // macOS has no itimerspec, and the kqueue timer doesn't need one.
  struct itimerspec its = {};
  its.it_value.tv_sec = static_cast<time_t>(nanoseconds / 1000000000ULL);
  its.it_value.tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);
#endif

#if CO_TIMER_MODE == CO_TIMER_TIMERFD
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tfd == -1) {
    return -1;
  }
  if (timerfd_settime(tfd, 0, &its, nullptr) == -1) {
    close(tfd);
    return -1;
  }
  timerfds_.insert(tfd);
  return tfd;

#elif CO_TIMER_MODE == CO_TIMER_EVENT
  int kq = kqueue();
  if (kq == -1) {
    return -1;
  }
  fcntl(kq, F_SETFD, FD_CLOEXEC);
  struct kevent kev;
  EV_SET(&kev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS,
         static_cast<intptr_t>(nanoseconds), 0);
  if (kevent(kq, &kev, 1, nullptr, 0, nullptr) == -1) {
    close(kq);
    return -1;
  }
  timerfds_.insert(kq);
  return kq;

#elif CO_TIMER_MODE == CO_TIMER_POSIX
  int pipe_fds[2];
  if (pipe(pipe_fds) == -1) {
    return -1;
  }
  int read_fd = pipe_fds[0];
  int write_fd = pipe_fds[1];
  for (int fd : {read_fd, write_fd}) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }

  struct sigevent se = {};
  se.sigev_notify = SIGEV_THREAD;
  se.sigev_notify_attributes = nullptr;
  se.sigev_value.sival_int = write_fd;
  se.sigev_notify_function = [](union sigval sv) {
    char c = 1;
    (void)write(sv.sival_int, &c, 1);
  };

  timer_t timer_id;
  if (timer_create(CLOCK_REALTIME, &se, &timer_id) == -1) {
    close(read_fd);
    close(write_fd);
    return -1;
  }
  if (timer_settime(timer_id, 0, &its, nullptr) == -1) {
    timer_delete(timer_id);
    close(read_fd);
    close(write_fd);
    return -1;
  }
  timerfds_.insert(read_fd);
  posix_timers_[read_fd] = {timer_id, write_fd};
  return read_fd;
#endif
}

void Scheduler::CloseTimer(int fd) {
  if (timerfds_.erase(fd) == 0) {
    return;
  }
#if CO_TIMER_MODE == CO_TIMER_POSIX
  auto pt = posix_timers_.find(fd);
  if (pt != posix_timers_.end()) {
    struct itimerspec disarm = {};
    timer_settime(pt->second.timer_id, 0, &disarm, nullptr);
    timer_delete(pt->second.timer_id);
    close(pt->second.write_fd);
    posix_timers_.erase(pt);
  }
#endif
  close(fd);
}

void Scheduler::ResumeCoroutine(Coroutine* coroutine, int value) {
  if (!coroutine || coroutine->GetState() != Coroutine::State::kWaiting) return;
  DetachWaits(coroutine);
  MakeReady(coroutine, value);
}

void Scheduler::DispatchFdEvents(int fd, uint32_t revents) {
  auto it = waiting_fds_.find(fd);
  if (it == waiting_fds_.end()) {
    return;
  }
  // Waking a coroutine removes it from the list, so iterate over a copy.
  const std::vector<Coroutine*> waiters = it->second;
  for (Coroutine* c : waiters) {
    // Errors and hangups wake every waiter.  Anything else wakes only the
    // waiters that asked for one of the events that is ready.
    if ((revents & (c->WaitMaskFor(fd) | POLLHUP | POLLERR)) == 0) {
      continue;
    }
    // A timeout makes the wait return -1.
    ResumeCoroutine(c, fd == c->timeout_fd_ ? -1 : fd);
  }
}

void Scheduler::FinishCoroutine(Coroutine* coroutine) {
  coroutine->SetState(Coroutine::State::kDead);
  DetachWaits(coroutine);
  ready_queue_.erase(std::remove(ready_queue_.begin(), ready_queue_.end(), coroutine),
                     ready_queue_.end());
  if (completion_callback_) {
    completion_callback_(coroutine);
  }
  std::exception_ptr exception = std::move(coroutine->exception_);
  auto it = coroutines_.find(coroutine);
  if (it != coroutines_.end()) {
    // Move it out before destroying it so that the coroutine's destructors
    // can spawn coroutines.
    std::unique_ptr<Coroutine> owned = std::move(it->second);
    coroutines_.erase(it);
  }
  if (exception) {
    std::rethrow_exception(exception);
  }
}

void Scheduler::ProcessReadyCoroutines() {
  // Prefer coroutines that have been parked longest (lowest LastTick), the
  // same rule as the stackful scheduler's sort of the poll wake batch.
  std::stable_sort(ready_queue_.begin(), ready_queue_.end(),
                   [](const Coroutine* a, const Coroutine* b) {
                     return a->LastTick() < b->LastTick();
                   });

  // One fair pass only.  Yield puts the coroutine back on ready_queue_, so
  // looping until empty would never return when anything is yielding.
  size_t to_run = ready_queue_.size();
  while (to_run > 0 && !ready_queue_.empty() && !stop_requested_) {
    --to_run;
    Coroutine* coroutine = ready_queue_.front();
    ready_queue_.pop_front();

    if (!coroutine || coroutine->GetState() != Coroutine::State::kReady) continue;

    coroutine->Resume(coroutine->GetWaitResult());

    // Without this, a Yield-only coroutine keeps ready_queue_ non-empty and
    // Run() never reaches the blocking ProcessEvents() call — so every
    // Wait/Sleep on the poll() backend (macOS) would hang.  epoll already
    // did a non-blocking check here; poll() needs the same.
    PollWaitingFds(/*timeout_ms=*/0);

    // A coroutine that is still running suspended in an awaitable that isn't
    // ours; nothing will resume it.
    if (coroutine->root_.done() || coroutine->GetState() == Coroutine::State::kDead ||
        coroutine->GetState() == Coroutine::State::kRunning) {
      FinishCoroutine(coroutine);
    }
  }
}

void Scheduler::PollWaitingFds(int timeout_ms) {
  if (waiting_fds_.empty()) return;

#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ != -1) {
    struct epoll_event events[64];
    int n = epoll_wait(epoll_fd_, events, 64, timeout_ms);
    for (int i = 0; i < n; i++) {
      if (events[i].data.fd == interrupt_fd_) {
        uint64_t val;
        (void)read(interrupt_fd_, &val, sizeof(val));
        continue;
      }
      DispatchFdEvents(events[i].data.fd, EpollToPollEvents(events[i].events));
    }
    return;
  }
#endif

  // poll() fallback.  Poll only for what the waiters asked for.  A socketpair
  // is always writable, so polling for POLLOUT on behalf of a POLLIN waiter
  // would report it ready on every pass.
  std::vector<struct pollfd> pfds;
  for (auto& [fd, waiters] : waiting_fds_) {
    uint32_t mask = 0;
    for (Coroutine* c : waiters) {
      mask |= c->WaitMaskFor(fd);
    }
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = static_cast<short>(mask);
    pfds.push_back(pfd);
  }

  struct pollfd interrupt_pfd = {};
  interrupt_pfd.fd = interrupt_fd_;
  interrupt_pfd.events = POLLIN;
  pfds.push_back(interrupt_pfd);

  if (pfds.size() > static_cast<size_t>(std::numeric_limits<nfds_t>::max())) {
    return;
  }
  int ret = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
  if (ret <= 0) return;

  if (pfds.back().revents & POLLIN) {
#if defined(__linux__)
    uint64_t val;
    (void)read(interrupt_fd_, &val, sizeof(val));
#else
    char val;
    (void)read(interrupt_fd_, &val, 1);
#endif
  }
  pfds.pop_back();

  for (const struct pollfd& pfd : pfds) {
    if (pfd.revents != 0) {
      DispatchFdEvents(pfd.fd,
                       static_cast<uint32_t>(static_cast<unsigned short>(pfd.revents)));
    }
  }
}

void Scheduler::WaitOnInterrupt(int timeout_ms) {
  if (interrupt_fd_ == -1) {
    if (timeout_ms > 0) {
      struct timespec ts = {};
      ts.tv_sec = timeout_ms / 1000;
      ts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);
      nanosleep(&ts, nullptr);
    }
    return;
  }

#if CO_POLL_MODE == CO_POLL_EPOLL
  // The interrupt fd is registered on the epoll set at construction.  A
  // timed epoll_wait still wakes on Stop()/TriggerInterrupt.
  if (epoll_fd_ != -1) {
    struct epoll_event events[8];
    int n = epoll_wait(epoll_fd_, events, 8, timeout_ms);
    for (int i = 0; i < n; i++) {
      if (events[i].data.fd == interrupt_fd_) {
        uint64_t val;
        (void)read(interrupt_fd_, &val, sizeof(val));
        continue;
      }
      DispatchFdEvents(events[i].data.fd, EpollToPollEvents(events[i].events));
    }
    return;
  }
#endif

  struct pollfd pfd = {};
  pfd.fd = interrupt_fd_;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
#if defined(__linux__)
    uint64_t val;
    (void)read(interrupt_fd_, &val, sizeof(val));
#else
    char val;
    (void)read(interrupt_fd_, &val, 1);
#endif
  }
}

void Scheduler::ProcessEvents() {
  PollWaitingFds(/*timeout_ms=*/-1);
}

void Scheduler::Run() {
  running_ = true;
  struct ResetOnExit {
    Scheduler* s;
    ~ResetOnExit() {
      s->running_ = false;
      s->stop_requested_ = false;
    }
  } reset{this};

  while (!stop_requested_) {
    ProcessReadyCoroutines();
    if (stop_requested_) break;
    if (ready_queue_.empty() && waiting_fds_.empty()) break;
    if (!ready_queue_.empty()) {
      // Never block forever while anyone is ready: a Yield-ing producer must
      // still run to feed waiters.  Park briefly so a Yield-only loop cannot
      // pin the CPU; wake immediately on interrupt or (if waiters exist) I/O.
      if (waiting_fds_.empty()) {
        WaitOnInterrupt(/*timeout_ms=*/1);
      } else {
        PollWaitingFds(/*timeout_ms=*/1);
      }
      continue;
    }
    ProcessEvents();
  }
}

void Scheduler::Show() const {
  for (const std::string& s : AllCoroutineStrings()) {
    fprintf(stderr, "%s\n", s.c_str());
  }
}

std::vector<std::string> Scheduler::AllCoroutineStrings() const {
  std::vector<const Coroutine*> sorted;
  sorted.reserve(coroutines_.size());
  for (const auto& [ptr, owned] : coroutines_) {
    sorted.push_back(ptr);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const Coroutine* a, const Coroutine* b) { return a->Id() < b->Id(); });
  std::vector<std::string> strings;
  strings.reserve(sorted.size());
  for (const Coroutine* c : sorted) {
    strings.push_back(c->ToString());
  }
  return strings;
}

std::vector<int> Scheduler::GetAllFds() const {
  std::vector<int> fds;
#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ != -1) fds.push_back(epoll_fd_);
#endif
  if (interrupt_fd_ != -1) fds.push_back(interrupt_fd_);
#if !defined(__linux__)
  if (interrupt_write_fd_ != -1) fds.push_back(interrupt_write_fd_);
#endif
#if CO_TIMER_MODE == CO_TIMER_POSIX
  for (const auto& [fd, info] : posix_timers_) {
    fds.push_back(info.write_fd);
  }
#endif
  for (const auto& [fd, waiters] : waiting_fds_) {
    fds.push_back(fd);
  }
  for (int fd : timerfds_) {
    if (waiting_fds_.find(fd) == waiting_fds_.end()) fds.push_back(fd);
  }
  for (const auto& [ptr, owned] : coroutines_) {
    if (ptr->interrupt_fd_ != -1 && waiting_fds_.find(ptr->interrupt_fd_) == waiting_fds_.end()) {
      fds.push_back(ptr->interrupt_fd_);
    }
  }
  return fds;
}

thread_local Coroutine* self = nullptr;
thread_local Scheduler* scheduler = nullptr;

} // namespace co20

#endif // CO20_HAVE_COROUTINES
