// stdalign.h for CSL Arm Toolchain (GCC 3.4.3) on Symbian.
// C11 header; GCC 3.4.3 lacks it. Used only in
// libpcsxcore/cdrom.c and cdrom-async.c in the form alignas(N) on arrays.

#ifndef PS1EMU_COMPAT_STDALIGN_H
#define PS1EMU_COMPAT_STDALIGN_H

#define alignas(x) __attribute__((aligned(x)))
#define alignof(x) __alignof__(x)

#define __alignas_is_defined 1
#define __alignof_is_defined 1

#endif
