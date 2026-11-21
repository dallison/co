#include "absl/status/status_matchers.h"
#include "co/coroutine.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if CO_HAVE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#define VAR(a) a##__COUNTER__
#define EVAL_AND_ASSERT_OK(expr) EVAL_AND_ASSERT_OK2(VAR(r_), expr)

#define EVAL_AND_ASSERT_OK2(result, expr)                                      \
  ({                                                                           \
    auto result = (expr);                                                      \
    if (!result.ok()) {                                                        \
      std::cerr << result.status() << std::endl;                               \
    }                                                                          \
    ASSERT_OK(result);                                                         \
    std::move(*result);                                                        \
  })

#define ASSERT_OK(e) ASSERT_THAT(e, ::absl_testing::IsOk())

class BorrowedInterruptFdCoroutine : public co::Coroutine {
public:
  BorrowedInterruptFdCoroutine(co::CoroutineScheduler &scheduler,
                               int interrupt_fd)
      : Coroutine(scheduler, [](co::Coroutine *) {}, "", -1) {
    interrupt_fd_ = interrupt_fd;
    interrupt_fd_owned_ = false;
  }
};

TEST(CoroutineTest, Basic) {
  co::CoroutineScheduler scheduler;
  co::Coroutine c1(scheduler, [](co::Coroutine *c) {
    for (int i = 0; i < 10; i++) {
      std::cerr << "yielding " << i << std::endl;
      c->Yield();
    }
  });
  scheduler.Run();
  std::cerr << "done" << std::endl;
}

TEST(CoroutineTest, Loop) {
  co::CoroutineScheduler scheduler;
  std::vector<std::unique_ptr<co::Coroutine>> coroutines;
  for (int i = 0; i < 10; i++) {
    coroutines.push_back(
        std::make_unique<co::Coroutine>(scheduler, [](co::Coroutine *c) {
          for (int i = 0; i < 10; i++) {
            c->Yield();
          }
        }));
  }
  scheduler.Run();
}

TEST(CoroutineTest, InterruptFdsAreClosedOnDestruction) {
  int interrupt_pipe[2];
  ASSERT_EQ(0, pipe(interrupt_pipe));

  std::vector<int> duplicated_fds;
  {
    co::CoroutineScheduler scheduler;
    co::CoroutineOptions options;
    options.interrupt_fd = interrupt_pipe[0];

    for (int i = 0; i < 32; ++i) {
      co::Coroutine *coroutine = scheduler.Spawn([]() {}, options);
      int duplicated_fd = coroutine->GetInterruptFd();
      ASSERT_GE(duplicated_fd, 0);
      EXPECT_NE(interrupt_pipe[0], duplicated_fd);
      EXPECT_NE(-1, fcntl(duplicated_fd, F_GETFD));
      duplicated_fds.push_back(duplicated_fd);
    }

    scheduler.Run();

    for (int duplicated_fd : duplicated_fds) {
      errno = 0;
      EXPECT_EQ(-1, fcntl(duplicated_fd, F_GETFD));
      EXPECT_EQ(EBADF, errno);
    }

    // The coroutine owns only its duplicate, not the caller's descriptor.
    EXPECT_NE(-1, fcntl(interrupt_pipe[0], F_GETFD));
  }

  EXPECT_EQ(0, close(interrupt_pipe[0]));
  EXPECT_EQ(0, close(interrupt_pipe[1]));
}

TEST(CoroutineTest, BorrowedInterruptFdIsNotClosedOnDestruction) {
  int interrupt_pipe[2];
  ASSERT_EQ(0, pipe(interrupt_pipe));

  {
    co::CoroutineScheduler scheduler;
    {
      BorrowedInterruptFdCoroutine coroutine(scheduler, interrupt_pipe[0]);
      EXPECT_EQ(interrupt_pipe[0], coroutine.GetInterruptFd());
      scheduler.Run();
    }

    EXPECT_NE(-1, fcntl(interrupt_pipe[0], F_GETFD));
  }

  EXPECT_EQ(0, close(interrupt_pipe[0]));
  EXPECT_EQ(0, close(interrupt_pipe[1]));
}

