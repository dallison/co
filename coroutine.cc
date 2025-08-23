// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "coroutine.h"
#include "detect_sanitizers.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <fcntl.h>
#include <iostream>

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

constexpr bool kCoDebug = false;

#if CO_CTX_MODE == CO_CTX_SETJMP
#define SETCONTEXT(ctx) __real_longjmp(ctx, 1)
#define GETCONTEXT(ctx) setjmp(ctx)
#define SWAPCONTEXT(from, to)                                                  \
  if (setjmp(from) == 0) {                                                     \
    __real_longjmp(to, 1);                                                     \
  }
#elif CO_CTX_MODE == CO_CTX_UCONTEXT
#define SETCONTEXT(ctx) setcontext(&ctx)
#define GETCONTEXT(ctx) getcontext(&ctx)
#define SWAPCONTEXT(from, to) swapcontext(&from, &to)
#else
#define SETCONTEXT(ctx) CoroutineSetContext(&ctx)
#define GETCONTEXT(ctx) CoroutineGetContext(&ctx)
#define SWAPCONTEXT(from, to) CoroutineSwapContext(&from, &to)
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

#if CO_CTX_MODE == CO_CTX_SETJMP
extern "C" {
// Linux, when building optimized ever-so-helpfully replaces
// longjmp with longjmp_chk unless _FORTIFY_SOURCE is not defined.
// We are using longjmp for context switches so the checks done
// by longjmp_chk break everything.  We could force the undef
// of _FORTIFY_SOURCE but that seems fragile.  It's better to
// stay out of the compiler's way and just call the function we
// really want directly to avoid any shenanigans.
void __real_longjmp(jmp_buf, int);
}
#endif

// Apple puts an underscore prefix for all external symbols.
#if defined(__APPLE__)
#define STRSYM(name) #name
#define SYM(name) STRSYM(_##name)
#else
#define SYM(name) #name
#endif

// Define __real_longjmp as a simple relay function that jumps
// to the real longjmp function.
// clang-format off
#if defined(__aarch64__)
asm(
  SYM(__real_longjmp) ":\n"
  "b " SYM(longjmp) "\n");
#elif defined(__x86_64__)
asm(
  SYM(__real_longjmp) ":\n"
  "jmp " SYM(longjmp) "\n");
#else
#error "Unsupported architecture"
#endif
// clang-format on

Coroutine::Coroutine(CoroutineScheduler &scheduler, CoroutineFunction functor,
                     std::string name, int interrupt_fd, bool autostart,
                     size_t stack_size, void *user_data)
    : Coroutine(scheduler,
                [functor = std::move(functor)](const Coroutine &c) {
                  functor(const_cast<Coroutine *>(&c));
                },
                std::move(name), interrupt_fd, autostart, stack_size,
                user_data) {}

Coroutine::Coroutine(CoroutineScheduler &scheduler,
                     CoroutineFunctionRef functor, std::string name,
                     int interrupt_fd, bool autostart, size_t stack_size,
                     void *user_data)
    : scheduler_(scheduler), function_(std::move(functor)),
      interrupt_fd_(interrupt_fd), user_data_(user_data) {
  id_ = scheduler_.AllocateId();
  if (name.empty()) {
    char buf[256];
    snprintf(buf, sizeof(buf), "co-%d", id_);
    name_ = buf;
  } else {
    name_ = std::move(name);
  }

  stack_.resize(stack_size);

#if CO_CTX_MODE == CO_CTX_UCONTEXT
  getcontext(&resume_);
  resume_.uc_stack.ss_sp = stack_.data();
  resume_.uc_stack.ss_size = stack_.size();
  resume_.uc_link = &exit_;
  void (*func)(void) = reinterpret_cast<void (*)(void)>(__co_Invoke);
  makecontext(&resume_, func, 1, this);
#elif CO_CTX_MODE == CO_CTX_CUSTOM
  CoroutineGetContext(&resume_);
  resume_.stack = stack_.data();
  resume_.stack_size = stack_.size();
  resume_.next = &exit_;
  void (*func)(void *) = reinterpret_cast<void (*)(void *)>(__co_Invoke);
  CoroutineMakeContext(&resume_, func, this);
#endif

#if CO_POLL_MODE == CO_POLL_EPOLL
  int efd = NewEventFd();
  if (efd == -1) {
    fprintf(stderr, "Failed to allocate event fd: %s\n", strerror(errno));
    abort();
  }
  yield_fd_ = YieldedCoroutine(this, efd, EPOLLIN);
#else
  event_fd_.fd = NewEventFd();
  if (event_fd_.fd == -1) {
    fprintf(stderr, "Failed to allocate event fd: %s\n", strerror(errno));
    abort();
  }
  event_fd_.events = POLLIN;
#endif

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
#if CO_POLL_MODE == CO_POLL_EPOLL
  CloseEventFd(yield_fd_.fd);
#else
  CloseEventFd(event_fd_.fd);
#endif
}

