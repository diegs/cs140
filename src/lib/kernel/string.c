#include <string.h>
#include "threads/malloc.h"

/* Makes a copy of src on the heap and returns it. */
char *
strdup (const char *src)
{
  size_t len = strlen (src) + 1;
  char *dst = malloc (len);
  strlcpy (dst, src, len);
  return dst;
}
