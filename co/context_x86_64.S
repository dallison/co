// Copyright 2024-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
//
// x86-64 context implementation. Included by context.S.

// Register layout in save area.
#define reg_rbx 0 + reg_start
#define reg_rcx 8 + reg_start
#define reg_rdx 16 + reg_start
#define reg_r8 24 + reg_start
#define reg_r9 32 + reg_start
#define reg_r12 40 + reg_start
#define reg_r13 48 + reg_start
#define reg_r14 56 + reg_start
#define reg_r15 64 + reg_start
#define reg_rsp 72 + reg_start
#define reg_rbp 80 + reg_start
#define reg_rsi 88 + reg_start
#define reg_rdi 96 + reg_start
#define reg_rip 104 + reg_start

#define reg_fp 112 + reg_start
#define reg_fp_size 512
#define reg_mxcsr 624 + reg_start // 4 bytes

// We get here when the coroutine's function returns.
1:
  mov 0(%rsp), %rdi
  test %rdi, %rdi
  je 2f         // Zero means abort.
  call SYM(CoroutineSetContext)
2:
  call SYM(abort)

// Entry:
// rdi: context pointer from CoroutineGetContext
// rsi: function to invoke in context
// rdx: coroutine point to pass to function
SYM(CoroutineMakeContext):
  mov %rsi, reg_rip(%rdi)
  mov %rdx, reg_rdi(%rdi)

  mov 0(%rdi), %rsi   // Stack pointer
  add 8(%rdi), %rsi   // End of Stack
  sub $8, %rsi        // Space for next context pointer

  and $-16, %rsi      // Align to 16
  sub $8, %rsi
  mov %rsi, reg_rsp(%rdi)   // Stack pointer in context

  mov 16(%rdi), %rax
  mov %rax, 8(%rsi)         // Next context in context

  lea 1b(%rip), %rax       // Place to go when finished.
  mov %rax, 0(%rsi)

  ret

#define STORE_REG(r) mov %r, reg_##r(%rdi)
#define LOAD_REG(r) mov reg_##r(%rdi), %r

// Entry:
// rdi: address of context
SYM(CoroutineGetContext):
  // Store all registers in save area.
  STORE_REG(rbx)
  STORE_REG(rcx)
  STORE_REG(rdx)
  STORE_REG(r8)
  STORE_REG(r9)
  STORE_REG(r12)
  STORE_REG(r13)
  STORE_REG(r14)
  STORE_REG(r15)
  STORE_REG(rbp)
  STORE_REG(rsi)
  STORE_REG(rdi)

  // Return address is on stack, get it and store in %rip
  mov (%rsp), %rax
  mov %rax, reg_rip(%rdi)

  // Previous SP is just above return address, store this in %rsp
  lea 8(%rsp), %rax
  mov %rax, reg_rsp(%rdi)

  // Floating point stuff.
  lea reg_fp(%rdi), %rax
  fnstenv (%rax)
  fldenv (%rax)   // Why do I need this instruction?  It doesn't work without it.
  stmxcsr reg_mxcsr(%rdi)
  ret

// Entry:
// rdi: address of context
SYM(CoroutineSetContext):
  // Load floating point save data.  fnstenv (%rax)

  lea reg_fp(%rdi), %rax
  fldenv (%rax)
  ldmxcsr reg_mxcsr(%rdi)

  // Load registers.
  LOAD_REG(rbx)
  LOAD_REG(rcx)
  LOAD_REG(rdx)
  LOAD_REG(r8)
  LOAD_REG(r9)
  LOAD_REG(r12)
  LOAD_REG(r13)
  LOAD_REG(r14)
  LOAD_REG(r15)
  LOAD_REG(rsp)
  LOAD_REG(rbp)
  LOAD_REG(rsi)

  // Push return address
  mov reg_rip(%rdi), %rax
  push %rax

  // Restore rdi.
  mov reg_rdi(%rdi), %rdi

  // Return to pushed return address.
  ret

#undef LOAD_REG
// Redefine LOAD_REG to load from %rax
#define LOAD_REG(r) mov reg_##r(%rax), %r

// Entry:
// rdi: 'from' context
// rsi: 'to' context
SYM(CoroutineSwapContext):
  // Store current registers in 'from' context
  STORE_REG(rbx)
  STORE_REG(rcx)
  STORE_REG(rdx)
  STORE_REG(r8)
  STORE_REG(r9)
  STORE_REG(r12)
  STORE_REG(r13)
  STORE_REG(r14)
  STORE_REG(r15)
  STORE_REG(rbp)
  STORE_REG(rsi)
  STORE_REG(rdi)
  STORE_REG(rbx)

  // Return address is on stack, store it in rdi
  mov (%rsp), %rax
  mov %rax, reg_rip(%rdi)

  // Previous SP is just above return address
  lea 8(%rsp), %rax
  mov %rax, reg_rsp(%rdi)

  // Store Floating point stuff.
  lea reg_fp(%rdi), %rax
  fnstenv (%rax)
  stmxcsr reg_mxcsr(%rdi)

  // Get 'to' context in rax
  mov %rsi, %rax

  lea reg_fp(%rdi), %rsi
  fldenv (%rsi)
  ldmxcsr reg_mxcsr(%rax)

  // Load registers from (rax)
  LOAD_REG(rbx)
  LOAD_REG(rcx)
  LOAD_REG(rdx)
  LOAD_REG(r8)
  LOAD_REG(r9)
  LOAD_REG(r12)
  LOAD_REG(r13)
  LOAD_REG(r14)
  LOAD_REG(r15)
  LOAD_REG(rsp)
  LOAD_REG(rbp)
  LOAD_REG(rsi)
  LOAD_REG(rdi)

  // Push return address
  mov reg_rip(%rax), %rax
  push %rax
  ret

#undef STORE_REG
#undef LOAD_REG
