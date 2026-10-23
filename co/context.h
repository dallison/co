#pragma once

// Copyright 2024-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include <cstddef>

namespace co {

// This is only supported on x86, x86_64, ARM, AArch64, RV32, and RV64.
#if defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) ||         \
    defined(__arm__) || defined(__riscv)
#if defined(__riscv) && defined(__riscv_32e)
#error "RV32E is not supported by the custom context switcher"
#endif
#if defined(__riscv) && (__riscv_xlen != 32) && (__riscv_xlen != 64)
#error "Unsupported RISC-V register width"
#endif
// Custom coroutine context.  Contains the stack and register save area/
// Needs 16-byte alignment for aarch64.
struct alignas(16) CoroutineContext {
  void *stack;            // Heap-allocated stack.
  size_t stack_size;      // Size of the stack in bytes.
  CoroutineContext *next; // Where to go when coroutine function returns.
#if defined(__arm__) || defined(__i386__) ||                                  \
    (defined(__riscv) && (__riscv_xlen == 32))
  char padding[20];       // Keep the register save area at offset 32.
#else
  char padding[8];        // Set size to 32 bytes.
#endif

// See the architecture-specific context_*.S file for the register save layout.
#if defined(__x86_64__)
  char regs[664];
#elif defined(__i386__)
  char regs[56];
#elif defined(__aarch64__)
  char regs[320];
#elif defined(__arm__)
  char regs[120];
#elif defined(__riscv) && (__riscv_xlen == 64)
#if defined(__riscv_float_abi_single) || defined(__riscv_float_abi_double)
  char regs[232];
#else
  char regs[128];
#endif
#elif defined(__riscv) && (__riscv_xlen == 32)
#if defined(__riscv_float_abi_single) || defined(__riscv_float_abi_double)
  char regs[168];
#else
  char regs[64];
#endif
#endif
};

static_assert(offsetof(CoroutineContext, regs) == 32,
              "Coroutine register save area must start at offset 32");

// C linkage to allow us to define them in assembly language.
extern "C" {
void CoroutineMakeContext(CoroutineContext *ctx, void (*func)(void *),
                          void *arg);
void CoroutineSwapContext(CoroutineContext *from, CoroutineContext *to);
void CoroutineGetContext(CoroutineContext *ctx);
void CoroutineSetContext(CoroutineContext *ctx);
}
#endif

} // namespace co
