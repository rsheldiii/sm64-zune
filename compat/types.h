/* CE's windows.h does `#include <types.h>` right after windef.h, and sm64 uses "types.h" for
 * its own header. With sm64's include/ on the /I path, windows.h used to get sm64's file
 * (dropping CE's typedefs and declaring the N64 API outside our extern "C" blocks). The compat
 * directory is searched first, so this file routes each include to the right header. */
#if defined(_WINDEF_) && !defined(_TYPES_H_)
#include "ce_types.h"                 /* CE's types.h, copied by tools/stage.py */
#else
#include "../sm64/include/types.h"
#endif
