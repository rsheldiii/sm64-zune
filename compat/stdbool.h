/* C99 <stdbool.h> for VC9's C compiler. Unlike _Bool, assigning 2 stores 2: compare with
 * zero rather than with `true`. C++ translation units use the built-in bool. */
#ifndef SM64_COMPAT_STDBOOL_H
#define SM64_COMPAT_STDBOOL_H
#ifndef __cplusplus
typedef unsigned char bool;
#define true 1
#define false 0
#endif
#define __bool_true_false_are_defined 1
#endif
