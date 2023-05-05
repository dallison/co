// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "coroutine.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include "bitset.h"

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

#elif defined(__linux__)
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#else
#error "Unknown operating system"
#endif

namespace co {
static int NewEventFd() {
  int event_fd;
#if defined(__APPLE__)
  event_fd = kqueue();
#elif defined(__linux__)
  event_fd = eventfd(0, EFD_NONBLOCK);
#else
#error "Unknown operating system"
#endif
  return event_fd;
}

static void CloseEventFd(int fd) {
  if (fd == -1) {
    return;
  }
  close(fd);
}

static void TriggerEvent(int fd) {
#if defined(__APPLE__)
  struct kevent e;
  EV_SET(&e, 1, EVFILT_USER, EV_ADD, NOTE_TRIGGER, 0, nullptr);
  kevent(fd, &e, 1, 0, 0, 0);  // Trigger USER event
#elif defined(__linux__)
  int64_t val = 1;
  (void)write(fd, &val, 8);
#else
#error "Unknown operating system"
#endif
}

static void ClearEvent(int fd) {
#if defined(__APPLE__)
  struct kevent e;
  EV_SET(&e, 1, EVFILT_USER, EV_DELETE, NOTE_TRIGGER, 0, nullptr);
  kevent(fd, &e, 1, nullptr, 0, 0);  // Clear USER event
#elif defined(__linux__)
  int64_t val;
  (void)read(fd, &val, 8);
#else
#error "Unknown operating system"
#endif
}

Coroutine::Coroutine(CoroutineScheduler &machine, CoroutineFunction functor,
                     const char *name, bool autostart, size_t stack_size,
                     void *user_data)
    : scheduler_(machine),
      function_(std::move(functor)),
      stack_size_(stack_size),
      user_data_(user_data) {
  id_ = scheduler_.AllocateId();
  if (name == nullptr) {
    char buf[256];
    snprintf(buf, sizeof(buf), "co-%zd", id_);
    name_ = buf;
  } else {
    name_ = name;
  }

  stack_ = malloc(stack_size);
  if (stack_ == nullptr) {
    fprintf(stderr, "Failed to allocate stack for coroutine with size %zd: %s",
            stack_size, strerror(errno));
    abort();
  }
  state_ = State::kCoNew;
  event_fd_.fd = NewEventFd();
  event_fd_.events = POLLIN;

  // Might as well take the hit for allocating the pollfd vector when the
  // coroutine is created rather than delay it until the first wait.  It's
  // unlikely there will be more than 2 fds to wait for, plus possibly a
  // timeout.  If that's untrue we will just expand the vector when the wait is
  // done.
  wait_fds_.reserve(3);
  scheduler_.AddCoroutine(this);
  if (autostart) {
    Start();
  }
}

Coroutine::~Coroutine() {
  free(stack_);
  CloseEventFd(event_fd_.fd);
}

void Coroutine::Exit() { longjmp(exit_, 1); }

void Coroutine::Start() {
  if (state_ == State::kCoNew) {
    state_ = State::kCoReady;
  }
}

static int MakeTimer(uint64_t ns) {
#if defined(__APPLE__)
  // On MacOS we use a kqueue.
  int kq = kqueue();
  struct kevent e;

  EV_SET(&e, 1, EVFILT_TIMER, EV_ADD, NOTE_NSECONDS, ns, 0);
  kevent(kq, &e, 1, NULL, 0, NULL);
  return kq;
#elif defined(__linux__)
  // Linux uses a timerfd.
  struct itimerspec new_value;
  struct timespec now;
  constexpr int kBillion = 1000000000;

  clock_gettime(CLOCK_REALTIME, &now);
  new_value.it_value.tv_sec = now.tv_sec + ns / kBillion;
  new_value.it_value.tv_nsec = now.tv_nsec + ns % kBillion;
  new_value.it_interval.tv_sec = 0;
  new_value.it_interval.tv_nsec = 0;
  int fd = timerfd_create(CLOCK_REALTIME, 0);
  timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL);
  return fd;
#endif
}

int Coroutine::EndOfWait(int timer_fd, int result) {
  wait_fds_.clear();
  if (timer_fd != -1) {
    close(timer_fd);
  }
  // result contains the onescomp of the value passed to longjmp.
  // This will be an fd.  If it's the same fd as the timer we got a timeout so
  // return -1 to tell the caller.  The caller has no idea what
  // the timer_fd is, so there's no point in returning that.
  result = ~result;
  if (result == timer_fd) {
    return -1;
  }
  // A garbage value as the Resume() value will be returned as garbage.
  return result;
}

