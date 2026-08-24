// va_list for objects built with GCCE 4.6.3.
//
// The SDK prelude (epoc32\include\gcce\gcce.h) declares its own
// `typedef struct __va_list { void *__ap; } va_list;` and sets _VA_LIST_DEFINED,
// so standard stdarg.h does not interfere. GCC 3.4.3 accepted this type in its
// builtin va_*, but 4.6.3 checks the type strictly and rejects it:
// "incompatible type for argument 1 of '__builtin_va_start'" (psxbios.c).
//
// On ARM EABI __builtin_va_list is the exact same `struct { void *__ap; }`, so
// the below is only a compile-time alias: layout and calling convention
// remain unchanged, and objects from both compilers remain compatible.
//
// Included via a second -include, after gcce.h (tools-patches\core463-regen.cmd).

#ifndef COMPAT_VA_GCCE46_H
#define COMPAT_VA_GCCE46_H

#undef va_start
#undef va_arg
#undef va_end
#undef va_copy

#define va_list            __builtin_va_list
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)

#endif
