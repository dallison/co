// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "coroutine.h"
#include <stdio.h>
#include <unistd.h>


using namespace co;

int pipes[2];

void Co1(Coroutine *c) {
  Coroutine generator(c->Machine(), [](Coroutine *c) {
    for (int i = 1; i < 5; i++) {
      c->YieldValue(&i);
    }
  });

  while (c->IsAlive(generator)) {
    int value = 0;
    c->Call(generator, &value, sizeof(value));
    if (c->IsAlive(generator)) {
      printf("Value: %d\n", value);
      c->Millisleep(1000);
    }
  }
}

void Writer(Coroutine *c) {
  for (int i = 0; i < 20; i++) {
    char buf[256];
    size_t n = snprintf(buf, sizeof(buf), "FOO %d\n", i);
    c->Wait(pipes[1], POLLOUT);
    write(pipes[1], buf, n);
    c->Yield();
  }
  close(pipes[1]);
}

void Reader(Coroutine *c) {
  for (;;) {
    char buf[256];
    c->Wait(pipes[0], POLLIN);
    ssize_t n = read(pipes[0], buf, sizeof(buf));
    if (n == 0) {
      printf("EOF\n");
      break;
    }
    buf[n] = '\0';
    printf("Received: %s", buf);
  }
  close(pipes[0]);
}

int main(int argc, const char *argv[]) {
  pipe(pipes);

  CoroutineMachine m;
  Coroutine c1(m, Co1);

  c1.Start();

  Coroutine writer(m, Writer);
  Coroutine reader(m, Reader);

  reader.Start();
  writer.Start();

  m.Run();
}
