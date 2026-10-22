// Copyright 2024-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
//
// Shared RV32/RV64 context implementation. Included by context.S.

#if defined(__riscv_32e)
#error "RV32E is not supported by the custom context switcher"
#endif

#if __riscv_xlen == 64
#define REG_L ld
#define REG_S sd
#define reg_s0 0 + reg_start
#define reg_s1 8 + reg_start
#define reg_s2 16 + reg_start
#define reg_s3 24 + reg_start
#define reg_s4 32 + reg_start
#define reg_s5 40 + reg_start
#define reg_s6 48 + reg_start
#define reg_s7 56 + reg_start
#define reg_s8 64 + reg_start
#define reg_s9 72 + reg_start
#define reg_s10 80 + reg_start
#define reg_s11 88 + reg_start
#define reg_sp 96 + reg_start
#define reg_ra 104 + reg_start
#define reg_pc 112 + reg_start
#define reg_a0 120 + reg_start
#define reg_fcsr 128 + reg_start
#define reg_fs0 136 + reg_start
#elif __riscv_xlen == 32
#define REG_L lw
#define REG_S sw
#define reg_s0 0 + reg_start
#define reg_s1 4 + reg_start
#define reg_s2 8 + reg_start
#define reg_s3 12 + reg_start
#define reg_s4 16 + reg_start
#define reg_s5 20 + reg_start
#define reg_s6 24 + reg_start
#define reg_s7 28 + reg_start
#define reg_s8 32 + reg_start
#define reg_s9 36 + reg_start
#define reg_s10 40 + reg_start
#define reg_s11 44 + reg_start
#define reg_sp 48 + reg_start
#define reg_ra 52 + reg_start
#define reg_pc 56 + reg_start
#define reg_a0 60 + reg_start
#define reg_fcsr 64 + reg_start
#define reg_fs0 72 + reg_start
#else
#error "Unsupported RISC-V register width"
#endif

// Floating-point save slots are eight bytes apart for a stable layout across
// the single- and double-precision ABIs.
#define reg_fs1 reg_fs0 + 8
#define reg_fs2 reg_fs0 + 16
#define reg_fs3 reg_fs0 + 24
#define reg_fs4 reg_fs0 + 32
#define reg_fs5 reg_fs0 + 40
#define reg_fs6 reg_fs0 + 48
#define reg_fs7 reg_fs0 + 56
#define reg_fs8 reg_fs0 + 64
#define reg_fs9 reg_fs0 + 72
#define reg_fs10 reg_fs0 + 80
#define reg_fs11 reg_fs0 + 88

#if defined(__riscv_float_abi_double)
#define FP_L fld
#define FP_S fsd
#define CO_RISCV_HARD_FLOAT 1
#elif defined(__riscv_float_abi_single)
#define FP_L flw
#define FP_S fsw
#define CO_RISCV_HARD_FLOAT 1
#endif

// This is the address that the coroutine returns to when its function is
// done.  The next context is kept in s0, which is callee-saved by the psABI.
1:
  mv a0, s0
  beqz a0, 2f
  tail SYM(CoroutineSetContext)

2:
  tail SYM(abort)

// Entry:
// a0: context
// a1: function to call
// a2: argument to pass to the function
SYM(CoroutineMakeContext):
  REG_S a1, reg_pc(a0)
  REG_S a2, reg_a0(a0)

  REG_L t0, 0(a0)
#if __riscv_xlen == 64
  REG_L t1, 8(a0)
#else
  REG_L t1, 4(a0)
#endif
  add t0, t0, t1
  andi t0, t0, -16
  REG_S t0, reg_sp(a0)

#if __riscv_xlen == 64
  REG_L t0, 16(a0)
#else
  REG_L t0, 8(a0)
#endif
  REG_S t0, reg_s0(a0)
  lla t0, 1b
  REG_S t0, reg_ra(a0)
  ret

#define STORE_S_REGS(base)                                                    \
  REG_S s0, reg_s0(base);                                                     \
  REG_S s1, reg_s1(base);                                                     \
  REG_S s2, reg_s2(base);                                                     \
  REG_S s3, reg_s3(base);                                                     \
  REG_S s4, reg_s4(base);                                                     \
  REG_S s5, reg_s5(base);                                                     \
  REG_S s6, reg_s6(base);                                                     \
  REG_S s7, reg_s7(base);                                                     \
  REG_S s8, reg_s8(base);                                                     \
  REG_S s9, reg_s9(base);                                                     \
  REG_S s10, reg_s10(base);                                                   \
  REG_S s11, reg_s11(base)