int Coroutine::AddTimeout(int64_t timeout_ns) {
  int timer_fd = -1;
  if (timeout_ns > 0) {
    timer_fd = MakeTimer(timeout_ns);
    struct pollfd timerfd = {.fd = timer_fd, .events = POLLIN};
    wait_fds_.push_back(timerfd);
  }
  return timer_fd;
}

int Coroutine::Wait(int fd, short event_mask, int64_t timeout_ns) {
  state_ = State::kCoWaiting;
  struct pollfd pfd = {.fd = fd, .events = event_mask};
  wait_fds_.push_back(pfd);
  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  int result = -1;
  if ((result = setjmp(resume_)) == 0) {
    longjmp(scheduler_.YieldBuf(), 1);
  }
  // Get here when resumed.
  return EndOfWait(timer_fd, result);
}

int Coroutine::Wait(struct pollfd &fd, int64_t timeout_ns) {
  state_ = State::kCoWaiting;
  wait_fds_.push_back(fd);
  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  int result = -1;
  if ((result = setjmp(resume_)) == 0) {
    longjmp(scheduler_.YieldBuf(), 1);
  }
  // Get here when resumed.
  return EndOfWait(timer_fd, result);
}

int Coroutine::Wait(const std::vector<struct pollfd> &fds, int64_t timeout_ns) {
  state_ = State::kCoWaiting;
  for (auto &fd : fds) {
    wait_fds_.push_back(fd);
  }
  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  int result = -1;
  if ((result = setjmp(resume_)) == 0) {
    longjmp(scheduler_.YieldBuf(), 1);
  }
  // Get here when resumed.
  return EndOfWait(timer_fd, result);
}

void Coroutine::Nanosleep(uint64_t ns) {
  int timer = MakeTimer(ns);
  Wait(timer);
  close(timer);
}

void Coroutine::TriggerEvent() { co::TriggerEvent(event_fd_.fd); }

void Coroutine::ClearEvent() { co::ClearEvent(event_fd_.fd); }

void Coroutine::AddPollFds(std::vector<struct pollfd> &pollfds,
                           std::vector<Coroutine *> &covec) {
  switch (state_) {
    case State::kCoReady:
    case State::kCoYielded:
      pollfds.push_back(event_fd_);
      covec.push_back(this);
      break;
    case State::kCoWaiting:
      for (auto &fd : wait_fds_) {
        pollfds.push_back(fd);
        covec.push_back(this);
      }
    case State::kCoNew:
    case State::kCoRunning:
    case State::kCoDead:
      break;
  }
}

void Coroutine::Show() {
  const char *state = "unknown";
  switch (state_) {
    case State::kCoNew:
      state = "new";
      break;
    case State::kCoDead:
      state = "dead";
      break;
    case State::kCoReady:
      state = "ready";
      break;
    case State::kCoRunning:
      state = "runnning";
      break;
    case State::kCoWaiting:
      state = "waiting";
      break;
    case State::kCoYielded:
      state = "yielded";
      break;
  }
  fprintf(stderr, "Coroutine %zd: %s: state: %s: address: %p\n", id_,
          name_.c_str(), state, yielded_address_);
}

bool Coroutine::IsAlive() { return scheduler_.IdExists(id_); }

void Coroutine::CallNonTemplate(Coroutine &callee) {
  // Start the callee running if it's not already running.  If it's running
  // we trigger its event to wake it up.
  if (callee.state_ == State::kCoNew) {
    callee.Start();
  } else {
    callee.TriggerEvent();
  }
  state_ = State::kCoYielded;
  last_tick_ = scheduler_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(scheduler_.YieldBuf(), 1);
    // Never get here.
  }
  // When we get here, the callee has done its work.  Remove this coroutine's
  // state from it.
  callee.caller_ = nullptr;
}

void Coroutine::Yield() {
  state_ = State::kCoYielded;
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  if (setjmp(resume_) == 0) {
    TriggerEvent();
    longjmp(scheduler_.YieldBuf(), 1);
    // Never get here.
  }
  // We get here when resumed.  We ignore the result of setjmp as we
  // are not waiting for anything and there is no yield with timeout
  // since the coroutine is automatically rescheduled.  If you want to
  // sleep, use the various Sleep functions.
}

void Coroutine::YieldNonTemplate() {
  if (caller_ != nullptr) {
    // Tell caller that there's a value available.
    caller_->TriggerEvent();
  }

  // Yield control to another coroutine but don't trigger a wakup event.
  // This will be done when another call is made.
  state_ = State::kCoYielded;
  last_tick_ = scheduler_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(scheduler_.YieldBuf(), 1);
    // Never get here.
  }
  // We get here when resumed from another call.
}

void Coroutine::InvokeFunction() { function_(this); }