const char *Coroutine::StateName(State state) {
  switch (state) {
  case State::kCoDead:
    return "dead";
  case State::kCoNew:
    return "new";
  case State::kCoReady:
    return "ready";
  case State::kCoRunning:
    return "running";
  case State::kCoWaiting:
    return "waiting";
  case State::kCoYielded:
    return "yielded";
  }
  return "unknown";
}

void Coroutine::SetState(State state) const {
  if (state == state_) {
    return;
  }
  if (kCoDebug) {
    std::cerr << Name() << " moving from state " << StateName(state_) << " to "
              << StateName(state) << std::endl;
  }
#if CO_POLL_MODE == CO_POLL_EPOLL
  // In epoll mode we manipulate the epoll fd set based on the state
  // we are leaving and that we are entering.

  // We are leaving this state, remove the epoll fds to prepare for the
  // new state.
  switch (state_) {
  case State::kCoWaiting:
    // Leaving waiting state, remove the wait fds from the poll set
    for (auto &fd : wait_fds_) {
      scheduler_.RemoveEpollFd(&fd);
    }
    break;
  case State::kCoYielded:
    [[fallthrough]];
  case State::kCoReady:
    scheduler_.RemoveEpollFd(&yield_fd_);
    break;
  default:
    break;
  }

  // Add the epoll fds based on the state we are entering.
  switch (state) {
  case State::kCoNew:
    break;
  case State::kCoReady:
    TriggerEvent();
    scheduler_.AddEpollFd(&yield_fd_, EPOLLIN);
    break;
  case State::kCoRunning:
    break;
  case State::kCoWaiting:
    for (auto &fd : wait_fds_) {
      scheduler_.AddEpollFd(&fd, fd.events);
    }
    break;
  case State::kCoYielded:
    scheduler_.AddEpollFd(&yield_fd_, EPOLLIN);
    break;
  case State::kCoDead:
    break;
  }
#endif
  state_ = state;
}

void Coroutine::Exit() { SETCONTEXT(exit_); }

void Coroutine::Start() {
  if (state_ == State::kCoNew) {
    SetState(State::kCoReady);
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
  constexpr uint64_t kBillion = 1000000000LL;

  clock_gettime(CLOCK_REALTIME, &now);
  uint64_t now_sec = static_cast<uint64_t>(now.tv_sec);
  uint64_t now_nsec = static_cast<uint64_t>(now.tv_nsec);

  uint64_t then_nsec = now_nsec + ns;
  uint64_t then_sec = now_sec + then_nsec / kBillion;

  new_value.it_value.tv_sec = then_sec;
  new_value.it_value.tv_nsec = then_nsec % kBillion;
  new_value.it_interval.tv_sec = 0;
  new_value.it_interval.tv_nsec = 0;
  int fd = timerfd_create(CLOCK_REALTIME, 0);
  timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL);
  return fd;
#endif
}

int Coroutine::EndOfWait(int timer_fd) const {
  wait_fds_.clear();
  if (timer_fd != -1) {
    close(timer_fd);
  }
  if (wait_result_ == timer_fd) {
    return -1;
  }
  // A garbage value as the Resume() value will be returned as garbage.
  return wait_result_;
}

int Coroutine::AddTimeout(uint64_t timeout_ns) const {
  int timer_fd = -1;
  if (timeout_ns > 0) {
    timer_fd = MakeTimer(timeout_ns);
#if CO_POLL_MODE == CO_POLL_EPOLL
    wait_fds_.push_back(YieldedCoroutine(this, timer_fd, EPOLLIN));
#else
    struct pollfd timerfd = {.fd = timer_fd, .events = POLLIN};
    wait_fds_.push_back(timerfd);
#endif
  }
  return timer_fd;
}

