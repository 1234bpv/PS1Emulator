// stdint.h для CSL Arm Toolchain (GCC 3.4.3) під Symbian.
// Ані ESTLIB (epoc32\include\libc), ані GCC 3.4.3 його не постачають:
// stdint.h з'явився в C99, а серед freestanding-заголовків GCC — лише з 4.x.
// Розкладка типів для 32-бітного ARM (ILP32).

#ifndef PS1EMU_COMPAT_STDINT_H
#define PS1EMU_COMPAT_STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;

typedef int                intptr_t;
typedef unsigned int       uintptr_t;

typedef long long          intmax_t;
typedef unsigned long long uintmax_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255U
#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535U
#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295U
#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define SIZE_MAX    UINT32_MAX

#define INT8_C(x)   x
#define UINT8_C(x)  x ## U
#define INT16_C(x)  x
#define UINT16_C(x) x ## U
#define INT32_C(x)  x
#define UINT32_C(x) x ## U
#define INT64_C(x)  x ## LL
#define UINT64_C(x) x ## ULL

#endif