// We use an intermediate function to do the invocation of
// the coroutine's function because we really want to avoid
// having mangled names coded into the assembly language in
// the Resume function.  A new compiler might change the
// name mangling rules and that would break the build.
extern "C" {
void __co_Invoke(Coroutine *c) { c->InvokeFunction(); }
}

void Coroutine::Resume(int value) {
  switch (state_) {
    case State::kCoReady:
      // Initial invocation of the coroutine.  We need to do a bit
      // of magic to switch to the coroutine's stack and invoke
      // the function using the stack.  When the function returns
      // we longjmp to the exit environment with the stack restored
      // to the current one, which is the stack used by the
      // CoroutineScheduler.
      state_ = State::kCoRunning;
      yielded_address_ = nullptr;
      if (setjmp(exit_) == 0) {
        void *sp = reinterpret_cast<char *>(stack_) + stack_size_;
        jmp_buf &exit_state = exit_;

#if defined(__aarch64__)
        asm("mov x12, sp\n"      // Save current stack pointer.
            "mov x13, x29\n"     // Save current frame pointer
            "sub sp, %0, #32\n"  // Set new stack pointer.
            "stp x12, x13, [sp, #16]\n"
            "str %1, [sp, #0]\n"  // Save exit state to stack.
            "mov x0, %2\n"
#if defined(__APPLE__)
            "bl ___co_Invoke\n"
#else
            "bl __co_Invoke\n"
#endif
            "ldr x0, [sp, #0]\n"  // Restore exit state.
            "ldp x12, x29, [sp, #16]\n"
            "mov sp, x12\n"  // Restore stack pointer
            "mov w1, #1\n"
#if defined(__APPLE__)
            "bl _longjmp\n"
#else
            "bl longjmp\n"
#endif
            :
            : "r"(sp), "r"(exit_state), "r"(this)
            : "x12", "x13");

#elif defined(__x86_64__)
      asm("movq %%rsp, %%r14\n" // Save current stack pointer.
          "movq %%rbp, %%r15\n" // Save current frame pointer
          "movq %0, %%rsp\n"    
          "pushq %%r14\n"    // Push rsp
          "pushq %%r15\n"    // Push rbp
          "pushq %1\n"       // Push env
          "subq $8, %%rsp\n" // Align to 16
          "movq %2, %%rdi\n" // this
#if defined(__APPLE__)
          "call ___co_Invoke\n"
#else
            "call __co_Invoke\n"
#endif
          "addq $8, %%rsp\n" // Remove alignment.
          "popq %%rdi\n"     // Pop env
          "popq %%rbp\n"
          "popq %%rsp\n"
          "movl $1, %%esi\n"
#if defined(__APPLE__)
          "call _longjmp\n"
#else
            "call longjmp\n"
            :
            : "r"(sp), "r"(exit_state), "r"(this)
            : "%r14", "%r15");
#endif

#else
#error "Unknown architecture"
#endif
      }
      // Trigger the caller when we exit.
      if (caller_ != nullptr) {
        caller_->TriggerEvent();
      }
      // Functor returned, we are dead.
      state_ = State::kCoDead;
      scheduler_.RemoveCoroutine(this);
      break;
    case State::kCoYielded:
    case State::kCoWaiting:
      state_ = State::kCoRunning;
      // We use the onescomp of the result so that we can wait
      // for standard input (fd 0).  We can't cause setjmp to
      // return 0 from longjmp.  This means that we can't
      // use -1 for the value as this would be returned from
      // setjmp as 1, which is stdout's fd and that would be
      // confusing.  Better to return something that can't be
      // a valid fd.
      if (value == -1) {
        value = 0;
      }
      longjmp(resume_, ~value);
      break;
    case State::kCoRunning:
    case State::kCoNew:
      // Should never get here.
      break;
    case State::kCoDead:
      longjmp(exit_, 1);
      break;
  }
}

CoroutineScheduler::CoroutineScheduler() {
  interrupt_fd_.fd = NewEventFd();
  interrupt_fd_.events = POLLIN;
}

CoroutineScheduler::~CoroutineScheduler() { CloseEventFd(interrupt_fd_.fd); }

void CoroutineScheduler::BuildPollFds(PollState *poll_state) {
  poll_state->pollfds.clear();
  poll_state->coroutines.clear();

  poll_state->pollfds.push_back(interrupt_fd_);
  for (auto *c : coroutines_) {
    auto state = c->GetState();
    if (state == Coroutine::State::kCoNew ||
        state == Coroutine::State::kCoRunning ||
        state == Coroutine::State::kCoDead) {
      continue;
    }
    c->AddPollFds(poll_state->pollfds, poll_state->coroutines);
    if (state == Coroutine::State::kCoReady) {
      // Coroutine is ready to go, trigger its event so that we can start
      // it.
      c->TriggerEvent();
    }
  }
}

