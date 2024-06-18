
#include "coroutine.h"
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

TEST(Coroutines, Simple) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;
  co::Coroutine co1(scheduler,
                    [&co1_run](co::Coroutine *c) { co1_run = true; });

  co::Coroutine co2(scheduler,
                    [&co2_run](co::Coroutine *c) { co2_run = true; });

  scheduler.Run();
  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
}

TEST(Coroutines, SimpleRef) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;
  co::Coroutine co1(scheduler,
                    [&co1_run](const co::Coroutine &c) { co1_run = true; });

  co::Coroutine co2(scheduler,
                    [&co2_run](const co::Coroutine &c) { co2_run = true; });

  scheduler.Run();
  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
}

TEST(Coroutines, Yield) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;
  int co1_yields = 0;
  int co2_yields = 0;

  co::Coroutine co1(scheduler, [&co1_run, &co1_yields](co::Coroutine *c) {
    co1_run = true;
    c->Yield();
    co1_yields++;
    c->Yield();
    co1_yields++;
  });

  co::Coroutine co2(scheduler, [&co2_run, &co2_yields](co::Coroutine *c) {
    co2_run = true;
    c->Yield();
    co2_yields++;
    c->Yield();
    co2_yields++;
  });

  scheduler.Run();
  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
  ASSERT_EQ(2, co1_yields);
  ASSERT_EQ(2, co2_yields);
}

TEST(Coroutines, YieldRef) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;
  int co1_yields = 0;
  int co2_yields = 0;

  co::Coroutine co1(scheduler, [&co1_run, &co1_yields](const co::Coroutine &c) {
    co1_run = true;
    c.Yield();
    co1_yields++;
    c.Yield();
    co1_yields++;
  });

  co::Coroutine co2(scheduler, [&co2_run, &co2_yields](const co::Coroutine &c) {
    co2_run = true;
    c.Yield();
    co2_yields++;
    c.Yield();
    co2_yields++;
  });

  scheduler.Run();
  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
  ASSERT_EQ(2, co1_yields);
  ASSERT_EQ(2, co2_yields);
}

TEST(Coroutines, Wait) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;

  int pipe1[2];
  int pipe2[2];

  pipe(pipe1);
  pipe(pipe2);

  int co1_value = 0;
  int co2_value = 0;

  bool co1_wokeup = false;
  bool co2_wokeup = false;

  co::Coroutine co1(scheduler, [&co1_run, pipe1, pipe2, &co1_value,
                                &co1_wokeup](co::Coroutine *c) {
    co1_run = true;
    char buf[1] = {1};
    (void)write(pipe2[1], buf, 1);
    co1_value = c->Wait(pipe1[0]);
    co1_wokeup = true;
  });

  co::Coroutine co2(scheduler, [&co2_run, pipe1, pipe2, &co2_value,
                                &co2_wokeup](co::Coroutine *c) {
    co2_run = true;
    co2_value = c->Wait(pipe2[0]);
    co2_wokeup = true;
    char buf[1] = {2};
    (void)write(pipe1[1], buf, 1);
  });

  scheduler.Run();

  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
  ASSERT_TRUE(co1_wokeup);
  ASSERT_TRUE(co2_wokeup);
  ASSERT_EQ(pipe1[0], co1_value);
  ASSERT_EQ(pipe2[0], co2_value);
}

TEST(Coroutines, WaitRef) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;

  int pipe1[2];
  int pipe2[2];

  pipe(pipe1);
  pipe(pipe2);

  int co1_value = 0;
  int co2_value = 0;

  bool co1_wokeup = false;
  bool co2_wokeup = false;

  co::Coroutine co1(scheduler, [&co1_run, pipe1, pipe2, &co1_value,
                                &co1_wokeup](const co::Coroutine &c) {
    co1_run = true;
    char buf[1] = {1};
    (void)write(pipe2[1], buf, 1);
    co1_value = c.Wait(pipe1[0]);
    co1_wokeup = true;
  });

  co::Coroutine co2(scheduler, [&co2_run, pipe1, pipe2, &co2_value,
                                &co2_wokeup](const co::Coroutine &c) {
    co2_run = true;
    co2_value = c.Wait(pipe2[0]);
    co2_wokeup = true;
    char buf[1] = {2};
    (void)write(pipe1[1], buf, 1);
  });

  scheduler.Run();

  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
  ASSERT_TRUE(co1_wokeup);
  ASSERT_TRUE(co2_wokeup);
  ASSERT_EQ(pipe1[0], co1_value);
  ASSERT_EQ(pipe2[0], co2_value);
}

