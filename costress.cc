#include <stdio.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include "coroutine.h"

using namespace co;

void Test(Coroutine* c) {
  for (int i = 0; i < 10; i++) {
    // printf("%s: %d\n", c->Name().c_str(), i);
    c->Yield();
  }
}

int main(int argc, char** argv) {
  CoroutineScheduler scheduler;
  std::vector<std::unique_ptr<Coroutine>> coroutines;
  for (int i = 0; i < 1000; i++) {
    coroutines.push_back(std::make_unique<Coroutine>(scheduler, Test));
  }
  scheduler.Run();
}
