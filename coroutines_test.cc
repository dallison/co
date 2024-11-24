#include <gtest/gtest.h>
#include "coroutine.h"

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
    coroutines.push_back(std::make_unique<co::Coroutine>(scheduler, [](co::Coroutine *c) {
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