TEST(CoroutineTest, Sleep) {
  co::CoroutineScheduler scheduler;
  co::Coroutine c1(scheduler, [](co::Coroutine *c) {
    for (int i = 0; i < 10; i++) {
      std::cerr << "Sleeping " << i << std::endl;
      c->Millisleep(100);
    }
  });
  scheduler.Run();
  std::cerr << "done" << std::endl;
}

TEST(CoroutinesTest, Wait) {
  co::CoroutineScheduler scheduler;
  int pipes[2];

  std::string result;

  ASSERT_EQ(0, pipe(pipes));
  co::Coroutine reader(scheduler, [&pipes, &result](co::Coroutine *c) {
    for (;;) {
      int fd = c->Wait(pipes[0], POLLIN);
      ASSERT_EQ(pipes[0], fd);
      char buf[1];
      ssize_t n = read(pipes[0], buf, 1);
      if (n == 0) {
        break;
      }
      ASSERT_EQ(1, n);
      result += buf[0];
    }
    (void)close(pipes[0]);
  });

  co::Coroutine writer(scheduler, [&pipes](co::Coroutine *c) {
    for (int i = 0; i < 10; i++) {
      int fd = c->Wait(pipes[1], POLLOUT);
      ASSERT_EQ(pipes[1], fd);
      char buf[1] = {char('A' + i)};
      ssize_t n = write(pipes[1], buf, 1);
      ASSERT_EQ(1, n);
    }
    (void)close(pipes[1]);
  });

  scheduler.Run();

  ASSERT_EQ("ABCDEFGHIJ", result);
}

#if CO_POLL_MODE == CO_POLL_EPOLL
// This test is only deterministic in EPOLL mode because ::poll randomly
// chooses a pollfd if more than one it ready.  ::epoll doesn't
// allow multiple fds to be added to the epoll fd so we keep track of
// the coroutines ourselves.
TEST(CoroutinesTest, MultipleFd) {
  co::CoroutineScheduler scheduler;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  // This will run first.
  co::Coroutine c1(
      scheduler,
      [pipes](co::Coroutine *c) {
        ASSERT_EQ(pipes[0], c->Wait(pipes[0], POLLIN));
        std::cerr << "foo woke up\n";
        char buf;
        ASSERT_EQ(1, ::read(pipes[0], &buf, 1));
      },
      "foo");

  // This will run second.
  co::Coroutine c2(
      scheduler,
      [pipes](co::Coroutine *c) {
        // Waiting on the same fd is supported.
        auto fd = c->Wait(pipes[0], POLLIN);
        if (fd != pipes[0]) {
          std::cerr << "bar done with fd " << fd << "\n";
          return;
        }
        std::cerr << "bar woke up\n";
        // We've closed the pipe, so this will get EOF.
        char buf;
        ASSERT_EQ(0, ::read(pipes[0], &buf, 1));
      },
      "bar");

  // After c1 and c2 we will run this and it will wake up c1.
  co::Coroutine c3(
      scheduler,
      [pipes](co::Coroutine *c) {
        // This will wake up foo but not bar.
        char buf = 'x';
        ASSERT_EQ(1, ::write(pipes[1], &buf, 1));
        c->Yield();
        std::cerr << "closing pipe\n";
        // This will wake bar.
        close(pipes[1]);
      },
      "baz");
  scheduler.Run();
  close(pipes[0]);
}

#endif

TEST(CoroutinesTest, NonInvasive) {
  co::CoroutineScheduler scheduler;
  scheduler.Spawn([]() {
    std::cerr << "coroutine running\n";
    co::Yield();
    std::cerr << "coroutine exiting\n";
  });

  scheduler.Run();
}

