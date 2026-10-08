// Allocation fault injection for tests: force-included (-include) into the sources under
// test, it routes their malloc/calloc/realloc/free through tools/failalloc.c.
#ifndef SHELF_FAILALLOC_H
#define SHELF_FAILALLOC_H
#include <stdlib.h>
#include <string.h>

void *fa_malloc(size_t n);
void *fa_calloc(size_t n, size_t size);
void *fa_realloc(void *p, size_t n);
void fa_free(void *p);
// The `n`th allocation from now (0 = the next) and every one after it fail; -1 disarms.
void fa_fail_from(long n);
// Allocations still live through these wrappers, and whether any failure was injected.
long fa_live(void);
int fa_failed(void);
// Allocations attempted through the wrappers so far.
long fa_total(void);

#define malloc fa_malloc
#define calloc fa_calloc
#define realloc fa_realloc
#define free fa_free
#endif
