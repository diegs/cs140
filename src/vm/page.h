#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdbool.h>
#include "threads/palloc.h"

enum vm_flags
{
  VM_ZERO = PAL_ZERO             /* Zero page contents. */
};

uint8_t * vm_add_page (uint8_t *uaddr, bool writable, enum vm_flags flags);
bool vm_free_page (uint8_t *uaddr);

#endif /* vm/page.h */