TEST(CoroutinesTest, NonInvasiveWaitWithName) {
  co::CoroutineScheduler scheduler;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  scheduler.Spawn(
      [pipes]() {
        std::cerr << "coroutine running\n";
        int fd = co::Wait(pipes[0]);
        ASSERT_EQ(pipes[0], fd);
        char c;
        ASSERT_EQ(1, read(fd, &c, 1));
        ASSERT_EQ('a', c);
        std::cerr << "coroutine exiting\n";
      },
      {.name = "foo"});

  scheduler.Spawn([pipes]() {
    char c = 'a';
    ASSERT_EQ(1, write(pipes[1], &c, 1));
  });
  scheduler.Run();
  close(pipes[0]);
  close(pipes[1]);
}

TEST(CoroutinesTest, NonInvasiveWaitWithTimeout) {
  co::CoroutineScheduler scheduler;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  // Will timeout.
  scheduler.Spawn(
      [pipes]() {
        std::cerr << "coroutine running\n";
        int fd = co::Wait(pipes[0], std::chrono::milliseconds(100));
        ASSERT_EQ(-1, fd);
        std::cerr << "coroutine exiting\n";
      },
      {.name = "bar"});

  scheduler.Run();
  close(pipes[0]);
  close(pipes[1]);
}

TEST(CoroutinesTest, NonInvasive2) {
  co::CoroutineScheduler scheduler;
  for (int i = 0; i < 10; i++) {
    scheduler.Spawn([i]() {
      std::cerr << "coroutine " << i << " running\n";
      co::Millisleep(100);
      std::cerr << "coroutine " << i << " exiting\n";
    });
  }

  scheduler.Run();
}

TEST(CoroutinesTest, NonInvasiveTemplated) {
  co::CoroutineScheduler scheduler;
  for (int i = 0; i < 10; i++) {
    scheduler.Spawn([i]() {
      std::cerr << "coroutine " << i << " running\n";
      co::Sleep(std::chrono::milliseconds(100));
      std::cerr << "coroutine " << i << " exiting\n";
    });
  }

  scheduler.Run();
}

TEST(CoroutinesTest, AbortWait) {
  co::CoroutineScheduler scheduler;
  scheduler.SetAbortOnStop(true);

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool aborted = false;
  scheduler.Spawn([pipes, &aborted]() {
    for (;;) {
      try {
        std::cerr << "coroutine running\n";
        [[maybe_unused]] int fd = co::Wait(pipes[0]);
        FAIL() << "coroutine should have been aborted";
      } catch (...) {
        std::cerr << "wait aborted\n";
        aborted = true;
      }
    }
  });

  scheduler.Spawn([&scheduler]() {
    co::Sleep(std::chrono::milliseconds(100));
    std::cerr << "stopping scheduler\n";
    scheduler.Stop();
  });

  scheduler.Run();
  close(pipes[0]);
  close(pipes[1]);
  ASSERT_TRUE(aborted);
}

TEST(CoroutinesTest, AbortDestructor) {
  co::CoroutineScheduler scheduler;
  scheduler.SetAbortOnStop(true);

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  struct Var {
    Var(const char *n, bool &a) : name(n), aborted(a) {}
    ~Var() {
      aborted = true;
      std::cerr << "variable " << name << " destructed\n";
    }
    const char *name;
    bool &aborted;
  };

  bool aborted1 = false;
  bool aborted2 = false;
  scheduler.Spawn([pipes, &aborted1, &aborted2]() {
    Var v1("v1", aborted1);
    for (;;) {
      // This must get destructed when the coroutine is aborted.
      Var v2("v2", aborted2);
      std::cerr << "coroutine running\n";
      [[maybe_unused]] int fd = co::Wait(pipes[0]);
      FAIL() << "coroutine should have been aborted";
    }
  });

  scheduler.Spawn([&scheduler]() {
    co::Sleep(std::chrono::milliseconds(100));
    std::cerr << "stopping scheduler\n";
    scheduler.Stop();
  });

  scheduler.Run();
  close(pipes[0]);
  close(pipes[1]);
  ASSERT_TRUE(aborted1);
  ASSERT_TRUE(aborted2);
}

