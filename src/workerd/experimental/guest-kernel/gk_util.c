// gk: raw stderr output, fatal errors and the fault-path allocation guard.
#include "gk_internal.h"

// ---- helpers ---------------------------------------------------------------
// Raw stderr output for the fault path and for fatal errors: a plain write(2),
// no stdio, no allocation, no lock the guest thread could hold (see the
// signal-safety note above region_add in gk_region.c). Debug lines are
// composed in a stack buffer (gk_dbuf) and written in one call.
static void write_stderr(const char *s) {
  ssize_t r = write(2, s, strlen(s));
  (void)r;
}

// Fatal integrity failure: something the host side must be able to trust has
// been tampered with (or gk has a bug). There is no safe way to continue.
static void gk_fatal(const char *what) __attribute__((noreturn));
static void gk_fatal(const char *what) {
  write_stderr("[gk] FATAL: ");
  write_stderr(what);
  write_stderr("\n");
  abort();
}

static void db_str(gk_dbuf *d, const char *s) {
  while (*s && d->n < sizeof d->b - 1) d->b[d->n++] = *s++;
}
static void db_hex(gk_dbuf *d, uint64_t v) {  // 0x-prefixed, like printf's %#lx
  char tmp[2 + 16], *p = tmp + sizeof tmp;
  do { *--p = "0123456789abcdef"[v & 0xf]; v >>= 4; } while (v);
  *--p = 'x'; *--p = '0';
  while (p < tmp + sizeof tmp && d->n < sizeof d->b - 1) d->b[d->n++] = *p++;
}
static void db_dec(gk_dbuf *d, long v) {
  char tmp[1 + 20], *p = tmp + sizeof tmp;
  unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
  do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0) *--p = '-';
  while (p < tmp + sizeof tmp && d->n < sizeof d->b - 1) d->b[d->n++] = *p++;
}
static void db_flush(gk_dbuf *d) {
  if (d->n < sizeof d->b) d->b[d->n++] = '\n';
  ssize_t r = write(2, d->b, d->n);
  (void)r;
  d->n = 0;
}

#ifdef GK_GUARD_HANDLER_ALLOC
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
static void gk_guard_alloc(const char *w) {
  if (!g_in_handler) return;
  const char a[] = "[gk] FATAL: ";
  const char b[] = "() on the fault-handler path (must be malloc-free)\n";
  write(2, a, sizeof a - 1);
  write(2, w, strlen(w));
  write(2, b, sizeof b - 1);
  abort();
}
void *__wrap_malloc(size_t n) { gk_guard_alloc("malloc"); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t s) { gk_guard_alloc("calloc"); return __real_calloc(n, s); }
void *__wrap_realloc(void *p, size_t n) { gk_guard_alloc("realloc"); return __real_realloc(p, n); }
#endif
