#ifndef VM_VM_H
#define VM_VM_H

#include <stdbool.h>
#include "threads/palloc.h"

enum vm_flags
{
  VM_ZERO = PAL_ZERO             /* Zero page contents. */
};

bool vm_add_page (uint8_t *uaddr, bool writable, enum vm_flags flags);
bool vm_free_page (uint8_t *uaddr);

#endif /* vm/vm.h */