TEST(CoroutinesTest, AbortSleep) {
  co::CoroutineScheduler scheduler;
  scheduler.SetAbortOnStop(true);

  bool aborted = false;
  scheduler.Spawn([&aborted]() {
    try {
      std::cerr << "coroutine running\n";
      co::Sleep(std::chrono::seconds(10));
      FAIL() << "coroutine should have been aborted";
    } catch (...) {
      std::cerr << "coroutine aborted\n";
      aborted = true;
    }
  });

  scheduler.Spawn([&scheduler]() {
    co::Sleep(std::chrono::milliseconds(100));
    std::cerr << "stopping scheduler\n";
    scheduler.Stop();
  });

  scheduler.Run();
  ASSERT_TRUE(aborted);
}

TEST(CoroutinesTest, AbortYield) {
  co::CoroutineScheduler scheduler;
  scheduler.SetAbortOnStop(true);

  bool aborted = false;
  scheduler.Spawn([&aborted]() {
    try {
      std::cerr << "coroutine running\n";
      for (;;) {
        co::Yield();
      }
      FAIL() << "coroutine should have been aborted";
    } catch (...) {
      std::cerr << "coroutine aborted\n";
      aborted = true;
    }
  });

  scheduler.Spawn([&scheduler]() {
    co::Sleep(std::chrono::milliseconds(100));
    std::cerr << "stopping scheduler\n";
    scheduler.Stop();
  });

  scheduler.Run();
  ASSERT_TRUE(aborted);
}

TEST(CoroutinesTest, AbortSingle) {
  co::CoroutineScheduler scheduler;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  scheduler.SetAbortOnStop(true);

  bool aborted = false;
  const co::Coroutine *c;
  scheduler.Spawn([pipes, &aborted, &c]() {
    c = co::self;
    for (;;) {
      try {
        std::cerr << "coroutine running\n";
        [[maybe_unused]] int fd = co::Wait(pipes[0]);
        FAIL() << "coroutine should have been aborted";
      } catch (...) {
        std::cerr << "wait aborted\n";
        aborted = true;
      }
    }
  });

  // Abort the other coroutine which is stored in 'c'.
  scheduler.Spawn([&c]() {
    co::Sleep(std::chrono::milliseconds(100));
    c->Abort();
  });

  scheduler.Run();
  close(pipes[0]);
  close(pipes[1]);
  ASSERT_TRUE(aborted);
}

TEST(CoroutinesTest, AbortNestedShutdown) {
  co::CoroutineScheduler scheduler;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  scheduler.SetAbortOnStop(true);
  scheduler.Spawn([&scheduler, pipes]() {
    scheduler.Spawn([&scheduler]() {
      co::Sleep(std::chrono::milliseconds(200));
      scheduler.Stop();
    });

    std::cerr << "coroutine running\n";
    [[maybe_unused]] int fd = co::Wait(pipes[0]);
  });

  scheduler.Run();
  close(pipes[0]);
  close(pipes[1]);
}

TEST(CoroutinesTest, StopBeforeRunIsNotLost) {
  co::CoroutineScheduler scheduler;

  bool ran = false;
  scheduler.Spawn([&ran]() { ran = true; });
  scheduler.Stop();
  scheduler.Run();

  EXPECT_FALSE(ran);
}

// A consumed stop applies to one run only; the next run must execute normally.
TEST(CoroutinesTest, StopDoesNotLeakIntoALaterRun) {
  co::CoroutineScheduler scheduler;
  scheduler.Stop();
  scheduler.Run();

  bool ran = false;
  scheduler.Spawn([&ran]() { ran = true; });
  scheduler.Run();
  EXPECT_TRUE(ran);
}

