/* Forced into every sm64ex translation unit compiled by VC9 (cl /FI) for the Zune HD.
 * VC9's C compiler is C89 plus Microsoft extensions; this supplies the few C99/GNU
 * spellings the game code uses. Keep it small: real incompatibilities are patched in source. */
#ifndef SM64_VC9_COMPAT_H
#define SM64_VC9_COMPAT_H

#if !defined(__cplusplus)
#define inline __inline
#endif

#define __attribute__(x)
#define __func__ __FUNCTION__

/* platform_info.h tests IS_BIG_ENDIAN as (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__). VC9 defines
 * neither, and undefined names are 0 in #if, so without these the whole game (level/geo
 * command encoding, gbi.h, audio structs, save checksums) builds big-endian. */
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__

#define SM64_PASTE2(a, b) a##b
#define SM64_PASTE(a, b) SM64_PASTE2(a, b)
/* C11 static assertion as a negative-size array; identical typedefs are benign in MSVC. */
#define _Static_assert(cond, message) typedef char SM64_PASTE(sm64_static_assert_, __LINE__)[(cond) ? 1 : -1]

/* The CE6 math.h has no M_PI family, not even behind _USE_MATH_DEFINES. */
#define M_PI 3.14159265358979323846
#define M_SQRT2 1.41421356237309504880

/* CE6's libm is coredll's software floating point; zune_math.h redirects the float functions
 * the game uses to VFP versions. <math.h> goes first so its declarations keep their names.
 * atan2f is the game's own (src/engine/math_util.c). */
#include <math.h>
#include "zune_math.h"
#ifdef __cplusplus
extern "C" {
#endif
float atan2f(float, float);
#ifdef __cplusplus
}
#endif

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#endif