int Coroutine::Wait(int fd, uint32_t event_mask, uint64_t timeout_ns) const {
#if CO_POLL_MODE == CO_POLL_EPOLL
  wait_fds_.push_back(YieldedCoroutine(this, fd, event_mask));
  if (interrupt_fd_ != -1) {
    wait_fds_.push_back(YieldedCoroutine(this, interrupt_fd_, EPOLLIN));
  }
#else
  struct pollfd pfd = {.fd = fd, .events = short(event_mask)};
  wait_fds_.push_back(pfd);
  if (interrupt_fd_ != -1) {
    struct pollfd ifd = {.fd = interrupt_fd_, .events = POLLIN};
    wait_fds_.push_back(ifd);
  }
#endif

  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  SetState(State::kCoWaiting);
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());
  // Get here when resumed.
  return EndOfWait(timer_fd);
}

int Coroutine::Wait(const std::vector<int> &fds, uint32_t event_mask,
                    uint64_t timeout_ns) const {
#if CO_POLL_MODE == CO_POLL_EPOLL
  for (auto &fd : fds) {
    wait_fds_.push_back(YieldedCoroutine(this, fd, event_mask));
  }
  if (interrupt_fd_ != -1) {
    wait_fds_.push_back(YieldedCoroutine(this, interrupt_fd_, EPOLLIN));
  }
#else
  for (auto &fd : fds) {
    wait_fds_.push_back({.fd = fd, .events = short(event_mask)});
  }
  if (interrupt_fd_ != -1) {
    struct pollfd ifd = {.fd = interrupt_fd_, .events = POLLIN};
    wait_fds_.push_back(ifd);
  }
#endif

  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  SetState(State::kCoWaiting);
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());
  // Get here when resumed.
  return EndOfWait(timer_fd);
}

#if CO_POLL_MODE == CO_POLL_EPOLL
int Coroutine::Wait(const std::vector<WaitFd> &fds, uint64_t timeout_ns) const {
  for (auto &fd : fds) {
    wait_fds_.push_back(YieldedCoroutine(this, fd.fd, fd.events));
  }
  if (interrupt_fd_ != -1) {
    wait_fds_.push_back(YieldedCoroutine(this, interrupt_fd_, EPOLLIN));
  }
  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  SetState(State::kCoWaiting);
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());

  // Get here when resumed.
  return EndOfWait(timer_fd);
}
#else
int Coroutine::Wait(struct pollfd &fd, uint64_t timeout_ns) const {
  wait_fds_.push_back(fd);
  if (interrupt_fd_ != -1) {
    struct pollfd ifd = {.fd = interrupt_fd_, .events = POLLIN};
    wait_fds_.push_back(ifd);
  }
  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  SetState(State::kCoWaiting);
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());

  // Get here when resumed.
  return EndOfWait(timer_fd);
}

int Coroutine::Wait(const std::vector<struct pollfd> &fds,
                    uint64_t timeout_ns) const {
  for (auto &fd : fds) {
    wait_fds_.push_back(fd);
  }
  if (interrupt_fd_ != -1) {
    struct pollfd ifd = {.fd = interrupt_fd_, .events = POLLIN};
    wait_fds_.push_back(ifd);
  }
  int timer_fd = AddTimeout(timeout_ns);
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  SetState(State::kCoWaiting);
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());

  // Get here when resumed.
  return EndOfWait(timer_fd);
}
#endif

void Coroutine::Nanosleep(uint64_t ns) const {
  int timer = MakeTimer(ns);
  Wait(timer);
  close(timer);
}

void Coroutine::TriggerEvent() const {
#if CO_POLL_MODE == CO_POLL_EPOLL
  co::TriggerEvent(yield_fd_.fd);
#else
  co::TriggerEvent(event_fd_.fd);
#endif
}

void Coroutine::ClearEvent() const {
#if CO_POLL_MODE == CO_POLL_EPOLL
  co::ClearEvent(yield_fd_.fd);
#else
  co::ClearEvent(event_fd_.fd);
#endif
}