namespace {

constexpr int kGuardSignalDetectedExitCode = 42;
constexpr int kGuardSignalMismatchExitCode = 99;
constexpr int kGuardNoOverflowExitCode = 100;

CO_DISABLE_ADDRESS_SANITIZER
CO_DISABLE_THREAD_SANITIZER
void GuardPageSignalHandler(int /*sig*/, siginfo_t *info, void * /*ucontext*/) {
  if (info != nullptr && co::self != nullptr &&
      co::self->IsInGuardPage(info->si_addr)) {
    _exit(kGuardSignalDetectedExitCode);
  }
  _exit(kGuardSignalMismatchExitCode);
}

CO_DISABLE_ADDRESS_SANITIZER
CO_DISABLE_THREAD_SANITIZER
__attribute__((noinline))
int OverflowCoroutineStack(int depth) {
  if (depth < 0) {
    return 0;
  }
  // Small per-frame allocation so SP steps directly into the guard page
  // rather than jumping over it.
  volatile char buf[256];
  buf[0] = static_cast<char>(depth & 0x7f);
  buf[sizeof(buf) - 1] = static_cast<char>((depth + 1) & 0x7f);
  asm volatile("" : : "r"(buf) : "memory");
  const int sum = static_cast<int>(buf[0]) +
                  static_cast<int>(buf[sizeof(buf) - 1]);
  return sum + OverflowCoroutineStack(depth + 1);
}

int WaitForChild(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) == -1) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return status;
}

bool SkipIntentionalFaultTest() {
#if defined(CO_ADDRESS_SANITIZER) || defined(CO_THREAD_SANITIZER)
  return true;
#elif CO_HAVE_VALGRIND
  return RUNNING_ON_VALGRIND != 0;
#else
  return false;
#endif
}

} // namespace

TEST(CoroutinesTest, GuardPageDisabledByDefault) {
  co::CoroutineScheduler scheduler;
  EXPECT_FALSE(scheduler.GuardPagesEnabled());

  co::Coroutine *spawned = scheduler.Spawn([]() {});
  EXPECT_FALSE(spawned->HasGuardPage());
  EXPECT_EQ(nullptr, spawned->GetGuardPage());
  EXPECT_EQ(0u, spawned->GetGuardPageSize());
  EXPECT_FALSE(spawned->IsInGuardPage(nullptr));
  EXPECT_EQ(co::kCoDefaultStackSize, spawned->GetStackSize());

  co::Coroutine direct(scheduler, [](co::Coroutine *) {});
  EXPECT_FALSE(direct.HasGuardPage());
  EXPECT_EQ(nullptr, direct.GetGuardPage());
  EXPECT_EQ(0u, direct.GetGuardPageSize());

  scheduler.Run();
}

TEST(CoroutinesTest, GuardPageEnabledViaOptions) {
  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  ASSERT_GT(page_size, 0u);

  co::CoroutineScheduler scheduler;
  co::CoroutineOptions opts;
  opts.name = "guarded";
  opts.stack_size = 10000; // Deliberately not a multiple of page_size.
  opts.guard_page = true;

  int steps = 0;
  co::Coroutine *c = scheduler.Spawn(
      [&steps]() {
        for (int i = 0; i < 5; ++i) {
          ++steps;
          co::Yield();
        }
      },
      opts);

  EXPECT_TRUE(c->HasGuardPage());
  ASSERT_NE(nullptr, c->GetGuardPage());
  EXPECT_EQ(page_size, c->GetGuardPageSize());
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(c->GetGuardPage()) % page_size);
  EXPECT_GE(c->GetStackSize(), opts.stack_size);
  EXPECT_EQ(0u, c->GetStackSize() % page_size);

  const char *guard_base = static_cast<const char *>(c->GetGuardPage());
  EXPECT_TRUE(c->IsInGuardPage(guard_base));
  EXPECT_TRUE(c->IsInGuardPage(guard_base + page_size - 1));
  EXPECT_FALSE(c->IsInGuardPage(guard_base + page_size));
  EXPECT_FALSE(c->IsInGuardPage(guard_base - 1));
  EXPECT_FALSE(c->IsInGuardPage(nullptr));

  scheduler.Run();
  EXPECT_EQ(5, steps);
  EXPECT_EQ(nullptr, co::self);
}

TEST(CoroutinesTest, GuardPageMultipleCoroutinesCleanup) {
  co::CoroutineScheduler scheduler;
  scheduler.EnableGuardPages(true);

  int completed = 0;
  for (int i = 0; i < 64; ++i) {
    co::Coroutine *c = scheduler.Spawn([&completed]() {
      co::Yield();
      ++completed;
    });
    ASSERT_TRUE(c->HasGuardPage());
  }

  scheduler.Run();
  EXPECT_EQ(64, completed);
  EXPECT_EQ(nullptr, co::self);
}

