// Copyright 2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include <cstdlib>
#include <iostream>

#include "co/coroutine.h"

class OneShotEvent {
 public:
  OneShotEvent() : event_(co::EventFd::Create()) {
    if (!event_.IsValid()) {
      std::cerr << "failed to create one-shot event\n";
      std::exit(1);
    }
  }

  ~OneShotEvent() { event_.Close(); }

  OneShotEvent(const OneShotEvent &) = delete;
  OneShotEvent &operator=(const OneShotEvent &) = delete;

  void Wait() {
    if (!signaled_) {
      co::Wait(event_.poll_fd);
    }
    event_.Clear();
  }

  void Signal() {
    if (signaled_) {
      return;
    }
    signaled_ = true;
    event_.Trigger();
  }

 private:
  co::EventFd event_;
  bool signaled_ = false;
};

struct Mailbox {
  int value = 0;
  OneShotEvent ready;
  OneShotEvent acknowledged;
};

void Sender(Mailbox *mailbox) {
  mailbox->value = 42;
  mailbox->ready.Signal();
  mailbox->acknowledged.Wait();
}

void Receiver(Mailbox *mailbox) {
  mailbox->ready.Wait();
  std::cout << "received: " << mailbox->value << '\n';
  mailbox->acknowledged.Signal();
}

int main() {
  Mailbox mailbox;
  co::CoroutineScheduler scheduler;

  scheduler.Spawn([&mailbox]() { Receiver(&mailbox); });
  scheduler.Spawn([&mailbox]() { Sender(&mailbox); });

  scheduler.Run();
  return 0;
}
