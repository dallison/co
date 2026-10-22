// Copyright 2024-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

// Common declarations and architecture dispatch for the custom context
// switcher. Architecture-specific implementations are included below so this
// remains the single assembly translation unit exposed by the build systems.
#if defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) ||         \
    defined(__arm__) || defined(__riscv)

// Apple puts an underscore prefix on all external symbols.
#if defined(__APPLE__)
#define SYM(name) _##name
#define SYM_DATA(name) .p2align 2
.section __TEXT,__text,regular,pure_instructions

#elif defined(__linux__) || defined(__QNX__) || defined(__QNXNTO__)
// QNX uses ELF and the same GNU-as syntax as Linux for symbol decoration,
// so the same directives work for both.
.text
#if defined(__arm__)
#define SYM(name) name
#define SYM_DATA(name) .type SYM(name), %function
#else
#define SYM(name) #name
#define SYM_DATA(name) .type SYM(name), @function
#endif
#else
#error "Unknown OS"
#endif

.global SYM(CoroutineMakeContext)
SYM_DATA(CoroutineMakeContext)

.global SYM(CoroutineSwapContext)
SYM_DATA(CoroutineSwapContext)

.global SYM(CoroutineGetContext)
SYM_DATA(CoroutineGetContext)

.global SYM(CoroutineSetContext)
SYM_DATA(CoroutineSetContext)

.global SYM(abort)
SYM_DATA(abort)

// Register storage area is 32 bytes into CoroutineContext.
#define reg_start 32

// See context.h for the architecture-specific register save layouts.
#if defined(__x86_64__)
#include "context_x86_64.S"
#elif defined(__i386__)
#include "context_i386.S"
#elif defined(__aarch64__)
#include "context_aarch64.S"
#elif defined(__arm__)
#include "context_arm.S"
#elif defined(__riscv)
#include "context_riscv.S"
#endif

#undef reg_start
#undef SYM_DATA
#undef SYM
#endif

#if defined(__linux__)
#if defined(__arm__) || defined(__aarch64__)
.section .note.GNU-stack,"",%progbits
#else
.section .note.GNU-stack,"",@progbits
#endif
#endif
