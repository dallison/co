// Test file for C++20 coroutine library

#include "coroutine_cpp20.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

#if CO20_HAVE_COROUTINES

namespace co20 {

TEST(Cpp20, Basic) {
  Scheduler sched;
  bool ran = false;
  
  sched.Spawn([&ran](Coroutine&) -> Task {
    ran = true;
    co_return;
  }, "test");
  
  sched.Run();
  
  EXPECT_TRUE(ran);
}

TEST(Cpp20, Yield) {
  Scheduler sched;
  int count = 0;
  
  sched.Spawn([&count](Coroutine& co) -> Task {
    count++;
    co_await co.Yield();
    count++;
    co_return;
  }, "test");
  
  sched.Run();
  
  EXPECT_EQ(2, count);
}

// A Yield-only coroutine must not starve waiters.  Without a non-blocking
// poll after each resume, the spinner keeps ready_queue_ non-empty and
// ProcessEvents is never reached on the poll() backend (macOS), so the
// reader hangs forever even though data is already in the pipe.
TEST(Cpp20, YieldDoesNotStarveWaiters) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool reader_done = false;
  int spins = 0;

  sched.Spawn([&](Coroutine& co) -> Task {
    int fd = co_await co.Wait(pipes[0], POLLIN);
    EXPECT_EQ(pipes[0], fd);
    char buf[1];
    EXPECT_EQ(1, read(pipes[0], buf, 1));
    reader_done = true;
    close(pipes[0]);
    co_return;
  }, "reader");

  sched.Spawn([&](Coroutine& co) -> Task {
    while (!reader_done) {
      ++spins;
      co_await co.Yield();
    }
    co_return;
  }, "spinner");

  sched.Spawn([&](Coroutine&) -> Task {
    char c = 'x';
    EXPECT_EQ(1, write(pipes[1], &c, 1));
    close(pipes[1]);
    co_return;
  }, "writer");

  sched.Run();
  EXPECT_TRUE(reader_done);
  EXPECT_GT(spins, 0);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// When several coroutines become ready together, the one that has been
// parked longest (lowest LastTick) runs first — same rule as stackful.
TEST(Cpp20, WaitTimeFairnessPrefersOlderWaiter) {
  Scheduler sched;
  int a[2];
  int b[2];
  ASSERT_EQ(0, pipe(a));
  ASSERT_EQ(0, pipe(b));

  std::vector<char> order;

  sched.Spawn([&](Coroutine& co) -> Task {
    // Parks first → lower LastTick.
    int fd = co_await co.Wait(a[0], POLLIN);
    EXPECT_EQ(a[0], fd);
    order.push_back('A');
    char buf[1];
    (void)read(a[0], buf, 1);
    co_return;
  }, "older");

  sched.Spawn([&](Coroutine& co) -> Task {
    // Parks second → higher LastTick after older has suspended.
    int fd = co_await co.Wait(b[0], POLLIN);
    EXPECT_EQ(b[0], fd);
    order.push_back('B');
    char buf[1];
    (void)read(b[0], buf, 1);
    co_return;
  }, "newer");

  sched.Spawn([&](Coroutine&) -> Task {
    // Wake both in the same event-loop turn; readiness order must not
    // override wait-age ordering.
    char c = 'x';
    (void)write(b[1], &c, 1);  // newer fd first
    (void)write(a[1], &c, 1);
    co_return;
  }, "waker");

  sched.Run();
  EXPECT_EQ((std::vector<char>{'A', 'B'}), order);
  close(a[0]);
  close(a[1]);
  close(b[0]);
  close(b[1]);
}

// A Yield-only busy loop must not pin the CPU or ignore Stop().  Run parks
// on the interrupt fd between fair passes so Stop() unblocks it.
TEST(Cpp20, YieldBusyLoopRespondsToStop) {
  Scheduler sched;
  int spins = 0;

  sched.Spawn([&](Coroutine& co) -> Task {
    for (;;) {
      ++spins;
      co_await co.Yield();
    }
  }, "spinner");

  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Sleep(std::chrono::milliseconds(20));
    sched.Stop();
    co_return;
  }, "stopper");

  sched.Run();
  EXPECT_GT(spins, 0);
  // Spinner is still alive (infinite loop); destroying the scheduler frees it.
  EXPECT_EQ(1u, sched.NumCoroutines());
}

