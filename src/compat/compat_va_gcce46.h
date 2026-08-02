// va_list для об'єктів, зібраних GCCE 4.6.3.
//
// Прелюдія SDK (epoc32\include\gcce\gcce.h) оголошує власний
// `typedef struct __va_list { void *__ap; } va_list;` і ставить _VA_LIST_DEFINED,
// щоб стандартний stdarg.h не втручався. GCC 3.4.3 приймав цей тип у своїх
// вбудованих va_*, а 4.6.3 звіряє тип строго й відмовляє:
// "incompatible type for argument 1 of '__builtin_va_start'" (psxbios.c).
//
// На ARM EABI __builtin_va_list - це той самий `struct { void *__ap; }`, тож
// нижче лише перейменування на рівні компіляції: розкладка й угода про виклик
// не змінюються, і об'єкти від обох компіляторів лишаються сумісними.
//
// Підключається другим -include, після gcce.h (tools-patches\core463-regen.cmd).

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