#if CO_POLL_MODE == CO_POLL_POLL
void Coroutine::AddPollFds(std::vector<struct pollfd> &pollfds,
                           std::vector<Coroutine *> &covec) {
  switch (state_) {
  case State::kCoReady:
    [[fallthrough]];
  case State::kCoYielded:
    pollfds.push_back(event_fd_);
    covec.push_back(this);
    break;
  case State::kCoWaiting:
    for (auto &fd : wait_fds_) {
      pollfds.push_back(fd);
      covec.push_back(this);
    }
    break;
  case State::kCoNew:
    [[fallthrough]];
  case State::kCoRunning:
    [[fallthrough]];
  case State::kCoDead:
    break;
  }
}
#endif

std::string Coroutine::ToString() const {
  if (IsAlive() && to_string_callback_ != nullptr) {
    return to_string_callback_();
  }
  return MakeDefaultString();
}

std::string Coroutine::MakeDefaultString() const {
  const char *state = StateName(state_);
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "Coroutine %d: %s: state: %s: address: %p",
           id_, name_.c_str(), state, yielded_address_);
  return buffer;
}

void Coroutine::Show() const {
  fprintf(stderr, "%s\n", MakeDefaultString().c_str());
}

bool Coroutine::IsAlive() const { return scheduler_.IdExists(id_); }

void Coroutine::CallNonTemplate(Coroutine &callee) const {
  // Start the callee running if it's not already running.  If it's running
  // we trigger its event to wake it up.
  if (callee.state_ == State::kCoNew) {
    callee.Start();
  } else {
    callee.TriggerEvent();
  }
  SetState(State::kCoYielded);
  last_tick_ = scheduler_.TickCount();
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());

  // When we get here, the callee has done its work.  Remove this coroutine's
  // state from it.
  callee.caller_ = nullptr;
}

void Coroutine::Yield() const {
  yielded_address_ = __builtin_return_address(0);
  last_tick_ = scheduler_.TickCount();
  SetState(State::kCoYielded);
  TriggerEvent();
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());

  // We get here when resumed.  We ignore the result of setjmp as we
  // are not waiting for anything and there is no yield with timeout
  // since the coroutine is automatically rescheduled.  If you want to
  // sleep, use the various Sleep functions.
}

void Coroutine::YieldToScheduler() const {
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());
}

void Coroutine::YieldNonTemplate() const {
  if (caller_ != nullptr) {
    // Tell caller that there's a value available.
    caller_->TriggerEvent();
  }

  // Yield control to another coroutine but don't trigger a wakup event.
  // This will be done when another call is made.
  SetState(State::kCoYielded);
  last_tick_ = scheduler_.TickCount();
  SWAPCONTEXT(resume_, scheduler_.YieldCtx());

  // We get here when resumed from another call.
}

void Coroutine::InvokeFunction() { function_(*this); }

// We use an intermediate function to do the invocation of
// the coroutine's function because we really want to avoid
// having mangled names coded into the assembly language in
// the Resume function.  A new compiler might change the
// name mangling rules and that would break the build.
extern "C" {
void __co_Invoke(Coroutine *c) { c->InvokeFunction(); }
}

