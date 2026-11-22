// Stress tests for the C++20 coroutine library.  These try to make the
// scheduler fail under high churn, nested awaits, shared-fd contention,
// aborts, timeouts, and Stop-while-waiting.

#include "coroutine_cpp20.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#if CO20_HAVE_COROUTINES

namespace co20 {
namespace {

struct LiveCounter {
  explicit LiveCounter(int* count) : count_(count) { ++*count_; }
  LiveCounter(const LiveCounter& other) : count_(other.count_) { ++*count_; }
  ~LiveCounter() { --*count_; }
  int* count_;
};

// Non-blocking write that yields until the whole buffer is out.
ValueTask<void> WriteAll(Coroutine& co, int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int ready = co_await co.Wait(fd, POLLOUT);
    if (ready != fd) {
      co_return;
    }
    ssize_t n = write(fd, data + sent, len - sent);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      co_return;
    }
    if (n == 0) {
      co_return;
    }
    sent += static_cast<size_t>(n);
  }
  co_return;
}

ValueTask<std::string> ReadExact(Coroutine& co, int fd, size_t len) {
  std::string out;
  out.reserve(len);
  while (out.size() < len) {
    int ready = co_await co.Wait(fd, POLLIN);
    if (ready != fd) {
      break;
    }
    char buf[256];
    size_t want = std::min(sizeof(buf), len - out.size());
    ssize_t n = read(fd, buf, want);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }
    out.append(buf, static_cast<size_t>(n));
  }
  co_return out;
}

void SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(0, fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

}  // namespace

