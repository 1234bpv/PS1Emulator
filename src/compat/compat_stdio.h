// Оголошення функцій, яких немає в ESTLIB. Реалізація - compat_snprintf.c.
// Включати ПІСЛЯ <stdio.h>.

#ifndef PS1EMU_COMPAT_STDIO_H
#define PS1EMU_COMPAT_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif
