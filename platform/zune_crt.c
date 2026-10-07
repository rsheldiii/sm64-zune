/* libultra's BSD memory helpers, which the CE6 C runtime lacks. */
#include <string.h>

void bzero(void *target, size_t size) { memset(target, 0, size); }
void bcopy(const void *source, void *target, size_t size) { memmove(target, source, size); }
