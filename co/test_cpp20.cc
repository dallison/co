// Test file for C++20 coroutine library

#include "coroutine_cpp20.h"
#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

#if CO20_HAVE_COROUTINES

namespace co20 {

TEST(Cpp20, Basic) {
  Scheduler scheduler;
  bool ran = false;
  
  scheduler.Spawn([&ran](Coroutine& co) -> Task {
    ran = true;
    co_return;
  }, "test");
  
  scheduler.Run();
  
  EXPECT_TRUE(ran);
}

TEST(Cpp20, Yield) {
  Scheduler scheduler;
  int count = 0;
  
  scheduler.Spawn([&count](Coroutine& co) -> Task {
    count++;
    co_await co.Yield();
    count++;
    co_return;
  }, "test");
  
  scheduler.Run();
  
  EXPECT_EQ(2, count);
}

TEST(Cpp20, Wait) {
  Scheduler scheduler;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  
  std::string result;
  bool reader_done = false;
  
  scheduler.Spawn([&pipes, &result, &reader_done](Coroutine& co) -> Task {
    for (;;) {
      int fd = co_await co.Wait(pipes[0], POLLIN);
      if (fd != pipes[0]) break;
      
      char buf[1];
      ssize_t n = read(pipes[0], buf, 1);
      if (n == 0) break;
      if (n == 1) {
        result += buf[0];
      }
    }
    
    close(pipes[0]);
    reader_done = true;
    co_return;
  }, "reader");
  
  scheduler.Spawn([&pipes](Coroutine& co) -> Task {
    for (int i = 0; i < 10; i++) {
      int fd = co_await co.Wait(pipes[1], POLLOUT);
      if (fd != pipes[1]) {
        // Error - should not happen in this test
        co_return;
      }
      
      char buf[1] = {char('A' + i)};
      write(pipes[1], buf, 1);
    }
    
    close(pipes[1]);
    co_return;
  }, "writer");
  
  scheduler.Run();
  
  EXPECT_TRUE(reader_done);
  EXPECT_EQ("ABCDEFGHIJ", result);
}

TEST(Cpp20, Sleep) {
  Scheduler scheduler;
  bool slept = false;
  
  scheduler.Spawn([&slept](Coroutine& co) -> Task {
    co_await co.Sleep(1000000); // 1ms
    slept = true;
    co_return;
  }, "sleep_test");
  
  scheduler.Run();
  
  EXPECT_TRUE(slept);
}

TEST(Cpp20, Loop) {
  Scheduler scheduler;
  
  // Create 10 coroutines, each yielding 10 times
  for (int i = 0; i < 10; i++) {
    scheduler.Spawn([i](Coroutine& co) -> Task {
      for (int j = 0; j < 10; j++) {
        co_await co.Yield();
      }
      co_return;
    }, "loop_coroutine_" + std::to_string(i));
  }
  
  scheduler.Run();
}

#if CO_POLL_MODE == CO_POLL_EPOLL
// This test is only deterministic in EPOLL mode because ::poll randomly
// chooses a pollfd if more than one is ready.  ::epoll doesn't
// allow multiple fds to be added to the epoll fd so we keep track of
// the coroutines ourselves.
TEST(Cpp20, MultipleFd) {
  Scheduler scheduler;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  struct TestState {
    bool foo_woke = false;
    bool bar_woke = false;
    int foo_read_result = -1;
    int bar_read_result = -1;
    int write_result = -1;
  } state;

  // This will run first.
  scheduler.Spawn([pipes, &state](Coroutine& co) -> Task {
    int fd = co_await co.Wait(pipes[0], POLLIN);
    if (fd != pipes[0]) {
      co_return;
    }
    state.foo_woke = true;
    char buf;
    state.foo_read_result = ::read(pipes[0], &buf, 1);
    co_return;
  }, "foo");

  // This will run second.
  scheduler.Spawn([pipes, &state](Coroutine& co) -> Task {
    // Waiting on the same fd is supported.
    int fd = co_await co.Wait(pipes[0], POLLIN);
    if (fd != pipes[0]) {
      // Should not happen in this test
      co_return;
    }
    state.bar_woke = true;
    // We've closed the pipe, so this will get EOF.
    char buf;
    state.bar_read_result = ::read(pipes[0], &buf, 1);
    co_return;
  }, "bar");

  // After c1 and c2 we will run this and it will wake up c1.
  scheduler.Spawn([pipes, &state](Coroutine& co) -> Task {
    // This will wake up foo but not bar.
    char buf = 'x';
    state.write_result = ::write(pipes[1], &buf, 1);
    co_await co.Yield();
    // This will wake bar.
    close(pipes[1]);
    co_return;
  }, "baz");

  scheduler.Run();
  close(pipes[0]);

  EXPECT_TRUE(state.foo_woke);
  EXPECT_TRUE(state.bar_woke);
  EXPECT_EQ(1, state.foo_read_result);
  EXPECT_EQ(0, state.bar_read_result);
  EXPECT_EQ(1, state.write_result);
}

TEST(Cpp20, AbortYield) {
  Scheduler scheduler;

  struct TestState {
    bool aborted = false;
    Coroutine* coroutine_ptr = nullptr;
  } state;

  scheduler.Spawn([&state](Coroutine& co) -> Task {
    state.coroutine_ptr = &co;
    try {
      for (;;) {
        co_await co.Sleep(1000000000); // 1 second
      }
    } catch (...) {
      state.aborted = true;
    }
    co_return;
  }, "sleeping_coroutine");

  scheduler.Spawn([&state](Coroutine& co) -> Task {
    co_await co.Sleep(100000000); // 100ms
    while (!state.coroutine_ptr) {
      co_await co.Yield();
    }
    co_await co.Yield();
    if (state.coroutine_ptr) {
      state.coroutine_ptr->Abort();
    }
    co_return;
  }, "abort_coroutine");

  scheduler.Run();
  EXPECT_TRUE(state.aborted);
}

TEST(Cpp20, AbortSingle) {
  Scheduler scheduler;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  struct TestState {
    bool aborted = false;
    Coroutine* coroutine_ptr = nullptr;
  } state;

  scheduler.Spawn([pipes, &state](Coroutine& co) -> Task {
    state.coroutine_ptr = &co;
    try {
      for (;;) {
        co_await co.Wait(pipes[0], POLLIN);
      }
    } catch (...) {
      state.aborted = true;
    }
    co_return;
  }, "waiting_coroutine");

  scheduler.Spawn([&state](Coroutine& co) -> Task {
    co_await co.Sleep(100000000); // 100ms
    while (!state.coroutine_ptr) {
      co_await co.Yield();
    }
    co_await co.Sleep(10000000); // 10ms
    if (state.coroutine_ptr) {
      state.coroutine_ptr->Abort();
    }
    co_return;
  }, "abort_coroutine");

  scheduler.Run();
  close(pipes[0]);
  close(pipes[1]);
  EXPECT_TRUE(state.aborted);
}
#endif

// --- Tests using the free-function API (co20::self, co20::Yield(), etc.) ---

TEST(Cpp20Free, Self) {
  Scheduler scheduler;
  const Coroutine* captured_self = nullptr;

  scheduler.Spawn([&captured_self]() -> Task {
    captured_self = co20::self;
    co_return;
  }, "self_test");

  scheduler.Run();

  ASSERT_NE(nullptr, captured_self);
  EXPECT_EQ("self_test", captured_self->Name());
}

TEST(Cpp20Free, SchedulerAccess) {
  Scheduler scheduler;
  Scheduler* captured_scheduler = nullptr;

  scheduler.Spawn([&captured_scheduler]() -> Task {
    captured_scheduler = co20::scheduler;
    co_return;
  }, "scheduler_test");

  scheduler.Run();

  EXPECT_EQ(&scheduler, captured_scheduler);
}

TEST(Cpp20Free, Yield) {
  Scheduler scheduler;
  int count = 0;

  scheduler.Spawn([&count]() -> Task {
    count++;
    co_await co20::Yield();
    count++;
    co_return;
  }, "test");

  scheduler.Run();

  EXPECT_EQ(2, count);
}

TEST(Cpp20Free, Wait) {
  Scheduler scheduler;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  std::string result;
  bool reader_done = false;

  scheduler.Spawn([&pipes, &result, &reader_done]() -> Task {
    for (;;) {
      int fd = co_await co20::Wait(pipes[0], POLLIN);
      if (fd != pipes[0]) break;

      char buf[1];
      ssize_t n = read(pipes[0], buf, 1);
      if (n == 0) break;
      if (n == 1) {
        result += buf[0];
      }
    }

    close(pipes[0]);
    reader_done = true;
    co_return;
  }, "reader");

  scheduler.Spawn([&pipes]() -> Task {
    for (int i = 0; i < 10; i++) {
      int fd = co_await co20::Wait(pipes[1], POLLOUT);
      if (fd != pipes[1]) {
        co_return;
      }

      char buf[1] = {char('A' + i)};
      write(pipes[1], buf, 1);
    }

    close(pipes[1]);
    co_return;
  }, "writer");

  scheduler.Run();

  EXPECT_TRUE(reader_done);
  EXPECT_EQ("ABCDEFGHIJ", result);
}

TEST(Cpp20Free, Sleep) {
  Scheduler scheduler;
  bool slept = false;

  scheduler.Spawn([&slept]() -> Task {
    co_await co20::Sleep(1000000ULL); // 1ms
    slept = true;
    co_return;
  }, "sleep_test");

  scheduler.Run();

  EXPECT_TRUE(slept);
}

TEST(Cpp20Free, SleepChrono) {
  Scheduler scheduler;
  bool slept = false;

  scheduler.Spawn([&slept]() -> Task {
    co_await co20::Sleep(std::chrono::milliseconds(1));
    slept = true;
    co_return;
  }, "sleep_chrono_test");

  scheduler.Run();

  EXPECT_TRUE(slept);
}

TEST(Cpp20Free, Millisleep) {
  Scheduler scheduler;
  bool slept = false;

  scheduler.Spawn([&slept]() -> Task {
    co_await co20::Millisleep(1);
    slept = true;
    co_return;
  }, "millisleep_test");

  scheduler.Run();

  EXPECT_TRUE(slept);
}

TEST(Cpp20Free, Loop) {
  Scheduler scheduler;

  for (int i = 0; i < 10; i++) {
    scheduler.Spawn([]() -> Task {
      for (int j = 0; j < 10; j++) {
        co_await co20::Yield();
      }
      co_return;
    }, "loop_coroutine_" + std::to_string(i));
  }

  scheduler.Run();
}

#if CO_POLL_MODE == CO_POLL_EPOLL
TEST(Cpp20Free, AbortWithSelf) {
  Scheduler scheduler;

  struct TestState {
    bool aborted = false;
    Coroutine* target = nullptr;
  } state;

  scheduler.Spawn([&state]() -> Task {
    state.target = co20::self;
    try {
      for (;;) {
        co_await co20::Sleep(std::chrono::seconds(10));
      }
    } catch (...) {
      state.aborted = true;
    }
    co_return;
  }, "target_coroutine");

  scheduler.Spawn([&state]() -> Task {
    co_await co20::Sleep(std::chrono::milliseconds(100));
    while (!state.target) {
      co_await co20::Yield();
    }
    co_await co20::Yield();
    if (state.target) {
      state.target->Abort();
    }
    co_return;
  }, "abort_coroutine");

  scheduler.Run();
  EXPECT_TRUE(state.aborted);
}
#endif

TEST(Cpp20, InterruptFd) {
  Scheduler scheduler;

#if defined(__linux__)
  int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  ASSERT_NE(efd, -1);
#else
  int p[2];
  ASSERT_EQ(pipe(p), 0);
  int efd = p[0];
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  fcntl(p[1], F_SETFL, O_NONBLOCK);
#endif

  int data_pipes[2];
  ASSERT_EQ(pipe(data_pipes), 0);

  bool interrupted = false;
  int wait_result = -1;

  scheduler.Spawn([&data_pipes, &interrupted, &wait_result](Coroutine& co) -> Task {
    int fd = co_await co.Wait(data_pipes[0], POLLIN);
    wait_result = fd;
    if (fd == co.GetInterruptFd()) {
      interrupted = true;
    }
    co_return;
  }, "waiting", efd);

  scheduler.Spawn([&efd
#if !defined(__linux__)
    , &p
#endif
    ](Coroutine& co) -> Task {
    co_await co.Sleep(50000000ULL); // 50ms
#if defined(__linux__)
    uint64_t val = 1;
    (void)write(efd, &val, sizeof(val));
#else
    char c = 'x';
    (void)write(p[1], &c, 1);
#endif
    co_return;
  }, "interrupter");

  scheduler.Run();

  EXPECT_TRUE(interrupted);
  EXPECT_NE(wait_result, data_pipes[0]);

  close(data_pipes[0]);
  close(data_pipes[1]);
#if defined(__linux__)
  close(efd);
#else
  close(p[0]);
  close(p[1]);
#endif
}

TEST(Cpp20, InterruptFdWithFreeFunction) {
  Scheduler scheduler;

#if defined(__linux__)
  int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  ASSERT_NE(efd, -1);
#else
  int p[2];
  ASSERT_EQ(pipe(p), 0);
  int efd = p[0];
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  fcntl(p[1], F_SETFL, O_NONBLOCK);
#endif

  int data_pipes[2];
  ASSERT_EQ(pipe(data_pipes), 0);

  bool interrupted = false;
  int wait_result = -1;

  scheduler.Spawn([&data_pipes, &interrupted, &wait_result]() -> Task {
    int fd = co_await co20::Wait(data_pipes[0], POLLIN);
    wait_result = fd;
    if (fd == co20::self->GetInterruptFd()) {
      interrupted = true;
    }
    co_return;
  }, "waiting", efd);

  scheduler.Spawn([&efd
#if !defined(__linux__)
    , &p
#endif
    ](Coroutine& co) -> Task {
    co_await co.Sleep(50000000ULL);
#if defined(__linux__)
    uint64_t val = 1;
    (void)write(efd, &val, sizeof(val));
#else
    char c = 'x';
    (void)write(p[1], &c, 1);
#endif
    co_return;
  }, "interrupter");

  scheduler.Run();

  EXPECT_TRUE(interrupted);
  EXPECT_NE(wait_result, data_pipes[0]);

  close(data_pipes[0]);
  close(data_pipes[1]);
#if defined(__linux__)
  close(efd);
#else
  close(p[0]);
  close(p[1]);
#endif
}

TEST(Cpp20, InterruptFdDataFirst) {
  Scheduler scheduler;

#if defined(__linux__)
  int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  ASSERT_NE(efd, -1);
#else
  int p[2];
  ASSERT_EQ(pipe(p), 0);
  int efd = p[0];
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  fcntl(p[1], F_SETFL, O_NONBLOCK);
#endif

  int data_pipes[2];
  ASSERT_EQ(pipe(data_pipes), 0);

  bool got_data = false;
  int wait_result = -1;

  scheduler.Spawn([&data_pipes, &got_data, &wait_result](Coroutine& co) -> Task {
    int fd = co_await co.Wait(data_pipes[0], POLLIN);
    wait_result = fd;
    if (fd == data_pipes[0]) {
      got_data = true;
      char buf[16];
      (void)read(data_pipes[0], buf, sizeof(buf));
    }
    co_return;
  }, "waiting", efd);

  scheduler.Spawn([&data_pipes](Coroutine& co) -> Task {
    co_await co.Yield();
    char c = 'D';
    (void)write(data_pipes[1], &c, 1);
    co_return;
  }, "writer");

  scheduler.Run();

  EXPECT_TRUE(got_data);
  EXPECT_EQ(wait_result, data_pipes[0]);

  close(data_pipes[0]);
  close(data_pipes[1]);
#if defined(__linux__)
  close(efd);
#else
  close(p[0]);
  close(p[1]);
#endif
}

} // namespace co20

#endif // CO20_HAVE_COROUTINES