CO_DISABLE_ADDRESS_SANITIZER
void Coroutine::Resume(int value) const {
  switch (state_) {
  case State::kCoReady:
    // Initial invocation of the coroutine.  We need to do a bit
    // of magic to switch to the coroutine's stack and invoke
    // the function using the stack.  When the function returns
    // we longjmp to the exit environment with the stack restored
    // to the current one, which is the stack used by the
    // CoroutineScheduler.
    SetState(State::kCoRunning);
    yielded_address_ = nullptr;
#if CO_CTX_MODE == CO_CTX_SETJMP
    if (setjmp(exit_) == 0) {
      const void *sp =
          reinterpret_cast<const char *>(stack_.data()) + stack_.size();
      jmp_buf &exit_state = exit_;

// clang-format off
#if defined(__aarch64__)
      asm("mov x12, sp\n"     // Save current stack pointer.
          "mov x13, x29\n"    // Save current frame pointer
          "sub sp, %0, #32\n" // Set new stack pointer.
          "stp x12, x13, [sp, #16]\n"
          "str %1, [sp, #0]\n" // Save exit state to stack.
          "mov x0, %2\n"
          "bl " SYM(__co_Invoke) "\n"
          "ldr x0, [sp, #0]\n" // Restore exit state.
          "ldp x12, x29, [sp, #16]\n"
          "mov sp, x12\n" // Restore stack pointer
          "mov w1, #1\n"
          "bl " SYM(longjmp) "\n"
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
          "call " SYM(__co_Invoke) "\n"
          "addq $8, %%rsp\n" // Remove alignment.
          "popq %%rdi\n"     // Pop env
          "popq %%rbp\n"
          "popq %%rsp\n"
          "movl $1, %%esi\n"
          "call " SYM(longjmp) "\n"
          :
          : "r"(sp), "r"(exit_state), "r"(this)
          : "%r14", "%r15");
#else
#error "Unknown architecture"
#endif
      // clang-format on
    }
#else
    GETCONTEXT(exit_);
    // We will get here when the coroutines's function returns.
    if (first_resume_) {
      first_resume_ = false;
      // This is the first time we have been resumed so set the context
      // to that set up by makecontext.  This will set the stack and invoke
      // the function.
      SETCONTEXT(resume_);
    }
#endif

    // Trigger the caller when we exit.
    if (caller_ != nullptr) {
      caller_->TriggerEvent();
    }
    // Functor returned, we are dead.
    SetState(State::kCoDead);
    scheduler_.RemoveCoroutine(this);
    SETCONTEXT(scheduler_.YieldCtx());
    break;
  case State::kCoYielded:
    [[fallthrough]];
  case State::kCoWaiting:
    SetState(State::kCoRunning);
    wait_result_ = value;
    SETCONTEXT(resume_);
    break;
  case State::kCoRunning:
    [[fallthrough]];
  case State::kCoNew:
    // Should never get here.
    break;
  case State::kCoDead:
    SETCONTEXT(exit_);
    break;
  }
}

void Coroutine::GetAllFds(std::vector<int> &fds) const {
#if CO_POLL_MODE == CO_POLL_EPOLL
  if (yield_fd_.fd != -1) {
    fds.push_back(yield_fd_.fd);
  }
#else
  if (event_fd_.fd != -1) {
    fds.push_back(event_fd_.fd);
  }
#endif
  if (state_ == State::kCoWaiting) {
    for (auto &fd : wait_fds_) {
      fds.push_back(fd.fd);
    }
  }
}

CoroutineScheduler::CoroutineScheduler() {
#if CO_POLL_MODE == CO_POLL_EPOLL
  interrupt_fd_ = NewEventFd();
  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ == -1) {
    std::cerr << "Failed to create epoll fd: " << strerror(errno) << std::endl;
    abort();
  }
  AddEpollFd(interrupt_fd_, EPOLLIN);
#else
  interrupt_fd_.fd = NewEventFd();
  interrupt_fd_.events = POLLIN;
#endif
}

CoroutineScheduler::~CoroutineScheduler() {
#if CO_POLL_MODE == CO_POLL_EPOLL
  close(epoll_fd_);
  close(interrupt_fd_);
#else
  CloseEventFd(interrupt_fd_.fd);
#endif
}

#if CO_POLL_MODE == CO_POLL_EPOLL

void CoroutineScheduler::AddEpollFd(int fd, uint32_t events) {
  if (fd == -1) {
    return;
  }
  if (kCoDebug) {
    std::cerr << "adding raw epoll fd " << fd << std::endl;
  }
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = events;
  event.data.fd = fd;
  int e = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
  if (e == -1) {
    std::cerr << "epoll_ctl failed: " << strerror(errno) << std::endl;
    abort();
  }
  num_epoll_events_++;
}

void CoroutineScheduler::AddEpollFd(YieldedCoroutine *c, uint32_t events) {
  if (c->fd == -1) {
    return;
  }
  if (kCoDebug) {
    std::cerr << "adding coroutine epoll fd " << c->fd << std::endl;
  }
  auto it = waiting_coroutines_.find(c->fd);
  if (it == waiting_coroutines_.end()) {
    absl::flat_hash_set<YieldedCoroutine *> s;
    s.insert(c);
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.fd = c->fd;
    int e = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, c->fd, &event);
    if (e == -1) {
      std::cerr << "epoll_ctl failed: " << strerror(errno) << std::endl;
      abort();
    }
    num_epoll_events_++;
    waiting_coroutines_.insert(std::make_pair(c->fd, std::move(s)));
  } else {
    it->second.insert(c);
  }
}

