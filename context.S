
// Apple puts an underscore prefix for all external symbols.
#if defined(__APPLE__)
#define SYM(name) _##name
#define TYPE(name) .p2align 2
.section        __TEXT,__text,regular,pure_instructions

#elif defined(__linux__)
.section ".text", "x"
#define SYM(name) #name
#define TYPE(name) .type SYM(name), @function
#else
#error "Unknown OS"
#endif

.global SYM(CoroutineMakeContext)
TYPE(CoroutineMakeContext)

.global SYM(CoroutineSwapContext)
TYPE(CoroutineSwapContext)

.global SYM(CoroutineGetContext)
TYPE(CoroutineGetContext)

.global SYM(CoroutineSetContext)
TYPE(CoroutineSetContext)

.global SYM(abort)
TYPE(abort)

// Register storage area is 24 bytes into context.
#define reg_start 32


#if defined(__x86_64__)

#define reg_rbx 0+reg_start
#define reg_rcx 8+reg_start
#define reg_rdx 16+reg_start
#define reg_r8 24+reg_start
#define reg_r9 32+reg_start
#define reg_r12 40+reg_start
#define reg_r13 48+reg_start
#define reg_r14 56+reg_start
#define reg_r15 64+reg_start
#define reg_rsp 72+reg_start
#define reg_rbp 80+reg_start
#define reg_rsi 88+reg_start
#define reg_rdi 96+reg_start
#define reg_rip 104+reg_start

#define reg_fp 112+reg_start
#define reg_fp_size 512
#define reg_mxcsr 624+reg_start // 4 bytes


2:
  mov 0(%rsp), %rdi
  test %rdi, %rdi
  je 1f
  call SYM(CoroutineSetContext)
1:
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

  add $-16, %rsi      // Align to 16
  sub $8, %rsi
  mov %rsi, reg_rsp(%rdi)   // Stack pointer in context

  mov 16(%rdi), %rax        
  mov %rax, 8(%rsi)         // Next context in context

  lea 2b(%rip), %rax       // Place to go when finished.
  mov %rax, 0(%rsi)

  ret

#define STORE_REG(r) mov %r, reg_##r(%rdi)
#define LOAD_REG(r) mov reg_##r(%rdi), %r

SYM(CoroutineGetContext):
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
  
  // Return address is on Stack
  mov (%rsp), %rax
  mov %rax, reg_rip(%rdi)

  // Previous SP is just above return address
  lea 8(%rsp), %rax
  mov %rax, reg_rsp(%rdi)

  // Floating point stuff.
  lea reg_fp(%rdi), %rax
  fnstenv (%rax)
  stmxcsr reg_mxcsr(%rdi)
  ret

SYM(CoroutineSetContext):
  lea reg_fp(%rdi), %rax
  fldenv (%rax)
  ldmxcsr reg_mxcsr(%rdi)

  LOAD_REG(rbx)
  LOAD_REG(rcx)
  LOAD_REG(rdx)
  LOAD_REG(r8)
  LOAD_REG(r9)
  LOAD_REG(r12)
  LOAD_REG(r13)
  LOAD_REG(r14)
  LOAD_REG(rsp)
  LOAD_REG(rbp)
  LOAD_REG(rsi)

  // Push return address
  mov reg_rip(%rdi), %rax
  push %rax

  // Restore rdi.
  mov reg_rdi(%rdi), %rdi
  ret

#undef LOAD_REG
// Redefine LOAD_REG to load from %rax
#define LOAD_REG(r) mov reg_##r(%rax), %r


SYM(CoroutineSwapContext):
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
  
  // Return address is on Stack
  mov (%rsp), %rax
  mov %rax, reg_rip(%rdi)

  // Previous SP is just above return address
  lea 8(%rsp), %rax
  mov %rax, reg_rsp(%rdi)

  // Floating point stuff.
  lea reg_fp(%rdi), %rax
  fnstenv (%rax)
  fldenv (%rax)
  stmxcsr reg_mxcsr(%rdi)

  // Get load context in rax
  mov %rsi, %rax

  lea reg_fp(%rdi), %rsi
  fldenv (%rsi)
  ldmxcsr reg_mxcsr(%rax)

  LOAD_REG(rbx)
  LOAD_REG(rcx)
  LOAD_REG(rdx)
  LOAD_REG(r8)
  LOAD_REG(r9)
  LOAD_REG(r12)
  LOAD_REG(r13)
  LOAD_REG(r14)
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

#elif defined(__aarch64__)
#define reg_x18 0+reg_start
#define reg_x19 8+reg_start
#define reg_x20 16+reg_start
#define reg_x21 24+reg_start
#define reg_x22 32+reg_start
#define reg_x23 40+reg_start
#define reg_x24 48+reg_start
#define reg_x25 56+reg_start
#define reg_x26 64+reg_start
#define reg_x27 72+reg_start
#define reg_x28 80+reg_start
#define reg_x29 88+reg_start
#define reg_x30 96+reg_start
#define reg_sp 104+reg_start
#define reg_pc 112+reg_start
#define reg_x0 120+reg_start

#define reg_fpsr 128+reg_start
#define reg_fpcr 132+reg_start

// Floating point
#define reg_q8 160+reg_start
#define reg_q9 176+reg_start
#define reg_q10 192+reg_start
#define reg_q11 208+reg_start
#define reg_q12 224+reg_start
#define reg_q13 240+reg_start
#define reg_q14 256+reg_start
#define reg_q15 272+reg_start

2:
  mov x0, x19
  cbz x0, 1f
  b SYM(CoroutineSetContext)

1:
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
  adr x1, 2b
  str x1, [x0, #reg_x30]      // link reg
  ret

#define STORE_REG(r) str r, [x0, # (reg_##r)]
#define STORE_PAIR(r1, r2) stp r1, r2, [x0, # (reg_##r1)]
#define LOAD_REG(r) ldr r, [x0, # (reg_##r)]
#define LOAD_PAIR(r1, r2) ldp r1, r2, [x0, # (reg_##r1)]

SYM(CoroutineGetContext):
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
  ret


SYM(CoroutineSetContext):
  LOAD_PAIR(x18, x19)
  LOAD_PAIR(x20, x21)
  LOAD_PAIR(x22, x23)
  LOAD_PAIR(x24, x25)
  LOAD_PAIR(x26, x27)
  LOAD_PAIR(x28, x29)
  ldr x30, [x0, #reg_x30]
  
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

  mov x0, x1
  b SYM(CoroutineSetContext)

#undef STORE_REG
#undef STORE_PAIR
#undef LOAD_REG
#undef LOAD_PAIR
#undef SYM
#else
#error "Unsupported architecture"
#endif