// Thousands of coroutines that only yield.  Stresses ready-queue churn and
// coroutine destruction.
TEST(Cpp20Stress, ManyYielders) {
  constexpr int kCoroutines = 2000;
  constexpr int kYields = 20;

  Scheduler sched;
  int finished = 0;
  int live = 0;

  for (int i = 0; i < kCoroutines; ++i) {
    LiveCounter counter(&live);
    sched.Spawn([counter, &finished](Coroutine& co) -> Task {
      for (int y = 0; y < kYields; ++y) {
        co_await co.Yield();
      }
      ++finished;
      co_return;
    });
  }

  EXPECT_EQ(kCoroutines, live);
  sched.Run();
  EXPECT_EQ(kCoroutines, finished);
  EXPECT_EQ(0, live);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// A long-lived spawner creates short-lived children as fast as it can.  The
// live count must stay bounded (children finish promptly).
TEST(Cpp20Stress, SpawnChurn) {
  constexpr int kSpawns = 5000;
  Scheduler sched;
  int finished = 0;
  size_t max_live = 0;

  sched.Spawn([&](Coroutine& co) -> Task {
    for (int i = 0; i < kSpawns; ++i) {
      sched.Spawn([&finished]() -> Task {
        ++finished;
        co_return;
      });
      if ((i & 7) == 0) {
        co_await co.Yield();
      }
      max_live = std::max(max_live, sched.NumCoroutines());
    }
    co_return;
  }, "spawner");

  sched.Run();
  EXPECT_EQ(kSpawns, finished);
  EXPECT_EQ(0u, sched.NumCoroutines());
  // Spawner + a handful of not-yet-destroyed children.
  EXPECT_LE(max_live, 64u);
}

// Many independent pipe pairs, each with a reader and a writer.  Stresses
// epoll registration churn across many fds.
TEST(Cpp20Stress, ManyPipes) {
  constexpr int kPairs = 200;
  constexpr int kMessages = 50;

  Scheduler sched;
  std::vector<std::array<int, 2>> pipes(static_cast<size_t>(kPairs));
  std::vector<int> received(static_cast<size_t>(kPairs), 0);

  for (int i = 0; i < kPairs; ++i) {
    ASSERT_EQ(0, pipe(pipes[static_cast<size_t>(i)].data()));
    SetNonBlocking(pipes[static_cast<size_t>(i)][0]);
    SetNonBlocking(pipes[static_cast<size_t>(i)][1]);
  }

  for (int i = 0; i < kPairs; ++i) {
    int rfd = pipes[static_cast<size_t>(i)][0];
    int wfd = pipes[static_cast<size_t>(i)][1];
    int* count = &received[static_cast<size_t>(i)];

    sched.Spawn([rfd, count](Coroutine& co) -> Task {
      for (;;) {
        int fd = co_await co.Wait(rfd, POLLIN);
        if (fd != rfd) {
          break;
        }
        char buf[1];
        ssize_t n = read(rfd, buf, 1);
        if (n == 0) {
          break;
        }
        if (n == 1) {
          ++*count;
        }
      }
      close(rfd);
      co_return;
    });

    sched.Spawn([wfd](Coroutine& co) -> Task {
      for (int m = 0; m < kMessages; ++m) {
        char c = 'x';
        co_await WriteAll(co, wfd, &c, 1);
      }
      close(wfd);
      co_return;
    });
  }

  sched.Run();
  for (int i = 0; i < kPairs; ++i) {
    EXPECT_EQ(kMessages, received[static_cast<size_t>(i)]) << "pair " << i;
  }
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// One writable fd, many coroutines waiting for POLLIN.  First write should
// wake exactly one waiter per byte (edge-triggered-ish fairness is not
// required; we just require every byte is consumed exactly once).
TEST(Cpp20Stress, ManyWaitersOneFd) {
  constexpr int kWaiters = 100;
  constexpr int kBytes = 1000;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  SetNonBlocking(pipes[0]);
  SetNonBlocking(pipes[1]);

  Scheduler sched;
  int total = 0;

  for (int i = 0; i < kWaiters; ++i) {
    sched.Spawn([&](Coroutine& co) -> Task {
      for (;;) {
        int fd = co_await co.Wait(pipes[0], POLLIN);
        if (fd != pipes[0]) {
          break;
        }
        char buf[16];
        ssize_t n = read(pipes[0], buf, sizeof(buf));
        if (n == 0) {
          break;
        }
        if (n > 0) {
          total += static_cast<int>(n);
        }
      }
      co_return;
    });
  }

  sched.Spawn([&](Coroutine& co) -> Task {
    for (int i = 0; i < kBytes; ++i) {
      char c = 'a' + (i % 26);
      co_await WriteAll(co, pipes[1], &c, 1);
      if ((i & 15) == 0) {
        co_await co.Yield();
      }
    }
    close(pipes[1]);
    co_return;
  }, "writer");

  sched.Run();
  EXPECT_EQ(kBytes, total);
  close(pipes[0]);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// Ping-pong between two coroutines over a socketpair for many rounds.
TEST(Cpp20Stress, SocketPingPong) {
  constexpr int kRounds = 5000;
  int sv[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
  SetNonBlocking(sv[0]);
  SetNonBlocking(sv[1]);

  Scheduler sched;
  int a_count = 0;
  int b_count = 0;

  // a writes first; each round is: peer reads, peer writes reply.  a does not
  // write after the final read so the peer never writes into a closed socket.
  sched.Spawn([&](Coroutine& co) -> Task {
    char token = 'A';
    co_await WriteAll(co, sv[0], &token, 1);
    for (int i = 0; i < kRounds; ++i) {
      std::string got = co_await ReadExact(co, sv[0], 1);
      if (got.size() != 1 || got[0] != 'B') {
        ADD_FAILURE() << "a got unexpected payload";
        co_return;
      }
      ++a_count;
      if (i + 1 < kRounds) {
        co_await WriteAll(co, sv[0], &token, 1);
      }
    }
    co_return;
  }, "a");

  sched.Spawn([&](Coroutine& co) -> Task {
    for (int i = 0; i < kRounds; ++i) {
      std::string got = co_await ReadExact(co, sv[1], 1);
      if (got.size() != 1 || got[0] != 'A') {
        ADD_FAILURE() << "b got unexpected payload";
        co_return;
      }
      ++b_count;
      char token = 'B';
      co_await WriteAll(co, sv[1], &token, 1);
    }
    co_return;
  }, "b");

  sched.Run();
  EXPECT_EQ(kRounds, a_count);
  EXPECT_EQ(kRounds, b_count);
  EXPECT_EQ(0u, sched.NumCoroutines());
  close(sv[0]);
  close(sv[1]);
}

ValueTask<int> DeepNest(Coroutine& co, int depth, int* yields) {
  if (depth == 0) {
    co_await co.Yield();
    ++*yields;
    co_return 1;
  }
  int inner = co_await DeepNest(co, depth - 1, yields);
  co_await co.Yield();
  ++*yields;
  co_return inner + 1;
}

// Deep ValueTask nesting with yields at every level, across many coroutines.
TEST(Cpp20Stress, DeepNestedValueTasks) {
  constexpr int kCoroutines = 100;
  constexpr int kDepth = 32;

  Scheduler sched;
  int total = 0;
  int yields = 0;
  int live = 0;

  for (int i = 0; i < kCoroutines; ++i) {
    LiveCounter counter(&live);
    sched.Spawn([counter, &total, &yields](Coroutine& co) -> Task {
      int v = co_await DeepNest(co, kDepth, &yields);
      total += v;
      co_return;
    });
  }

  sched.Run();
  EXPECT_EQ(kCoroutines * (kDepth + 1), total);
  EXPECT_EQ(kCoroutines * (kDepth + 1), yields);
  EXPECT_EQ(0, live);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// Mix Yield, short Sleep, and Wait across many coroutines.  Each coroutine
// owns its own pipe so writers and readers cannot deadlock each other.
TEST(Cpp20Stress, MixedYieldSleepWait) {
  constexpr int kCoroutines = 100;
  constexpr int kIters = 20;

  std::vector<std::array<int, 2>> pipes(static_cast<size_t>(kCoroutines));
  for (int i = 0; i < kCoroutines; ++i) {
    ASSERT_EQ(0, pipe(pipes[static_cast<size_t>(i)].data()));
    SetNonBlocking(pipes[static_cast<size_t>(i)][0]);
    SetNonBlocking(pipes[static_cast<size_t>(i)][1]);
  }

  Scheduler sched;
  int completed = 0;

  for (int i = 0; i < kCoroutines; ++i) {
    int rfd = pipes[static_cast<size_t>(i)][0];
    int wfd = pipes[static_cast<size_t>(i)][1];
    sched.Spawn([&, rfd, wfd](Coroutine& co) -> Task {
      for (int n = 0; n < kIters; ++n) {
        switch (n % 3) {
          case 0:
            co_await co.Yield();
            break;
          case 1:
            // Short sleep; just enough to exercise timer creation.
            co_await co.Sleep(std::chrono::microseconds(100));
            break;
          default: {
            char c = 'x';
            co_await WriteAll(co, wfd, &c, 1);
            int fd = co_await co.Wait(rfd, POLLIN);
            if (fd == rfd) {
              char buf[1];
              (void)read(rfd, buf, 1);
            }
            break;
          }
        }
      }
      close(rfd);
      close(wfd);
      ++completed;
      co_return;
    });
  }

  sched.Run();
  EXPECT_EQ(kCoroutines, completed);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// Abort many waiting coroutines while a few keep running.
TEST(Cpp20Stress, AbortWaitingCrowd) {
  constexpr int kWaiters = 300;
  constexpr int kSurvivors = 20;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  Scheduler sched;
  std::vector<Coroutine*> waiters;
  waiters.reserve(static_cast<size_t>(kWaiters));
  int aborted = 0;
  int survived = 0;

  for (int i = 0; i < kWaiters; ++i) {
    Coroutine* c = sched.Spawn([&](Coroutine& co) -> Task {
      try {
        (void)co_await co.Wait(pipes[0], POLLIN);
      } catch (const AbortException&) {
        ++aborted;
        co_return;
      }
      co_return;
    });
    waiters.push_back(c);
  }

  for (int i = 0; i < kSurvivors; ++i) {
    sched.Spawn([&](Coroutine& co) -> Task {
      for (int y = 0; y < 10; ++y) {
        co_await co.Yield();
      }
      ++survived;
      co_return;
    });
  }

  sched.Spawn([&](Coroutine& co) -> Task {
    // Let waiters park.
    co_await co.Yield();
    co_await co.Yield();
    for (Coroutine* c : waiters) {
      c->Abort();
    }
    co_return;
  }, "aborter");

  sched.Run();
  EXPECT_EQ(kWaiters, aborted);
  EXPECT_EQ(kSurvivors, survived);
  EXPECT_EQ(0u, sched.NumCoroutines());
  close(pipes[0]);
  close(pipes[1]);
}

// Stop the scheduler while many coroutines are waiting; destroying the
// scheduler must free every suspended frame (no leaks under ASan).
TEST(Cpp20Stress, StopWhileManyWaiting) {
  constexpr int kWaiters = 500;
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  int live = 0;

  {
    Scheduler sched;
    for (int i = 0; i < kWaiters; ++i) {
      LiveCounter counter(&live);
      sched.Spawn([counter, &pipes](Coroutine& co) -> Task {
        (void)co_await co.Wait(pipes[0], POLLIN);
        co_return;
      });
    }
    sched.Spawn([&](Coroutine& co) -> Task {
      co_await co.Yield();
      sched.Stop();
      co_return;
    }, "stopper");

    sched.Run();
    EXPECT_EQ(kWaiters, live);
  }
  EXPECT_EQ(0, live);
  close(pipes[0]);
  close(pipes[1]);
}

// Many short timeouts racing with occasional data.  Dedicated pipe per
// coroutine so hits and timeouts are deterministic: even indices get a write
// before the wait; odd indices never get data and must time out.
TEST(Cpp20Stress, TimeoutRaces) {
  constexpr int kCoroutines = 100;
  std::vector<std::array<int, 2>> pipes(static_cast<size_t>(kCoroutines));
  for (int i = 0; i < kCoroutines; ++i) {
    ASSERT_EQ(0, pipe(pipes[static_cast<size_t>(i)].data()));
    SetNonBlocking(pipes[static_cast<size_t>(i)][0]);
    SetNonBlocking(pipes[static_cast<size_t>(i)][1]);
  }

  Scheduler sched;
  int timeouts = 0;
  int hits = 0;

  for (int i = 0; i < kCoroutines; ++i) {
    int rfd = pipes[static_cast<size_t>(i)][0];
    int wfd = pipes[static_cast<size_t>(i)][1];
    bool expect_hit = (i % 2) == 0;
    sched.Spawn([&, rfd, wfd, expect_hit](Coroutine& co) -> Task {
      if (expect_hit) {
        char c = 'z';
        (void)write(wfd, &c, 1);
      }
      int fd = co_await co.Wait(rfd, POLLIN, /*timeout_ns=*/2'000'000ULL);  // 2ms
      if (fd == -1) {
        ++timeouts;
      } else if (fd == rfd) {
        char buf[1];
        if (read(rfd, buf, 1) == 1) {
          ++hits;
        }
      }
      close(rfd);
      close(wfd);
      co_return;
    });
  }

  sched.Run();
  EXPECT_EQ(kCoroutines / 2, timeouts);
  EXPECT_EQ(kCoroutines / 2, hits);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// WaitAll under load: many fds, wake a subset, expect those back.
TEST(Cpp20Stress, WaitAllManyFds) {
  constexpr int kFds = 64;
  constexpr int kRounds = 40;

  std::vector<std::array<int, 2>> pipes(static_cast<size_t>(kFds));
  for (int i = 0; i < kFds; ++i) {
    ASSERT_EQ(0, pipe(pipes[static_cast<size_t>(i)].data()));
    SetNonBlocking(pipes[static_cast<size_t>(i)][0]);
    SetNonBlocking(pipes[static_cast<size_t>(i)][1]);
  }

  Scheduler sched;
  int rounds_ok = 0;

  sched.Spawn([&](Coroutine& co) -> Task {
    for (int r = 0; r < kRounds; ++r) {
      std::vector<WaitFd> wait_fds;
      wait_fds.reserve(static_cast<size_t>(kFds));
      for (int i = 0; i < kFds; ++i) {
        wait_fds.push_back({pipes[static_cast<size_t>(i)][0], POLLIN});
      }

      // Wake every third fd.
      for (int i = 0; i < kFds; i += 3) {
        char c = 'w';
        (void)write(pipes[static_cast<size_t>(i)][1], &c, 1);
      }

      auto ready = co_await co.WaitAll(wait_fds);
      if (ready.empty()) {
        ADD_FAILURE() << "WaitAll returned empty on round " << r;
        co_return;
      }
      for (const WaitFd& w : ready) {
        char buf[1];
        if (read(w.fd, buf, 1) != 1) {
          ADD_FAILURE() << "failed to drain ready fd " << w.fd;
          co_return;
        }
      }
      ++rounds_ok;
      co_await co.Yield();
    }
    co_return;
  }, "waiter");

  sched.Run();
  EXPECT_EQ(kRounds, rounds_ok);

  for (int i = 0; i < kFds; ++i) {
    close(pipes[static_cast<size_t>(i)][0]);
    close(pipes[static_cast<size_t>(i)][1]);
  }
}

// Create and destroy many schedulers, each running a small workload.  Catches
// leaks that only show up across scheduler lifetimes.
TEST(Cpp20Stress, SchedulerRecreateCycles) {
  constexpr int kCycles = 200;
  int live = 0;

  for (int c = 0; c < kCycles; ++c) {
    int pipes[2];
    ASSERT_EQ(0, pipe(pipes));
    {
      Scheduler sched;
      LiveCounter counter(&live);
      sched.Spawn([counter, &pipes](Coroutine& co) -> Task {
        char c = 'x';
        (void)write(pipes[1], &c, 1);
        int fd = co_await co.Wait(pipes[0], POLLIN);
        if (fd == pipes[0]) {
          char buf[1];
          (void)read(pipes[0], buf, 1);
        }
        co_await co.Yield();
        co_return;
      });
      sched.Spawn([](Coroutine& co) -> Task {
        co_await co.Sleep(std::chrono::microseconds(10));
        co_return;
      });
      sched.Run();
      EXPECT_EQ(0u, sched.NumCoroutines());
    }
    EXPECT_EQ(0, live);
    close(pipes[0]);
    close(pipes[1]);
  }
}

ValueTask<int> ThrowAtDepth(Coroutine& co, int depth) {
  co_await co.Yield();
  if (depth == 0) {
    throw std::runtime_error("boom");
  }
  co_return co_await ThrowAtDepth(co, depth - 1);
}

// Exceptions escaping through nested ValueTasks under concurrent load.
TEST(Cpp20Stress, NestedExceptionStorm) {
  constexpr int kThrowers = 50;
  constexpr int kDepth = 16;
  constexpr int kNormal = 50;

  Scheduler sched;
  int caught = 0;
  int normal = 0;

  for (int i = 0; i < kThrowers; ++i) {
    sched.Spawn([&](Coroutine& co) -> Task {
      try {
        (void)co_await ThrowAtDepth(co, kDepth);
      } catch (const std::runtime_error&) {
        ++caught;
      }
      co_return;
    });
  }
  for (int i = 0; i < kNormal; ++i) {
    sched.Spawn([&](Coroutine& co) -> Task {
      for (int y = 0; y < 5; ++y) {
        co_await co.Yield();
      }
      ++normal;
      co_return;
    });
  }

  sched.Run();
  EXPECT_EQ(kThrowers, caught);
  EXPECT_EQ(kNormal, normal);
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// Free-function Wait/Yield path under the same load as ManyYielders+pipes.
TEST(Cpp20Stress, FreeFunctionChurn) {
  constexpr int kPairs = 100;
  constexpr int kMessages = 40;

  Scheduler sched;
  std::vector<std::array<int, 2>> pipes(static_cast<size_t>(kPairs));
  std::vector<int> received(static_cast<size_t>(kPairs), 0);

  for (int i = 0; i < kPairs; ++i) {
    ASSERT_EQ(0, pipe(pipes[static_cast<size_t>(i)].data()));
  }

  for (int i = 0; i < kPairs; ++i) {
    int rfd = pipes[static_cast<size_t>(i)][0];
    int wfd = pipes[static_cast<size_t>(i)][1];
    int* count = &received[static_cast<size_t>(i)];

    sched.Spawn([rfd, count](Coroutine&) -> Task {
      for (;;) {
        int fd = co_await co20::Wait(rfd, POLLIN);
        if (fd != rfd) {
          break;
        }
        char buf[1];
        ssize_t n = read(rfd, buf, 1);
        if (n <= 0) {
          break;
        }
        ++*count;
      }
      close(rfd);
      co_return;
    });

    sched.Spawn([wfd](Coroutine&) -> Task {
      for (int m = 0; m < kMessages; ++m) {
        int fd = co_await co20::Wait(wfd, POLLOUT);
        if (fd != wfd) {
          co_return;
        }
        char c = 'y';
        (void)write(wfd, &c, 1);
        co_await co20::Yield();
      }
      close(wfd);
      co_return;
    });
  }

  sched.Run();
  for (int i = 0; i < kPairs; ++i) {
    EXPECT_EQ(kMessages, received[static_cast<size_t>(i)]) << "pair " << i;
  }
}

// Many socketpairs, each with a POLLIN waiter and a POLLOUT waiter parked on
// the same end at the same time.  Filling the send buffer makes POLLOUT
// start false so both waits suspend.  Draining (via the peer) must wake only
// the POLLOUT waiter; a subsequent peer write wakes only the POLLIN waiter.
TEST(Cpp20Stress, SharedFdMaskContention) {
  constexpr int kPairs = 50;

  std::vector<std::array<int, 2>> sockets(static_cast<size_t>(kPairs));
  for (int i = 0; i < kPairs; ++i) {
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets[static_cast<size_t>(i)].data()));
    SetNonBlocking(sockets[static_cast<size_t>(i)][0]);
    SetNonBlocking(sockets[static_cast<size_t>(i)][1]);

    // Fill so POLLOUT on [0] is false and both waiters actually suspend.
    char buf[1024];
    std::memset(buf, 'x', sizeof(buf));
    for (;;) {
      ssize_t n = write(sockets[static_cast<size_t>(i)][0], buf, sizeof(buf));
      if (n < 0) {
        ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
        break;
      }
    }
  }

  Scheduler sched;
  int reader_before_data = 0;
  int reader_got_data = 0;
  int writer_wakes = 0;

  for (int i = 0; i < kPairs; ++i) {
    int a = sockets[static_cast<size_t>(i)][0];
    int b = sockets[static_cast<size_t>(i)][1];

    sched.Spawn([&, a](Coroutine& co) -> Task {
      int fd = co_await co.Wait(a, POLLIN);
      if (fd != a) {
        ADD_FAILURE() << "reader woken by unexpected fd " << fd;
        co_return;
      }
      char buf[1];
      if (read(a, buf, 1) == 1) {
        ++reader_got_data;
      } else {
        // Woken without data → POLLOUT (or HUP) leaked into this waiter.
        ++reader_before_data;
      }
      co_return;
    });

    sched.Spawn([&, a](Coroutine& co) -> Task {
      int fd = co_await co.Wait(a, POLLOUT);
      if (fd != a) {
        ADD_FAILURE() << "writer woken by unexpected fd " << fd;
        co_return;
      }
      ++writer_wakes;
      co_return;
    });

    // Peer drains the filled buffer (waking POLLOUT) then writes one byte
    // (waking POLLIN).  Sleep between the two so the POLLOUT wake is
    // observable as a distinct event from the POLLIN wake.
    sched.Spawn([&, b](Coroutine& co) -> Task {
      co_await co.Sleep(std::chrono::microseconds(100));
      char buf[4096];
      while (read(b, buf, sizeof(buf)) > 0) {
      }
      co_await co.Sleep(std::chrono::microseconds(100));
      char v = 'x';
      (void)write(b, &v, 1);
      co_return;
    });
  }

  sched.Run();
  EXPECT_EQ(kPairs, writer_wakes);
  EXPECT_EQ(kPairs, reader_got_data);
  EXPECT_EQ(0, reader_before_data);

  for (int i = 0; i < kPairs; ++i) {
    close(sockets[static_cast<size_t>(i)][0]);
    close(sockets[static_cast<size_t>(i)][1]);
  }
  EXPECT_EQ(0u, sched.NumCoroutines());
}

// Spawner creates waiters; an aborter aborts them mid-wait; survivors keep
// going.  Stresses DetachWaits + AbortException unwinding together.
TEST(Cpp20Stress, SpawnAbortWaitTriangle) {
  constexpr int kBatches = 50;
  constexpr int kPerBatch = 20;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  Scheduler sched;
  int aborted = 0;
  int survivors = 0;
  std::vector<Coroutine*> pending;

  sched.Spawn([&](Coroutine& co) -> Task {
    for (int b = 0; b < kBatches; ++b) {
      pending.clear();
      for (int i = 0; i < kPerBatch; ++i) {
        Coroutine* c = sched.Spawn([&](Coroutine& inner) -> Task {
          try {
            (void)co_await inner.Wait(pipes[0], POLLIN);
          } catch (const AbortException&) {
            ++aborted;
            co_return;
          }
          co_return;
        });
        pending.push_back(c);
      }
      co_await co.Yield();
      for (Coroutine* c : pending) {
        c->Abort();
      }
      co_await co.Yield();
    }
    // Survivors: a few yield loops that must not be aborted.
    for (int i = 0; i < 10; ++i) {
      sched.Spawn([&](Coroutine& inner) -> Task {
        for (int y = 0; y < 5; ++y) {
          co_await inner.Yield();
        }
        ++survivors;
        co_return;
      });
    }
    co_return;
  }, "spawner");

  sched.Run();
  EXPECT_EQ(kBatches * kPerBatch, aborted);
  EXPECT_EQ(10, survivors);
  EXPECT_EQ(0u, sched.NumCoroutines());
  close(pipes[0]);
  close(pipes[1]);
}

// Completion callback fires for every finished coroutine under churn, and
// never for ones destroyed by Stop without finishing.
TEST(Cpp20Stress, CompletionCallbackUnderChurn) {
  constexpr int kFinish = 1000;
  constexpr int kStopped = 200;

  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));
  int completed = 0;
  int live = 0;

  {
    Scheduler sched;
    sched.SetCompletionCallback([&](Coroutine*) { ++completed; });

    for (int i = 0; i < kFinish; ++i) {
      sched.Spawn([&](Coroutine& co) -> Task {
        co_await co.Yield();
        co_return;
      });
    }
    for (int i = 0; i < kStopped; ++i) {
      LiveCounter counter(&live);
      sched.Spawn([counter, &pipes](Coroutine& co) -> Task {
        (void)co_await co.Wait(pipes[0], POLLIN);
        co_return;
      });
    }
    sched.Spawn([&](Coroutine& co) -> Task {
      // Let finishers run, then stop with waiters still parked.
      for (int i = 0; i < 5; ++i) {
        co_await co.Yield();
      }
      sched.Stop();
      co_return;
    }, "stopper");

    sched.Run();
    // Finishers + stopper completed; waiters did not.
    EXPECT_EQ(kFinish + 1, completed);
    EXPECT_EQ(kStopped, live);
  }
  EXPECT_EQ(0, live);
  close(pipes[0]);
  close(pipes[1]);
}

// Multi-fd Wait where readiness rotates across fds each round.
TEST(Cpp20Stress, RotatingMultiFdWait) {
  constexpr int kFds = 32;
  constexpr int kRounds = 200;

  std::vector<std::array<int, 2>> pipes(static_cast<size_t>(kFds));
  for (int i = 0; i < kFds; ++i) {
    ASSERT_EQ(0, pipe(pipes[static_cast<size_t>(i)].data()));
  }

  Scheduler sched;
  int hits = 0;

  sched.Spawn([&](Coroutine& co) -> Task {
    std::vector<int> fds;
    fds.reserve(static_cast<size_t>(kFds));
    for (int i = 0; i < kFds; ++i) {
      fds.push_back(pipes[static_cast<size_t>(i)][0]);
    }
    for (int r = 0; r < kRounds; ++r) {
      int which = r % kFds;
      char c = 'r';
      (void)write(pipes[static_cast<size_t>(which)][1], &c, 1);
      int fd = co_await co.Wait(fds, POLLIN);
      if (fd != pipes[static_cast<size_t>(which)][0]) {
        ADD_FAILURE() << "round " << r << " expected fd "
                      << pipes[static_cast<size_t>(which)][0] << " got " << fd;
        co_return;
      }
      char buf[1];
      if (read(fd, buf, 1) != 1) {
        ADD_FAILURE() << "failed to read ready fd on round " << r;
        co_return;
      }
      ++hits;
    }
    co_return;
  }, "waiter");

  sched.Run();
  EXPECT_EQ(kRounds, hits);

  for (int i = 0; i < kFds; ++i) {
    close(pipes[static_cast<size_t>(i)][0]);
    close(pipes[static_cast<size_t>(i)][1]);
  }
}

}  // namespace co20

#endif  // CO20_HAVE_COROUTINES
