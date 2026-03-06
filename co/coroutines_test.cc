#include "absl/status/status_matchers.h"
#include "co/coroutine.h"
#include <gtest/gtest.h>

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