TEST(CoroutinesTest, GuardPageEnabledViaSchedulerAndGenerator) {
  co::CoroutineScheduler scheduler;
  scheduler.EnableGuardPages(true);
  EXPECT_TRUE(scheduler.GuardPagesEnabled());

  int sum = 0;
  co::Generator<int> gen(scheduler, [](co::Generator<int> *g) {
    for (int i = 1; i <= 3; ++i) {
      g->YieldValue(i);
    }
  });
  EXPECT_TRUE(gen.HasGuardPage());

  co::Coroutine *consumer = scheduler.Spawn([&gen, &sum]() {
    while (gen.IsAlive()) {
      int v = co::self->Call(gen);
      if (gen.IsAlive()) {
        sum += v;
      }
    }
  });
  EXPECT_TRUE(consumer->HasGuardPage());

  scheduler.EnableGuardPages(false);
  EXPECT_FALSE(scheduler.GuardPagesEnabled());

  co::Coroutine *unguarded = scheduler.Spawn([]() {});
  EXPECT_FALSE(unguarded->HasGuardPage());

  // Direct constructor and Generator with explicit guard_page = true when
  // scheduler default is off.
  co::Coroutine explicit_guarded(scheduler, [](co::Coroutine *) {}, "explicit",
                                 -1, true, co::kCoDefaultStackSize, nullptr,
                                 /*guard_page=*/true);
  EXPECT_TRUE(explicit_guarded.HasGuardPage());

  co::Generator<int> explicit_gen(
      scheduler, [](co::Generator<int> *g) { g->YieldValue(7); },
      "explicit_gen", -1, co::kCoDefaultStackSize, nullptr,
      /*guard_page=*/true);
  EXPECT_TRUE(explicit_gen.HasGuardPage());

  int explicit_val = 0;
  scheduler.Spawn([&explicit_gen, &explicit_val]() {
    while (explicit_gen.IsAlive()) {
      int v = co::self->Call(explicit_gen);
      if (explicit_gen.IsAlive()) {
        explicit_val += v;
      }
    }
  });

  scheduler.Run();
  EXPECT_EQ(6, sum);
  EXPECT_EQ(7, explicit_val);
}

TEST(CoroutinesTest, GuardPageOverflowDetectedBySignalHandler) {
  if (SkipIntentionalFaultTest()) {
    GTEST_SKIP()
        << "Sanitizers/Valgrind intercept intentional guard page faults";
  }

  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);

    const size_t alt_stack_size =
        std::max<size_t>(static_cast<size_t>(SIGSTKSZ), 64 * 1024);
    std::vector<char> alt_stack_mem(alt_stack_size);
    stack_t ss = {};
    ss.ss_sp = alt_stack_mem.data();
    ss.ss_size = alt_stack_mem.size();
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
      _exit(101);
    }

    struct sigaction sa = {};
    sa.sa_sigaction = GuardPageSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &sa, nullptr) != 0 ||
        sigaction(SIGBUS, &sa, nullptr) != 0) {
      _exit(102);
    }

    co::CoroutineScheduler scheduler;
    co::CoroutineOptions opts;
    opts.name = "overflower";
    opts.stack_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    opts.guard_page = true;

    scheduler.Spawn(
        []() {
          volatile int result = OverflowCoroutineStack(0);
          (void)result;
        },
        opts);
    scheduler.Run();
    _exit(kGuardNoOverflowExitCode);
  }

  const int status = WaitForChild(pid);
  ASSERT_NE(-1, status);
  ASSERT_TRUE(WIFEXITED(status))
      << "Child did not exit cleanly (signaled=" << WIFSIGNALED(status)
      << ", term_sig=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ")";
  EXPECT_EQ(kGuardSignalDetectedExitCode, WEXITSTATUS(status));
}

