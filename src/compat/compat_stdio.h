// Declarations of functions absent in ESTLIB. Implementation is in compat_snprintf.c.
// Include AFTER <stdio.h>.

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
