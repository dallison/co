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
      c->YieldValue(i);
    }
  });

  while (generator.IsAlive()) {
    int value = c->Call<int>(generator);
    if (generator.IsAlive()) {
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
    (void)write(pipes[1], buf, n);
    // Yield here so that all the writes don't go at once.
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

void TestWaitWithTimeout(Coroutine *c) {
  int waitpipe1[2];
  int waitpipe2[2];
  int waitpipe3[2];
  (void)pipe(waitpipe1);
  (void)pipe(waitpipe2);
  (void)pipe(waitpipe3);

  int wait1_end = waitpipe1[0];
  int trigger1_end = waitpipe1[1];
  int wait2_end = waitpipe2[0];
  int trigger2_end = waitpipe2[1];
  int wait3_end = waitpipe3[0];
  int trigger3_end = waitpipe3[1];

  // Waits for a single fd with a timeout.
  auto wait1_func = [wait1_end](Coroutine *c) {
    printf("Waiter %s waiting\n", c->Name().c_str());
    int fd = c->Wait(wait1_end, POLLIN, 1000000000); // Wait 1 second.
    if (fd == -1) {
      printf("Waiter %s resumed due to timeout\n", c->Name().c_str());
    } else if (fd == wait1_end) {
      printf("Waiter %s resumed due to input ready\n", c->Name().c_str());
      char buf[1];
      (void)read(fd, buf, 1); // Clear pipe.
    } else {
      printf("Waiter %s resumed due to unknown value %d\n", c->Name().c_str(),
             fd);
    }
  };

  // Waits for multiple fds with timeout.
  auto wait2_func = [wait2_end, wait3_end](Coroutine *c) {
    printf("Waiter %s waiting\n", c->Name().c_str());
    struct pollfd fd1 = {.fd = wait2_end, .events = POLLIN};
    struct pollfd fd2 = {.fd = wait3_end, .events = POLLIN};
    int fd = c->Wait({fd1, fd2}, 1000000000); // Wait 1 second.
    if (fd == -1) {
      printf("Waiter %s resumed due to timeout\n", c->Name().c_str());
    } else if (fd == wait3_end) {
      printf("Waiter %s resumed due to input ready\n", c->Name().c_str());
      char buf[1];
      (void)read(fd, buf, 1); // Clear pipe.
    } else {
      printf("Waiter %s resumed due to unknown value %d\n", c->Name().c_str(),
             fd);
    }
  };

  Coroutine waiter1(c->Machine(), wait1_func, "waiter1");
  c->Sleep(2); // Cause timeout in waiter1.

  Coroutine waiter2(c->Machine(), wait1_func, "waiter2");
  // Trigger waiter2.
  (void)write(trigger1_end, "x", 1);

  Coroutine waiter3(c->Machine(), wait2_func, "waiter3");
  c->Sleep(2); // Cause timeout in waiter3.

  Coroutine waiter4(c->Machine(), wait2_func, "waiter4");
  // Trigger waiter4.
  (void)write(trigger3_end, "x", 1);

  // Don't forget to tidy up.
  close(wait1_end);
  close(trigger1_end);
  close(wait2_end);
  close(trigger2_end);
  close(wait3_end);
  close(trigger3_end);
}

int main(int argc, const char *argv[]) {
  (void)pipe(pipes);

  CoroutineMachine m;
  Coroutine c1(m, Co1);

  c1.Start();

  Coroutine writer(m, Writer);
  Coroutine reader(m, Reader);

  reader.Start();
  writer.Start();

  Coroutine wait_test(m, TestWaitWithTimeout);

  m.Run();
}
