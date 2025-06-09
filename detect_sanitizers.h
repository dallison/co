// Copyright 2025 Mikael Persson
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef coroutine_detect_sanitizers_h
#define coroutine_detect_sanitizers_h

// This header produces the defined(CO_ADDRESS_SANITIZER) if address sanitizer is being used
// to compile the code, and produces CO_DISABLE_ADDRESS_SANITIZER as an attribute to disable
// address sanitizer on a function or variable.
// The longjmp / ucontext or any equivalent stack switching mechanism necessary for coroutines
// will trigger false-positives with address-sanitizer.
//
// NOTE: Users will likely have to prefix their coroutine functions with CO_DISABLE_ADDRESS_SANITIZER.

#ifndef CO_ADDRESS_SANITIZER
#if defined(__has_feature)
#if __has_feature(address_sanitizer) // for clang
#define CO_ADDRESS_SANITIZER
#endif
#else                             // defined(__has_feature)
#if defined(__SANITIZE_ADDRESS__) // for gcc
#define CO_ADDRESS_SANITIZER
#endif
#endif // defined(__has_feature
#endif // CO_ADDRESS_SANITIZER

#ifdef CO_ADDRESS_SANITIZER
#ifndef CO_DISABLE_ADDRESS_SANITIZER
#define CO_DISABLE_ADDRESS_SANITIZER __attribute__((no_sanitize("address")))
#endif // CO_DISABLE_ADDRESS_SANITIZER
#else
#define CO_DISABLE_ADDRESS_SANITIZER
#endif // CO_ADDRESS_SANITIZER

#endif // coroutine_detect_sanitizers_h
