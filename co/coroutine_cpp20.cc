// Copyright 2023-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "coroutine_cpp20.h"

#if CO20_HAVE_COROUTINES

#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#endif
#include <algorithm>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace co20 {

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
    interrupt_fd_ = pipes[0];
    interrupt_write_fd_ = pipes[1];
  }
#endif

#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ != -1 && interrupt_fd_ != -1) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = interrupt_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, interrupt_fd_, &ev);
  }
#endif
}

Scheduler::~Scheduler() {
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

void Scheduler::ScheduleCoroutine(Coroutine* coroutine) {
  if (!coroutine) return;
  auto s = coroutine->GetState();
  if (s == Coroutine::State::kYielded || s == Coroutine::State::kReady) {
    coroutine->SetState(Coroutine::State::kReady);
    ready_queue_.push_back(coroutine);
    TriggerInterrupt();
  }
}

int Scheduler::PollFd(int fd, uint32_t event_mask) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = event_mask;
  pfd.revents = 0;

  int ret = poll(&pfd, 1, 0);
  if (ret <= 0) return -1;

  if (event_mask & POLLIN) {
    if (pfd.revents & (POLLIN | POLLERR)) return fd;
  } else {
    if ((pfd.revents & event_mask) || (pfd.revents & POLLERR)) return fd;
  }
  return -1;
}

// Remove a coroutine from all waiting data structures and clean up
// any scheduler-owned FDs (timerfds). Called when a coroutine finishes.
void Scheduler::CleanupCoroutine(Coroutine* coroutine) {
  auto fd_it = coroutine_fds_.find(coroutine);
  if (fd_it == coroutine_fds_.end()) return;

  int fd = fd_it->second;
  auto waiting_it = waiting_fds_.find(fd);
  if (waiting_it != waiting_fds_.end()) {
    auto& list = waiting_it->second;
    list.erase(std::remove(list.begin(), list.end(), coroutine), list.end());
    if (list.empty()) {
      waiting_fds_.erase(waiting_it);
#if CO_POLL_MODE == CO_POLL_EPOLL
      if (epoll_fd_ != -1) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
      }
#endif
    }
  }
  coroutine_fds_.erase(fd_it);
}

void Scheduler::WaitForFd(Coroutine* coroutine, int fd, uint32_t event_mask,
                           uint64_t /*timeout_ns*/) {
  if (!coroutine) return;

  // If already ready, schedule immediately.
  if (PollFd(fd, event_mask) == fd) {
    coroutine->SetWaitResult(fd);
    coroutine->SetState(Coroutine::State::kReady);
    ready_queue_.push_back(coroutine);
    TriggerInterrupt();
    return;
  }

  coroutine->SetState(Coroutine::State::kWaiting);

  // If this coroutine was already waiting on a different FD, detach it first.
  auto existing_it = coroutine_fds_.find(coroutine);
  if (existing_it != coroutine_fds_.end()) {
    int old_fd = existing_it->second;
    if (old_fd != fd) {
      auto old_fd_it = waiting_fds_.find(old_fd);
      if (old_fd_it != waiting_fds_.end()) {
        auto& old_list = old_fd_it->second;
        old_list.erase(std::remove(old_list.begin(), old_list.end(), coroutine),
                       old_list.end());
        if (old_list.empty()) {
          waiting_fds_.erase(old_fd_it);
#if CO_POLL_MODE == CO_POLL_EPOLL
          if (epoll_fd_ != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, old_fd, nullptr);
          }
#endif
        }
      }
      existing_it->second = fd;
    }
    // else: same FD, fall through to add to waiting list
  } else {
    coroutine_fds_[coroutine] = fd;
  }

  // Add coroutine to waiting list for this FD.
  bool fd_already_tracked = waiting_fds_.count(fd) > 0;
  auto& wait_list = waiting_fds_[fd];
  if (std::find(wait_list.begin(), wait_list.end(), coroutine) == wait_list.end()) {
    wait_list.push_back(coroutine);
  }

#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ != -1) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = 0;
    if (event_mask & POLLIN) ev.events |= EPOLLIN | EPOLLRDHUP;
    if (event_mask & POLLOUT) ev.events |= EPOLLOUT;

    if (fd_already_tracked) {
      epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    } else {
      int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
      if (ret == -1 && errno == EEXIST) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
      } else if (ret == -1 && errno == EBADF) {
        // FD is invalid; schedule the coroutine with an error result.
        wait_list.erase(std::remove(wait_list.begin(), wait_list.end(), coroutine),
                        wait_list.end());
        if (wait_list.empty()) waiting_fds_.erase(fd);
        coroutine_fds_.erase(coroutine);
        coroutine->SetWaitResult(-1);
        coroutine->SetState(Coroutine::State::kReady);
        ready_queue_.push_back(coroutine);
        TriggerInterrupt();
      }
    }
  }
#endif
}

void Scheduler::SleepFor(Coroutine* coroutine, uint64_t nanoseconds) {
  if (!coroutine || nanoseconds == 0) {
    ScheduleCoroutine(coroutine);
    return;
  }

#if defined(__linux__)
  int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd == -1) {
    ScheduleCoroutine(coroutine);
    return;
  }

  timerfds_.insert(timer_fd);

  struct itimerspec its = {};
  its.it_value.tv_sec = nanoseconds / 1000000000ULL;
  its.it_value.tv_nsec = nanoseconds % 1000000000ULL;
  timerfd_settime(timer_fd, 0, &its, nullptr);

  WaitForFd(coroutine, timer_fd, POLLIN, 0);
#else
  // Non-Linux: no timerfd available. Schedule immediately as a fallback.
  // TODO: implement proper timer using kqueue/POSIX timers.
  ScheduleCoroutine(coroutine);