TEST(CoroutinesTest, GuardPageOverflowTerminatesWithSignalByDefault) {
  if (SkipIntentionalFaultTest()) {
    GTEST_SKIP()
        << "Sanitizers/Valgrind intercept intentional guard page faults";
  }

  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);

    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);

    co::CoroutineScheduler scheduler;
    co::CoroutineOptions opts;
    opts.name = "overflower_default";
    opts.stack_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    opts.guard_page = true;

    scheduler.Spawn(
        []() {
          volatile int result = OverflowCoroutineStack(0);
          (void)result;
        },
        opts);
    scheduler.Run();
    _exit(kGuardNoOverflowExitCode);
  }

  const int status = WaitForChild(pid);
  ASSERT_NE(-1, status);
  ASSERT_TRUE(WIFSIGNALED(status))
      << "Child was not terminated by signal (exited=" << WIFEXITED(status)
      << ", exit_code=" << (WIFEXITED(status) ? WEXITSTATUS(status) : 0) << ")";
  const int sig = WTERMSIG(status);
  EXPECT_TRUE(sig == SIGSEGV || sig == SIGBUS) << "Unexpected signal: " << sig;
}

namespace {

constexpr int kCustomCallbackExitCode = 43;
constexpr int kPriorHandlerExitCode = 77;

CO_DISABLE_ADDRESS_SANITIZER
CO_DISABLE_THREAD_SANITIZER
void CustomOverflowCallback(const co::Coroutine *co, void *fault_addr) {
  if (co != nullptr && co->IsInGuardPage(fault_addr) &&
      std::strcmp(co->Name().c_str(), "cb_overflow_co") == 0) {
    _exit(kCustomCallbackExitCode);
  }
  _exit(kGuardSignalMismatchExitCode);
}

CO_DISABLE_ADDRESS_SANITIZER
CO_DISABLE_THREAD_SANITIZER
void PriorSegvHandler(int /*sig*/, siginfo_t * /*info*/, void * /*ucontext*/) {
  _exit(kPriorHandlerExitCode);
}

} // namespace

TEST(CoroutinesTest, InstallAndUninstallStackOverflowHandler) {
  ASSERT_TRUE(co::InstallStackOverflowHandler());
  // Re-installing is idempotent.
  ASSERT_TRUE(co::InstallStackOverflowHandler());

  co::CoroutineScheduler scheduler;
  scheduler.EnableGuardPages(true);
  bool ran = false;
  scheduler.Spawn([&ran]() {
    co::Yield();
    ran = true;
  });
  scheduler.Run();
  EXPECT_TRUE(ran);

  ASSERT_TRUE(co::UninstallStackOverflowHandler());
  // Uninstalling again is idempotent.
  ASSERT_TRUE(co::UninstallStackOverflowHandler());
}

TEST(CoroutinesTest, StackOverflowHandlerDefaultReport) {
  if (SkipIntentionalFaultTest()) {
    GTEST_SKIP()
        << "Sanitizers/Valgrind intercept intentional guard page faults";
  }

  int err_pipe[2];
  ASSERT_EQ(0, pipe(err_pipe));

  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    close(err_pipe[0]);
    dup2(err_pipe[1], STDERR_FILENO);
    close(err_pipe[1]);

    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);

    if (!co::InstallStackOverflowHandler()) {
      _exit(103);
    }

    co::CoroutineScheduler scheduler;
    co::CoroutineOptions opts;
    opts.name = "my_overflow_co";
    opts.stack_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    opts.guard_page = true;

    scheduler.Spawn(
        []() {
          volatile int result = OverflowCoroutineStack(0);
          (void)result;
        },
        opts);
    scheduler.Run();
    _exit(kGuardNoOverflowExitCode);
  }

  close(err_pipe[1]);
  std::string stderr_output;
  char buf[512];
  ssize_t n = 0;
  while ((n = read(err_pipe[0], buf, sizeof(buf))) > 0) {
    stderr_output.append(buf, static_cast<size_t>(n));
  }
  close(err_pipe[0]);

  const int status = WaitForChild(pid);
  ASSERT_NE(-1, status);
  ASSERT_TRUE(WIFSIGNALED(status))
      << "Child was not terminated by signal (exited=" << WIFEXITED(status)
      << ", exit_code=" << (WIFEXITED(status) ? WEXITSTATUS(status) : 0)
      << "), stderr=" << stderr_output;
  const int sig = WTERMSIG(status);
  EXPECT_TRUE(sig == SIGSEGV || sig == SIGBUS) << "Unexpected signal: " << sig;

  EXPECT_NE(std::string::npos,
            stderr_output.find("Coroutine stack overflow detected"))
      << stderr_output;
  EXPECT_NE(std::string::npos, stderr_output.find("\"my_overflow_co\""))
      << stderr_output;
  EXPECT_NE(std::string::npos, stderr_output.find("stack size:"))
      << stderr_output;
  EXPECT_NE(std::string::npos, stderr_output.find("guard page:"))
      << stderr_output;
  EXPECT_NE(std::string::npos, stderr_output.find("fault address:"))
      << stderr_output;
}

