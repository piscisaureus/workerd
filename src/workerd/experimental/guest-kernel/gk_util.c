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

// The window a report falls in: a coarse monotonic second, read with a raw
// syscall so the path allocates nothing and takes no lock the guest thread
// could be holding (see the note above write_stderr). A clock that will not
// answer leaves every report in window 0, where the burst alone bounds them.
static gk_thread *thread_cur_lockfree(void);  // defined with the thread records

static uint32_t log_window_now(void) {
  struct timespec ts;
  if (host_syscall(SYS_clock_gettime, CLOCK_MONOTONIC_COARSE, (long)(uintptr_t)&ts, 0, 0, 0, 0) <
      0)
    return 0;
  return (uint32_t)ts.tv_sec;
}

long gk_log_budget(unsigned site) {
  // The caller's ring, as gk_resolve reads it: at guest ring 3 the block is
  // unreachable, so there is no decision to make here and the caller is told
  // to ask the host for one.
  if (at_guest_ring3()) return -GK_EPERM;
  // An id gk does not know is the caller's mistake, and the safe answer to a
  // mistake is the report, not silence.
  if (site >= GK_LOG_SITES) return 1;
  // The arena the calling thread has active, from gk's own record of it and
  // never from the caller: a turn counts against its own isolate's budget and
  // against no other's. A thread with no arena -- the event loop, a teardown
  // -- counts in the block's own buckets, which no ring 3 reaches.
  gk_thread *t = thread_cur_lockfree();
  gk_arena *a = t ? __atomic_load_n(&t->active_arena, __ATOMIC_ACQUIRE) : NULL;
  gk_log_bucket *b = a ? &a->log_budget[site] : &G->log_budget[site];
  uint32_t now = log_window_now();
  uint32_t was = atomic_load_explicit(&b->window, memory_order_relaxed);
  if (now != was && atomic_compare_exchange_strong_explicit(&b->window, &was, now,
                        memory_order_relaxed, memory_order_relaxed)) {
    // A window of its own for this report. A bucket starts at window 0 with
    // nothing allowed, so this is also how the first report at a site passes:
    // it needs nothing to have happened before it.
    atomic_store_explicit(&b->allowed, 1, memory_order_relaxed);
    return 1 + (long)atomic_exchange_explicit(&b->suppressed, 0, memory_order_relaxed);
  }
  if (atomic_load_explicit(&b->allowed, memory_order_relaxed) < GK_LOG_BURST) {
    // Racing callers can let a few more than GK_LOG_BURST through in a window,
    // which costs a line; taking the lock to avoid that would cost the fault
    // and handler paths a lock they are not allowed to take.
    atomic_fetch_add_explicit(&b->allowed, 1, memory_order_relaxed);
    return 1 + (long)atomic_exchange_explicit(&b->suppressed, 0, memory_order_relaxed);
  }
  atomic_fetch_add_explicit(&b->suppressed, 1, memory_order_relaxed);
  return 0;
}

// A refusal report: composed in a stack buffer and written in one call, and
// only while the site has budget left. Both halves matter on these paths.
// They are reachable from a hostcall handler, where stdio's lock may be held
// by the very guest thread that made the call; and the refusals are the
// turn's to provoke, so an unbudgeted report is a log the turn writes.
// log_report opens the line and answers 0 for a report to hold back;
// log_report_end closes it, saying how many like it went unsaid.
static int log_report(gk_dbuf *d, unsigned site, const char *who) {
  long n = gk_log_budget(site);
  if (n <= 0) return 0;
  d->n = 0;
  db_str(d, "[gk] ");
  db_str(d, who);
  db_str(d, ": ");
  return (int)n;
}
static void log_report_end(gk_dbuf *d, int n) {
  if (n > 1) {
    db_str(d, " (");
    db_dec(d, n - 1);
    db_str(d, " more like it went unreported)");
  }
  db_flush(d);
}

#ifdef GK_GUARD_HANDLER_ALLOC
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
static void gk_guard_alloc(const char *w) {
  if (!g_in_handler) return;
  const char a[] = "[gk] FATAL: ";
  const char b[] = "() inside a handler (the fault path, the signal-time field write or a "
                   "hostcall handler; all must be malloc-free)\n";
  write(2, a, sizeof a - 1);
  write(2, w, strlen(w));
  write(2, b, sizeof b - 1);
  abort();
}
void *__wrap_malloc(size_t n) { gk_guard_alloc("malloc"); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t s) { gk_guard_alloc("calloc"); return __real_calloc(n, s); }
void *__wrap_realloc(void *p, size_t n) { gk_guard_alloc("realloc"); return __real_realloc(p, n); }
#endif
