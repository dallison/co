#pragma once

#include <cstddef>

namespace co {

class Coroutine;

struct alignas(16) CoroutineContext {
  void *stack;
  size_t stack_size;
  CoroutineContext *next;
  char padding[8];

// Offset 32  - 16 byte alignment.
#if defined(__x86_64__)
  char regs[640];
#elif defined(__aarch64__)
  char regs[320];
#else
#error "Unsupported architecture"
#endif
};

extern "C" {
  void CoroutineMakeContext(CoroutineContext * ctx, void (*func)(void *),
                            void *arg);
  void CoroutineSwapContext(CoroutineContext * from, CoroutineContext * to);
  void CoroutineGetContext(CoroutineContext * ctx);
  void CoroutineSetContext(CoroutineContext * ctx);
}
} // namespace co