#define LOAD_S_REGS(base)                                                     \
  REG_L s0, reg_s0(base);                                                     \
  REG_L s1, reg_s1(base);                                                     \
  REG_L s2, reg_s2(base);                                                     \
  REG_L s3, reg_s3(base);                                                     \
  REG_L s4, reg_s4(base);                                                     \
  REG_L s5, reg_s5(base);                                                     \
  REG_L s6, reg_s6(base);                                                     \
  REG_L s7, reg_s7(base);                                                     \
  REG_L s8, reg_s8(base);                                                     \
  REG_L s9, reg_s9(base);                                                     \
  REG_L s10, reg_s10(base);                                                   \
  REG_L s11, reg_s11(base)

#if defined(CO_RISCV_HARD_FLOAT)
#define STORE_FP_REGS(base)                                                   \
  FP_S fs0, reg_fs0(base);                                                    \
  FP_S fs1, reg_fs1(base);                                                    \
  FP_S fs2, reg_fs2(base);                                                    \
  FP_S fs3, reg_fs3(base);                                                    \
  FP_S fs4, reg_fs4(base);                                                    \
  FP_S fs5, reg_fs5(base);                                                    \
  FP_S fs6, reg_fs6(base);                                                    \
  FP_S fs7, reg_fs7(base);                                                    \
  FP_S fs8, reg_fs8(base);                                                    \
  FP_S fs9, reg_fs9(base);                                                    \
  FP_S fs10, reg_fs10(base);                                                  \
  FP_S fs11, reg_fs11(base)

#define LOAD_FP_REGS(base)                                                    \
  FP_L fs0, reg_fs0(base);                                                    \
  FP_L fs1, reg_fs1(base);                                                    \
  FP_L fs2, reg_fs2(base);                                                    \
  FP_L fs3, reg_fs3(base);                                                    \
  FP_L fs4, reg_fs4(base);                                                    \
  FP_L fs5, reg_fs5(base);                                                    \
  FP_L fs6, reg_fs6(base);                                                    \
  FP_L fs7, reg_fs7(base);                                                    \
  FP_L fs8, reg_fs8(base);                                                    \
  FP_L fs9, reg_fs9(base);                                                    \
  FP_L fs10, reg_fs10(base);                                                  \
  FP_L fs11, reg_fs11(base)
#endif

// Entry:
// a0: address of context
SYM(CoroutineGetContext):
  STORE_S_REGS(a0)
  REG_S sp, reg_sp(a0)
  REG_S ra, reg_ra(a0)
  REG_S ra, reg_pc(a0)
  REG_S a0, reg_a0(a0)

#if defined(CO_RISCV_HARD_FLOAT)
  frcsr t0
  sw t0, reg_fcsr(a0)
  STORE_FP_REGS(a0)
#endif
  ret

// Entry:
// a0: address of context
SYM(CoroutineSetContext):
#if defined(CO_RISCV_HARD_FLOAT)
  lw t0, reg_fcsr(a0)
  fscsr t0
  LOAD_FP_REGS(a0)
#endif

  LOAD_S_REGS(a0)
  REG_L sp, reg_sp(a0)
  REG_L ra, reg_ra(a0)
  REG_L t0, reg_pc(a0)
  REG_L a0, reg_a0(a0)
  jr t0

// Entry:
// a0: address of 'from' context
// a1: address of 'to' context
SYM(CoroutineSwapContext):
  STORE_S_REGS(a0)
  REG_S sp, reg_sp(a0)
  REG_S ra, reg_ra(a0)
  REG_S ra, reg_pc(a0)
  REG_S a0, reg_a0(a0)

#if defined(CO_RISCV_HARD_FLOAT)
  frcsr t0
  sw t0, reg_fcsr(a0)
  STORE_FP_REGS(a0)
#endif

  mv a0, a1
  tail SYM(CoroutineSetContext)

#undef STORE_S_REGS
#undef LOAD_S_REGS
#if defined(CO_RISCV_HARD_FLOAT)
#undef STORE_FP_REGS
#undef LOAD_FP_REGS
#undef CO_RISCV_HARD_FLOAT
#undef FP_L
#undef FP_S
#endif
#undef REG_L
#undef REG_S
