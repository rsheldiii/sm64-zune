/* Minimal C99 <stdint.h> for VC9 on Windows CE (ARM, ILP32). */
#ifndef SM64_COMPAT_STDINT_H
#define SM64_COMPAT_STDINT_H
#include <crtdefs.h>   /* intptr_t, uintptr_t */
#include <limits.h>    /* SIZE_MAX */

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
typedef int64_t intmax_t;
typedef uint64_t uintmax_t;

#define INT8_MIN (-127 - 1)
#define INT8_MAX 127
#define UINT8_MAX 0xff
#define INT16_MIN (-32767 - 1)
#define INT16_MAX 32767
#define UINT16_MAX 0xffff
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 0xffffffffU
#define INT64_MIN (-9223372036854775807i64 - 1)
#define INT64_MAX 9223372036854775807i64
#define UINT64_MAX 0xffffffffffffffffui64
#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX

#define INT8_C(v) (v)
#define INT16_C(v) (v)
#define INT32_C(v) (v)
#define INT64_C(v) (v##i64)
#define UINT8_C(v) (v)
#define UINT16_C(v) (v)
#define UINT32_C(v) (v##U)
#define UINT64_C(v) (v##ui64)
#endif
