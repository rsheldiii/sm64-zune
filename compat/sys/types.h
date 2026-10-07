/* Minimal <sys/types.h> for VC9 on Windows CE: sm64ex's ultratypes.h only needs ssize_t. */
#ifndef SM64_COMPAT_SYS_TYPES_H
#define SM64_COMPAT_SYS_TYPES_H
#include <crtdefs.h>
#ifndef _SSIZE_T_DEFINED
typedef int ssize_t;
#define _SSIZE_T_DEFINED
#endif
#endif
