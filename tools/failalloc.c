#include <stdlib.h>

static long countdown = -1, live;
static int failed;

void fa_fail_from(long n) { countdown = n; failed = 0; }
long fa_live(void) { return live; }
int fa_failed(void) { return failed; }

static long total;
long fa_total(void) { return total; }

static int fail(void)
{
    total++;
    if (countdown < 0) return 0;
    if (countdown == 0) { failed = 1; return 1; }
    countdown--;
    return 0;
}

void *fa_malloc(size_t n)
{
    void *p = fail() ? NULL : malloc(n);
    live += p != NULL;
    return p;
}

void *fa_calloc(size_t n, size_t size)
{
    void *p = fail() ? NULL : calloc(n, size);
    live += p != NULL;
    return p;
}

void *fa_realloc(void *p, size_t n)
{
    if (fail()) return NULL;
    void *q = realloc(p, n);
    live += q && !p;
    return q;
}

void fa_free(void *p)
{
    live -= p != NULL;
    free(p);
}