TEST(CoroutinesTest, StackOverflowHandlerCustomCallback) {
  if (SkipIntentionalFaultTest()) {
    GTEST_SKIP()
        << "Sanitizers/Valgrind intercept intentional guard page faults";
  }

  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);

    if (!co::InstallStackOverflowHandler(CustomOverflowCallback)) {
      _exit(103);
    }

    co::CoroutineScheduler scheduler;
    co::CoroutineOptions opts;
    opts.name = "cb_overflow_co";
    opts.stack_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    opts.guard_page = true;

    scheduler.Spawn(
        []() {
          volatile int result = OverflowCoroutineStack(0);
          (void)result;
        },
        opts);
    scheduler.Run();
    _exit(kGuardNoOverflowExitCode);
  }

  const int status = WaitForChild(pid);
  ASSERT_NE(-1, status);
  ASSERT_TRUE(WIFEXITED(status))
      << "Child did not exit cleanly (signaled=" << WIFSIGNALED(status)
      << ", term_sig=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ")";
  EXPECT_EQ(kCustomCallbackExitCode, WEXITSTATUS(status));
}

TEST(CoroutinesTest, StackOverflowHandlerChainsNonGuardFaults) {
  if (SkipIntentionalFaultTest()) {
    GTEST_SKIP()
        << "Sanitizers/Valgrind intercept intentional guard page faults";
  }

  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);

    struct sigaction prev_sa = {};
    prev_sa.sa_sigaction = PriorSegvHandler;
    sigemptyset(&prev_sa.sa_mask);
    prev_sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &prev_sa, nullptr) != 0 ||
        sigaction(SIGBUS, &prev_sa, nullptr) != 0) {
      _exit(104);
    }

    if (!co::InstallStackOverflowHandler(CustomOverflowCallback)) {
      _exit(105);
    }

    // Trigger a fault on an unrelated PROT_NONE page (not a coroutine guard
    // page) while inside a guarded coroutine; the prior handler must run.
    const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
    const int map_anon = MAP_ANON;
#else
    const int map_anon = MAP_ANONYMOUS;
#endif
    void *unrelated =
        mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | map_anon, -1, 0);
    if (unrelated == MAP_FAILED) {
      _exit(106);
    }

    co::CoroutineScheduler scheduler;
    co::CoroutineOptions opts;
    opts.name = "non_guard_fault_co";
    opts.guard_page = true;

    scheduler.Spawn(
        [unrelated]() {
          volatile char *p = static_cast<volatile char *>(unrelated);
          *p = 1;
        },
        opts);
    scheduler.Run();
    _exit(kGuardNoOverflowExitCode);
  }

  const int status = WaitForChild(pid);
  ASSERT_NE(-1, status);
  ASSERT_TRUE(WIFEXITED(status))
      << "Child did not exit cleanly (signaled=" << WIFSIGNALED(status)
      << ", term_sig=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ")";
  EXPECT_EQ(kPriorHandlerExitCode, WEXITSTATUS(status));
}
