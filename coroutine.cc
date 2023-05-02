// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.


#include "coroutine.h"
#include "bitset.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  kevent(fd, &e, 1, 0, 0, 0); // Trigger USER event
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
  kevent(fd, &e, 1, nullptr, 0, 0); // Clear USER event
#elif defined(__linux__)
  int64_t val;
  (void)read(fd, &val, 8);
#else
#error "Unknown operating system"
#endif
}

Coroutine::Coroutine(CoroutineMachine &machine, CoroutineFunctor functor,
                     bool autostart, size_t stack_size, void *user_data)
    : machine_(machine), functor_(std::move(functor)), stack_size_(stack_size),
      user_data_(user_data) {
  id_ = machine_.AllocateId();
  char buf[256];
  snprintf(buf, sizeof(buf), "co-%zd", id_);
  name_ = buf;

  stack_ = malloc(stack_size);
  state_ = CoroutineState::kCoNew;
  event_fd_.fd = NewEventFd();
  event_fd_.events = POLLIN;

  wait_fd_.fd = -1;
  wait_fd_.events = POLLIN;
  machine_.AddCoroutine(this);
  if (autostart) {
    Start();
  }
}

Coroutine::~Coroutine() {
  free(stack_);
  CloseEventFd(event_fd_.fd);
  CloseEventFd(wait_fd_.fd);
}

void Coroutine::Exit() { longjmp(exit_, 1); }

void Coroutine::Start() {
  if (state_ == CoroutineState::kCoNew) {
    state_ = CoroutineState::kCoReady;
  }
}

void Coroutine::Wait(int fd, int event_mask) {
  state_ = CoroutineState::kCoWaiting;
  wait_fd_.fd = fd;
  wait_fd_.events = event_mask;
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = machine_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(machine_.YieldBuf(), 1);
  }
  wait_fd_.fd = -1;
}

void Coroutine::Wait(struct pollfd &fd) {
  state_ = CoroutineState::kCoWaiting;
  wait_fd_ = fd;
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = machine_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(machine_.YieldBuf(), 1);
  }
  wait_fd_.fd = -1;
}

void Coroutine::Nanosleep(uint64_t ns) {
#if defined(__APPLE__)
  int kq = kqueue();
  struct kevent e;

  EV_SET(&e, 1, EVFILT_TIMER, EV_ADD, NOTE_NSECONDS, ns, 0);
  kevent(kq, &e, 1, NULL, 0, NULL);
  Wait(kq, POLLIN);
  EV_SET(&e, 1, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
  kevent(kq, &e, 1, NULL, 0, NULL);
  close(kq);
#elif defined(__linux__)
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
  Wait(fd, POLLIN);
  close(fd);
#endif
}

void Coroutine::TriggerEvent() { co::TriggerEvent(event_fd_.fd); }

void Coroutine::ClearEvent() { co::ClearEvent(event_fd_.fd); }

struct pollfd *Coroutine::GetPollFd() {
  static struct pollfd empty = {.fd = -1, .events = 0, .revents = 0};
  switch (state_) {
  case CoroutineState::kCoReady:
  case CoroutineState::kCoYielded:
    return &event_fd_;
  case CoroutineState::kCoWaiting:
    return &wait_fd_;
  case CoroutineState::kCoNew:
  case CoroutineState::kCoRunning:
  case CoroutineState::kCoDead:
    break;
  }
  return &empty;
}

void Coroutine::Show() {
  const char *state = "unknown";
  switch (state_) {
  case CoroutineState::kCoNew:
    state = "new";
    break;
  case CoroutineState::kCoDead:
    state = "dead";
    break;
  case CoroutineState::kCoReady:
    state = "ready";
    break;
  case CoroutineState::kCoRunning:
    state = "runnning";
    break;
  case CoroutineState::kCoWaiting:
    state = "waiting";
    break;
  case CoroutineState::kCoYielded:
    state = "yielded";
    break;
  }
  fprintf(stderr, "Coroutine %zd: %s: state: %s: address: %p\n", id_,
          name_.c_str(), state, yielded_address_);
}

bool Coroutine::IsAlive(Coroutine &query) {
  return machine_.IdExists(query.id_);
}

void Coroutine::Yield() {
  state_ = CoroutineState::kCoYielded;
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = machine_.TickCount();
  if (setjmp(resume_) == 0) {
    TriggerEvent();
    longjmp(machine_.YieldBuf(), 1);
    // Never get here.
  }
  // We get here when resumed.
}

void Coroutine::YieldValue(void *value) {
  // Copy value.
  if (result_ != nullptr) {
    memcpy(result_, value, result_size_);
  }
  if (caller_ != nullptr) {
    // Tell caller that there's a value available.
    caller_->TriggerEvent();
  }

  // Yield control to another coroutine but don't trigger a wakup event.
  // This will be done when another call is made.
  state_ = CoroutineState::kCoYielded;
  last_tick_ = machine_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(machine_.YieldBuf(), 1);
    // Never get here.
  }
  // We get here when resumed from another call.
}

