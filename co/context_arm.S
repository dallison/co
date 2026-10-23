// Copyright 2024-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
//
// AAPCS32 ARM context implementation. Included by context.S.

.syntax unified
.arm

// Register layout in save area.
#define reg_r4 0 + reg_start
#define reg_r5 4 + reg_start
#define reg_r6 8 + reg_start
#define reg_r7 12 + reg_start
#define reg_r8 16 + reg_start
#define reg_r9 20 + reg_start
#define reg_r10 24 + reg_start
#define reg_r11 28 + reg_start
#define reg_sp 32 + reg_start
#define reg_lr 36 + reg_start
#define reg_pc 40 + reg_start
#define reg_r0 44 + reg_start

// VFP control state and callee-saved registers.  These are used only when the
// target enables VFP instructions; the layout remains reserved otherwise.
#define reg_fpscr 48 + reg_start
#define reg_d8 56 + reg_start

// This is the address that the coroutine returns to when its function is
// done.  The next context is kept in r4, which is callee-saved by AAPCS32.
1:
  mov r0, r4
  cmp r0, #0
  beq 2f
  b SYM(CoroutineSetContext)

2:
  b SYM(abort)

// Entry:
// r0: context
// r1: function to call
// r2: argument to pass to the function
SYM(CoroutineMakeContext):
  str r1, [r0, #reg_pc]
  str r2, [r0, #reg_r0]

  ldr r1, [r0, #0]
  ldr r2, [r0, #4]
  add r1, r1, r2       // End of stack
  bic r1, r1, #7       // AAPCS32 public-interface stack alignment
  str r1, [r0, #reg_sp]

  ldr r2, [r0, #8]     // Next context
  str r2, [r0, #reg_r4]
  mov r2, #0
  str r2, [r0, #reg_r11] // No frame pointer
  adr r2, 1b
  str r2, [r0, #reg_lr]
  bx lr

// Entry:
// r0: address of context
SYM(CoroutineGetContext):
  str r0, [r0, #reg_r0]
  add r2, r0, #reg_r4
  stmia r2, {r4-r11}
  str sp, [r0, #reg_sp]
  str lr, [r0, #reg_lr]
  str lr, [r0, #reg_pc]

#if defined(__ARM_FP) && (__ARM_FP != 0)
  vmrs r2, fpscr
  str r2, [r0, #reg_fpscr]
  add r2, r0, #reg_d8
  vstmia r2, {d8-d15}
#endif
  bx lr

// Entry:
// r0: address of context
SYM(CoroutineSetContext):
#if defined(__ARM_FP) && (__ARM_FP != 0)
  ldr r2, [r0, #reg_fpscr]
  vmsr fpscr, r2
  add r2, r0, #reg_d8
  vldmia r2, {d8-d15}
#endif

  add r2, r0, #reg_r4
  ldmia r2, {r4-r11}
  ldr sp, [r0, #reg_sp]
  ldr lr, [r0, #reg_lr]
  ldr r2, [r0, #reg_pc]
  ldr r0, [r0, #reg_r0]
  bx r2

// Entry:
// r0: address of 'from' context
// r1: address of 'to' context
SYM(CoroutineSwapContext):
  str r0, [r0, #reg_r0]
  add r2, r0, #reg_r4
  stmia r2, {r4-r11}
  str sp, [r0, #reg_sp]
  str lr, [r0, #reg_lr]
  str lr, [r0, #reg_pc]

#if defined(__ARM_FP) && (__ARM_FP != 0)
  vmrs r2, fpscr
  str r2, [r0, #reg_fpscr]
  add r2, r0, #reg_d8
  vstmia r2, {d8-d15}
#endif

  mov r0, r1
  b SYM(CoroutineSetContext)
