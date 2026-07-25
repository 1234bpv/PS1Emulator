// stdalign.h для CSL Arm Toolchain (GCC 3.4.3) під Symbian.
// Заголовок з C11; GCC 3.4.3 його не має. Вживається лише в
// libpcsxcore/cdrom.c та cdrom-async.c у формі alignas(N) на масивах.

#ifndef PS1EMU_COMPAT_STDALIGN_H
#define PS1EMU_COMPAT_STDALIGN_H

#define alignas(x) __attribute__((aligned(x)))
#define alignof(x) __alignof__(x)

#define __alignas_is_defined 1
#define __alignof_is_defined 1

#endif
