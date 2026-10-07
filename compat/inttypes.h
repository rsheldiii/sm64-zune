/* Minimal C99 <inttypes.h> for VC9 (MSVC printf length modifiers). */
#ifndef SM64_COMPAT_INTTYPES_H
#define SM64_COMPAT_INTTYPES_H
#include "stdint.h"
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRId64 "I64d"
#define PRIu64 "I64u"
#define PRIx64 "I64x"
#endif
