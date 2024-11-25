#pragma once

// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include <cstddef>

namespace co {

// Custom coroutine context.  Contains the stack and register save area/
// Needs 16-byte alignment for aarch64.
struct alignas(16) CoroutineContext {
  void *stack;            // Heap-allocated stack.
  size_t stack_size;      // Size of the stack in bytes.
  CoroutineContext *next; // Where to go when coroutine function returns.
  char padding[8];        // Set size to 32 bytes.

// See context.S for the layout of the register save area.
#if defined(__x86_64__)
  char regs[640];
#elif defined(__aarch64__)
  char regs[320];
#else
#error "Unsupported architecture"
#endif
};

// C linkage to allow us to define them in assembly language.
extern "C" {
void CoroutineMakeContext(CoroutineContext *ctx, void (*func)(void *),
                          void *arg);
void CoroutineSwapContext(CoroutineContext *from, CoroutineContext *to);
void CoroutineGetContext(CoroutineContext *ctx);
void CoroutineSetContext(CoroutineContext *ctx);
}
} // namespace co
