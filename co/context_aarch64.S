// Copyright 2024-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
//
// AArch64 context implementation. Included by context.S.

// Register layout in save area.
#define reg_x18 0 + reg_start
#define reg_x19 8 + reg_start
#define reg_x20 16 + reg_start
#define reg_x21 24 + reg_start
#define reg_x22 32 + reg_start
#define reg_x23 40 + reg_start
#define reg_x24 48 + reg_start
#define reg_x25 56 + reg_start
#define reg_x26 64 + reg_start
#define reg_x27 72 + reg_start
#define reg_x28 80 + reg_start
#define reg_x29 88 + reg_start
#define reg_x30 96 + reg_start
#define reg_sp 104 + reg_start
#define reg_pc 112 + reg_start
#define reg_x0 120 + reg_start

// Floating point status and control.
#define reg_fpsr 128 + reg_start
#define reg_fpcr 132 + reg_start

// Floating point registers.
#define reg_q8 160 + reg_start
#define reg_q9 176 + reg_start
#define reg_q10 192 + reg_start
#define reg_q11 208 + reg_start
#define reg_q12 224 + reg_start
#define reg_q13 240 + reg_start
#define reg_q14 256 + reg_start
#define reg_q15 272 + reg_start

// This is the address that the coroutine returns to when
// its function is done.  The context address is in x19.
1:
  mov x0, x19
  cbz x0, 2f      // Must not be zero, or we abort.
  b SYM(CoroutineSetContext)

2:
  b SYM(abort)

// Entry:
// x0: context
// x1: function to call
// x2: arg to func (coroutine pointer)
SYM(CoroutineMakeContext):
  str x1, [x0, #reg_pc]
  str x2, [x0, #reg_x0]

  ldr x1, [x0, #0]
  ldr x2, [x0, #8]
  add x1, x1, x2      // End of Stack

  bic x1, x1, #16   // Aligned to 16

  str x1, [x0, #reg_sp]
  ldr x1, [x0, #16]   // next context
  str x1, [x0, #reg_x19]    // x19 is used by return context
  str xzr, [x0, #reg_x29]   // No frame pointer
  adr x1, 1b
  str x1, [x0, #reg_x30]      // link reg
  ret

#define STORE_REG(r) str r, [x0, # (reg_##r)]
#define STORE_PAIR(r1, r2) stp r1, r2, [x0, # (reg_##r1)]
#define LOAD_REG(r) ldr r, [x0, # (reg_##r)]
#define LOAD_PAIR(r1, r2) ldp r1, r2, [x0, # (reg_##r1)]

// Entry:
// x0: address of context
SYM(CoroutineGetContext):
  STORE_PAIR(x18, x19)
  STORE_PAIR(x20, x21)
  STORE_PAIR(x22, x23)
  STORE_PAIR(x24, x25)
  STORE_PAIR(x26, x27)
  STORE_PAIR(x28, x29)
  STORE_REG(x30)
  // Also store link register as PC for return.
  str x30, [x0, #reg_pc]

  mov x2, sp
  str x2, [x0, #reg_sp]     // Stack pointer

  STORE_PAIR(q8, q9)
  STORE_PAIR(q10, q11)
  STORE_PAIR(q12, q13)
  STORE_PAIR(q14, q15)

  mrs x2, fpsr
  str w2, [x0, #reg_fpsr]
  mrs x2, fpcr
  str w2, [x0, #reg_fpcr]
  ret

// Entry:
// x0: address of context
SYM(CoroutineSetContext):
  LOAD_PAIR(x18, x19)
  LOAD_PAIR(x20, x21)
  LOAD_PAIR(x22, x23)
  LOAD_PAIR(x24, x25)
  LOAD_PAIR(x26, x27)
  LOAD_PAIR(x28, x29)
  LOAD_REG(x30)

  // Set the stack pointer from saved sp.
  ldr x2, [x0, #reg_sp]
  mov sp, x2

  LOAD_PAIR(q8, q9)
  LOAD_PAIR(q10, q11)
  LOAD_PAIR(q12, q13)
  LOAD_PAIR(q14, q15)

  ldr w2, [x0, #reg_fpsr]
  msr fpsr, x2
  ldr w2, [x0, #reg_fpcr]
  msr fpcr, x2

  ldr x1, [x0, #reg_pc]
  ldr x0, [x0, #reg_x0]
  br x1

// Entry:
// x0: address of 'from' context
// x1: address of 'to' context
SYM(CoroutineSwapContext):
  STORE_PAIR(x18, x19)
  STORE_PAIR(x20, x21)
  STORE_PAIR(x22, x23)
  STORE_PAIR(x24, x25)
  STORE_PAIR(x26, x27)
  STORE_PAIR(x28, x29)
  STORE_REG(x30)
  str x30, [x0, #reg_pc]

  mov x2, sp
  str x2, [x0, #reg_sp]     // Stack pointer

  STORE_PAIR(q8, q9)
  STORE_PAIR(q10, q11)
  STORE_PAIR(q12, q13)
  STORE_PAIR(q14, q15)

  mrs x2, fpsr
  str w2, [x0, #reg_fpsr]
  mrs x2, fpcr
  str w2, [x0, #reg_fpcr]

  // Now swap to the 'to' context, whose address is in x1.
  mov x0, x1
  b SYM(CoroutineSetContext)

#undef STORE_REG
#undef STORE_PAIR
#undef LOAD_REG
#undef LOAD_PAIR
