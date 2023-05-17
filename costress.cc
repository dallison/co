#include "coroutine.h"
#include <stdio.h>
#include <unistd.h>
#include <vector>
#include <memory>

using namespace co;

void Test(Coroutine* c) {
  for (int i = 0; i < 10; i++) {
    printf("%s: %d\n", c->Name().c_str(), i);
    c->Yield();
  }
}

int main(int argc, char** argv) {
  CoroutineScheduler scheduler;
  std::vector<std::unique_ptr<Coroutine>> coroutines;
  for (int i = 0; i < 100; i++) {
    coroutines.push_back(std::make_unique<Coroutine>(scheduler, Test));
  }
  scheduler.Run();
}