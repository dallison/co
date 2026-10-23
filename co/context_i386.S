// Copyright 2024-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
//
// SysV i386 context implementation. Included by context.S.

.code32

// Register layout in save area.
#define reg_ebx 0 + reg_start
#define reg_esi 4 + reg_start
#define reg_edi 8 + reg_start
#define reg_ebp 12 + reg_start
#define reg_esp 16 + reg_start
#define reg_eip 20 + reg_start
#define reg_fp 24 + reg_start
#define reg_mxcsr 52 + reg_start

// We get here when the coroutine function returns.  The next context is kept
// in ebx, which is callee-saved by the SysV i386 ABI.
1:
  mov %ebx, %eax
  test %eax, %eax
  je 2f
  jmp 3f

2:
#if defined(__APPLE__)
  call SYM(abort)
#else
  call SYM(abort)@PLT
#endif

// Entry:
// 4(%esp): context
// 8(%esp): function to call
// 12(%esp): argument to pass to the function
SYM(CoroutineMakeContext):
  mov 4(%esp), %eax
  mov 8(%esp), %edx
  mov %edx, reg_eip(%eax)

  mov 0(%eax), %edx
  add 4(%eax), %edx       // End of stack
  and $-16, %edx
  sub $20, %edx           // Function entry has esp % 16 == 12
  mov %edx, reg_esp(%eax)

  // Keep the next context in a callee-saved register.
  mov 8(%eax), %ecx
  mov %ecx, reg_ebx(%eax)

  // Form a cdecl frame: return address followed by the first argument.
  call 4f
4:
  pop %ecx
  add $(1b - 4b), %ecx
  mov %ecx, 0(%edx)
  mov 12(%esp), %ecx
  mov %ecx, 4(%edx)
  ret

// Entry:
// 4(%esp): address of context
SYM(CoroutineGetContext):
  mov 4(%esp), %eax
  mov %ebx, reg_ebx(%eax)
  mov %esi, reg_esi(%eax)
  mov %edi, reg_edi(%eax)
  mov %ebp, reg_ebp(%eax)

  mov 0(%esp), %ecx
  mov %ecx, reg_eip(%eax)
  lea 4(%esp), %ecx
  mov %ecx, reg_esp(%eax)

  lea reg_fp(%eax), %edx
  fnstenv (%edx)
  fldenv (%edx)
#if defined(__SSE__)
  stmxcsr reg_mxcsr(%eax)
#endif
  ret

// Entry:
// 4(%esp): address of context
SYM(CoroutineSetContext):
  mov 4(%esp), %eax
3:
  lea reg_fp(%eax), %edx
  fldenv (%edx)
#if defined(__SSE__)
  ldmxcsr reg_mxcsr(%eax)
#endif

  mov reg_eip(%eax), %edx
  mov reg_esp(%eax), %esp
  push %edx

  mov reg_ebx(%eax), %ebx
  mov reg_esi(%eax), %esi
  mov reg_edi(%eax), %edi
  mov reg_ebp(%eax), %ebp
  ret

// Entry:
// 4(%esp): address of 'from' context
// 8(%esp): address of 'to' context
SYM(CoroutineSwapContext):
  mov 4(%esp), %eax
  mov %ebx, reg_ebx(%eax)
  mov %esi, reg_esi(%eax)
  mov %edi, reg_edi(%eax)
  mov %ebp, reg_ebp(%eax)

  mov 0(%esp), %ecx
  mov %ecx, reg_eip(%eax)
  lea 4(%esp), %ecx
  mov %ecx, reg_esp(%eax)

  lea reg_fp(%eax), %edx
  fnstenv (%edx)
  fldenv (%edx)
#if defined(__SSE__)
  stmxcsr reg_mxcsr(%eax)
#endif

  mov 8(%esp), %eax
  jmp 3b