// Schedule the next coroutine to run.  This scheduler chooses the
// coroutine that has been waiting longest.  Unless they are just new
// no two coroutines can have been waiting for the same amount of time.
// This is a completely fair scheduler with all coroutines given the
// same priority.
CoroutineScheduler::ChosenCoroutine CoroutineScheduler::ChooseRunnable(
    PollState *poll_state, int num_ready) {
  ready_coroutines_.clear();
  ready_coroutines_.reserve(num_ready);
  for (size_t i = 1; i < poll_state->pollfds.size(); i++) {
    struct pollfd *fd = &poll_state->pollfds[i];
    if (fd->revents != 0) {
      ready_coroutines_.emplace_back(poll_state->coroutines[i - 1], fd->fd);
    }
  }
  if (ready_coroutines_.empty()) {
    // Only interrrupt set with no coroutines ready.
    return ChosenCoroutine();
  }

  // Sort in descending order of time waiting.
  std::sort(ready_coroutines_.begin(), ready_coroutines_.end(),
            [](const ChosenCoroutine &c1, const ChosenCoroutine &c2) {
              uint64_t t1 = c1.co->Scheduler().TickCount() - c1.co->LastTick();
              uint64_t t2 = c2.co->Scheduler().TickCount() - c2.co->LastTick();
              return t1 >= t2;
            });
  return ready_coroutines_[0];
}

CoroutineScheduler::ChosenCoroutine CoroutineScheduler::GetRunnableCoroutine(
    PollState *poll_state, int num_ready) {
  if (interrupt_fd_.revents != 0) {
    // Interrupted.
    ClearEvent(interrupt_fd_.fd);
  }

  ChosenCoroutine chosen = ChooseRunnable(poll_state, num_ready);

  if (chosen.co != nullptr) {
    chosen.co->ClearEvent();
  }
  return chosen;
}

void CoroutineScheduler::Run() {
  running_ = true;
  while (running_) {
    if (coroutines_.empty()) {
      // No coroutines, nothing to do.
      break;
    }
    setjmp(yield_);
    // We get here any time a coroutine yields or waits.

    BuildPollFds(&poll_state_);

    // Wait for coroutines (or the interrupt fd) to trigger.
    int num_ready =
        ::poll(poll_state_.pollfds.data(), poll_state_.pollfds.size(), -1);
    if (num_ready <= 0) {
      continue;
    }

    // One more tick.
    tick_count_++;

    // Choose a runnable coroutine.
    ChosenCoroutine c = GetRunnableCoroutine(&poll_state_, num_ready);
    if (c.co != nullptr) {
      c.co->Resume(c.fd);
    }
  }
}

void CoroutineScheduler::GetPollState(PollState *poll_state) {
  BuildPollFds(poll_state);
}

void CoroutineScheduler::ProcessPoll(PollState *poll_state) {
  int num_ready = 0;
  for (size_t i = 1; i < poll_state->pollfds.size(); i++) {
    if (poll_state->pollfds[i].revents != 0) {
      num_ready++;
    }
  }
  // One more tick.
  tick_count_++;

  // Choose a runnable coroutine.
  ChosenCoroutine c = GetRunnableCoroutine(poll_state, num_ready);
  if (c.co != nullptr) {
    c.co->Resume(c.fd);
  }
}

void CoroutineScheduler::AddCoroutine(Coroutine *c) { coroutines_.push_back(c); }

// Removes a coroutine but doesn't destruct it.  The coroutines's id will
// be removed and can be reused immediately after the completion callback
// is called.
void CoroutineScheduler::RemoveCoroutine(Coroutine *c) {
  for (auto it = coroutines_.begin(); it != coroutines_.end(); it++) {
    if (*it == c) {
      coroutines_.erase(it);
      // Call completion callback to allow for external memory management.
      if (completion_callback_ != nullptr) {
        completion_callback_(c);
      }
      break;
    }
  }
  coroutine_ids_.Free(c->Id());
  last_freed_coroutine_id_ = c->Id();
}

size_t CoroutineScheduler::AllocateId() {
  size_t id;
  if (last_freed_coroutine_id_ != -1) {
    id = last_freed_coroutine_id_;
    last_freed_coroutine_id_ = -1;
    coroutine_ids_.Set(id);
  } else {
    id = coroutine_ids_.Allocate();
  }
  return id;
}

void CoroutineScheduler::Stop() {
  running_ = false;
  TriggerEvent(interrupt_fd_.fd);
}

void CoroutineScheduler::Show() {
  for (auto *co : coroutines_) {
    co->Show();
  }
}

}  // namespace co