TEST(Cpp20, Wait) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  
  std::string result;
  bool reader_done = false;
  
  sched.Spawn([&pipes, &result, &reader_done](Coroutine& co) -> Task {
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
  
  sched.Spawn([&pipes](Coroutine& co) -> Task {
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
  
  sched.Run();
  
  EXPECT_TRUE(reader_done);
  EXPECT_EQ("ABCDEFGHIJ", result);
}

TEST(Cpp20, Sleep) {
  Scheduler sched;
  bool slept = false;
  
  sched.Spawn([&slept](Coroutine& co) -> Task {
    co_await co.Sleep(1000000); // 1ms
    slept = true;
    co_return;
  }, "sleep_test");
  
  sched.Run();
  
  EXPECT_TRUE(slept);
}

TEST(Cpp20, Loop) {
  Scheduler sched;
  
  // Create 10 coroutines, each yielding 10 times
  for (int i = 0; i < 10; i++) {
    sched.Spawn([](Coroutine& co) -> Task {
      for (int j = 0; j < 10; j++) {
        co_await co.Yield();
      }
      co_return;
    }, "loop_coroutine_" + std::to_string(i));
  }
  
  sched.Run();
}

#if CO_POLL_MODE == CO_POLL_EPOLL
// This test is only deterministic in EPOLL mode because ::poll randomly
// chooses a pollfd if more than one is ready.  ::epoll doesn't
// allow multiple fds to be added to the epoll fd so we keep track of
// the coroutines ourselves.
TEST(Cpp20, MultipleFd) {
  Scheduler sched;

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
  sched.Spawn([pipes, &state](Coroutine& co) -> Task {
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
  sched.Spawn([pipes, &state](Coroutine& co) -> Task {
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
  sched.Spawn([pipes, &state](Coroutine& co) -> Task {
    // This will wake up foo but not bar.
    char buf = 'x';
    state.write_result = ::write(pipes[1], &buf, 1);
    co_await co.Yield();
    // This will wake bar.
    close(pipes[1]);
    co_return;
  }, "baz");

  sched.Run();
  close(pipes[0]);

  EXPECT_TRUE(state.foo_woke);
  EXPECT_TRUE(state.bar_woke);
  EXPECT_EQ(1, state.foo_read_result);
  EXPECT_EQ(0, state.bar_read_result);
  EXPECT_EQ(1, state.write_result);
}

TEST(Cpp20, AbortYield) {
  Scheduler sched;

  struct TestState {
    bool aborted = false;
    Coroutine* coroutine_ptr = nullptr;
  } state;

  sched.Spawn([&state](Coroutine& co) -> Task {
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

  sched.Spawn([&state](Coroutine& co) -> Task {
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

  sched.Run();
  EXPECT_TRUE(state.aborted);
}

TEST(Cpp20, AbortSingle) {
  Scheduler sched;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  struct TestState {
    bool aborted = false;
    Coroutine* coroutine_ptr = nullptr;
  } state;

  sched.Spawn([pipes, &state](Coroutine& co) -> Task {
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

  sched.Spawn([&state](Coroutine& co) -> Task {
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

  sched.Run();
  close(pipes[0]);
  close(pipes[1]);
  EXPECT_TRUE(state.aborted);
}
#endif

// --- Tests using the free-function API (co20::self, co20::Yield(), etc.) ---

TEST(Cpp20Free, Self) {
  Scheduler sched;
  const Coroutine* spawned = nullptr;
  const Coroutine* captured_self = nullptr;
  std::string name;

  spawned = sched.Spawn([&captured_self, &name]() -> Task {
    captured_self = co20::self;
    name = co20::self->Name();
    co_return;
  }, "self_test");

  sched.Run();

  // The coroutine has been destroyed now, so only compare the pointer.
  EXPECT_EQ(spawned, captured_self);
  EXPECT_EQ("self_test", name);
  EXPECT_EQ(nullptr, co20::self);
}

TEST(Cpp20Free, SchedulerAccess) {
  Scheduler sched;
  Scheduler* captured_scheduler = nullptr;

  sched.Spawn([&captured_scheduler]() -> Task {
    captured_scheduler = co20::scheduler;
    co_return;
  }, "scheduler_test");

  sched.Run();

  EXPECT_EQ(&sched, captured_scheduler);
}

TEST(Cpp20Free, Yield) {
  Scheduler sched;
  int count = 0;

  sched.Spawn([&count]() -> Task {
    count++;
    co_await co20::Yield();
    count++;
    co_return;
  }, "test");

  sched.Run();

  EXPECT_EQ(2, count);
}

TEST(Cpp20Free, Wait) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  std::string result;
  bool reader_done = false;

  sched.Spawn([&pipes, &result, &reader_done]() -> Task {
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

  sched.Spawn([&pipes]() -> Task {
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

  sched.Run();

  EXPECT_TRUE(reader_done);
  EXPECT_EQ("ABCDEFGHIJ", result);
}

#if CO_POLL_MODE == CO_POLL_POLL
// A socketpair is always writable.  A POLLIN waiter and a POLLOUT waiter on
// the same fd must each wake only for their own event.  Before the fix the
// scheduler polled with POLLIN|POLLOUT and resumed every waiter on the fd.
TEST(Cpp20Free, WaitMaskOnSharedFdWakesOnlyMatchingWaiter) {
  Scheduler sched;
  int sv[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

  // Nonblocking, so a wakeup without data cannot block the scheduler thread.
  ASSERT_EQ(0, fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK));

  int reader_wakeups_before_data = -1;
  int reader_wakeups = 0;
  bool writer_done = false;

  sched.Spawn([&sv, &reader_wakeups]() -> Task {
    for (;;) {
      int fd = co_await co20::Wait(sv[0], POLLIN);
      reader_wakeups++;
      if (fd != sv[0]) co_return;
      char buf[1];
      if (read(sv[0], buf, 1) == 1) co_return;
    }
  }, "reader");

  sched.Spawn([&sv, &reader_wakeups, &reader_wakeups_before_data,
               &writer_done]() -> Task {
    // Writable right away, so this waiter resumes without the reader's data.
    int fd = co_await co20::Wait(sv[0], POLLOUT);
    EXPECT_EQ(sv[0], fd);
    // Let the reader run and check that it is still waiting.
    co_await co20::Sleep(1000000ULL);
    reader_wakeups_before_data = reader_wakeups;
    char v = 'x';
    EXPECT_EQ(1, write(sv[1], &v, 1));
    writer_done = true;
    co_return;
  }, "writer");

  sched.Run();

  EXPECT_TRUE(writer_done);
  EXPECT_EQ(0, reader_wakeups_before_data);
  EXPECT_EQ(1, reader_wakeups);
  close(sv[0]);
  close(sv[1]);
}
#endif

TEST(Cpp20Free, Sleep) {
  Scheduler sched;
  bool slept = false;

  sched.Spawn([&slept]() -> Task {
    co_await co20::Sleep(1000000ULL); // 1ms
    slept = true;
    co_return;
  }, "sleep_test");

  sched.Run();

  EXPECT_TRUE(slept);
}

TEST(Cpp20Free, SleepChrono) {
  Scheduler sched;
  bool slept = false;

  sched.Spawn([&slept]() -> Task {
    co_await co20::Sleep(std::chrono::milliseconds(1));
    slept = true;
    co_return;
  }, "sleep_chrono_test");

  sched.Run();

  EXPECT_TRUE(slept);
}

TEST(Cpp20Free, Millisleep) {
  Scheduler sched;
  bool slept = false;

  sched.Spawn([&slept]() -> Task {
    co_await co20::Millisleep(1);
    slept = true;
    co_return;
  }, "millisleep_test");

  sched.Run();

  EXPECT_TRUE(slept);
}

TEST(Cpp20Free, Loop) {
  Scheduler sched;

  for (int i = 0; i < 10; i++) {
    sched.Spawn([]() -> Task {
      for (int j = 0; j < 10; j++) {
        co_await co20::Yield();
      }
      co_return;
    }, "loop_coroutine_" + std::to_string(i));
  }

  sched.Run();
}

#if CO_POLL_MODE == CO_POLL_EPOLL
TEST(Cpp20Free, AbortWithSelf) {
  Scheduler sched;

  struct TestState {
    bool aborted = false;
    Coroutine* target = nullptr;
  } state;

  sched.Spawn([&state]() -> Task {
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

  sched.Spawn([&state]() -> Task {
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

  sched.Run();
  EXPECT_TRUE(state.aborted);
}
#endif

TEST(Cpp20, InterruptFd) {
  Scheduler sched;

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

  sched.Spawn([&data_pipes, &interrupted, &wait_result](Coroutine& co) -> Task {
    int fd = co_await co.Wait(data_pipes[0], POLLIN);
    wait_result = fd;
    if (fd == co.GetInterruptFd()) {
      interrupted = true;
    }
    co_return;
  }, "waiting", efd);

  sched.Spawn([
#if defined(__linux__)
    &efd
#else
    &p
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

  sched.Run();

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
  Scheduler sched;

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

  sched.Spawn([&data_pipes, &interrupted, &wait_result]() -> Task {
    int fd = co_await co20::Wait(data_pipes[0], POLLIN);
    wait_result = fd;
    if (fd == co20::self->GetInterruptFd()) {
      interrupted = true;
    }
    co_return;
  }, "waiting", efd);

  sched.Spawn([
#if defined(__linux__)
    &efd
#else
    &p
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

  sched.Run();

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
  Scheduler sched;

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

  sched.Spawn([&data_pipes, &got_data, &wait_result](Coroutine& co) -> Task {
    int fd = co_await co.Wait(data_pipes[0], POLLIN);
    wait_result = fd;
    if (fd == data_pipes[0]) {
      got_data = true;
      char buf[16];
      (void)read(data_pipes[0], buf, sizeof(buf));
    }
    co_return;
  }, "waiting", efd);

  sched.Spawn([&data_pipes](Coroutine& co) -> Task {
    co_await co.Yield();
    char c = 'D';
    (void)write(data_pipes[1], &c, 1);
    co_return;
  }, "writer");

  sched.Run();

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

// --- ValueTask<T> tests ---

ValueTask<int> AddAsync(Coroutine& co, int a, int b) {
  co_await co.Yield();
  co_return a + b;
}

TEST(Cpp20ValueTask, BasicReturn) {
  Scheduler sched;
  int result = 0;

  sched.Spawn([&result](Coroutine& co) -> Task {
    result = co_await AddAsync(co, 3, 4);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_EQ(7, result);
}

ValueTask<std::string> ReadFromPipeAsync(Coroutine& co, int read_fd) {
  std::string data;
  for (;;) {
    int fd = co_await co.Wait(read_fd, POLLIN);
    if (fd != read_fd) break;
    char buf[64];
    ssize_t n = read(read_fd, buf, sizeof(buf));
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
  }
  co_return data;
}

TEST(Cpp20ValueTask, WithWait) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  std::string result;

  sched.Spawn([&pipes, &result](Coroutine& co) -> Task {
    result = co_await ReadFromPipeAsync(co, pipes[0]);
    close(pipes[0]);
    co_return;
  }, "reader");

  sched.Spawn([&pipes](Coroutine& co) -> Task {
    co_await co.Yield();
    const char* msg = "hello";
    (void)write(pipes[1], msg, 5);
    co_await co.Yield();
    close(pipes[1]);
    co_return;
  }, "writer");

  sched.Run();
  EXPECT_EQ("hello", result);
}

ValueTask<int> InnerCompute(Coroutine& co, int x) {
  co_await co.Yield();
  co_return x * 2;
}

ValueTask<int> OuterCompute(Coroutine& co, int x) {
  int doubled = co_await InnerCompute(co, x);
  co_await co.Yield();
  co_return doubled + 1;
}

TEST(Cpp20ValueTask, Nested) {
  Scheduler sched;
  int result = 0;

  sched.Spawn([&result](Coroutine& co) -> Task {
    result = co_await OuterCompute(co, 5);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_EQ(11, result);
}

ValueTask<void> IncrementAsync(Coroutine& co, int& counter) {
  co_await co.Yield();
  counter++;
  co_return;
}

TEST(Cpp20ValueTask, VoidReturn) {
  Scheduler sched;
  int counter = 0;

  sched.Spawn([&counter](Coroutine& co) -> Task {
    co_await IncrementAsync(co, counter);
    co_await IncrementAsync(co, counter);
    co_await IncrementAsync(co, counter);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_EQ(3, counter);
}

ValueTask<void> WaitAndWrite(Coroutine& co, int write_fd) {
  co_await co.Wait(write_fd, POLLOUT);
  char c = 'V';
  (void)write(write_fd, &c, 1);
  co_return;
}

TEST(Cpp20ValueTask, VoidWithWait) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  char read_buf = 0;

  sched.Spawn([&pipes, &read_buf](Coroutine& co) -> Task {
    co_await WaitAndWrite(co, pipes[1]);
    close(pipes[1]);
    int fd = co_await co.Wait(pipes[0], POLLIN);
    (void)fd;
    (void)read(pipes[0], &read_buf, 1);
    close(pipes[0]);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_EQ('V', read_buf);
}

ValueTask<int> AddWithFreeFunction(int a, int b) {
  co_await co20::Yield();
  co_return a + b;
}

TEST(Cpp20ValueTask, FreeFunctions) {
  Scheduler sched;
  int result = 0;

  sched.Spawn([&result]() -> Task {
    result = co_await AddWithFreeFunction(10, 20);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_EQ(30, result);
}

TEST(Cpp20ValueTask, MultipleSequentialCalls) {
  Scheduler sched;
  int sum = 0;

  sched.Spawn([&sum](Coroutine& co) -> Task {
    for (int i = 1; i <= 5; i++) {
      sum += co_await AddAsync(co, i, 0);
    }
    co_return;
  }, "test");

  sched.Run();
  EXPECT_EQ(15, sum);
}

ValueTask<bool> CheckPipeReady(Coroutine& co, int read_fd, uint64_t timeout_ns) {
  int fd = co_await co.Wait(read_fd, POLLIN, timeout_ns);
  co_return fd == read_fd;
}

TEST(Cpp20ValueTask, BoolReturn) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool pipe_ready = false;

  sched.Spawn([&pipes, &pipe_ready](Coroutine& co) -> Task {
    // Write data first so the pipe is ready.
    char c = 'x';
    (void)write(pipes[1], &c, 1);
    co_await co.Yield();
    pipe_ready = co_await CheckPipeReady(co, pipes[0], 0);
    close(pipes[0]);
    close(pipes[1]);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_TRUE(pipe_ready);
}

TEST(Cpp20WaitTimeout, DataArrivesBeforeTimeout) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool success = false;

  sched.Spawn([&](Coroutine& co) -> Task {
    char c = 'x';
    (void)write(pipes[1], &c, 1);
    co_await co.Yield();

    int fd = co_await co.Wait(pipes[0], POLLIN, 1000000000ULL);
    success = (fd == pipes[0]);

    close(pipes[0]);
    close(pipes[1]);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_TRUE(success);
}

TEST(Cpp20WaitTimeout, TimeoutExpires) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool timed_out = false;

  sched.Spawn([&](Coroutine& co) -> Task {
    int fd = co_await co.Wait(pipes[0], POLLIN, 50000000ULL);
    timed_out = (fd != pipes[0]);

    close(pipes[0]);
    close(pipes[1]);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_TRUE(timed_out);
}

TEST(Cpp20WaitTimeout, FreeFunctionTimeout) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool timed_out = false;

  sched.Spawn([&]() -> Task {
    int fd = co_await co20::Wait(pipes[0], POLLIN, 50000000ULL);
    timed_out = (fd != pipes[0]);

    close(pipes[0]);
    close(pipes[1]);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_TRUE(timed_out);
}

TEST(Cpp20WaitTimeout, ValueTaskWithTimeout) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool result = true;

  sched.Spawn([&](Coroutine& co) -> Task {
    result = co_await CheckPipeReady(co, pipes[0], 50000000ULL);

    close(pipes[0]);
    close(pipes[1]);
    co_return;
  }, "test");

  sched.Run();
  EXPECT_FALSE(result);
}

TEST(Cpp20WaitTimeout, DataArrivesDuringWait) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  bool success = false;

  sched.Spawn([&](Coroutine& co) -> Task {
    // Spawn a helper that writes after a short delay.
    co_await co.Yield();

    int fd = co_await co.Wait(pipes[0], POLLIN, 500000000ULL);
    success = (fd == pipes[0]);

    close(pipes[0]);
    close(pipes[1]);
    co_return;
  }, "waiter");

  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Sleep(std::chrono::milliseconds(20));
    char c = 'x';
    (void)write(pipes[1], &c, 1);
    co_return;
  }, "writer");

  sched.Run();
  EXPECT_TRUE(success);
}

// --- Coroutine lifetime ---

// Counts live instances so tests can see when coroutine frames and captures
// are destroyed.
struct LiveCounter {
  explicit LiveCounter(int* count) : count_(count) { ++*count_; }
  LiveCounter(const LiveCounter& other) : count_(other.count_) { ++*count_; }
  ~LiveCounter() { --*count_; }
  int* count_;
};

TEST(Cpp20Lifetime, FinishedCoroutinesAreDestroyed) {
  Scheduler sched;
  int live = 0;
  std::vector<std::string> completed;
  sched.SetCompletionCallback(
      [&completed](Coroutine* c) { completed.push_back(c->Name()); });

  {
    LiveCounter counter(&live);
    sched.Spawn([counter]() -> Task { co_return; }, "first");
  }
  sched.Spawn([&live, &sched](Coroutine& co) -> Task {
    co_await co.Yield();
    // "first" finished on its first resume, so it is gone by now.
    EXPECT_EQ(0, live);
    EXPECT_EQ(1u, sched.NumCoroutines());
    co_return;
  }, "second");
  EXPECT_EQ(1, live);
  EXPECT_EQ(2u, sched.NumCoroutines());

  sched.Run();
  EXPECT_EQ(0u, sched.NumCoroutines());
  EXPECT_EQ(std::vector<std::string>({"first", "second"}), completed);
}

TEST(Cpp20Lifetime, LongRunningSpawnerDoesNotAccumulateCoroutines) {
  Scheduler sched;
  size_t max_coroutines = 0;
  sched.Spawn([&sched, &max_coroutines](Coroutine& co) -> Task {
    for (int i = 0; i < 1000; i++) {
      sched.Spawn([]() -> Task { co_return; });
      co_await co.Yield();
      max_coroutines = std::max(max_coroutines, sched.NumCoroutines());
    }
    co_return;
  }, "spawner");
  sched.Run();
  EXPECT_LE(max_coroutines, 2u);
}

ValueTask<int> WaitInNestedTask(Coroutine& co, int fd, int* live) {
  LiveCounter counter(live);
  co_await co.Wait(fd, POLLIN);
  co_return 1;
}

TEST(Cpp20Lifetime, DestroyingSchedulerDestroysNestedFrames) {
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  int live = 0;
  {
    Scheduler sched;
    sched.Spawn([&pipes, &live](Coroutine& co) -> Task {
      LiveCounter outer(&live);
      co_await WaitInNestedTask(co, pipes[0], &live);
      co_return;
    }, "waiter");
    sched.Spawn([&sched](Coroutine& co) -> Task {
      co_await co.Yield();
      sched.Stop();
      co_return;
    }, "stopper");
    sched.Run();
    // Both the coroutine frame and the nested ValueTask frame are suspended.
    EXPECT_EQ(2, live);
  }
  EXPECT_EQ(0, live);
  close(pipes[0]);
  close(pipes[1]);
}

// --- Waiting on several fds ---

TEST(Cpp20MultiWait, ReturnsTheReadyFd) {
  Scheduler sched;
  int a[2];
  int b[2];
  ASSERT_EQ(0, pipe(a));
  ASSERT_EQ(0, pipe(b));
  int result = -1;

  sched.Spawn([&](Coroutine& co) -> Task {
    result = co_await co.Wait({a[0], b[0]}, POLLIN);
    co_return;
  }, "waiter");
  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Yield();
    char c = 'x';
    (void)write(b[1], &c, 1);
    co_return;
  }, "writer");

  sched.Run();
  EXPECT_EQ(b[0], result);
  for (int fd : {a[0], a[1], b[0], b[1]}) close(fd);
}

TEST(Cpp20MultiWait, MixedEventMasks) {
  Scheduler sched;
  int in[2];
  int out[2];
  ASSERT_EQ(0, pipe(in));
  ASSERT_EQ(0, pipe(out));
  int result = -1;

  sched.Spawn([&]() -> Task {
    // The write end of a pipe is writable straight away.
    result = co_await co20::Wait(
        std::vector<WaitFd>{{in[0], POLLIN}, {out[1], POLLOUT}});
    co_return;
  }, "waiter");

  sched.Run();
  EXPECT_EQ(out[1], result);
  for (int fd : {in[0], in[1], out[0], out[1]}) close(fd);
}

TEST(Cpp20MultiWait, TimeoutReturnsMinusOne) {
  Scheduler sched;
  int a[2];
  int b[2];
  ASSERT_EQ(0, pipe(a));
  ASSERT_EQ(0, pipe(b));
  int result = 0;

  sched.Spawn([&](Coroutine& co) -> Task {
    result = co_await co.Wait(std::vector<int>{a[0], b[0]}, POLLIN, 10000000ULL);
    co_return;
  }, "waiter");

  sched.Run();
  EXPECT_EQ(-1, result);
  for (int fd : {a[0], a[1], b[0], b[1]}) close(fd);
}

TEST(Cpp20MultiWait, SameFdWaitedOnByTwoCoroutinesWithDifferentMasks) {
  Scheduler sched;
  int sv[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
  int reader_result = -1;
  int writer_result = -1;

  sched.Spawn([&](Coroutine& co) -> Task {
    reader_result = co_await co.Wait(sv[0], POLLIN);
    co_return;
  }, "reader");
  sched.Spawn([&](Coroutine& co) -> Task {
    writer_result = co_await co.Wait(sv[0], POLLOUT);
    char c = 'x';
    (void)write(sv[1], &c, 1);
    co_return;
  }, "writer");

  sched.Run();
  EXPECT_EQ(sv[0], reader_result);
  EXPECT_EQ(sv[0], writer_result);
  close(sv[0]);
  close(sv[1]);
}

TEST(Cpp20MultiWait, RegularFileIsReady) {
  Scheduler sched;
  char path[] = "/tmp/co20_testXXXXXX";
  int fd = mkstemp(path);
  ASSERT_NE(-1, fd);
  unlink(path);
  int result = -1;

  sched.Spawn([&](Coroutine& co) -> Task {
    result = co_await co.Wait(fd, POLLIN);
    co_return;
  }, "waiter");

  sched.Run();
  EXPECT_EQ(fd, result);
  close(fd);
}

TEST(Cpp20WaitAll, ReturnsEveryReadyFd) {
  Scheduler sched;
  int a[2];
  int b[2];
  int c[2];
  ASSERT_EQ(0, pipe(a));
  ASSERT_EQ(0, pipe(b));
  ASSERT_EQ(0, pipe(c));
  std::vector<WaitFd> ready;

  sched.Spawn([&](Coroutine& co) -> Task {
    ready = co_await co.WaitAll({{a[0], POLLIN}, {b[0], POLLIN}, {c[0], POLLIN}});
    co_return;
  }, "waiter");
  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Yield();
    char x = 'x';
    (void)write(a[1], &x, 1);
    (void)write(c[1], &x, 1);
    co_return;
  }, "writer");

  sched.Run();
  ASSERT_EQ(2u, ready.size());
  EXPECT_EQ(a[0], ready[0].fd);
  EXPECT_EQ(c[0], ready[1].fd);
  EXPECT_TRUE(ready[0].events & POLLIN);
  for (int fd : {a[0], a[1], b[0], b[1], c[0], c[1]}) close(fd);
}

TEST(Cpp20WaitAll, TimeoutReturnsNothing) {
  Scheduler sched;
  int a[2];
  ASSERT_EQ(0, pipe(a));
  bool ran = false;
  std::vector<WaitFd> ready = {{0, 0}};

  sched.Spawn([&]() -> Task {
    ready = co_await co20::WaitAll({{a[0], POLLIN}}, 10000000ULL);
    ran = true;
    co_return;
  }, "waiter");

  sched.Run();
  EXPECT_TRUE(ran);
  EXPECT_TRUE(ready.empty());
  close(a[0]);
  close(a[1]);
}

// --- Exceptions ---

ValueTask<int> ThrowAfterYield(Coroutine& co) {
  co_await co.Yield();
  throw std::runtime_error("nested failure");
}

TEST(Cpp20Exceptions, NestedExceptionReachesCaller) {
  Scheduler sched;
  std::string caught;

  sched.Spawn([&caught](Coroutine& co) -> Task {
    try {
      (void)co_await ThrowAfterYield(co);
    } catch (const std::runtime_error& e) {
      caught = e.what();
    }
    co_return;
  }, "caller");

  sched.Run();
  EXPECT_EQ("nested failure", caught);
}

ValueTask<void> SleepForever(Coroutine& co) {
  for (;;) {
    co_await co.Sleep(std::chrono::seconds(10));
  }
}

TEST(Cpp20Exceptions, AbortUnwindsThroughNestedTasks) {
  Scheduler sched;
  Coroutine* target = nullptr;
  bool after_nested_call = false;

  target = sched.Spawn([&after_nested_call](Coroutine& co) -> Task {
    co_await SleepForever(co);
    after_nested_call = true;
    co_return;
  }, "target");
  sched.Spawn([&target](Coroutine& co) -> Task {
    co_await co.Yield();
    target->Abort();
    co_return;
  }, "aborter");

  sched.Run();
  EXPECT_FALSE(after_nested_call);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

TEST(Cpp20Exceptions, AbortingSelfDoesNotBlock) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  bool aborted = false;

  sched.Spawn([&](Coroutine& co) -> Task {
    co.Abort();
    try {
      co_await co.Wait(pipes[0], POLLIN);
    } catch (const AbortException&) {
      aborted = true;
    }
    co_return;
  }, "self_abort");

  sched.Run();
  EXPECT_TRUE(aborted);
  close(pipes[0]);
  close(pipes[1]);
}

TEST(Cpp20Exceptions, EscapingExceptionIsRethrownFromRun) {
  Scheduler sched;
  int live = 0;
  {
    LiveCounter counter(&live);
    sched.Spawn([counter](Coroutine& co) -> Task {
      co_await co.Yield();
      throw std::runtime_error("escaped");
    }, "thrower");
  }
  EXPECT_THROW(sched.Run(), std::runtime_error);
  EXPECT_EQ(0, live);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// --- ValueTask with types that have no default constructor ---

struct NoDefault {
  explicit NoDefault(int v) : value(v) {}
  int value;
};

ValueTask<NoDefault> MakeNoDefault(Coroutine& co) {
  co_await co.Yield();
  co_return NoDefault(7);
}

TEST(Cpp20ValueTask, NonDefaultConstructibleResult) {
  Scheduler sched;
  int value = 0;
  sched.Spawn([&value](Coroutine& co) -> Task {
    value = (co_await MakeNoDefault(co)).value;
    co_return;
  }, "caller");
  sched.Run();
  EXPECT_EQ(7, value);
}

// --- Scheduler control and debugging ---

TEST(Cpp20Scheduler, StopBeforeRunReturnsImmediately) {
  Scheduler sched;
  bool ran = false;
  sched.Spawn([&ran]() -> Task {
    ran = true;
    co_return;
  });
  sched.Stop();
  sched.Run();
  EXPECT_FALSE(ran);
  // The stop is used up, so the next Run runs the coroutine.
  sched.Run();
  EXPECT_TRUE(ran);
}

TEST(Cpp20Scheduler, IsRunning) {
  Scheduler sched;
  bool running_inside = false;
  sched.Spawn([&]() -> Task {
    running_inside = sched.IsRunning();
    co_return;
  });
  EXPECT_FALSE(sched.IsRunning());
  sched.Run();
  EXPECT_TRUE(running_inside);
  EXPECT_FALSE(sched.IsRunning());
}

#if defined(__linux__)
TEST(Cpp20Scheduler, InterruptFdCopyIsCloseOnExec) {
  Scheduler sched;
  int efd = eventfd(0, EFD_NONBLOCK);
  ASSERT_NE(-1, efd);
  Coroutine* c = sched.Spawn([]() -> Task { co_return; }, "c", efd);
  ASSERT_NE(-1, c->GetInterruptFd());
  EXPECT_TRUE(fcntl(c->GetInterruptFd(), F_GETFD) & FD_CLOEXEC);
  sched.Run();
  close(efd);
}
#endif

TEST(Cpp20Scheduler, DebugStringsAndFds) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  std::vector<std::string> strings;
  std::vector<int> fds;

  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Wait(pipes[0], POLLIN);
    co_return;
  }, "waiter");
  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Yield();
    strings = sched.AllCoroutineStrings();
    fds = sched.GetAllFds();
    char c = 'x';
    (void)write(pipes[1], &c, 1);
    co_return;
  }, "inspector");

  sched.Run();
  ASSERT_EQ(2u, strings.size());
  EXPECT_NE(std::string::npos, strings[0].find("waiter"));
  EXPECT_NE(std::string::npos, strings[0].find("waiting"));
  EXPECT_NE(std::string::npos, strings[1].find("inspector"));
  EXPECT_NE(fds.end(), std::find(fds.begin(), fds.end(), pipes[0]));
  close(pipes[0]);
  close(pipes[1]);
}

TEST(Cpp20AsyncWaiter, IsAnAsyncWaiter) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  int poll_before = -2;
  int poll_after = -2;
  int poll_vector = -2;
  int blocking_wait = -2;
  int blocking_timeout = -2;
  bool done = false;

  sched.Spawn([&](Coroutine& co) -> Task {
    toolbelt::AsyncWaiter& waiter = co;
    poll_before = waiter.Poll(std::vector<int>{pipes[0]});
    char c = 'x';
    (void)write(pipes[1], &c, 1);
    poll_after = waiter.Poll(std::vector<int>{pipes[0]});
    poll_vector = co20::Poll(std::vector<int>{pipes[1]}, POLLOUT);
    // Through the interface Wait blocks the thread but is otherwise correct.
    blocking_wait = waiter.Wait(pipes[0]);
    (void)read(pipes[0], &c, 1);
    blocking_timeout =
        waiter.Wait(pipes[0], POLLIN, static_cast<uint64_t>(10'000'000));
    waiter.Millisleep(1);
    waiter.Yield();
    done = true;
    co_return;
  }, "waiter");

  sched.Run();
  EXPECT_TRUE(done);
  EXPECT_EQ(-1, poll_before);
  EXPECT_EQ(pipes[0], poll_after);
  EXPECT_EQ(pipes[1], poll_vector);
  EXPECT_EQ(pipes[0], blocking_wait);
  EXPECT_EQ(-1, blocking_timeout);
  close(pipes[0]);
  close(pipes[1]);
}

TEST(Cpp20AsyncWaiter, AwaitableOperationsStillSuspend) {
  Scheduler sched;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  std::vector<int> order;

  sched.Spawn([&](Coroutine& co) -> Task {
    order.push_back(1);
    int fd = co_await co.Wait(pipes[0], POLLIN);
    EXPECT_EQ(pipes[0], fd);
    order.push_back(3);
    co_await co.Millisleep(1);
    co_await co.Yield();
    order.push_back(4);
    co_return;
  }, "waiter");
  sched.Spawn([&](Coroutine&) -> Task {
    order.push_back(2);
    char c = 'x';
    (void)write(pipes[1], &c, 1);
    co_return;
  }, "writer");

  sched.Run();
  EXPECT_EQ((std::vector<int>{1, 2, 3, 4}), order);
  close(pipes[0]);
  close(pipes[1]);
}

TEST(Cpp20AsyncWaiter, InterruptFdEndsBlockingWait) {
  Scheduler sched;
  int pipes[2];
  int interrupt[2];
  ASSERT_EQ(0, pipe(pipes));
  ASSERT_EQ(0, pipe(interrupt));
  char c = 'x';
  (void)write(interrupt[1], &c, 1);
  int result = -2;
  int interrupt_fd = -2;

  sched.Spawn([&](Coroutine& co) -> Task {
    toolbelt::AsyncWaiter& waiter = co;
    interrupt_fd = co.GetInterruptFd();
    result = waiter.Wait(pipes[0]);
    co_return;
  }, "waiter", interrupt[0]);

  sched.Run();
  EXPECT_EQ(sched.NumCoroutines(), 0u);
  // Like the awaitable Wait, the result is the coroutine's own copy of the
  // interrupt fd, not the fd that was passed in.
  EXPECT_NE(-1, interrupt_fd);
  EXPECT_EQ(interrupt_fd, result);
  close(pipes[0]);
  close(pipes[1]);
  close(interrupt[0]);
  close(interrupt[1]);
}

// Both fds become ready in the same poll.  The coroutine must be resumed once
// and must then be able to wait again.
TEST(Cpp20MultiWait, BothReadyResumesOnce) {
  Scheduler sched;
  int a[2];
  int b[2];
  ASSERT_EQ(0, pipe(a));
  ASSERT_EQ(0, pipe(b));

  int wakes = 0;
  bool second_wait_done = false;
  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Wait(std::vector<int>{a[0], b[0]}, POLLIN);
    wakes++;
    char c;
    (void)read(a[0], &c, 1);
    (void)read(b[0], &c, 1);
    co_await co.Sleep(std::chrono::milliseconds(20));
    second_wait_done = true;
  }, "waiter");
  sched.Spawn([&](Coroutine& co) -> Task {
    co_await co.Sleep(std::chrono::milliseconds(10));
    char c = 'x';
    (void)write(a[1], &c, 1);
    (void)write(b[1], &c, 1);
  }, "writer");

  sched.Run();
  EXPECT_EQ(1, wakes);
  EXPECT_TRUE(second_wait_done);
  close(a[0]);
  close(a[1]);
  close(b[0]);
  close(b[1]);
}

// Fails to compile if a promise holding it is built from the coroutine's
// arguments instead of being default constructed.  GCC treats a promise with
// no user-declared constructor as an aggregate and tries to initialize it from
// the arguments of a member function coroutine.
struct FromAnything {
  FromAnything() = default;
  template <typename U>
  FromAnything(U&&) {
    static_assert(!std::is_same_v<U, U>, "promise built from an argument");
  }
};

struct MemberCoroutines {
  int value = 7;
  ValueTask<FromAnything> Make() { co_return FromAnything(); }
  ValueTask<int> Get() { co_return value; }
  ValueTask<void> Nothing() { co_return; }
};

TEST(Cpp20ValueTask, MemberFunctionValueTasks) {
  Scheduler sched;
  MemberCoroutines m;
  int got = 0;
  sched.Spawn([&](Coroutine& co) -> Task {
    (void)co_await m.Make();
    co_await m.Nothing();
    got = co_await m.Get();
  });
  sched.Run();
  EXPECT_EQ(7, got);
}

// Counts its live instances.
struct FrameLocal {
  explicit FrameLocal(int& live) : live_(live) { ++live_; }
  ~FrameLocal() { --live_; }
  int& live_;
};

ValueTask<void> WaitInner(Coroutine& co, int fd, int& live) {
  FrameLocal local(live);
  co_await co.Wait(fd, POLLIN);
}

ValueTask<int> WaitOuter(Coroutine& co, int fd, int& live) {
  FrameLocal local(live);
  co_await WaitInner(co, fd, live);
  co_return 1;
}

// Stop() from one coroutine ends Run() while another waits on an fd, and the
// nested frames of the waiting coroutine are destroyed with the scheduler.
TEST(Cpp20Lifetime, StopWhileWaitingDestroysNestedFrames) {
  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  int live = 0;
  {
    Scheduler sched;
    sched.Spawn([&](Coroutine& co) -> Task {
      FrameLocal local(live);
      (void)co_await WaitOuter(co, fds[0], live);
    });
    sched.Spawn([&](Coroutine& co) -> Task {
      co_await co.Sleep(std::chrono::milliseconds(1));
      sched.Stop();
    });
    sched.Run();
    EXPECT_EQ(3, live);
  }
  EXPECT_EQ(0, live);
  close(fds[0]);
  close(fds[1]);
}

// A finished coroutine is freed, along with the callable that made it.
TEST(Cpp20Lifetime, FinishedCoroutineReleasesCallable) {
  Scheduler sched;
  auto token = std::make_shared<int>(0);
  sched.Spawn([token](Coroutine& co) -> Task {
    co_await co.Yield();
  }, "holder");
  EXPECT_EQ(2, token.use_count());
  sched.Run();
  EXPECT_EQ(1, token.use_count());
}

} // namespace co20

#endif // CO20_HAVE_COROUTINES