void CoroutineScheduler::RemoveEpollFd(YieldedCoroutine *c) {
  if (c->fd == -1) {
    return;
  }
  if (kCoDebug) {
    std::cerr << "removing coroutine epoll fd " << c->fd << std::endl;
  }
  auto it = waiting_coroutines_.find(c->fd);
  if (it == waiting_coroutines_.end()) {
    // Not found, nothing to do.
    return;
  }
  auto &list = it->second;
  list.erase(c);
  if (!list.empty()) {
    return;
  }
  struct epoll_event event = {.events = EPOLLIN, .data = {.ptr = nullptr}};
  int e = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, c->fd, &event);
  if (e == -1) {
    if (errno == EBADF) {
      // Ignore removing a file descriptor that doesn't exist.
    } else {
      std::cerr << "epoll_ctl failed: " << strerror(errno) << std::endl;
      abort();
    }
  }
  num_epoll_events_--;
  waiting_coroutines_.erase(c->fd);
}

#else
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
#endif

void CoroutineScheduler::Run() {
  running_ = true;
#if CO_POLL_MODE == CO_POLL_EPOLL
  std::vector<struct epoll_event> epoll_events;
#endif

  // To support both epoll and poll modes we copy the results
  // into a vector of YieldedCoroutine objects.  There are likely to
  // be a small number of events triggering at once, so this is
  // not a big overhead.
  std::vector<YieldedCoroutine> events;

  while (running_) {
    if (coroutines_.empty()) {
      // No coroutines, nothing to do.
      break;
    }

    if (!running_) {
      break;
    }

    int max_fd = -1;

#if CO_POLL_MODE == CO_POLL_EPOLL
    epoll_events.resize(num_epoll_events_);
    int num_ready =
        epoll_wait(epoll_fd_, epoll_events.data(), epoll_events.size(), -1);
    if (num_ready <= 0) {
      continue;
    }
    if (!running_) {
      break;
    }
    // Copy the epoll_events into the events vector,
    // converting the epoll_event to a YieldedCoroutine.

    events.clear();
    events.reserve(num_ready);
    for (int i = 0; i < num_ready; i++) {
      struct epoll_event &event = epoll_events[i];
      if (event.data.fd > max_fd) {
        max_fd = event.data.fd;
      }
      auto it = waiting_coroutines_.find(event.data.fd);
      if (it == waiting_coroutines_.end()) {

        events.push_back({nullptr, event.data.fd, 0});
        continue;
      }
      for (auto &list : it->second) {
        events.push_back(*list);
      }
    }
#else
    // Poll mode.
    BuildPollFds(&poll_state_);
    int num_ready =
        ::poll(poll_state_.pollfds.data(), poll_state_.pollfds.size(), -1);
    if (num_ready <= 0) {
      continue;
    }
    if (!running_) {
      break;
    }
    // Copy all triggered pollfds into the events vector.
    events.clear();
    events.reserve(num_ready);
    for (size_t i = 0; i < poll_state_.pollfds.size(); i++) {
      if (poll_state_.pollfds[i].fd > max_fd) {
        max_fd = poll_state_.pollfds[i].fd;
      }
      if (poll_state_.pollfds[i].revents != 0) {
        if (i == 0) {
          // Interrupt fd.
          YieldedCoroutine c = {nullptr, poll_state_.pollfds[i].fd, 0};
          events.push_back(c);
          continue;
        }
        YieldedCoroutine c = {poll_state_.coroutines[i - 1],
                              poll_state_.pollfds[i].fd, 0};
        events.push_back(c);
      }
    }
#endif

    // Sort the events by the time they have been waiting.
    std::sort(events.begin(), events.end(),
              [](const YieldedCoroutine &a, const YieldedCoroutine &b) {
                if (a.co == nullptr || b.co == nullptr) {
                  return false;
                }
                return a.co->LastTick() < b.co->LastTick();
              });

    // Keep this out of a register.
    volatile int index = 0;

    // We need to keep track of which coroutines have been triggered.
    BitSet triggered(coroutine_ids_.SizeInBits());

    // To keep track of the file descriptors we have already processed.  This is
    // so that we can check that a coroutine that is waiting for the same fd
    // isn't resumed when the fd is no longer ready.
    BitSet processed_fds(max_fd + 1);

    // Build the context/jmp_buf for the yield.  This is where coroutines
    // will yield to.
    GETCONTEXT(yield_);

    // This loop resumes all ready coroutines once each.  It allows
    // the scheduler to scale linearly with the number of ready
    // coroutines.
    for (;;) {
      // We get here on a coroutine yield.  The "index" variable is
      // our current position in the events vector.
      if (!running_) {
        break;
      }
      if (index >= num_ready) {
        break;
      }
      YieldedCoroutine *c = &events[index];
      index++;
      if (c->co == nullptr) {
// Interrupt fd.
#if CO_POLL_MODE == CO_POLL_EPOLL
        ClearEvent(interrupt_fd_);
#else
        ClearEvent(interrupt_fd_.fd);
#endif
        continue;
      }

      tick_count_++;

      if (processed_fds.Contains(c->fd)) {
        // Since we can have more than one coroutine waiting for an fd we need
        // to check that the fd is still ready to prevent blocking.  We only do
        // this if the fd is in blocking mode.
        int e = fcntl(c->fd, F_GETFL, 0);
        if (e != -1 && (e & O_NONBLOCK) == 0) {
          struct pollfd pfd = {c->fd, int16_t(c->events), 0};
          poll(&pfd, 1, 0);
          if ((pfd.revents & (int16_t(c->events) | POLLHUP)) == 0) {
            // Fd is not ready so we would block.  Don't resume the
            // coroutine.
            if (kCoDebug) {
              std::cerr << "Coroutine " << c->co->Name() << " is blocked on fd "
                        << c->fd << std::endl;
            }
            continue;
          }
        }
      }
      processed_fds.Set(c->fd);

      // Clear the event for the corouutine since we will be resuming
      // it.
      c->co->ClearEvent();

      // Only trigger a coroutine once in this loop.
      if (triggered.Contains(c->co->Id())) {
        continue;
      }
      triggered.Set(c->co->Id());

      c->co->Resume(c->fd);
      // Never get here.
    }
    CommitDeletions();
  }
}