#endif
}

void Scheduler::ResumeCoroutine(Coroutine* coroutine, int value) {
  if (!coroutine || coroutine->handle_.done()) return;

  // Detach from waiting state.
  auto it = coroutine_fds_.find(coroutine);
  if (it != coroutine_fds_.end()) {
    int fd = it->second;

    auto fd_it = waiting_fds_.find(fd);
    if (fd_it != waiting_fds_.end()) {
      auto& list = fd_it->second;
      list.erase(std::remove(list.begin(), list.end(), coroutine), list.end());
    }

    // Clean up scheduler-owned timerfds.
#if defined(__linux__)
    if (fd != interrupt_fd_ && timerfds_.count(fd) > 0) {
      uint64_t val;
      (void)read(fd, &val, sizeof(val));
      close(fd);
#if CO_POLL_MODE == CO_POLL_EPOLL
      if (epoll_fd_ != -1) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
      }
#endif
      if (fd_it != waiting_fds_.end() && fd_it->second.empty()) {
        waiting_fds_.erase(fd_it);
      }
      coroutine_fds_.erase(it);
      timerfds_.erase(fd);
    }
#endif
  }

  coroutine->SetWaitResult(value);
  coroutine->SetState(Coroutine::State::kReady);
  ready_queue_.push_back(coroutine);
  TriggerInterrupt();
}

#if CO_POLL_MODE == CO_POLL_EPOLL
void Scheduler::DispatchEpollEvents(struct epoll_event* events, int count) {
  for (int i = 0; i < count; i++) {
    int fd = events[i].data.fd;
    if (fd == interrupt_fd_) {
      uint64_t val;
      (void)read(interrupt_fd_, &val, sizeof(val));
      continue;
    }
    auto it = waiting_fds_.find(fd);
    if (it != waiting_fds_.end()) {
      // Copy because ResumeCoroutine mutates the list.
      std::vector<Coroutine*> to_resume = it->second;
      for (Coroutine* c : to_resume) {
        ResumeCoroutine(c, fd);
      }
    }
  }
}
#endif

void Scheduler::ProcessReadyCoroutines() {
  while (!ready_queue_.empty()) {
    Coroutine* coroutine = ready_queue_.front();
    ready_queue_.pop_front();

    if (!coroutine) continue;
    if (coroutine->handle_.done()) {
      coroutine->SetState(Coroutine::State::kDead);
      continue;
    }
    if (coroutine->GetState() != Coroutine::State::kReady) continue;

    coroutine->Resume(coroutine->GetWaitResult());

    // After resuming, poll for newly-ready FDs so we don't miss
    // events caused by this coroutine's actions (e.g. writing to a pipe).
#if CO_POLL_MODE == CO_POLL_EPOLL
    if (epoll_fd_ != -1 && !waiting_fds_.empty()) {
      struct epoll_event events[64];
      int n = epoll_wait(epoll_fd_, events, 64, 0);
      if (n > 0) DispatchEpollEvents(events, n);
    }
#endif

    if (coroutine->handle_.done() ||
        coroutine->GetState() == Coroutine::State::kRunning) {
      coroutine->SetState(Coroutine::State::kDead);
      CleanupCoroutine(coroutine);
    }
  }
}

void Scheduler::ProcessEvents() {
  if (waiting_fds_.empty()) return;

#if CO_POLL_MODE == CO_POLL_EPOLL
  if (epoll_fd_ != -1) {
    struct epoll_event events[64];
    // Non-blocking check first.
    int n = epoll_wait(epoll_fd_, events, 64, 0);
    if (n <= 0) {
      if (waiting_fds_.empty()) return;
      n = epoll_wait(epoll_fd_, events, 64, -1);
      if (n <= 0) return;
    }
    DispatchEpollEvents(events, n);
    return;
  }
#endif

  // poll() fallback
  std::vector<struct pollfd> pfds;
  std::vector<std::vector<Coroutine*>> coroutines_per_fd;

  for (auto& [fd, coroutine_list] : waiting_fds_) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    pfds.push_back(pfd);
    coroutines_per_fd.push_back(coroutine_list);
  }

  struct pollfd interrupt_pfd;
  interrupt_pfd.fd = interrupt_fd_;
  interrupt_pfd.events = POLLIN;
  interrupt_pfd.revents = 0;
  pfds.push_back(interrupt_pfd);

  int ret = poll(pfds.data(), pfds.size(), 0);
  if (ret <= 0) {
    if (waiting_fds_.empty()) return;
    ret = poll(pfds.data(), pfds.size(), -1);
    if (ret <= 0) return;
  }

  // Drain interrupt fd.
  if (pfds.back().revents & POLLIN) {
#if defined(__linux__)
    uint64_t val;
    (void)read(interrupt_fd_, &val, sizeof(val));
#else
    char val;
    (void)read(interrupt_fd_, &val, 1);
#endif
  }

  for (size_t i = 0; i < coroutines_per_fd.size(); i++) {
    if (pfds[i].revents & (POLLIN | POLLOUT | POLLHUP)) {
      std::vector<Coroutine*> to_resume = coroutines_per_fd[i];
      for (Coroutine* c : to_resume) {
        ResumeCoroutine(c, pfds[i].fd);
      }
    }
  }
}

void Scheduler::Run() {
  running_ = true;

  while (running_) {
    ProcessReadyCoroutines();

    if (waiting_fds_.empty() && ready_queue_.empty()) break;
    if (!ready_queue_.empty()) continue;
    if (waiting_fds_.empty()) break;

    ProcessEvents();
  }

  running_ = false;
}

thread_local Coroutine* self = nullptr;
thread_local Scheduler* scheduler = nullptr;

} // namespace co20

#endif // CO20_HAVE_COROUTINES