TEST(Coroutines, Timeout) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;

  int pipe1[2];
  int pipe2[2];

  pipe(pipe1);
  pipe(pipe2);

  int co1_value = 0;
  int co2_value = 0;

  bool co1_wokeup = false;
  bool co2_wokeup = false;

  co::Coroutine co1(
      scheduler, [&co1_run, pipe1, &co1_value, &co1_wokeup](co::Coroutine *c) {
        co1_run = true;
        co1_value = c->Wait(pipe1[0], POLLIN, 1000 * 1000 * 1000);
        co1_wokeup = true;
      });

  co::Coroutine co2(
      scheduler, [&co2_run, pipe2, &co2_value, &co2_wokeup](co::Coroutine *c) {
        co2_run = true;
        co2_value = c->Wait(pipe2[0], POLLIN, 1000 * 1000 * 1000);
        co2_wokeup = true;
      });

  scheduler.Run();

  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
  ASSERT_TRUE(co1_wokeup);
  ASSERT_TRUE(co2_wokeup);
  ASSERT_EQ(-1, co1_value);
  ASSERT_EQ(-1, co2_value);
}

TEST(Coroutines, TimeoutRef) {
  co::CoroutineScheduler scheduler;
  bool co1_run = false;
  bool co2_run = false;

  int pipe1[2];
  int pipe2[2];

  pipe(pipe1);
  pipe(pipe2);

  int co1_value = 0;
  int co2_value = 0;

  bool co1_wokeup = false;
  bool co2_wokeup = false;

  co::Coroutine co1(scheduler, [&co1_run, pipe1, &co1_value,
                                &co1_wokeup](const co::Coroutine &c) {
    co1_run = true;
    co1_value = c.Wait(pipe1[0], POLLIN, 1000 * 1000 * 1000);
    co1_wokeup = true;
  });

  co::Coroutine co2(scheduler, [&co2_run, pipe2, &co2_value,
                                &co2_wokeup](const co::Coroutine &c) {
    co2_run = true;
    co2_value = c.Wait(pipe2[0], POLLIN, 1000 * 1000 * 1000);
    co2_wokeup = true;
  });

  scheduler.Run();

  ASSERT_TRUE(co1_run);
  ASSERT_TRUE(co2_run);
  ASSERT_TRUE(co1_wokeup);
  ASSERT_TRUE(co2_wokeup);
  ASSERT_EQ(-1, co1_value);
  ASSERT_EQ(-1, co2_value);
}

TEST(Coroutines, Generator) {
  co::CoroutineScheduler scheduler;
  int factorial = 1;
  co::Coroutine co(scheduler, [&factorial](co::Coroutine *c) {
    co::Generator<int> generator(c->Scheduler(), [](co::Generator<int> *c) {
      for (int i = 1; i < 10; i++) {
        c->YieldValue(i);
      }
    });

    while (generator.IsAlive()) {
      int value = c->Call(generator);
      if (generator.IsAlive()) {
        factorial *= value;
      }
    }
  });

  scheduler.Run();
  ASSERT_EQ(9 * 8 * 7 * 6 * 5 * 4 * 3 * 2, factorial);
}

TEST(Coroutines, GeneratorRef) {
  co::CoroutineScheduler scheduler;
  int factorial = 1;
  co::Coroutine co(scheduler, [&factorial](const co::Coroutine &c) {
    co::Generator<int> generator(c.Scheduler(),
                                 [](const co::Generator<int> &c) {
                                   for (int i = 1; i < 10; i++) {
                                     c.YieldValue(i);
                                   }
                                 });

    while (generator.IsAlive()) {
      int value = c.Call(generator);
      if (generator.IsAlive()) {
        factorial *= value;
      }
    }
  });

  scheduler.Run();
  ASSERT_EQ(9 * 8 * 7 * 6 * 5 * 4 * 3 * 2, factorial);
}