#if CO_POLL_MODE == CO_POLL_POLL
void CoroutineScheduler::GetPollState(PollState *poll_state) {
  BuildPollFds(poll_state);
}
#endif

void CoroutineScheduler::AddCoroutine(Coroutine *c) {
  coroutines_.push_back(c);
}

// Removes a coroutine but doesn't destruct it.  The coroutines's id will
// be removed and can be reused immediately after the completion callback
// is called.
void CoroutineScheduler::RemoveCoroutine(const Coroutine *c) {
  deletions_.insert(c);
}

void CoroutineScheduler::CommitDeletions() {
  for (auto *c : deletions_) {
    coroutine_ids_.Free(c->Id());
    last_freed_coroutine_id_ = c->Id();

    for (auto it = coroutines_.begin(); it != coroutines_.end(); it++) {
      if (*it == c) {
        coroutines_.erase(it);
        // Call completion callback to allow for external memory management.
        if (completion_callback_ != nullptr) {
          completion_callback_(const_cast<Coroutine *>(c));
        }
        break;
      }
    }
  }
  deletions_.clear();
}

uint32_t CoroutineScheduler::AllocateId() {
  uint32_t id;
  if (last_freed_coroutine_id_ != -1U) {
    id = last_freed_coroutine_id_;
    last_freed_coroutine_id_ = -1U;
    coroutine_ids_.Set(id);
  } else {
    id = coroutine_ids_.Allocate();
  }
  return id;
}

void CoroutineScheduler::Stop() {
  running_ = false;
#if CO_POLL_MODE == CO_POLL_EPOLL
  TriggerEvent(interrupt_fd_);
#else
  TriggerEvent(interrupt_fd_.fd);
#endif
}

void CoroutineScheduler::Show() {
  for (auto *co : coroutines_) {
    co->Show();
  }
}

std::vector<std::string> CoroutineScheduler::AllCoroutineStrings() const {
  std::vector<std::string> r;
  for (auto *co : coroutines_) {
    r.emplace_back(co->ToString());
  }
  return r;
}

std::vector<int> CoroutineScheduler::GetAllFds() const {
  std::vector<int> fds;
#if CO_POLL_MODE == CO_POLL_EPOLL
  fds.push_back(epoll_fd_);
  fds.push_back(interrupt_fd_);
#else
  fds.push_back(interrupt_fd_.fd);
#endif
  for (auto *co : coroutines_) {
    co->GetAllFds(fds);
  }
  return fds;
}

} // namespace co