void Coroutine::Call(Coroutine &callee, void *result, size_t result_size) {
  // Tell the callee that it's being called and where to store the value.
  callee.caller_ = this;
  callee.result_ = result;
  callee.result_size_ = result_size;

  // Start the callee running if it's not already running.  If it's running
  // we trigger its event to wake it up.
  if (callee.state_ == CoroutineState::kCoNew) {
    callee.Start();
  } else {
    callee.TriggerEvent();
  }
  state_ = CoroutineState::kCoYielded;
  last_tick_ = machine_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(machine_.YieldBuf(), 1);
    // Never get here.
  }
  // When we get here, the callee has done its work.  Remove this coroutine's
  // state from it.
  callee.caller_ = nullptr;
  callee.result_ = nullptr;
}

void Coroutine::InvokeFunctor() {
  functor_(this);
}

void Coroutine::Resume() {
  switch (state_) {
  case CoroutineState::kCoReady:
    state_ = CoroutineState::kCoRunning;
    yielded_address_ = nullptr;
    if (setjmp(exit_) == 0) {
      void *sp = reinterpret_cast<char *>(stack_) + stack_size_;
      jmp_buf &exit_state = exit_;

#if defined(__aarch64__)
      asm("mov x12, sp\n"     // Save current stack pointer.
          "mov x13, x29\n"    // Save current frame pointer
          "sub sp, %0, #32\n" // Set new stack pointer.
          "stp x12, x13, [sp, #16]\n"
          "str %1, [sp, #0]\n" // Save exit state to stack.
          "mov x0, %2\n"
#if defined(__APPLE__)
          "bl __ZN2co9Coroutine13InvokeFunctorEv\n"
#else
          "bl _ZN2co9Coroutine13InvokeFunctorEv\n"
#endif
          "ldr x0, [sp, #0]\n" // Restore exit state.
          "ldp x12, x29, [sp, #16]\n"
          "mov sp, x12\n" // Restore stack pointer
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
          "movq $0, %%rbp\n"    // FP = 0
          "movq %0, %%rsp\n"    
          "pushq %%r14\n"    // Push rsp
          "pushq %%r15\n"    // Push rbp
          "pushq %1\n"       // Push env
          "subq $8, %%rsp\n" // Align to 16
          "movq %2, %%rdi\n" // this
#if defined(__APPLE__)
          "call __ZN2co9Coroutine13InvokeFunctorEv\n"
#else
          "call _ZN2co9Coroutine13InvokeFunctorEv\n"
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
    state_ = CoroutineState::kCoDead;
    machine_.RemoveCoroutine(this);
    break;
  case CoroutineState::kCoYielded:
  case CoroutineState::kCoWaiting:
    state_ = CoroutineState::kCoRunning;
    longjmp(resume_, 1);
    break;
  case CoroutineState::kCoRunning:
  case CoroutineState::kCoNew:
    // Should never get here.
    break;
  case CoroutineState::kCoDead:
    longjmp(exit_, 1);
    break;
  }
}

CoroutineMachine::CoroutineMachine() {
  interrupt_fd_.fd = NewEventFd();
  interrupt_fd_.events = POLLIN;
}

CoroutineMachine::~CoroutineMachine() { CloseEventFd(interrupt_fd_.fd); }

void CoroutineMachine::BuildPollFds(PollState *poll_state) {
  poll_state->pollfds.clear();
  poll_state->coroutines.clear();

  poll_state->pollfds.push_back(interrupt_fd_);
  for (auto *c : coroutines_) {
    auto state = c->State();
    if (state == CoroutineState::kCoNew ||
        state == CoroutineState::kCoRunning ||
        state == CoroutineState::kCoDead) {
      continue;
    }
    struct pollfd *fd = c->GetPollFd();
    poll_state->pollfds.push_back(*fd);
    poll_state->coroutines.push_back(c);
    if (state == CoroutineState::kCoReady) {
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
//
// We could use malloc/free here but that is a memory allocation.  The
// use of alloca or a VLA is much faster, albeit not in POSIX.
//
// If you want this to be more portable, call malloc or calloc and free it
// after the chosen coroutine is determined.

Coroutine *CoroutineMachine::ChooseRunnable(PollState *poll_state,
                                            int num_ready) {

  ready_coroutines_.clear();
  ready_coroutines_.reserve(num_ready);
  for (size_t i = 1; i < poll_state->pollfds.size(); i++) {
    struct pollfd *fd = &poll_state->pollfds[i];
    if (fd->revents != 0) {
      ready_coroutines_.push_back(poll_state->coroutines[i - 1]);
    }
  }
  if (ready_coroutines_.empty()) {
    // Only interrrupt set with no coroutines ready.
    return nullptr;
  }

  // Sort in descending order of time waiting.
  std::sort(ready_coroutines_.begin(), ready_coroutines_.end(),
            [](const Coroutine *c1, const Coroutine *c2) {
              uint64_t t1 = c1->Machine().TickCount() - c1->LastTick();
              uint64_t t2 = c2->Machine().TickCount() - c2->LastTick();
              return t1 >= t2;
            });
  return ready_coroutines_[0];
}

Coroutine *CoroutineMachine::GetRunnableCoroutine(PollState *poll_state,
                                                  int num_ready) {
  if (interrupt_fd_.revents != 0) {
    // Interrupted.
    ClearEvent(interrupt_fd_.fd);
  }

  Coroutine *chosen = ChooseRunnable(poll_state, num_ready);

  if (chosen != nullptr) {
    chosen->ClearEvent();
  }
  return chosen;
}

void CoroutineMachine::Run() {
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
    Coroutine *c = GetRunnableCoroutine(&poll_state_, num_ready);
    if (c != nullptr) {
      c->Resume();
    }
  }
}

void CoroutineMachine::GetPollState(PollState *poll_state) {
  BuildPollFds(poll_state);
}

void CoroutineMachine::ProcessPoll(PollState *poll_state) {
  int num_ready = 0;
  for (size_t i = 1; i < poll_state->pollfds.size(); i++) {
    if (poll_state->pollfds[i].revents != 0) {
      num_ready++;
    }
  }
  // One more tick.
  tick_count_++;

  // Choose a runnable coroutine.
  Coroutine *c = GetRunnableCoroutine(poll_state, num_ready);
  if (c != nullptr) {
    c->Resume();
  }
}

void CoroutineMachine::AddCoroutine(Coroutine *c) { coroutines_.push_back(c); }

// Removes a coroutine but doesn't free it.
void CoroutineMachine::RemoveCoroutine(Coroutine *c) {
  coroutine_ids_.Free(c->Id());
  last_freed_coroutine_id_ = c->Id();
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
}

size_t CoroutineMachine::AllocateId() {
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

void CoroutineMachine::Stop() {
  running_ = false;
  TriggerEvent(interrupt_fd_.fd);
}

void CoroutineMachine::Show() {
  for (auto *co : coroutines_) {
    co->Show();
  }
}

} // namespace co
