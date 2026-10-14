// Tests for the gk library.
#define _GNU_SOURCE
#include "gk.h"
extern void gk_user_exit_tramp(void);
#if defined(__x86_64__)
extern void gk_syscall_tramp(void);  // the guest's syscall entry (gk_asm.S)
#define GK_SYSCALL_ENTRY gk_syscall_tramp
#else
extern void gk_vectors(void);        // the guest's exception vectors (gk_asm.S)
#define GK_SYSCALL_ENTRY gk_vectors
#endif

#include <stdint.h>

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

// What the tests can exercise on this host. macOS (Hypervisor.framework) has no
// guest-created threads, and a stage-2 page there does not follow a host-side
// remap (see the HVF section in gk.c), so the host-side decommit test does not
// apply; it also has no pthread barriers, no CPU affinity and no /proc.
#if defined(__APPLE__)
#define GK_TEST_GUEST_THREADS 0
#define GK_TEST_HOST_DECOMMIT 0
#include <mach/mach.h>
typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned n, waiting, round; } pthread_barrier_t;
static int pthread_barrier_init(pthread_barrier_t *b, const void *attr, unsigned n) {
  (void)attr;
  pthread_mutex_init(&b->m, NULL);
  pthread_cond_init(&b->c, NULL);
  b->n = n; b->waiting = 0; b->round = 0;
  return 0;
}
static int pthread_barrier_wait(pthread_barrier_t *b) {
  pthread_mutex_lock(&b->m);
  unsigned round = b->round;
  if (++b->waiting == b->n) {
    b->waiting = 0; b->round++;
    pthread_cond_broadcast(&b->c);
  } else {
    while (round == b->round) pthread_cond_wait(&b->c, &b->m);
  }
  pthread_mutex_unlock(&b->m);
  return 0;
}
static int pthread_barrier_destroy(pthread_barrier_t *b) {
  pthread_cond_destroy(&b->c);
  pthread_mutex_destroy(&b->m);
  return 0;
}
#else
#define GK_TEST_GUEST_THREADS 1
#define GK_TEST_HOST_DECOMMIT 1
#endif

// The host's page size: the unit of the mprotects and mmaps the tests issue.
// The guest's pages are 4 KiB whatever the host's are, so the per-page counts
// below (demand faults, neighbors) stay in 4 KiB pages.
static uintptr_t hpage;

// The few instructions the tests need that differ between the architectures:
// a raw syscall, a read only ring 0 can do (priv_probe), and the selector a
// ring-3 probe reports (USER_CS).
#if defined(__x86_64__)
// CR0, readable only at ring 0; PG (bit 31) is set in the guest.
#define PRIV_PROBE_OK(v) ((v) & (1UL << 31))
#define USER_CS 0x2b  // the ring-3 code selector
#elif defined(__APPLE__)
// macOS: the number in x16, failure in the carry flag with the errno in x0,
// returned Linux-style as -errno.
static long raw_syscall3(long nr, long a1, long a2, long a3) {
  register long x16 __asm__("x16") = nr;
  register long x0 __asm__("x0") = a1;
  register long x1 __asm__("x1") = a2;
  register long x2 __asm__("x2") = a3;
  long cf;
  __asm__ __volatile__("svc #0x80\n\tcset %1, cs"
                       : "+r"(x0), "=r"(cf), "+r"(x1), "+r"(x2), "+r"(x16)
                       :
                       : "memory", "cc");
  return cf ? -x0 : x0;
}
// gk's own ring probe is an svc #0 with the number in x8 on either OS (see gk.h).
static long gk_probe_syscall(long nr) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = 0;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
#else
static long raw_syscall3(long nr, long a1, long a2, long a3) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a1;
  register long x1 __asm__("x1") = a2;
  register long x2 __asm__("x2") = a3;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}
static long gk_probe_syscall(long nr) { return raw_syscall3(nr, 0, 0, 0); }
#endif
#if defined(__aarch64__)
// CurrentEL, readable only at EL1 and above: EL1 (1 << 2) at guest ring 0.
static unsigned long __attribute__((unused)) priv_probe(void) {
  unsigned long el;
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(el));
  return el;
}
#define PRIV_PROBE_OK(v) ((v) == (1UL << 2))
// arm64 has no unprivileged read of the exception level: gk answers a
// reserved syscall with the caller's ring (see gk.h), and the tests' "CS" is
// that ring.
static unsigned long user_ring(void) { return (unsigned long)gk_probe_syscall(GK_DEBUG_RING_SYSCALL); }
#define USER_CS 3
#endif

// Pure computation, no syscalls: exercises entering and leaving the guest.
static long test_compute(void *arg) {
  long n = (long)arg, sum = 0;
  for (long i = 0; i < n; i++) sum += i * i;
  return sum;
}

// Real libc inside the guest: malloc (mmap/brk, dynamically mapped), snprintf
// (TLS/locale), and write (forwarded). Returns a checksum of the message.
static long test_libc(void *arg) {
  (void)arg;
  char *buf = malloc(160);
  if (!buf) return -1;
  int len = snprintf(buf, 160,
                     "hello from libc running inside the guest (heap ptr %p)\n",
                     (void *)buf);
  write(1, buf, (size_t)len);
  long sum = 0;
  for (int i = 0; i < len; i++) sum += (unsigned char)buf[i];
  free(buf);
  return sum;
}

// Raw syscall from inside the guest, to confirm forwarding works in gk.
static long test_raw_write(void *arg) {
  (void)arg;
  const char msg[] = "raw syscall write from inside gk guest\n";
#if defined(__aarch64__)
  return raw_syscall3(SYS_write, 1, (long)msg, (long)sizeof(msg) - 1);
#else
  long ret;
  register long r10 __asm__("r10") = 0, r8 __asm__("r8") = 0, r9 __asm__("r9") = 0;
  __asm__ __volatile__("syscall"
                       : "=a"(ret)
                       : "a"(1L), "D"(1L), "S"(msg), "d"((long)sizeof(msg) - 1),
                         "r"(r10), "r"(r8), "r"(r9)
                       : "rcx", "r11", "memory");
  return ret;
#endif
}

// A text address, for checks that ask which memory is not host memory.
static void gk_test_dummy_text(void) {}

// Reads the first byte at the address given as arg, and returns it. Used to
// probe arena isolation: reachable memory returns its byte; unreachable memory
// faults and gk_run returns GK_EFAULT.
static long read_byte(void *arg) {
  volatile unsigned char *p = arg;
  return (long)*p;
}

// ---- arena tests -------------------------------------------------------------
// An arena is a PROT_NONE reservation: its owner commits pages with mprotect,
// which the guest issues as a forwarded syscall, and the guest's first access
// then demand-pages the committed page into the active arena's root.
struct ap { void *page; unsigned char val; };

// Runs in the guest: commit one page (mprotect RW), write a byte, read it back.
static long commit_write(void *arg) {
  struct ap *p = arg;
  if (mprotect(p->page, 4096, PROT_READ | PROT_WRITE) != 0) return -errno;
  volatile unsigned char *b = p->page;
  b[0] = p->val;
  return b[0];
}

// Runs in the guest: decommit a page (mprotect PROT_NONE), then read it. The
// read must fault: the decommit has to be reflected into the arena root, or a
// stale PTE would let it through (as a KVM_RUN EFAULT, or worse, silently).
static long decommit_read(void *arg) {
  struct ap *p = arg;
  if (mprotect(p->page, 4096, PROT_NONE) != 0) return -errno;
  volatile unsigned char *b = p->page;
  return b[0];
}

// Runs in the guest: make a committed page read-only (mprotect R), then read
// it. The read goes through the PTE the guest already has, rewritten in place.
static long protect_read(void *arg) {
  struct ap *p = arg;
  if (mprotect(p->page, 4096, PROT_READ) != 0) return -errno;
  volatile unsigned char *b = p->page;
  return b[0];
}

// Runs in the guest: write a byte to the page, no mprotect. On a read-only
// page this faults and gk_run returns GK_EFAULT.
static long write_byte(void *arg) {
  struct ap *p = arg;
  volatile unsigned char *b = p->page;
  b[0] = p->val;
  return b[0];
}


// ---- arena churn -------------------------------------------------------------
// A runtime creates and destroys arenas constantly (one per isolate). Each use
// below commits one page in each of several backing windows of the arena and
// writes and reads it, so the arena acquires memslots and private PD/PT pages
// of its own, then decommits one page so a reflected unmap is in the mix too.
struct churn_arena { unsigned char *base; size_t stride; int npages; unsigned seed; };

static unsigned char *churn_page(const struct churn_arena *c, int i) {
  return c->base + (size_t)i * c->stride + hpage * (size_t)i;
}

static long churn_use(void *arg) {
  struct churn_arena *c = arg;
  long sum = 0;
  for (int i = 0; i < c->npages; i++) {
    unsigned char *p = churn_page(c, i);
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0) return -errno;
    volatile unsigned char *b = p;
    b[0] = (unsigned char)(c->seed + i);
    b[4095] = (unsigned char)(c->seed ^ i);
    sum += b[0] * 3 + b[4095];
  }
  if (mprotect(churn_page(c, c->npages - 1), 4096, PROT_NONE) != 0) return -errno;
  return sum;
}

static long churn_want(const struct churn_arena *c) {
  long sum = 0;
  for (int i = 0; i < c->npages; i++)
    sum += (unsigned char)(c->seed + i) * 3 + (unsigned char)(c->seed ^ i);
  return sum;
}

// A thread that enters an arena and holds it active until told to leave, to
// exercise gk_arena_destroy on an arena some other thread still uses.
struct holder { gk_arena *a; pthread_barrier_t *b; long r; };
static void *hold_arena(void *arg) {
  struct holder *h = arg;
  gk_arena_enter(h->a);
  h->r = gk_run(read_byte, gk_arena_base(h->a));
  pthread_barrier_wait(h->b);  // entered; main destroys the arena now
  pthread_barrier_wait(h->b);  // destroyed; leave
  gk_arena_enter(NULL);
  return NULL;
}

// ---- multi-vCPU tests ------------------------------------------------------
// Each host thread becomes its own vCPU. This worker runs compute plus a
// forwarded write inside the guest, concurrently with the other threads.
struct work { int id; long n; long result; int ok; };

static long guest_work(void *arg) {
  struct work *w = arg;
  long sum = 0;
  for (long i = 0; i < w->n; i++) sum += (i ^ w->id) & 0xffff;
  return sum;
}

static void *thread_main(void *arg) {
  struct work *w = arg;
  long want = 0;
  for (long i = 0; i < w->n; i++) want += (i ^ w->id) & 0xffff;
  long got = gk_run(guest_work, w);
  w->result = got;
  w->ok = (got == want);
  return NULL;
}

// Per-thread arena: two threads, each entering its own arena, running
// concurrently. Each reads its own cage (ok) and the peer's cage (must fault).
struct aw { gk_arena *own; gk_arena *peer; int own_ok; int peer_faulted; long r_own; long r_peer; unsigned long fa; pthread_barrier_t *b; };

static long read_first(void *arg) {
  volatile unsigned char *p = arg;
  return (long)*p;
}

static void *arena_thread(void *arg) {
  struct aw *w = arg;
  gk_arena_enter(w->own);
  pthread_barrier_wait(w->b);  // ensure both are in their arenas at once
  long r_own = gk_run(read_first, gk_arena_base(w->own));
  long r_peer = gk_run(read_first, gk_arena_base(w->peer));
  w->r_own = r_own; w->r_peer = r_peer; w->fa = gk_fault_addr();
  w->own_ok = (r_own >= 0 && r_own != GK_EFAULT);
  w->peer_faulted = (r_peer == GK_EFAULT);
  gk_arena_enter(NULL);
  return NULL;
}

// Guest function: raw write(1,...), returns the raw kernel result (negative on
// error). Used to test the syscall policy.
static long guest_try_write(void *arg) {
  (void)arg;
  const char msg[] = "this write should be denied by the filter\n";
#if defined(__aarch64__)
  return raw_syscall3(SYS_write, 1, (long)msg, (long)sizeof(msg) - 1);
#else
  long ret;
  register long r10 __asm__("r10") = 0, r8 __asm__("r8") = 0, r9 __asm__("r9") = 0;
  __asm__ __volatile__("syscall"
                       : "=a"(ret)
                       : "a"(1L), "D"(1L), "S"(msg), "d"((long)sizeof(msg) - 1),
                         "r"(r10), "r"(r8), "r"(r9)
                       : "rcx", "r11", "memory");
  return ret;
#endif
}

#if GK_TEST_GUEST_THREADS
// ---- guest-created threads --------------------------------------------------
// Threads created *from inside the guest* (pthread_create -> clone3/clone) must
// enter the guest themselves. The thread body reads CR0 (CurrentEL on arm64),
// which only works in ring 0: on the host it would SIGSEGV (SIGILL), in the
// guest it returns CR0 with PG set (EL1).
struct gt { int id; long n; unsigned long cr0; long sum; };

static void *guest_thread_body(void *arg) {
  struct gt *t = arg;
  unsigned long cr0;
#if defined(__x86_64__)
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
#else
  cr0 = priv_probe();
#endif
  t->cr0 = cr0;
  long sum = 0;
  for (long i = 0; i < t->n; i++) sum += (i * 3 + t->id) & 0xff;
  t->sum = sum;
  char *buf = malloc(64);  // libc from a guest-created thread
  int len = snprintf(buf, 64, "guest thread %d ran in ring 0\n", t->id);
  write(1, buf, (size_t)len);
  free(buf);
  return (void *)(uintptr_t)(t->id * 10);
}

// Runs in the guest: creates threads, joins them, checks their return values.
static long spawn_in_guest(void *arg) {
  struct gt *ts = arg;
  enum { N = 3 };
  pthread_t th[N];
  for (int i = 0; i < N; i++)
    if (pthread_create(&th[i], NULL, guest_thread_body, &ts[i]) != 0) return -1;
  long ok = 1;
  for (int i = 0; i < N; i++) {
    void *rv = NULL;
    if (pthread_join(th[i], &rv) != 0) return -2;
    if ((long)(uintptr_t)rv != ts[i].id * 10) ok = 0;
  }
  return ok;
}

// Thread churn: KVM never destroys a vCPU, so gk must recycle the vCPUs of
// ended threads or a long-lived process would exhaust KVM's vCPU limit. Runs
// in the guest: spawns and joins a few threads per iteration, many iterations.
struct churn { int iters; int per_iter; long bad; };

static void *churn_body(void *arg) {
  unsigned long cr0;
#if defined(__x86_64__)
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
#else
  cr0 = priv_probe();
#endif
  return (void *)(uintptr_t)(PRIV_PROBE_OK(cr0) ? (uintptr_t)arg : 0);
}

static long churn_in_guest(void *arg) {
  struct churn *c = arg;
  pthread_t th[8];
  for (int it = 0; it < c->iters; it++) {
    for (int i = 0; i < c->per_iter; i++)
      if (pthread_create(&th[i], NULL, churn_body, (void *)(uintptr_t)(it * 8 + i + 1)) != 0)
        return -1;
    for (int i = 0; i < c->per_iter; i++) {
      void *rv = NULL;
      if (pthread_join(th[i], &rv) != 0) return -2;
      if ((uintptr_t)rv != (uintptr_t)(it * 8 + i + 1)) c->bad++;
    }
  }
  return 0;
}

#endif  // GK_TEST_GUEST_THREADS

// Host-side churn: a host thread enters the guest via gk_run and then ends.
static void *host_churn_thread(void *arg) {
  return (void *)(uintptr_t)gk_run(test_compute, arg);
}

// Filter: deny write, allow everything else.
static int deny_write(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
  return nr != SYS_write;  // 0 = deny
}

#if defined(__x86_64__)
// ---- memory protection keys -------------------------------------------------
// The guest's pkeys must be enforced by the guest CPU: a page assigned a key
// with pkey_mprotect is only accessible as the calling thread's PKRU allows,
// and the PKRU is the guest's own (wrpkru in the guest, via glibc's pkey_set).
// Each step is a separate gk_run because a denied access ends the run with
// GK_EFAULT; the vCPU (and its PKRU) persists across the runs of one thread.
struct pk { int key; volatile unsigned char *page; unsigned child_pkru; };

static unsigned read_pkru(void) {
  unsigned v;
  __asm__ __volatile__("rdpkru" : "=a"(v) : "c"(0) : "edx");
  return v;
}

// Allocate a key whose initial rights disable writes, map a page, write it
// while it still has the default key, then assign the new key. Returns the
// key's rights as the guest sees them: pkey_alloc's initial rights must have
// reached this thread's guest PKRU, as they reach the calling thread natively.
static long pk_setup(void *arg) {
  struct pk *p = arg;
  p->key = pkey_alloc(0, PKEY_DISABLE_WRITE);
  if (p->key <= 0) return -1;
  p->page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p->page == MAP_FAILED) return -2;
  p->page[0] = 0x5a;
  if (pkey_mprotect((void *)p->page, 4096, PROT_READ | PROT_WRITE, p->key) != 0) return -3;
  return pkey_get(p->key);
}
static long pk_read(void *arg) { return ((struct pk *)arg)->page[0]; }
static long pk_write(void *arg) { ((struct pk *)arg)->page[0] = 0x77; return 0; }
static long pk_disable_access_read(void *arg) {
  struct pk *p = arg;
  pkey_set(p->key, PKEY_DISABLE_ACCESS);
  return p->page[0];
}
static long pk_enable_write_read(void *arg) {
  struct pk *p = arg;
  pkey_set(p->key, 0);
  p->page[0] = 0x77;
  return p->page[0];
}
// A thread created in the guest inherits its creator's PKRU.
static void *pk_child(void *arg) {
  ((struct pk *)arg)->child_pkru = read_pkru();
  return NULL;
}
static long pk_inherit(void *arg) {
  struct pk *p = arg;
  pkey_set(p->key, PKEY_DISABLE_WRITE);
  pthread_t t;
  if (pthread_create(&t, NULL, pk_child, p) != 0) return -1;
  pthread_join(t, NULL);
  return (p->child_pkru >> (2 * p->key)) & 3;
}
// A key nobody allocated is not refused with EINVAL: gk cannot tell it from
// one the host allocated behind its back (see host_key_check), and a caller
// like V8 aborts on any errno but ENOMEM. The freed key's page, reassigned the
// default key, is plainly accessible again.
static long pk_teardown(void *arg) {
  struct pk *p = arg;
  int bad = 15;
  while (bad > 0 && bad == p->key) bad--;
  if (pkey_mprotect((void *)p->page, 4096, PROT_READ | PROT_WRITE, bad) != 0 && errno == EINVAL)
    return -1;
  if (pkey_mprotect((void *)p->page, 4096, PROT_READ | PROT_WRITE, 0) != 0) return -2;
  if (pkey_free(p->key) != 0) return -3;
  pkey_set(p->key, PKEY_DISABLE_ACCESS);  // the page no longer has this key
  long v = p->page[0];
  munmap((void *)p->page, 4096);
  return v;
}

// ---- host-allocated keys ------------------------------------------------------
// A key allocated on the host, outside any guest entry, is one gk never saw
// handed out; workerd's V8 allocates its sandbox, JIT and pointer-table keys at
// initialization this way and keys pages with them from inside a turn. Such a
// pkey_mprotect must be virtualized like one whose pkey_alloc gk forwarded:
// the call succeeds, the page carries the key in the guest PTEs, and the guest
// PKRU decides the access. A key gk cannot reflect at all fails with ENOMEM,
// the one errno a caller like V8 tolerates. Runs on a fresh thread, so that
// the key exists before the thread's guest PKRU is first taken from its host
// PKRU, as it does for workerd's request threads.
struct hk {
  int key;
  volatile unsigned char *page;
  long setup, read, denied, again, bad;
  unsigned long fault;
};

static long hk_setup(void *arg) {
  struct hk *h = arg;
  h->page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (h->page == MAP_FAILED) return -1;
  h->page[0] = 0x42;
  if (pkey_mprotect((void *)h->page, 4096, PROT_READ | PROT_WRITE, h->key) != 0)
    return -(100 + errno);
  return 0;
}
static long hk_read(void *arg) { return ((struct hk *)arg)->page[0]; }
static long hk_deny_read(void *arg) {
  struct hk *h = arg;
  pkey_set(h->key, PKEY_DISABLE_ACCESS);
  return h->page[0];
}
static long hk_allow_read(void *arg) {
  struct hk *h = arg;
  pkey_set(h->key, 0);
  return h->page[0];
}
// Keys outside the PTE field: ENOMEM, never EINVAL. Then the page gets the
// default key back and goes away.
static long hk_bad_keys(void *arg) {
  struct hk *h = arg;
  long r = 0;
  if (pkey_mprotect((void *)h->page, 4096, PROT_READ | PROT_WRITE, 16) == 0 || errno != ENOMEM)
    r |= 1;
  if (pkey_mprotect((void *)h->page, 4096, PROT_READ | PROT_WRITE, -2) == 0 || errno != ENOMEM)
    r |= 2;
  if (pkey_mprotect((void *)h->page, 4096, PROT_READ | PROT_WRITE, 0) != 0) r |= 4;
  munmap((void *)h->page, 4096);
  return r;
}
static void *hk_thread(void *arg) {
  struct hk *h = arg;
  h->key = pkey_alloc(0, 0);  // on the host: no guest entry sees this
  if (h->key <= 0) { h->setup = -99; return NULL; }
  h->setup = gk_run(hk_setup, h);
  h->read = gk_run(hk_read, h);
  h->denied = gk_run(hk_deny_read, h);
  h->fault = gk_fault_addr();
  h->again = gk_run(hk_allow_read, h);
  h->bad = gk_run(hk_bad_keys, h);
  pkey_free(h->key);
  return NULL;
}
static int host_key_check(void) {
  struct hk h = {0};
  pthread_t t;
  if (pthread_create(&t, NULL, hk_thread, &h) != 0) return 0;
  pthread_join(t, NULL);
  int ok = h.setup == 0 && h.read == 0x42 && h.denied == GK_EFAULT &&
           h.fault == (uintptr_t)h.page && h.again == 0x42 && h.bad == 0;
  printf("host-allocated key: key %d, pkey_mprotect from the guest -> %ld, read=%#lx, "
         "read with access disabled->%s, re-enabled=%#lx, keys outside 0..15 -> ENOMEM %s "
         "[%s]\n", h.key, h.setup, h.read, h.denied == GK_EFAULT ? "FAULT" : "allowed",
         h.again, h.bad == 0 ? "yes" : "no", ok ? "OK" : "FAIL");
  return ok;
}
#else
// No protection keys to virtualize on arm64 (see GK_HAS_PKEYS in gk.c).
static int host_key_check(void) {
  printf("host-allocated key: no protection keys on arm64; skipped [OK]\n");
  return 1;
}
#endif

// ---- gk_run_here: the guest on the caller's stack ---------------------------
// The guest function records where its own frame is, then does real work on
// that stack: a deep non-tail recursion (well over a megabyte of stack, so on
// the main thread the kernel-grown stack must grow under the guest), snprintf
// into a local buffer, and a forwarded write. Returns a checksum.
struct here { uintptr_t frame; long deep; long len; };

static long __attribute__((noinline)) deep(long depth, long acc) {
  volatile char pad[1024];
  pad[0] = (char)depth;
  pad[1023] = (char)acc;
  if (depth == 0) return acc + pad[0];
  long r = deep(depth - 1, acc ^ (depth * 7));
  return r + pad[1023];
}
#define DEEP_DEPTH 1500  // ~1.5 MB of stack

static long here_fn(void *arg) {
  struct here *h = arg;
  h->frame = (uintptr_t)__builtin_frame_address(0);
  h->deep = deep(DEEP_DEPTH, 1);
  char buf[256];
  h->len = snprintf(buf, sizeof buf, "guest on the caller's stack: frame %#lx, deep=%ld\n",
                    (unsigned long)h->frame, h->deep);
  write(1, buf, (size_t)h->len);
  long sum = 0;
  for (long i = 0; i < h->len; i++) sum += (unsigned char)buf[i];
  return sum;
}

// The range of the calling thread's stack, as V8 would learn it.
static int thread_stack(uintptr_t *lo, uintptr_t *hi) {
#if defined(__APPLE__)
  *hi = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
  *lo = *hi - pthread_get_stacksize_np(pthread_self());
  return 1;
#else
  pthread_attr_t a;
  void *addr; size_t size;
  if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
  int ok = pthread_attr_getstack(&a, &addr, &size) == 0;
  pthread_attr_destroy(&a);
  *lo = (uintptr_t)addr; *hi = (uintptr_t)addr + size;
  return ok;
#endif
}

// Runs the caller's-stack check on one thread (the main thread's stack is
// kernel-grown; a pthread's is a fixed mapping). Returns 1 if every check held.
static long __attribute__((noinline)) here_check(const char *who) {
  uintptr_t lo, hi;
  if (!thread_stack(&lo, &hi)) return 0;
  volatile char marker = 0;
  uintptr_t caller = (uintptr_t)&marker;  // a local of the frame calling gk_run_here
  struct here h = {0};
  long r = gk_run_here(here_fn, &h);
  long want_deep = deep(DEEP_DEPTH, 1);
  long want_sum = 0;
  { char buf[256];
    int n = snprintf(buf, sizeof buf, "guest on the caller's stack: frame %#lx, deep=%ld\n",
                     (unsigned long)h.frame, want_deep);
    for (int i = 0; i < n; i++) want_sum += (unsigned char)buf[i]; }
  int in_stack = h.frame >= lo && h.frame < hi;
  // gk_run_here leaves a page plus a red zone of slack between the switch
  // point and the guest's first frame, so "just below" is within two pages.
  int just_below = h.frame < caller && caller - h.frame < 2 * 4096;
  int ok = in_stack && just_below && h.deep == want_deep && r == want_sum && h.len > 0;
  printf("gk_run_here (%s): thread stack [%#lx,%#lx), caller local %#lx, guest frame %#lx "
         "(%ld bytes below caller, %s), deep recursion %s, checksum %ld %s [%s]\n",
         who, (unsigned long)lo, (unsigned long)hi, (unsigned long)caller,
         (unsigned long)h.frame, (long)(caller - h.frame),
         in_stack && just_below ? "on the caller's stack" : "NOT on the caller's stack",
         h.deep == want_deep ? "ok" : "WRONG", r, r == want_sum ? "ok" : "WRONG",
         ok ? "OK" : "FAIL");
  return ok;
}

static void *here_thread(void *arg) {
  (void)arg;
  return (void *)(uintptr_t)here_check("pthread");
}

// ---- the host-frame wall (ring 3 on the caller's stack) ---------------------
// gk_run_here_user maps the caller's stack above the turn's entry read-only for
// the turn, so ring-3 code cannot overwrite the host frames it will return
// into. A ring-3 fn therefore reports through heap memory (arg points to a
// calloc'd struct), never by assigning a caller-frame local. The turn runs on a
// deep host call chain, mirroring a production turn (the KJ event loop down
// through runImpl/runJsTurn); see wall_descend.
struct wall {
  uintptr_t host_frame;  // in: a local in the host caller frame just above the switch point
  long host_intact;      // out (host-side): the host-frame local was not clobbered by the turn
  uintptr_t frame;       // out: the fn's own frame pointer
  uintptr_t boundary;    // out: the first page boundary at or above the fn's entry rsp
  uintptr_t target;      // out: the address the fn stored to (where a fault is expected)
  long host_val;         // out: what the fn read at host_frame (reads of walled frames work)
  long tls_ok;           // out: a TLS variable was written and read back
  long deep;             // out: the deep recursion's result (the working stack is writable)
  int which;             // in: 0 store at the wall's first page, 1 store in the host caller frame,
                         //     2 store in the entry slack (below the wall), 3 no store (returns 0x600d)
  int ring0;             // in: run the turn at ring 0 (gk_run_here) rather than ring 3
};
static __thread long wall_tls;
static long user_cpl(void *arg);  // defined with the ring-3 privilege tests below
static long wall_fn(void *arg);

// Depth of the host call chain synthesised above a ring-3 turn's switch point.
// 24 frames of ~512 bytes push it about three pages below the top of the stack,
// so the wall has whole pages of host frames to protect even on a pthread,
// whose static TLS block (which stays writable) sits close above the switch
// point on a shallow chain. See wall_descend.
#define WALL_DEPTH 24
static volatile long wall_sink;

// Run the turn under a chain of WALL_DEPTH real host frames, so more than a page
// of host frames sits between the guest's entry and the top of the stack. On a
// shallow chain -- a pthread whose gk_run_here_user call lands within a page of
// its static TLS block at the top of the stack -- there is no whole page below
// the TLS to wall and the window is legitimately empty; that sub-page case is an
// accepted limitation, because the acute target is a production turn, which runs
// on a deep chain like the one synthesised here. At the deepest frame the
// address of a live local is recorded as host_frame: a genuine host caller frame
// (one gk returns into) that the wall must hold read-only. The volatile pad,
// touched after the nested call, keeps each level a distinct non-tail frame and
// leaves the turn's return value exactly intact.
static long __attribute__((noinline)) wall_descend(int depth, struct wall *w) {
  volatile char pad[512];
  __asm__ __volatile__("" : : "r"(pad) : "memory");  // the whole pad exists in the frame
  pad[0] = (char)depth;
  pad[sizeof pad - 1] = (char)~depth;
  long r;
  if (depth <= 0) {
    volatile long host_local = 0x1111;  // a host caller frame just above the switch point
    w->host_frame = (uintptr_t)&host_local;
    r = w->ring0 ? gk_run_here(wall_fn, w) : gk_run_here_user(wall_fn, w);
    w->host_intact = host_local == 0x1111;
  } else {
    r = wall_descend(depth - 1, w);
  }
  wall_sink = pad[0] + pad[sizeof pad - 1];  // touch the pad; forbid a tail call
  return r;
}

static long __attribute__((noinline)) wall_fn(void *arg) {
  struct wall *w = arg;
  uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
#if defined(__x86_64__)
  // The entry rsp is two words above the frame pointer (return address, saved
  // rbp); the wall starts at the first page boundary at or above it.
  uintptr_t entry = frame + 16;
#else
  // A call pushes nothing on arm64: the entry sp is the call's CFA.
  uintptr_t entry = (uintptr_t)__builtin_dwarf_cfa();
#endif
  w->frame = frame;
  w->boundary = (entry + 0xfff) & ~0xfffUL;
  w->host_val = *(volatile long *)w->host_frame;  // read a walled host frame: reads work
  wall_tls = 42;  // TLS (atop a pthread's stack mapping) stays writable
  w->tls_ok = wall_tls == 42;
  w->deep = deep(DEEP_DEPTH, 1);  // ~1.5MB of stores below the entry
  switch (w->which) {
    case 0: w->target = w->boundary; break;      // the wall's first page
    case 1: w->target = w->host_frame; break;    // a host caller frame the wall covers
    case 3: w->target = 0; return 0x600d;        // a clean turn: no store above the entry
    default: w->target = entry + 64; break;      // entry slack: below the wall, used by neither
  }
  *(volatile unsigned long *)w->target = 0x4b1d;
  return (long)*(volatile unsigned long *)w->target;
}

// Runs the wall checks on one thread (the main thread's TLS lives in ld.so's
// mapping; a pthread's sits at the top of its stack mapping, just above the host
// frames the wall covers). Returns 1 if every check held.
static long __attribute__((noinline)) wall_check(const char *who) {
  struct wall *w = calloc(1, sizeof *w);
  if (!w) return 0;
  long want_deep = deep(DEEP_DEPTH, 1);

  // 1. A store at the wall's first page (at or above the guest's entry rsp)
  //    faults there; meanwhile the turn's read of a walled host frame, its TLS
  //    write and its deep recursion below the entry all worked.
  w->which = 0;
  long r0 = wall_descend(WALL_DEPTH, w);
  unsigned long f0 = gk_fault_addr();
  int reads_ok = w->host_val == 0x1111 && w->tls_ok && w->deep == want_deep;
  uintptr_t disp_frame = w->frame, disp_boundary = w->boundary;
  int ok0 = r0 == GK_EFAULT && f0 == w->boundary && w->boundary > w->frame && reads_ok;
  // 2. A store into a host caller frame the wall covers -- the kind of frame
  //    whose return address an escape would target -- faults there and leaves
  //    the host local untouched.
  w->which = 1;
  w->target = 0;
  long r1 = wall_descend(WALL_DEPTH, w);
  unsigned long f1 = gk_fault_addr();
  uintptr_t disp_host = w->host_frame;
  int intact = w->host_intact;
  int ok1 = r1 == GK_EFAULT && f1 == disp_host && intact;
  // 3. Ring 0 on the caller's stack has no wall: a store just above the entry
  //    (in the slack, which neither host nor guest uses) succeeds.
  volatile long ring0_local = 0x2222;  // a live, readable host frame for wall_fn's read
  w->host_frame = (uintptr_t)&ring0_local;
  w->which = 2;
  long r2 = gk_run_here(wall_fn, w);
  int ok2 = r2 == 0x4b1d && w->deep == want_deep;
  // 4. The process survived both faults: ring 3 on the caller's stack still runs
  //    a full turn.
  long cpl = gk_run_here_user(user_cpl, NULL);
  int ok = ok0 && ok1 && ok2 && cpl == 3;
  printf("host-frame wall (%s): guest frame %#lx, wall from %#lx, host caller frame %#lx; ring-3 "
         "store at wall -> %s (fault %#lx), into host caller frame -> %s (fault %#lx, local %s), "
         "reads/TLS/recursion %s; ring-0 store above entry -> %#lx; ring 3 after at CPL %ld "
         "[%s]\n",
         who, (unsigned long)disp_frame, (unsigned long)disp_boundary, (unsigned long)disp_host,
         r0 == GK_EFAULT ? "FAULT" : "WROTE", f0, r1 == GK_EFAULT ? "FAULT" : "WROTE", f1,
         intact ? "intact" : "CLOBBERED", reads_ok ? "ok" : "BROKEN", r2, cpl, ok ? "OK" : "FAIL");
  free(w);
  return ok;
}


static void *wall_thread(void *arg) {
  (void)arg;
  return (void *)(uintptr_t)wall_check("pthread");
}

// The wall's TLB flush is skipped when it is provably unnecessary (see
// wall_raise in gk.c): a ring-3 turn whose window lies within the previous
// turn's, with the vCPU not having run in between. Every turn here descends
// the same host chain, so consecutive turns meet that condition, and the
// counters in gk_stats say which path each raise took. Checks that the skip
// is taken and that the wall still holds on a turn that skipped, that each
// condition whose failure makes a flush necessary again does force one and
// the wall holds there too, and that skipping resumes afterwards. Runs on one
// thread; returns 1 if every check held.
static long __attribute__((noinline)) wall_skip_check(const char *who) {
  struct wall *w = calloc(1, sizeof *w);
  if (!w) return 0;
  gk_stats a, b;
#define WALL_DELTA(field) ((b).field - (a).field)

  // Two clean turns at WALL_DEPTH make that window this vCPU's previous one,
  // whatever ran on the thread before (the first raise after a flush-forcing
  // event flushes; the second is a steady-state turn).
  w->which = 3;
  long p0 = wall_descend(WALL_DEPTH, w);
  long p1 = wall_descend(WALL_DEPTH, w);
  int okp = p0 == 0x600d && p1 == 0x600d;

  // 1. Same depth, nothing ran in between: the raise skips its flush, and a
  //    store into a host caller frame the wall covers still faults there.
  gk_get_stats(&a);
  w->which = 1;
  w->target = 0;
  long r1 = wall_descend(WALL_DEPTH, w);
  unsigned long f1 = gk_fault_addr();
  gk_get_stats(&b);
  long sk1 = WALL_DELTA(wall_skipped), fl1 = WALL_DELTA(wall_flushed);
  uintptr_t host1 = w->host_frame;
  int ok1 = r1 == GK_EFAULT && f1 == host1 && w->host_intact && sk1 == 1 && fl1 == 0 &&
            WALL_DELTA(tlb_flushes) == 0;
  // 1b. Again after that fault, at the wall's first page: still skipping,
  //     still faulting.
  gk_get_stats(&a);
  w->which = 0;
  long r1b = wall_descend(WALL_DEPTH, w);
  unsigned long f1b = gk_fault_addr();
  gk_get_stats(&b);
  uintptr_t bound1 = w->boundary;
  int ok1b = r1b == GK_EFAULT && f1b == bound1 && WALL_DELTA(wall_skipped) == 1 &&
             WALL_DELTA(wall_flushed) == 0;

  // 2. A deeper turn: its window starts lower, taking in pages that were the
  //    previous turn's writable working stack (its frames and deep recursion
  //    sat right below the previous entry), so the raise must flush (one full
  //    flush, through whichever stub is in use), and its store at the wall's
  //    first page -- one of those pages, whose writable translation the flush
  //    had to drop -- faults.
  gk_get_stats(&a);
  w->which = 0;
  long r2 = wall_descend(WALL_DEPTH + 16, w);
  unsigned long f2 = gk_fault_addr();
  gk_get_stats(&b);
  uintptr_t bound2 = w->boundary;
  int ok2 = r2 == GK_EFAULT && f2 == bound2 && bound2 < bound1 && WALL_DELTA(wall_skipped) == 0 &&
            WALL_DELTA(wall_flushed) == 1 && WALL_DELTA(tlb_flushes) == 1;
#if defined(__x86_64__)
  const char *stub = b.flush_invpcid ? "invpcid" : "cr4.pge toggle";
#else
  const char *stub = "tlbi vmalle1";
#endif

  // 3. Back at the shallower depth the window lies within the deeper one, so
  //    the raise skips again, and the store into the host frame faults.
  gk_get_stats(&a);
  w->which = 1;
  w->target = 0;
  long r3 = wall_descend(WALL_DEPTH, w);
  unsigned long f3 = gk_fault_addr();
  gk_get_stats(&b);
  int ok3 = r3 == GK_EFAULT && f3 == host1 && w->host_intact && WALL_DELTA(wall_skipped) == 1 &&
            WALL_DELTA(wall_flushed) == 0 && WALL_DELTA(tlb_flushes) == 0;

  // 4. A ring-0 turn on the same chain runs the vCPU between two ring-3 turns
  //    and, having no wall, writes the very host frame the next wall covers
  //    (so a writable translation of that page is now in the TLB). The next
  //    raise must flush, and the ring-3 store into that frame faults.
  w->ring0 = 1;
  w->which = 1;
  long r4a = wall_descend(WALL_DEPTH, w);
  int clobbered = !w->host_intact;  // ring 0 may write there: the local was overwritten
  w->ring0 = 0;
  gk_get_stats(&a);
  w->which = 1;
  w->target = 0;
  long r4 = wall_descend(WALL_DEPTH, w);
  unsigned long f4 = gk_fault_addr();
  gk_get_stats(&b);
  int ok4 = r4a == 0x4b1d && clobbered && r4 == GK_EFAULT && f4 == host1 && w->host_intact &&
            WALL_DELTA(wall_skipped) == 0 && WALL_DELTA(wall_flushed) == 1 &&
            WALL_DELTA(tlb_flushes) == 1;

  // 5. Skipping resumes: the next clean turn at the same depth skips.
  gk_get_stats(&a);
  w->which = 3;
  long r5 = wall_descend(WALL_DEPTH, w);
  gk_get_stats(&b);
  int ok5 = r5 == 0x600d && WALL_DELTA(wall_skipped) == 1 && WALL_DELTA(wall_flushed) == 0 &&
            WALL_DELTA(tlb_flushes) == 0;
#undef WALL_DELTA

  int ok = okp && ok1 && ok1b && ok2 && ok3 && ok4 && ok5;
  printf("wall flush skip (%s, flush stub %s): warm-up %s; skipped raise: host-frame store -> %s "
         "(fault %#lx, local %s, skipped %ld flushed %ld), wall-page store -> %s (fault %#lx) "
         "[%s]; deeper turn (wall from %#lx < %#lx) flushed, store -> %s (fault %#lx) [%s]; "
         "shallower again skipped, store -> %s [%s]; ring-0 turn between (wrote %#lx) -> next "
         "raise flushed, store -> %s (local %s) [%s]; skipping resumed [%s] [%s]\n",
         who, stub, okp ? "ok" : "BROKEN", r1 == GK_EFAULT ? "FAULT" : "WROTE", f1,
         w->host_intact ? "intact" : "CLOBBERED", sk1, fl1, r1b == GK_EFAULT ? "FAULT" : "WROTE",
         f1b, ok1 && ok1b ? "OK" : "FAIL", (unsigned long)bound2, (unsigned long)bound1,
         r2 == GK_EFAULT ? "FAULT" : "WROTE", f2, ok2 ? "OK" : "FAIL",
         r3 == GK_EFAULT ? "FAULT" : "WROTE", ok3 ? "OK" : "FAIL", (unsigned long)r4a,
         r4 == GK_EFAULT ? "FAULT" : "WROTE", w->host_intact ? "intact" : "CLOBBERED",
         ok4 ? "OK" : "FAIL", ok5 ? "OK" : "FAIL", ok ? "OK" : "FAIL");
  free(w);
  return ok;
}

static void *wall_skip_thread(void *arg) {
  (void)arg;
  return (void *)(uintptr_t)wall_skip_check("pthread");
}

#if GK_TEST_HOST_DECOMMIT
// ---- a host-side decommit under an installed PTE ----------------------------
// The host can change a mapping without the change passing through the guest
// (a decommit issued by another host thread, say): the guest's PTE for the page
// then outlives its backing, and the guest's next access fails in KVM_RUN
// rather than as a guest #PF. It must end the turn like any other fault, with
// GK_EFAULT at the page, and leave the thread's vCPU usable, so that the next
// turn on the thread runs normally instead of failing in its turn.

// Pull the backing from under `page` on the host side: a fixed PROT_NONE
// mapping replaces it, and gk sees no forwarded syscall.
static void host_decommit(void *page) {
  mmap(page, hpage, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

// One page: committed and written by a guest turn (installing its PTE),
// decommitted from the host side, then read by a guest turn, then a plain
// computation as the next turn on the thread. `run` is gk_run_here_user or
// gk_run_here.
struct backing { long write, read, next; unsigned long fault; };
static void backing_probe(long (*run)(long (*)(void *), void *), void *page,
                          struct backing *b) {
  struct ap w = {page, 0x5a};
  b->write = run(commit_write, &w);
  host_decommit(page);
  b->read = run(read_byte, page);
  b->fault = gk_fault_addr();
  b->next = run(test_compute, (void *)100);
}

static int backing_ok(const struct backing *b, unsigned long want_fault, long want_next) {
  return b->write == 0x5a && b->read == GK_EFAULT && b->fault == want_fault &&
         b->next == want_next;
}

// Runs the checks on one thread: an arena page at ring 3 on the caller's stack
// (the production path) and at ring 0, both reported at the page; then a page
// outside any arena, where the address cannot be recovered and the fault is
// reported at the faulting instruction (in read_byte) instead. Returns 1 if
// every check held.
static long host_decommit_check(const char *who) {
  long want_next = 0;
  for (long i = 0; i < 100; i++) want_next += i * i;
  gk_arena *a = gk_arena_create(1UL << 20);
  if (!a) {
    printf("host-side decommit (%s): no arena [FAIL]\n", who);
    return 0;
  }
  unsigned char *p3 = gk_arena_base(a), *p0 = p3 + hpage;
  struct backing u, k, h;
  gk_arena_enter(a);
  backing_probe(gk_run_here_user, p3, &u);
  backing_probe(gk_run_here, p0, &k);
  gk_arena_enter(NULL);
  gk_arena_destroy(a);  // takes the decommitted pages with it
  void *hp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  backing_probe(gk_run_here_user, hp, &h);
  munmap(hp, 4096);
  int ok_u = backing_ok(&u, (unsigned long)(uintptr_t)p3, want_next);
  int ok_k = backing_ok(&k, (unsigned long)(uintptr_t)p0, want_next);
  unsigned long rb = (unsigned long)(uintptr_t)read_byte;
  int ok_h = h.write == 0x5a && h.read == GK_EFAULT && h.fault >= rb && h.fault < rb + 64 &&
             h.next == want_next;
  int ok = ok_u && ok_k && ok_h;
  printf("host-side decommit (%s): arena page at ring 3 -> %s (fault %#lx, page %p), next turn "
         "%ld; at ring 0 -> %s (fault %#lx, page %p), next turn %ld; page outside arenas -> %s "
         "(fault %#lx, read_byte %#lx), next turn %ld; want next %ld [%s]\n",
         who, u.read == GK_EFAULT ? "FAULT" : "READ", u.fault, (void *)p3, u.next,
         k.read == GK_EFAULT ? "FAULT" : "READ", k.fault, (void *)p0, k.next,
         h.read == GK_EFAULT ? "FAULT" : "READ", h.fault, rb, h.next, want_next,
         ok ? "OK" : "FAIL");
  return ok;
}

static void *host_decommit_thread(void *arg) {
  (void)arg;
  return (void *)(uintptr_t)host_decommit_check("pthread");
}
#endif  // GK_TEST_HOST_DECOMMIT

// ---- transparent huge pages behind an arena ---------------------------------
// A range of 2MB or more that the owner commits in one piece is advised
// MADV_HUGEPAGE as the commit is reflected (see arena_thp_advise), so it is
// backed by 2MB host pages when the guest first touches it, and KVM maps it
// with one nested entry per 2MB. The backing is read from /proc/self/smaps
// (AnonHugePages over the range, and whether its VMAs carry the advice, the
// "hg" VmFlag). A range decommitted V8-style, with a fixed PROT_NONE mmap,
// and recommitted has to come back huge too: the fixed mmap replaced the VMA,
// and the advice with it, so the recommit must have re-advised it. A small
// commit (a V8 heap chunk) must get neither the advice nor a huge page, so
// khugepaged has nothing to collapse there.
struct thp_range { unsigned char *base; size_t len; };

// Runs in the guest at ring 3: commit the range RW and write one byte per 4KB
// page, first to last. Returns the number of pages touched.
static long thp_commit_touch(void *arg) {
  struct thp_range *r = arg;
  if (mprotect(r->base, r->len, PROT_READ | PROT_WRITE) != 0) return -errno;
  long n = 0;
  for (size_t off = 0; off < r->len; off += 4096, n++) ((volatile unsigned char *)r->base)[off] = 1;
  return n;
}

// Runs in the guest: decommit the range the way V8 does, by mapping fresh
// PROT_NONE memory over it.
static long thp_decommit(void *arg) {
  struct thp_range *r = arg;
  void *p = mmap(r->base, r->len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  return p == r->base ? 0 : -errno;
}

// AnonHugePages (kB) summed over the VMAs inside [s, e), from /proc/self/smaps.
// Through `advised`, if given, whether any of those VMAs carries MADV_HUGEPAGE
// (the "hg" VmFlag).
static long anon_huge_kb(uintptr_t s, uintptr_t e, int *advised) {
  FILE *f = fopen("/proc/self/smaps", "r");
  if (!f) return -1;
  char line[512];
  long kb = 0;
  int inside = 0, hg = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long vs, ve;
    if (sscanf(line, "%lx-%lx ", &vs, &ve) == 2) { inside = vs >= s && ve <= e; continue; }
    if (!inside) continue;
    long v;
    if (sscanf(line, "AnonHugePages: %ld kB", &v) == 1) kb += v;
    if (strncmp(line, "VmFlags:", 8) == 0 && strstr(line, " hg")) hg = 1;
  }
  fclose(f);
  if (advised) *advised = hg;
  return kb;
}

// The kernel's THP mode: 1 if the advice can take effect ("always" or
// "madvise"), 0 for "never" or a kernel without THP.
static int thp_available(void) {
  FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
  if (!f) return 0;
  char line[128] = "";
  if (!fgets(line, sizeof line, f)) line[0] = 0;
  fclose(f);
  return strstr(line, "[never]") == NULL;
}

static long thp_check(void) {
#if defined(__APPLE__)
  printf("arena huge pages: no huge-page advice on macOS; skipped [OK]\n");
  return 1;
#endif
  if (!thp_available()) {
    printf("arena huge pages: THP disabled on this kernel; skipped [OK]\n");
    return 1;
  }
  gk_arena *a = gk_arena_create(16UL << 20);
  if (!a) { printf("arena huge pages: no arena [FAIL]\n"); return 0; }
  unsigned char *base = gk_arena_base(a);
  // Each range is measured as the VMAs inside it, so the ranges are kept
  // apart by uncommitted memory, which never merges with them.
  struct thp_range whole = {base + (2UL << 20), 4UL << 20};   // aligned 4MB, in one piece
  struct thp_range first = {base + (2UL << 20), 2UL << 20};   // its first half
  struct thp_range apart = {base + (8UL << 20), 2UL << 20};   // an aligned 2MB on its own
  struct thp_range small = {base + (12UL << 20), 256UL << 10}; // a V8 heap chunk's worth
  int hg1, hg7;
  gk_arena_enter(a);
  long n1 = gk_run_user(thp_commit_touch, &whole);
  long kb1 = anon_huge_kb((uintptr_t)whole.base, (uintptr_t)whole.base + whole.len, &hg1);
  // Decommitted from the guest: the fixed mmap is forwarded, the recommit
  // re-advises.
  long d = gk_run_user(thp_decommit, &first);
  long kb2 = anon_huge_kb((uintptr_t)first.base, (uintptr_t)first.base + first.len, NULL);
  long n3 = gk_run_user(thp_commit_touch, &first);
  long kb3 = anon_huge_kb((uintptr_t)first.base, (uintptr_t)first.base + first.len, NULL);
  // Decommitted from the host, by a thread outside the guest (V8's background
  // sweeper): gk sees nothing of it, so the guest's recommit must re-advise.
  long n4 = gk_run_user(thp_commit_touch, &apart);
  long kb4 = anon_huge_kb((uintptr_t)apart.base, (uintptr_t)apart.base + apart.len, NULL);
  long dh = thp_decommit(&apart);
  long kb5 = anon_huge_kb((uintptr_t)apart.base, (uintptr_t)apart.base + apart.len, NULL);
  long n6 = gk_run_user(thp_commit_touch, &apart);
  long kb6 = anon_huge_kb((uintptr_t)apart.base, (uintptr_t)apart.base + apart.len, NULL);
  // A small commit: below GK_THP_MIN, so neither advised nor huge.
  long n7 = gk_run_user(thp_commit_touch, &small);
  long kb7 = anon_huge_kb((uintptr_t)small.base, (uintptr_t)small.base + small.len, &hg7);
  gk_arena_enter(NULL);
  gk_arena_destroy(a);
  // The kernel may fail to find a free 2MB page (fragmentation), so one huge
  // page in the 4MB is enough to prove the backing is eligible.
  int ok = n1 == 1024 && kb1 >= 2048 && hg1 && d == 0 && kb2 == 0 && n3 == 512 &&
           kb3 == 2048 && n4 == 512 && kb4 == 2048 && dh == 0 && kb5 == 0 && n6 == 512 &&
           kb6 == 2048 && n7 == 64 && kb7 == 0 && !hg7;
  printf("arena huge pages: 4MB committed and touched from ring 3 -> %ld pages, AnonHugePages "
         "%ld kB, advised %s; its first 2MB decommitted by a guest fixed mmap -> %ld, %ld kB, "
         "recommitted and touched -> %ld pages, %ld kB; a 2MB apart touched -> %ld pages, "
         "%ld kB, decommitted by a host fixed mmap -> %ld, %ld kB, recommitted and touched -> "
         "%ld pages, %ld kB; a 256KB commit touched -> %ld pages, %ld kB, advised %s [%s]\n",
         n1, kb1, hg1 ? "yes" : "no", d, kb2, n3, kb3, n4, kb4, dh, kb5, n6, kb6, n7, kb7,
         hg7 ? "yes" : "no", ok ? "OK" : "FAIL");
  return ok;
}

// ---- guest ring-3 privilege split -------------------------------------------
// gk_run_user runs fn at guest ring 3. Untrusted code runs there so that even
// arbitrary code execution cannot breach the arena walls: ring-3 code cannot
// run privileged instructions, reload CR3 or touch gk's supervisor/refused
// pages. These probe every part of that split.

// Returns its own CPL (CS.RPL) and records the CS selector through arg. Run at
// ring 3, CPL is 3 and CS is the ring-3 code selector (0x2b).
static long user_cpl(void *arg) {
  unsigned short cs;
#if defined(__x86_64__)
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
#else
  cs = (unsigned short)user_ring();
#endif
  if (arg) *(unsigned long *)arg = cs;
  return (long)(cs & 3);
}

// Reads CR3 -- a privileged instruction, so #GP at ring 3. (A read, not a
// write, so if isolation were broken and it ran at ring 0 it would be
// harmless.)
static long user_read_cr3(void *arg) {
  (void)arg;
  unsigned long v;
#if defined(__x86_64__)
  __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
#else
  __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(v));  // traps at EL0
#endif
  return (long)v;
}

// Writes one byte at the address passed as arg, and reads it back. Against a
// supervisor or refused address this faults; against a normal user page it
// works.
static long user_write_here(void *arg) {
  volatile unsigned char *p = arg;
  p[0] = 0x5a;
  return p[0];
}

// A forwarded getpid(2) issued from ring 3: the syscall trampoline runs in ring
// 0, the host forwards it, and SYSRET returns to ring 3 with the pid.
static long user_getpid(void *arg) {
  (void)arg;
#if defined(__aarch64__)
  return raw_syscall3(SYS_getpid, 0, 0, 0);
#else
  long pid;
  register long r10 __asm__("r10") = 0, r8 __asm__("r8") = 0, r9 __asm__("r9") = 0;
  __asm__ __volatile__("syscall" : "=a"(pid)
                       : "a"((long)SYS_getpid), "r"(r10), "r"(r8), "r"(r9)
                       : "rcx", "r11", "memory");
  return pid;
#endif
}

#if GK_TEST_GUEST_THREADS
// ---- guest-created threads at ring 3 ----------------------------------------
// A thread that ring-3 code creates must run at ring 3 as well: gaining ring 0
// through pthread_create would void the privilege split. These bodies run in
// threads spawned by a gk_run_user function.
struct ugt {
  int id;
  unsigned long cs;        // the thread's CS selector (0x2b at ring 3)
  unsigned long child_cs;  // a grandchild's, spawned from this thread
  long pid;                // a forwarded getpid from the thread
  long wrote;              // the byte read back after writing u->page
  unsigned char *page;     // a user page to write, or NULL
  unsigned long addr;      // an address to touch (the faulting bodies)
  volatile int before, after;  // reached the faulting instruction / got past it
};

static void *user_grandchild(void *arg) {
  unsigned short cs;
#if defined(__x86_64__)
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
#else
  cs = (unsigned short)user_ring();
#endif
  *(unsigned long *)arg = cs;
  return NULL;
}

// The well-behaved ring-3 thread: records its CS, forwards a getpid, uses libc,
// writes a user page, and spawns a grandchild that records its own CS.
static void *user_thread_body(void *arg) {
  struct ugt *u = arg;
  unsigned short cs;
#if defined(__x86_64__)
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
#else
  cs = (unsigned short)user_ring();
#endif
  u->cs = cs;
  u->pid = user_getpid(NULL);
  char *buf = malloc(64);
  int len = snprintf(buf, 64, "guest thread %d ran at CPL %d\n", u->id, cs & 3);
  write(1, buf, (size_t)len);
  free(buf);
  if (u->page) {
    volatile unsigned char *p = u->page;
    p[0] = (unsigned char)(0x50 + u->id);
    u->wrote = p[0];
  }
  pthread_t gc;
  if (pthread_create(&gc, NULL, user_grandchild, &u->child_cs) == 0) pthread_join(gc, NULL);
  return (void *)(uintptr_t)(u->id * 10);
}

// Executes a privileged instruction (mov %cr3) after recording its address: at
// ring 3 it #GPs, the thread ends there, and gk_fault_addr is that address.
static void *user_thread_priv(void *arg) {
  struct ugt *u = arg;
  unsigned long v;
  u->before = 1;
#if defined(__x86_64__)
  __asm__ __volatile__("leaq 1f(%%rip), %0\n\t"
                       "movq %0, %1\n\t"
                       "1: movq %%cr3, %0"
                       : "=&r"(v), "=m"(u->addr));
#else
  __asm__ __volatile__("adr %0, 1f\n\t"
                       "str %0, %1\n\t"
                       "1: mrs %0, ttbr0_el1"
                       : "=&r"(v), "=m"(u->addr));
#endif
  u->after = 1;  // not reached at ring 3
  return (void *)v;
}

// Writes the byte at u->addr: a user page works, gk's memory or another
// arena's faults and ends the thread.
static void *user_thread_touch(void *arg) {
  struct ugt *u = arg;
  u->before = 1;
  volatile unsigned char *p = (void *)u->addr;
  p[0] = 0x5a;
  u->after = 1;
  return NULL;
}

// Runs at ring 3: spawns three user_thread_body threads at once and joins
// them, checking their return values.
static long spawn_users_in_guest(void *arg) {
  struct ugt *us = arg;
  enum { N = 3 };
  pthread_t th[N];
  for (int i = 0; i < N; i++)
    if (pthread_create(&th[i], NULL, user_thread_body, &us[i]) != 0) return -1;
  long ok = 1;
  for (int i = 0; i < N; i++) {
    void *rv = NULL;
    if (pthread_join(th[i], &rv) != 0) return -2;
    if ((long)(uintptr_t)rv != us[i].id * 10) ok = 0;
  }
  return ok;
}

// Runs at ring 3: spawns one thread running `body` on `arg` and joins it. For
// a thread that faults, the join must still return (the kernel clears its tid
// on exit as for any thread), and the spawner and the process live on.
struct spawn { void *(*body)(void *); void *arg; };
static long spawn_one_in_guest(void *arg) {
  struct spawn *s = arg;
  pthread_t th;
  if (pthread_create(&th, NULL, s->body, s->arg) != 0) return -1;
  if (pthread_join(th, NULL) != 0) return -2;
  return 1;
}
#endif  // GK_TEST_GUEST_THREADS

// ---- global pages across a root switch --------------------------------------
// A turn for another isolate loads that isolate's root from inside the guest,
// which keeps the TLB's global entries: the shared runtime's pages. Nothing an
// arena holds may be global, or a translation of one isolate's page would
// survive into the next isolate's turn. The alternation below (one thread,
// arenas A and B in turn; ring 0, ring 3 and ring 3 on the caller's stack)
// checks that after every switch the previous arena's page faults while the
// current one's and a shared runtime page read fine.
//
// gk_debug_drop_pte then tells cached translations from fresh page walks: with
// a page's PTE gone, an access that takes no demand fault used a TLB entry.
// That shows an arena page's translation is cached at all (same root: no
// fault), that it does not survive a switch away and back (its entry is not
// global: one fault), that a shared page's does survive the switch (global
// pages at work: no fault), and that gk's own flush -- a reflected mprotect --
// drops the shared page's entry too (the flush includes global entries: one
// fault). A survival check gets a few attempts, as the CPU may evict the entry
// or KVM flush the vCPU on its own; the thread is pinned to one CPU meanwhile
// so that KVM does not flush it on every move between CPUs.
struct dp { void *dummy; void *target; };

// Runs in the guest: an mprotect on a page of its own (a reflected syscall,
// which flushes this vCPU's TLB), then a read of the target page.
static long flush_then_read(void *arg) {
  struct dp *d = arg;
  if (mprotect(d->dummy, 4096, PROT_READ | PROT_WRITE) != 0) return -errno;
  volatile unsigned char *p = d->target;
  return (long)*p;
}

static unsigned char global_shared[2 * 4096] __attribute__((aligned(4096)));

static long global_check(void) {
#if !defined(__APPLE__)  // no CPU affinity on macOS
  cpu_set_t old, one;
  int pinned = sched_getaffinity(0, sizeof old, &old) == 0;
  CPU_ZERO(&one);
  CPU_SET(sched_getcpu(), &one);
  if (pinned) sched_setaffinity(0, sizeof one, &one);
#endif
  unsigned char *sp = global_shared;  // a page of the runtime's .bss: shared by every root
  sp[0] = 0x77;
  gk_arena *A = gk_arena_create(4096), *B = gk_arena_create(4096);
  void *dummy = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!A || !B || dummy == MAP_FAILED || mprotect(gk_arena_base(A), 4096, PROT_READ | PROT_WRITE) != 0 ||
      mprotect(gk_arena_base(B), 4096, PROT_READ | PROT_WRITE) != 0) {
    printf("global pages: setup failed [FAIL]\n");
    return 0;
  }
  unsigned char *pa = gk_arena_base(A), *pb = gk_arena_base(B);
  pa[0] = 0xAA;
  pb[0] = 0xBB;
  gk_stats s0, s1, sfirst;
  gk_get_stats(&sfirst);

  // 1. Alternation. Start on the base root, so every round's first entry is a
  //    root switch.
  gk_arena_enter(NULL);
  gk_run(test_compute, (void *)10);
  gk_get_stats(&s0);
  enum { ROUNDS = 8 };
  int alt_ok = 1, rounds_ok = 0;
  for (int r = 0; r < ROUNDS && alt_ok; r++) {
    gk_arena *own = (r & 1) ? B : A;
    unsigned char *po = (r & 1) ? pb : pa, *px = (r & 1) ? pa : pb;
    long want = (r & 1) ? 0xBB : 0xAA;
    gk_arena_enter(own);
    long r0 = gk_run(read_byte, po), r3 = gk_run_user(read_byte, po),
         rh = gk_run_here_user(read_byte, po);
    long rs0 = gk_run(read_byte, sp), rsh = gk_run_here_user(read_byte, sp);
    long x0 = gk_run(read_byte, px);
    unsigned long f0 = gk_fault_addr();
    long x3 = gk_run_user(read_byte, px);
    unsigned long f3 = gk_fault_addr();
    long xh = gk_run_here_user(read_byte, px);
    unsigned long fh = gk_fault_addr();
    alt_ok = r0 == want && r3 == want && rh == want && rs0 == 0x77 && rsh == 0x77 &&
             x0 == GK_EFAULT && f0 == (uintptr_t)px && x3 == GK_EFAULT && f3 == (uintptr_t)px &&
             xh == GK_EFAULT && fh == (uintptr_t)px;
    if (alt_ok) rounds_ok++;
    else
      printf("  round %d under %s: own %#lx/%#lx/%#lx, shared %#lx/%#lx, other -> %s %#lx / %s "
             "%#lx / %s %#lx (want fault at %p)\n", r, (r & 1) ? "B" : "A", r0, r3, rh, rs0, rsh,
             x0 == GK_EFAULT ? "FAULT" : "READ", f0, x3 == GK_EFAULT ? "FAULT" : "READ", f3,
             xh == GK_EFAULT ? "FAULT" : "READ", fh, (void *)px);
  }
  gk_get_stats(&s1);
  long switches = s1.root_switches - s0.root_switches;
  int sw_ok = switches == ROUNDS;

  // 2a. Same root: an arena page's translation is cached (methodology check).
  //     A VM that keeps no translation from one guest entry to the next at
  //     all, with gk flushing nothing in between (seen under nested
  //     virtualization, where the outer hypervisor may drop the nested
  //     guest's TLB at every entry), cannot show what survives a switch: the
  //     survival checks (2a, 2c) are then reported as not observable.
  int cached = 0;
  long gk_flushed = 0;
  for (int attempt = 0; attempt < 3 && !cached; attempt++) {
    gk_arena_enter(A);
    gk_run(read_byte, pa);
    gk_debug_drop_pte((unsigned long)pa);
    gk_get_stats(&s0);
    long v = gk_run(read_byte, pa);
    gk_get_stats(&s1);
    cached = v == 0xAA && s1.demand_faults == s0.demand_faults;
    gk_flushed += s1.tlb_flushes - s0.tlb_flushes;
  }
  int no_tlb = !cached && gk_flushed == 0;
  // 2b. An arena page's translation does not survive a switch away and back,
  //     and the page is unreachable from the other arena in between.
  gk_arena_enter(A);
  gk_run(read_byte, pa);
  gk_debug_drop_pte((unsigned long)pa);
  gk_get_stats(&s0);
  gk_arena_enter(B);
  long xb = gk_run(read_byte, pa);
  unsigned long fxb = gk_fault_addr();
  gk_arena_enter(A);
  long va = gk_run(read_byte, pa);
  gk_get_stats(&s1);
  long arena_faults = s1.demand_faults - s0.demand_faults;
  int arena_ok = xb == GK_EFAULT && fxb == (uintptr_t)pa && va == 0xAA && arena_faults == 1 &&
                 s1.root_switches - s0.root_switches == 2;
  // 2c. A shared page's translation survives the switch to another arena.
  int survived = 0, shared_attempts = 0;
  long shared_faults = -1;
  for (int attempt = 0; attempt < 3 && !survived; attempt++) {
    shared_attempts++;
    gk_arena_enter(A);
    gk_run(read_byte, sp);
    gk_debug_drop_pte((unsigned long)sp);
    gk_get_stats(&s0);
    gk_arena_enter(B);
    long v = gk_run(read_byte, sp);
    gk_get_stats(&s1);
    shared_faults = s1.demand_faults - s0.demand_faults;
    survived = v == 0x77 && shared_faults == 0 && s1.root_switches - s0.root_switches == 1;
  }
  // 2d. gk's flush drops it: first a warm-up so that every page the guest
  //     function touches is mapped, then the same with the PTE dropped.
  struct dp d = {dummy, sp};
  gk_arena_enter(A);
  gk_run(read_byte, sp);
  gk_run(flush_then_read, &d);
  gk_get_stats(&s0);
  long warm = gk_run(flush_then_read, &d);
  gk_get_stats(&s1);
  long warm_faults = s1.demand_faults - s0.demand_faults;
  gk_debug_drop_pte((unsigned long)sp);
  gk_get_stats(&s0);
  long after = gk_run(flush_then_read, &d);
  gk_get_stats(&s1);
  long flush_faults = s1.demand_faults - s0.demand_faults;
  // The reflected mprotect is one full flush (counted as such), and that flush
  // has to drop the shared page's global entry: one fault.
  int flush_ok = warm == 0x77 && warm_faults == 0 && after == 0x77 && flush_faults == 1 &&
                 s1.tlb_flushes - s0.tlb_flushes == 1;

  gk_arena_enter(NULL);
  gk_get_stats(&s1);
  long globals = s1.global_pages - sfirst.global_pages;
  gk_arena_destroy(A);
  gk_arena_destroy(B);
  munmap(dummy, 4096);
#if !defined(__APPLE__)
  if (pinned) sched_setaffinity(0, sizeof old, &old);
#endif

  int ok = alt_ok && sw_ok && (no_tlb || (cached && survived)) && arena_ok && flush_ok && globals > 0;
  printf("global pages: %d/%d alternating turns isolated (%ld root switches); arena page cached "
         "on the same root %s, after a switch away and back %s (%ld fault, unreachable in "
         "between %s); shared page after a switch %s (%ld faults, %d attempt%s); after gk's "
         "flush %s (%ld fault, warm-up %ld); %ld global PTEs installed%s [%s]\n",
         rounds_ok, ROUNDS, switches, cached ? "ok" : no_tlb ? "not observable" : "NOT CACHED",
         arena_ok ? "re-walked" : "STALE", arena_faults,
         xb == GK_EFAULT && fxb == (uintptr_t)pa ? "faults" : "READABLE",
         survived ? "survived" : "DROPPED", shared_faults, shared_attempts,
         shared_attempts == 1 ? "" : "s", flush_ok ? "dropped" : "SURVIVED", flush_faults,
         warm_faults, globals,
         no_tlb ? "; this VM keeps no TLB entry across guest entries, so what survives a switch "
                  "is not observable here" : "",
         ok ? "OK" : "FAIL");
  return ok;
}

// ---- demand-fault neighborhoods ----------------------------------------------
// A demand fault maps the pages around its page from the same /proc/self/maps
// read, within an aligned 256 KiB window clipped to the host mapping (see
// demand_map_neighbors in gk.c). Checks, with gk_stats' demand_faults and
// neighbor_pages, that pages in the window are then usable with no fault of
// their own while pages past the window still fault; that a protection
// boundary inside the window stops it (the other side faults on its own and
// keeps its own protection); that inside an arena the window stays within the
// committed range and the pre-mapped pages stay unreachable from every other
// root; and that a REFUSE or SUPER page inside the window is left to its own
// fault (refused, or mapped supervisor-only). Every region is fresh, its PTEs
// dropped and the vCPU's TLB flushed before counting, so that an earlier
// test's leftovers at a reused address cannot perturb the counts; each sits
// between PROT_NONE guard pages, so the kernel cannot merge it with a
// neighbor and its bounds are exactly known.
#define NB_PAGE 4096UL                     // a guest page
#define NB_WIN (256UL << 10)
#define NB_LEN (8 * NB_WIN + 2 * hpage)  // nb_region's mapping, guards included
enum { NB_PAGES = 64 };

// Drops the PTEs of [p, p+len) in every root, then flushes this vCPU's TLB.
static void nb_fresh(void *p, size_t len, void *dummy) {
  for (size_t off = 0; off < len; off += NB_PAGE) gk_debug_drop_pte((unsigned long)p + off);
  struct dp d = {dummy, global_shared};
  gk_run(flush_then_read, &d);
}

// A fresh read-write mapping of `usable` bytes between two PROT_NONE guard
// pages. Returns the mapping, usable + 2 pages long, whose usable part starts
// a page in; NULL on failure.
static unsigned char *nb_guarded(size_t usable) {
  size_t len = usable + 2 * hpage;
  unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return NULL;
  if (mprotect(m, hpage, PROT_NONE) != 0 ||
      mprotect(m + hpage + usable, hpage, PROT_NONE) != 0) {
    munmap(m, len);
    return NULL;
  }
  return m;
}

// A 256 KiB-aligned window inside a fresh guarded 2 MiB mapping (NB_LEN long
// with its guards), with at least a page to spare on either side. Returns the
// mapping and sets *win.
static unsigned char *nb_region(unsigned char **win) {
  unsigned char *m = nb_guarded(8 * NB_WIN);
  if (!m) return NULL;
  *win = (unsigned char *)(((uintptr_t)m + hpage + 2 * NB_WIN - 1) & ~(NB_WIN - 1));
  return m;
}

static long neighborhood_check(void) {
  void *dummy = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  gk_stats a, b;
#define NB_DELTA(field) ((b).field - (a).field)
#define P(i) (w + (i) * NB_PAGE)

  // 1. The window: one fault maps its 64 pages; the pages just outside it on
  //    either side still fault, each mapping its own window.
  unsigned char *w, *m1 = nb_region(&w);
  int ok1 = 0;
  long f1 = -1, n1 = -1, f1b = -1, f1c = -1, f1d = -1, n1c = -1;
  if (m1) {
    nb_fresh(m1, NB_LEN, dummy);
    gk_get_stats(&a);
    long r = gk_run(read_byte, P(5));
    gk_get_stats(&b);
    f1 = NB_DELTA(demand_faults); n1 = NB_DELTA(neighbor_pages);
    gk_get_stats(&a);
    long r0 = gk_run(read_byte, P(0)), r6 = gk_run(read_byte, P(6)), r63 = gk_run(read_byte, P(63));
    struct ap wr = {P(7), 0x5e};
    long rw = gk_run(write_byte, &wr);
    gk_get_stats(&b);
    f1b = NB_DELTA(demand_faults);
    gk_get_stats(&a);
    long r64 = gk_run(read_byte, P(64));
    gk_get_stats(&b);
    f1c = NB_DELTA(demand_faults); n1c = NB_DELTA(neighbor_pages);
    gk_get_stats(&a);
    long rm1 = gk_run(read_byte, P(-1));
    gk_get_stats(&b);
    f1d = NB_DELTA(demand_faults);
    ok1 = r == 0 && f1 == 1 && n1 == NB_PAGES - 1 && r0 == 0 && r6 == 0 && r63 == 0 &&
          rw == 0x5e && P(7)[0] == 0x5e && f1b == 0 && r64 == 0 && f1c == 1 &&
          n1c == NB_PAGES - 1 && rm1 == 0 && f1d == 1;
  }
  printf("neighborhood: fault at page 5 -> %ld fault, %ld neighbors; pages 0, 6, 63 read and 7 "
         "written with %ld faults; page 64 -> %ld fault (%ld neighbors), page -1 -> %ld fault "
         "[%s]\n", f1, n1, f1b, f1c, n1c, f1d, ok1 ? "OK" : "FAIL");

  // 2. A protection boundary inside the window: pages 8..15 are read-only on
  //    the host, a mapping of their own. A fault at page 1 maps pages 0..7
  //    only (the window starts at page 0, being aligned); page 8 faults on
  //    its own and maps 9..15 read-only, so a write to page 12 faults and
  //    page 16 (writable again) faults on its own.
  unsigned char *m2 = nb_region(&w);
  int ok2 = 0;
  long f2 = -1, n2 = -1, f2b = -1, f2c = -1, n2c = -1, f2d = -1, wf = -1, f2e = -1;
  unsigned long wfa = 0;
  if (m2 && mprotect(P(8), 8 * NB_PAGE, PROT_READ) == 0) {
    nb_fresh(m2, NB_LEN, dummy);
    gk_get_stats(&a);
    long r1 = gk_run(read_byte, P(1));
    gk_get_stats(&b);
    f2 = NB_DELTA(demand_faults); n2 = NB_DELTA(neighbor_pages);
    gk_get_stats(&a);
    long r3 = gk_run(read_byte, P(3));
    gk_get_stats(&b);
    f2b = NB_DELTA(demand_faults);
    gk_get_stats(&a);
    long r8 = gk_run(read_byte, P(8));
    gk_get_stats(&b);
    f2c = NB_DELTA(demand_faults); n2c = NB_DELTA(neighbor_pages);
    gk_get_stats(&a);
    long r12 = gk_run(read_byte, P(12));
    gk_get_stats(&b);
    f2d = NB_DELTA(demand_faults);
    struct ap wr = {P(12), 0x33};
    wf = gk_run(write_byte, &wr);
    wfa = gk_fault_addr();
    gk_get_stats(&a);
    long r16 = gk_run(read_byte, P(16));
    gk_get_stats(&b);
    f2e = NB_DELTA(demand_faults);
    ok2 = r1 == 0 && f2 == 1 && n2 == 7 && r3 == 0 && f2b == 0 && r8 == 0 && f2c == 1 &&
          n2c == 7 && r12 == 0 && f2d == 0 && wf == GK_EFAULT && wfa == (uintptr_t)P(12) &&
          P(12)[0] == 0 && r16 == 0 && f2e == 1;
  }
  printf("neighborhood, protection boundary: fault at page 1 -> %ld fault, %ld neighbors; page 3 "
         "%ld faults; read-only page 8 -> %ld fault, %ld neighbors; page 12 %ld faults, write "
         "-> %s (fault %#lx); page 16 -> %ld fault [%s]\n", f2, n2, f2b, f2c, n2c, f2d,
         wf == GK_EFAULT ? "FAULT" : "WROTE", wfa, f2e, ok2 ? "OK" : "FAIL");

  // 3. In an arena: pages 0..23 committed, so a fault at page 17 maps the
  //    other 23 (the window covers them all) and page 24 is refused as
  //    before; the pre-mapped pages are reachable from the arena's root only.
  gk_arena *A = gk_arena_create(4 * NB_WIN), *B = gk_arena_create(4096);
  int ok3 = 0;
  long f3 = -1, n3 = -1, f3b = -1, x24 = 0, x0 = 0, xb = 0, f3c = -1, va = -1;
  unsigned long fx24 = 0, fx0 = 0, fxb = 0;
  if (A && B && mprotect(gk_arena_base(A), 24 * NB_PAGE, PROT_READ | PROT_WRITE) == 0) {
    w = gk_arena_base(A);
    gk_arena_enter(A);
    gk_get_stats(&a);
    long r17 = gk_run(read_byte, P(17));
    gk_get_stats(&b);
    f3 = NB_DELTA(demand_faults); n3 = NB_DELTA(neighbor_pages);
    gk_get_stats(&a);
    long r16 = gk_run(read_byte, P(16)), r23 = gk_run(read_byte, P(23));
    gk_get_stats(&b);
    f3b = NB_DELTA(demand_faults);
    x24 = gk_run(read_byte, P(24));
    fx24 = gk_fault_addr();
    gk_arena_enter(NULL);
    x0 = gk_run(read_byte, P(20));
    fx0 = gk_fault_addr();
    gk_arena_enter(B);
    xb = gk_run(read_byte, P(20));
    fxb = gk_fault_addr();
    gk_arena_enter(A);
    gk_get_stats(&a);
    va = gk_run(read_byte, P(20));
    gk_get_stats(&b);
    f3c = NB_DELTA(demand_faults);
    gk_arena_enter(NULL);
    ok3 = r17 == 0 && f3 == 1 && n3 == 23 && r16 == 0 && r23 == 0 && f3b == 0 &&
          x24 == GK_EFAULT && fx24 == (uintptr_t)P(24) && x0 == GK_EFAULT &&
          fx0 == (uintptr_t)P(20) && xb == GK_EFAULT && fxb == (uintptr_t)P(20) && va == 0 &&
          f3c == 0;
  }
  if (A) gk_arena_destroy(A);
  if (B) gk_arena_destroy(B);
  printf("neighborhood, arena: fault at page 17 of 24 committed -> %ld fault, %ld neighbors; "
         "pages 16, 23 %ld faults; page 24 -> %s (fault %#lx); page 20 from the base root -> "
         "%s (fault %#lx), from another arena -> %s (fault %#lx), back in the arena -> %ld "
         "(%ld faults) [%s]\n", f3, n3, f3b, x24 == GK_EFAULT ? "FAULT" : "READ", fx24,
         x0 == GK_EFAULT ? "FAULT" : "READ", fx0, xb == GK_EFAULT ? "FAULT" : "READ", fxb, va,
         f3c, ok3 ? "OK" : "FAIL");

  // 4. A REFUSE page (8) and a SUPER page (9) inside the window: a ring-3
  //    fault at page 1 maps the other 61; page 8 is still refused, page 9
  //    faults on its own and comes up supervisor-only, page 10 is usable from
  //    ring 3 with no fault.
  unsigned char *m4 = nb_region(&w);
  int ok4 = 0;
  long f4 = -1, n4 = -1, x8 = 0, f4b = -1, r9 = -1, x9 = 0, r10 = -1, f4c = -1;
  unsigned long fx8 = 0, fx9 = 0;
  if (m4) {
    gk_debug_protect_range((unsigned long)P(8), NB_PAGE, GK_DEBUG_PROT_REFUSE);
    gk_debug_protect_range((unsigned long)P(9), NB_PAGE, GK_DEBUG_PROT_SUPER);
    nb_fresh(m4, NB_LEN, dummy);
    gk_get_stats(&a);
    long r1 = gk_run_user(read_byte, P(1));
    gk_get_stats(&b);
    f4 = NB_DELTA(demand_faults); n4 = NB_DELTA(neighbor_pages);
    x8 = gk_run(read_byte, P(8));
    fx8 = gk_fault_addr();
    gk_get_stats(&a);
    r9 = gk_run(read_byte, P(9));
    gk_get_stats(&b);
    f4b = NB_DELTA(demand_faults);
    x9 = gk_run_user(read_byte, P(9));
    fx9 = gk_fault_addr();
    gk_get_stats(&a);
    r10 = gk_run_user(read_byte, P(10));
    gk_get_stats(&b);
    f4c = NB_DELTA(demand_faults);
    gk_debug_protect_range((unsigned long)P(8), NB_PAGE, GK_DEBUG_PROT_KEEP);
    gk_debug_protect_range((unsigned long)P(9), NB_PAGE, GK_DEBUG_PROT_KEEP);
    ok4 = r1 == 0 && f4 == 1 && n4 == NB_PAGES - 3 && x8 == GK_EFAULT && fx8 == (uintptr_t)P(8) &&
          r9 == 0 && f4b == 1 && x9 == GK_EFAULT && fx9 == (uintptr_t)P(9) && r10 == 0 && f4c == 0;
  }
  printf("neighborhood, class boundary: ring-3 fault at page 1 -> %ld fault, %ld neighbors; "
         "REFUSE page 8 -> %s (fault %#lx); SUPER page 9 from ring 0 -> %ld (%ld fault), from "
         "ring 3 -> %s (fault %#lx); page 10 from ring 3 -> %ld (%ld faults) [%s]\n", f4, n4,
         x8 == GK_EFAULT ? "FAULT" : "READ", fx8, r9, f4b, x9 == GK_EFAULT ? "FAULT" : "READ",
         fx9, r10, f4c, ok4 ? "OK" : "FAIL");

#undef P
#undef NB_DELTA
  if (m1) munmap(m1, NB_LEN);
  if (m2) munmap(m2, NB_LEN);
  if (m4) munmap(m4, NB_LEN);
  if (dummy != MAP_FAILED) munmap(dummy, 4096);
  return ok1 && ok2 && ok3 && ok4;
}

// ---- arena nesting and the restore path ------------------------------------
// gk_arena_leave returns to the arena gk recorded for the thread, never to one
// the caller names, and gk_arena_enter accepts only a live arena. A pointer to
// memory that is not an arena struct (heap memory shaped like one) is refused
// unread, and so is a pool struct whose arena has been destroyed; either way
// the thread's root stays as it was. Nested enters restore in order. An arena
// destroyed while it sits in a thread's history is replaced by the base root
// there, even once its struct has been recycled for a new arena. A leave with
// nothing entered selects the base root, and nesting past GK_ARENA_NEST is
// refused with the root unchanged. gk_arena_destroy validates its argument the
// same way, and destroying the arena the calling thread is under leaves that
// thread on the base root.
static int nesting_check(void) {
  gk_arena *A = gk_arena_create(4096), *B = gk_arena_create(4096);
  if (!A || !B) { printf("nesting: create failed\n"); return 0; }
  unsigned char *pa = gk_arena_base(A), *pb = gk_arena_base(B);
  struct ap wa = {pa, 0xA5}, wb = {pb, 0xB6};
  gk_arena_enter(NULL);

  // Nesting: A, then B inside it. Under B only B is reachable; the first leave
  // is back under A, the second on the base root.
  int e1 = gk_arena_enter(A);
  long ra = gk_run(commit_write, &wa);
  int e2 = gk_arena_enter(B);
  long rb = gk_run(commit_write, &wb);
  long rb_a = gk_run(read_byte, pa);
  int l1 = gk_arena_leave();
  long ra2 = gk_run(read_byte, pa);
  long ra_b = gk_run(read_byte, pb);
  int l2 = gk_arena_leave();
  long r0_a = gk_run(read_byte, pa), r0_b = gk_run(read_byte, pb);
  int nest_ok = e1 == 0 && ra == 0xA5 && e2 == 0 && rb == 0xB6 && rb_a == GK_EFAULT &&
                l1 == 0 && ra2 == 0xA5 && ra_b == GK_EFAULT && l2 == 0 &&
                r0_a == GK_EFAULT && r0_b == GK_EFAULT;

  // A bogus arena: heap memory the caller controls, shaped like an arena struct.
  // It is refused, nothing in it is touched (no reference count is written
  // through it), and the thread stays under A.
  enum { JUNK = 8192 };
  unsigned char *junk = malloc(JUNK);
  memset(junk, 0xEE, JUNK);
  gk_arena_enter(A);
  int eb = gk_arena_enter((gk_arena *)junk);
  long ra3 = gk_run(read_byte, pa);
  int junk_intact = 1;
  for (int i = 0; i < JUNK; i++) if (junk[i] != 0xEE) junk_intact = 0;
  free(junk);
  // A pool struct that no live arena owns: C's, after C is destroyed. Refused
  // too, and the thread stays under A.
  gk_arena *C = gk_arena_create(4096);
  gk_arena_destroy(C);
  int ec = gk_arena_enter(C);
  long ra4 = gk_run(read_byte, pa);
  int bogus_ok = eb == -22 && ra3 == 0xA5 && junk_intact && ec == -22 && ra4 == 0xA5;

  // Destroy refuses the same two pointers. It would otherwise read the struct,
  // free the page tables it names and unmap the range it describes, so the
  // heap-shaped one must come back untouched and the thread must keep working
  // under A. Destroying C a second time is refused by its flag.
  unsigned char *junk2 = malloc(JUNK);
  memset(junk2, 0xEE, JUNK);
  gk_arena_destroy((gk_arena *)junk2);
  int junk2_intact = 1;
  for (int i = 0; i < JUNK; i++) if (junk2[i] != 0xEE) junk2_intact = 0;
  free(junk2);
  gk_arena_destroy(C);
  long ra5 = gk_run(read_byte, pa);
  int destroy_bogus_ok = junk2_intact && ra5 == 0xA5;

  // A destroyed arena in the history. Under B on top of A, A is destroyed and a
  // new arena D takes its struct (the pool recycles it); leaving B must land on
  // the base root, not under D, which is reachable only once entered itself.
  gk_arena_enter(B);
  gk_arena_destroy(A);
  gk_arena *D = gk_arena_create(4096);
  unsigned char *pd = gk_arena_base(D);
  int recycled = D == A;
  int md = mprotect(pd, 4096, PROT_READ | PROT_WRITE) == 0;  // committed by the host
  pd[0] = 0xD4;
  int l3 = gk_arena_leave();
  long rd0 = gk_run(read_byte, pd), rb0 = gk_run(read_byte, pb);
  gk_arena_enter(D);
  long rd1 = gk_run(read_byte, pd);
  gk_arena_enter(NULL);
  int scrub_ok = md && l3 == 0 && rd0 == GK_EFAULT && rb0 == GK_EFAULT && rd1 == 0xD4;

  // Underflow: a leave with nothing entered selects the base root and says so.
  gk_arena_enter(B);
  int l4 = gk_arena_leave(), l5 = gk_arena_leave();
  long ru = gk_run(read_byte, pb);
  int under_ok = l4 == 0 && l5 == -22 && ru == GK_EFAULT;

  // Overflow: the bound holds, the refused enter leaves the thread under B, and
  // the matching leaves unwind to the base root.
  int over_ok = 1;
  for (int i = 0; i < GK_ARENA_NEST; i++) if (gk_arena_enter(B) != 0) over_ok = 0;
  int eo = gk_arena_enter(B);
  long ro = gk_run(read_byte, pb);
  for (int i = 0; i < GK_ARENA_NEST; i++) if (gk_arena_leave() != 0) over_ok = 0;
  long ro0 = gk_run(read_byte, pb);
  over_ok = over_ok && eo == -12 && ro == 0xB6 && ro0 == GK_EFAULT;

  // Destroying the arena the calling thread is under: the thread leaves it
  // first, so the destroy goes ahead and the thread is left on the base root,
  // where the arena's memory is gone.
  gk_arena *E = gk_arena_create(4096);
  unsigned char *pe = gk_arena_base(E);
  gk_arena_enter(E);
  gk_arena_destroy(E);
  long re = gk_run(read_byte, pe);
  gk_arena_enter(NULL);
  int self_ok = pe != NULL && re == GK_EFAULT;

  gk_arena_enter(NULL);
  gk_arena_destroy(B);
  gk_arena_destroy(D);
  int ok = nest_ok && bogus_ok && destroy_bogus_ok && scrub_ok && under_ok && over_ok && self_ok;
  printf("arena nesting: A/B nest+leave %s; bogus heap pointer -> %d (root kept %s, memory "
         "untouched %s), destroyed pool struct -> %d (root kept %s); destroyed-in-history "
         "(struct recycled: %s) leave -> base root %s; underflow -> %d (base root %s); "
         "overflow at %d -> %d (root kept %s, unwinds %s); destroy refuses bogus %s, "
         "self-destroy -> base root %s [%s]\n",
         nest_ok ? "OK" : "FAIL", eb, ra3 == 0xA5 ? "yes" : "no", junk_intact ? "yes" : "no",
         ec, ra4 == 0xA5 ? "yes" : "no", recycled ? "yes" : "no",
         scrub_ok ? "yes" : "no", l5, ru == GK_EFAULT ? "yes" : "no", GK_ARENA_NEST, eo,
         ro == 0xB6 ? "yes" : "no", ro0 == GK_EFAULT ? "yes" : "no",
         destroy_bogus_ok ? "yes" : "no", self_ok ? "yes" : "no", ok ? "OK" : "FAIL");
  return ok;
}

// ---- mapping-change policy ------------------------------------------------
// With a filter that applies gk_mapping_syscall_ok, a ring-3 turn inside an
// arena cannot change or remove the mappings of the test binary's own text, a
// page of another live arena, gk's control block, gk's page tables or the
// main thread's stack: each of mprotect, munmap, mremap, madvise and
// pkey_mprotect on them fails with EPERM and the memory is intact afterwards.
// The same turn still commits, writes, remaps and decommits its own arena's
// pages, and takes an ordinary kernel-placed mmap, so the policy refuses what
// is not the guest's rather than everything. With no arena active, the
// arena's pages are off limits too.
static int guard_filter(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
  return gk_mapping_syscall_ok(nr, a1, a2, a3, a4, a5, a6);
}

// Lives in the test binary's text; its page is a target below and it is
// called afterwards, from ring 3 and from the host, to show the page still
// maps and executes.
static long __attribute__((noinline)) guard_text_fn(long x) {
  __asm__ __volatile__("" : "+r"(x));
  return x * 3 + 1;
}

enum { GUARD_TARGETS = 5 };
struct guard_target { void *page; long mprotect, munmap, mremap, madvise, pkey; };
struct guard_probe {
  struct guard_target tgt[GUARD_TARGETS];
  void *mine;  // a page of the active arena
  long own_mprotect, own_write, own_mmap, own_mmap_write, own_madvise, own_munmap,
       own_reserve, own_anon, text_call;
};

// Runs at ring 3 under the arena that owns `mine`.
static long guard_probe_fn(void *arg) {
  struct guard_probe *p = arg;
  for (int i = 0; i < GUARD_TARGETS; i++) {
    struct guard_target *g = &p->tgt[i];
    g->mprotect = mprotect(g->page, 4096, PROT_READ | PROT_WRITE) ? -errno : 0;
    g->munmap = munmap(g->page, 4096) ? -errno : 0;
    g->mremap = mremap(g->page, 4096, 8192, 0) == MAP_FAILED ? -errno : 0;
    g->madvise = madvise(g->page, 4096, MADV_DONTNEED) ? -errno : 0;
    g->pkey = pkey_mprotect(g->page, 4096, PROT_READ, -1) ? -errno : 0;
  }
  // The arena's own page, as V8 treats one: commit, use, replace with a fresh
  // fixed mapping, trim, decommit, and reserve again.
  volatile unsigned char *b = p->mine;
  p->own_mprotect = mprotect(p->mine, 4096, PROT_READ | PROT_WRITE) ? -errno : 0;
  if (p->own_mprotect == 0) { b[0] = 0x5A; p->own_write = b[0]; }
  void *m = mmap(p->mine, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  p->own_mmap = m == MAP_FAILED ? -errno : (m == p->mine ? 0 : -1000);
  if (p->own_mmap == 0) { b[0] = 0x6B; p->own_mmap_write = b[0]; }
  p->own_madvise = madvise(p->mine, 4096, MADV_DONTNEED) ? -errno : 0;
  p->own_munmap = munmap(p->mine, 4096) ? -errno : 0;
  m = mmap(p->mine, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
           -1, 0);
  p->own_reserve = m == MAP_FAILED ? -errno : (m == p->mine ? 0 : -1000);
  // A mapping the kernel places, as the process allocator takes.
  unsigned char *anon = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                             -1, 0);
  if (anon == MAP_FAILED) p->own_anon = -errno;
  else {
    anon[0] = 1;
    long seen = anon[0];
    p->own_anon = munmap(anon, 4096) ? -errno : seen == 1 ? 0 : -1000;
  }
  p->text_call = guard_text_fn(7);
  return 0;
}

// Runs at ring 3 with no arena active: one mprotect of an arena page.
static long guard_mprotect_only(void *arg) {
  return mprotect(arg, 4096, PROT_READ | PROT_WRITE) ? -errno : 0;
}

// ---- mremap and the guest's translations ------------------------------------
// A mapping the guest moves with mremap must take its guest PTEs with it. A
// translation left at the old address keeps resolving, and guest physical is
// host virtual, so it would reach whatever the host maps there next without
// that mapping passing the class check a demand fault makes. So after a move
// from the guest, a read of the old address must fault, at that address.
struct mremap_case {
  unsigned char *old;
  unsigned char *dest;
  size_t len;
  unsigned char *moved;
  long err;
};

// Runs at ring 3: move the range to `dest`, then read its first byte there. The
// destination is named outright, as an mremap of an unchanged size to no
// particular address is free to leave the mapping where it is.
static long mremap_move(void *arg) {
  struct mremap_case *c = arg;
  void *p = mremap(c->old, c->len, c->len, MREMAP_MAYMOVE | MREMAP_FIXED, c->dest);
  if (p == MAP_FAILED) { c->err = errno; return -1; }
  c->moved = p;
  return ((volatile unsigned char *)p)[0];
}

static int mremap_reflect_check(void) {
  size_t len = 8192;
  unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) { printf("mremap reflection: mmap failed [FAIL]\n"); return 0; }
  m[0] = 0x7e;
  m[4096] = 0x7e;
  // The guest reads it first, so it holds translations for both pages.
  long before = gk_run_here_user(read_byte, m);
  long before2 = gk_run_here_user(read_byte, m + 4096);

  // On the heap, not this frame: gk_run_here_user walls the caller's stack off
  // from the turn, so a ring-3 turn cannot read an argument that lives there.
  // The destination is reserved up front, so the move has to relocate the
  // mapping rather than keeping it in place.
  unsigned char *dest = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (dest == MAP_FAILED) { printf("mremap reflection: reserve failed [FAIL]\n"); return 0; }
  struct mremap_case *c = malloc(sizeof *c);
  if (c == NULL) { printf("mremap reflection: malloc failed [FAIL]\n"); return 0; }
  *c = (struct mremap_case){m, dest, len, NULL, 0};
  long moved = gk_run_here_user(mremap_move, c);

  // The old address: its mapping is gone, so the guest must fault there, at
  // that address, instead of still reading through a stale translation.
  long after = gk_run_here_user(read_byte, m);
  uintptr_t fault = gk_fault_addr();
  long after2 = gk_run_here_user(read_byte, m + 4096);
  uintptr_t fault2 = gk_fault_addr();
  // The new address is readable from the guest, with the contents it had.
  long at_new = c->moved ? gk_run_here_user(read_byte, c->moved) : -1;

  int ok = before == 0x7e && before2 == 0x7e && moved == 0x7e && c->moved == dest &&
           after == GK_EFAULT && fault == (uintptr_t)m &&
           after2 == GK_EFAULT && fault2 == (uintptr_t)(m + 4096) && at_new == 0x7e;
  if (c->moved) munmap(c->moved, len);
  printf("mremap reflection: old range read %ld, %ld before the move; moved %s (guest read "
         "%ld, errno %ld); old range after -> %ld at %#lx and %ld at %#lx; new range -> %ld "
         "[%s]\n",
         before, before2, c->moved ? "yes" : "no", moved, c->err, after, (unsigned long)fault,
         after2, (unsigned long)fault2, at_new, ok ? "OK" : "FAIL");
  free(c);
  return ok;
}

static int mapping_guard_check(void) {
  gk_arena *A = gk_arena_create(4096), *B = gk_arena_create(4096);
  if (!A || !B) { printf("mapping policy: create failed [FAIL]\n"); return 0; }
  unsigned char *pa = gk_arena_base(A), *pb = gk_arena_base(B);
  // B holds a live page, committed and written by the host.
  int mb = mprotect(pb, 4096, PROT_READ | PROT_WRITE) == 0;
  pb[0] = 0xB7;
  unsigned long ctl = gk_debug_ctl_addr(GK_CTL_SCRATCH), refuse = 0;
  gk_debug_control_addrs(NULL, &refuse);
  *(volatile unsigned long *)ctl = 0x5c7a7c4;
  volatile unsigned char stack_local = 0x3C;  // on the main thread's stack

  struct guard_probe *p = calloc(1, sizeof *p);
  const char *names[GUARD_TARGETS] = {"own text", "other arena", "control block",
                                      "page tables", "main stack"};
  p->tgt[0].page = (void *)((uintptr_t)&guard_text_fn & ~0xfffUL);
  p->tgt[1].page = pb;
  p->tgt[2].page = (void *)(ctl & ~0xfffUL);
  p->tgt[3].page = (void *)refuse;
  p->tgt[4].page = (void *)((uintptr_t)&stack_local & ~0xfffUL);
  p->mine = pa;

  // The filter installed for this case only; whatever was installed before
  // (the suite runs without one) is put back afterwards.
  gk_syscall_filter saved = *(gk_syscall_filter *)gk_debug_ctl_addr(GK_CTL_FILTER);
  gk_set_syscall_filter(guard_filter);
  gk_arena_enter(A);
  long run = gk_run_user(guard_probe_fn, p);
  gk_arena_enter(NULL);
  long none = gk_run_user(guard_mprotect_only, pa);
  gk_set_syscall_filter(saved);

  int refused = 1;
  for (int i = 0; i < GUARD_TARGETS; i++) {
    const struct guard_target *g = &p->tgt[i];
    int r = g->mprotect == -EPERM && g->munmap == -EPERM && g->mremap == -EPERM &&
            g->madvise == -EPERM && g->pkey == -EPERM;
    if (!r)
      printf("mapping policy: %s: mprotect %ld munmap %ld mremap %ld madvise %ld "
             "pkey_mprotect %ld (want all %d) [FAIL]\n", names[i], g->mprotect, g->munmap,
             g->mremap, g->madvise, g->pkey, -EPERM);
    refused = refused && r;
  }
  // Still intact: the text executes (the probe called it at ring 3 and the
  // host calls it now), B's byte, the control word and the stack local read
  // back, and the registry still holds its ranges.
  gk_stats st;
  gk_get_stats(&st);
  int intact = p->text_call == 22 && guard_text_fn(7) == 22 && pb[0] == 0xB7 &&
               *(volatile unsigned long *)ctl == 0x5c7a7c4 && stack_local == 0x3C &&
               st.prot_ranges > 0;
  int own_ok = run == 0 && p->own_mprotect == 0 && p->own_write == 0x5A && p->own_mmap == 0 &&
               p->own_mmap_write == 0x6B && p->own_madvise == 0 && p->own_munmap == 0 &&
               p->own_reserve == 0 && p->own_anon == 0;
  int none_ok = none == -EPERM;
  int ok = mb && refused && intact && own_ok && none_ok;
  printf("mapping policy: ring 3 in arena A: own text, arena B page, control block, page "
         "tables, main stack: mprotect/munmap/mremap/madvise/pkey_mprotect -> %s; afterwards "
         "text runs %s, B byte %#x, control word %s, stack local %#x; A's own page: mprotect "
         "%ld write %#lx, fixed mmap %ld write %#lx, madvise %ld, munmap %ld, re-reserve %ld, "
         "kernel-placed mmap %ld, run %ld; no arena active: mprotect A page -> %ld [%s]\n",
         refused ? "all EPERM" : "NOT all refused", p->text_call == 22 && guard_text_fn(7) == 22 ? "yes" : "no",
         pb[0], *(volatile unsigned long *)ctl == 0x5c7a7c4 ? "intact" : "CHANGED",
         stack_local, p->own_mprotect, p->own_write, p->own_mmap, p->own_mmap_write,
         p->own_madvise, p->own_munmap, p->own_reserve, p->own_anon, run, none,
         ok ? "OK" : "FAIL");
  free(p);
  gk_arena_destroy(A);
  gk_arena_destroy(B);
  return ok;
}

// ---- host memory -------------------------------------------------------------
// gk_host_alloc hands out memory the host uses as ordinary memory while no
// guest root maps it: a ring-3 read or write of it faults at the accessed
// address, on the base root and inside an arena (on the caller's stack, the
// production path), and so does a ring-0 read, since a REFUSE page is never
// mapped at all. The host's bytes are intact afterwards. The allocator recycles
// blocks through free lists inside its regions, so many allocate/free cycles,
// and a burst larger than one region, leave the supervisor/refuse registry
// where the regions put it rather than growing it per block; a recycled block
// comes back zero-filled; a zero or oversized request is refused.
static int host_memory_check(void) {
  gk_stats s0, s1, s2, s3, s4;
  gk_get_stats(&s0);
  unsigned char *k = gk_host_alloc(32);
  if (!k) { printf("host memory: gk_host_alloc failed [FAIL]\n"); return 0; }
  gk_get_stats(&s1);
  int zero = 1;
  for (int i = 0; i < 32; i++) if (k[i]) zero = 0;
  for (int i = 0; i < 32; i++) k[i] = (unsigned char)(0xC0 + i);
  unsigned char stack_byte = 0;
  unsigned char *heap = malloc(16);
  int owns = gk_host_owns(k) && gk_host_owns(k + 31) && !gk_host_owns(&stack_byte) &&
             !gk_host_owns(heap) && !gk_host_owns(gk_test_dummy_text);
  free(heap);
  // The first region may already exist: every thread record carries a
  // hostcall frame in host memory from gk_init on. Either way the block costs
  // the registry exactly what it costs in regions, and nothing per block.
  int registered = s1.host_regions >= 1 &&
                   s1.prot_ranges - s0.prot_ranges == s1.host_regions - s0.host_regions &&
                   s1.host_blocks == s0.host_blocks + 1;
  int host_rw = k[0] == 0xC0 && k[31] == 0xDF;

  // Ring 3 on the base root: a read of one byte in the middle of the block, and
  // a write, each fault at the byte's own address.
  long r3 = gk_run_user(read_byte, k + 7);
  unsigned long f3 = gk_fault_addr();
  long w3 = gk_run_user(user_write_here, k + 9);
  unsigned long fw3 = gk_fault_addr();
  // Ring 3 under an arena, on the caller's stack.
  gk_arena *a = gk_arena_create(4096);
  long r3a = -1; unsigned long f3a = 0;
  if (a) {
    gk_arena_enter(a);
    r3a = gk_run_here_user(read_byte, k + 11);
    f3a = gk_fault_addr();
    gk_arena_enter(NULL);
    gk_arena_destroy(a);
  }
  // Ring 0, both ways of entering the guest.
  long r0 = gk_run(read_byte, k + 13);
  unsigned long f0 = gk_fault_addr();
  long r0h = gk_run_here(read_byte, k + 15);
  unsigned long f0h = gk_fault_addr();
  int faults = r3 == GK_EFAULT && f3 == (unsigned long)(k + 7) && w3 == GK_EFAULT &&
               fw3 == (unsigned long)(k + 9) && a && r3a == GK_EFAULT &&
               f3a == (unsigned long)(k + 11) && r0 == GK_EFAULT && f0 == (unsigned long)(k + 13) &&
               r0h == GK_EFAULT && f0h == (unsigned long)(k + 15);
  int intact = 1;
  for (int i = 0; i < 32; i++) if (k[i] != (unsigned char)(0xC0 + i)) intact = 0;
  long cpl = gk_run_user(user_cpl, NULL);  // the process survived every fault

  // Churn: allocate and free blocks of every class, one at a time, then in
  // bursts that together exceed one region (so a second region is mapped and
  // registered once), repeated; the registry and the region count settle
  // after the first burst and then hold.
  gk_get_stats(&s2);
  int churn_ok = 1;
  for (int i = 0; i < 20000 && churn_ok; i++) {
    size_t n = (size_t)16 << (i % 17);  // 16 bytes .. 1 MiB
    unsigned char *p = gk_host_alloc(n);
    if (!p || p[0] || p[n - 1]) { churn_ok = 0; break; }
    memset(p, 0xff, n);
    gk_host_free(p, n);
  }
  enum { BURST = 6, ROUNDS = 5 };
  unsigned char *burst[BURST];
  for (int round = 0; round < ROUNDS && churn_ok; round++) {
    for (int j = 0; j < BURST; j++) {
      burst[j] = gk_host_alloc(1 << 20);
      if (!burst[j]) churn_ok = 0;
      else burst[j][0] = (unsigned char)j, burst[j][(1 << 20) - 1] = (unsigned char)~j;
    }
    for (int j = 0; j < BURST && churn_ok; j++)
      if (burst[j][0] != (unsigned char)j || burst[j][(1 << 20) - 1] != (unsigned char)~j)
        churn_ok = 0;
    for (int j = 0; j < BURST; j++) if (burst[j]) gk_host_free(burst[j], 1 << 20);
    if (round == 0) gk_get_stats(&s3);
  }
  gk_get_stats(&s4);
  int bounded = churn_ok && s3.host_regions >= 2 && s4.host_regions == s3.host_regions &&
                s4.prot_ranges == s3.prot_ranges &&
                s3.prot_ranges - s2.prot_ranges == s3.host_regions - s2.host_regions &&
                s4.host_blocks == s2.host_blocks;

  // A freed block is recycled zero-filled, and the limits hold.
  unsigned char *q = gk_host_alloc(64);
  memset(q, 0xAB, 64);
  gk_host_free(q, 64);
  unsigned char *q2 = gk_host_alloc(64);
  int rezeroed = q2 == q;
  for (int i = 0; i < 64; i++) if (q2[i]) rezeroed = 0;
  gk_host_free(q2, 64);
  int limits = gk_host_alloc(0) == NULL && gk_host_alloc(GK_HOST_MAX_ALLOC + 1) == NULL;
  unsigned char *big = gk_host_alloc(GK_HOST_MAX_ALLOC);
  limits = limits && big != NULL;
  gk_host_free(big, GK_HOST_MAX_ALLOC);
  gk_host_free(k, 32);
  gk_stats s5;
  gk_get_stats(&s5);
  int balanced = s5.host_blocks == s0.host_blocks && s5.prot_ranges == s4.prot_ranges;

  int ok = zero && owns && registered && host_rw && faults && intact && cpl == 3 && bounded &&
           rezeroed && limits && balanced;
  printf("host memory: block @%p zero-filled %s, host r/w %s, owned %s, +%d registry range(s) "
         "for %d region(s); ring-3 read -> %s (fault %#lx == %p), ring-3 write -> %s (fault "
         "%#lx == %p), ring 3 in arena -> %s (fault %#lx == %p), ring 0 -> %s (fault %#lx == %p), "
         "ring 0 on caller stack -> %s (fault %#lx == %p); host bytes %s, ring 3 at CPL %ld; "
         "20000 cycles + %dx%d MiB bursts: regions %d -> %d, registry %d -> %d (%s); "
         "recycled block %s; limits %s; after frees %ld live blocks [%s]\n",
         (void *)k, zero ? "yes" : "NO", host_rw ? "ok" : "BROKEN", owns ? "yes" : "NO",
         s1.prot_ranges - s0.prot_ranges, s1.host_regions,
         r3 == GK_EFAULT ? "FAULT" : "READ", f3, (void *)(k + 7),
         w3 == GK_EFAULT ? "FAULT" : "WROTE", fw3, (void *)(k + 9),
         r3a == GK_EFAULT ? "FAULT" : "READ", f3a, (void *)(k + 11),
         r0 == GK_EFAULT ? "FAULT" : "READ", f0, (void *)(k + 13),
         r0h == GK_EFAULT ? "FAULT" : "READ", f0h, (void *)(k + 15),
         intact ? "intact" : "CHANGED", cpl, ROUNDS, BURST, s3.host_regions, s4.host_regions,
         s3.prot_ranges, s4.prot_ranges, bounded ? "bounded" : "GREW",
         rezeroed ? "same address, zero" : "WRONG", limits ? "ok" : "WRONG", s5.host_blocks,
         ok ? "OK" : "FAIL");
  return ok;
}

// ---- host slots --------------------------------------------------------------
// A host slot names a host-memory block by a handle that is validated against
// gk's own table, which lives in the supervisor control block: the handle
// resolves to its block, which the host reads and writes and ring 3 cannot;
// a handle gk did not issue -- an index past capacity, the right index with
// another generation, a pointer-shaped value, the address of an unmapped page,
// random bits -- resolves to nothing, with nothing read through the value;
// a freed slot's handle resolves to nothing, also once the slot has been
// recycled under a new handle; the capacity bound holds, exhaustion is
// reported as 0 rather than aborting, and filling the table costs the
// supervisor/refuse registry nothing per slot.
static int host_slot_check(void) {
  gk_stats s0, s1, s2, s3, s4, s5, s6;
  gk_get_stats(&s0);
  gk_host_handle h = gk_host_slot_alloc(32);
  if (!h) { printf("host slots: gk_host_slot_alloc failed [FAIL]\n"); return 0; }
  unsigned char *k = gk_host_slot_get(h);
  if (!k) { printf("host slots: a fresh handle resolved to nothing [FAIL]\n"); return 0; }
  gk_get_stats(&s1);
  int zero = 1;
  for (int i = 0; i < 32; i++) if (k[i]) zero = 0;
  for (int i = 0; i < 32; i++) k[i] = (unsigned char)(0xC0 + i);
  unsigned char *again = gk_host_slot_get(h);
  int resolves = gk_host_owns(k) && again == k && again[0] == 0xC0 && again[31] == 0xDF &&
                 s1.host_slots == s0.host_slots + 1 && s1.host_blocks == s0.host_blocks + 1;

  // Ring 3 reaches neither the block (host memory) nor the table (supervisor).
  long r3 = gk_run_user(read_byte, k + 7);
  unsigned long f3 = gk_fault_addr();
  unsigned long table = gk_debug_ctl_addr(GK_CTL_HOST_SLOTS);
  long w3 = gk_run_user(user_write_here, (void *)table);
  unsigned long fw3 = gk_fault_addr();
  int walled = r3 == GK_EFAULT && f3 == (unsigned long)(k + 7) && w3 == GK_EFAULT && fw3 == table;

  // Handles gk did not issue. The PROT_NONE page is the check that resolution
  // reads nothing through the caller's value: were the handle, or its index or
  // generation bits, dereferenced, the test would take SIGSEGV here.
  uint64_t idx = h & 0xffff, gen = h >> 16;
  unsigned char *none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char *heap = malloc(64);
  memset(heap, 0xEE, 64);
  gk_host_handle bad[] = {
      (gen << 16) | GK_HOST_SLOTS,          // index at capacity
      (gen << 16) | 0xffff,                 // index past capacity
      ((gen + 1) << 16) | idx,              // a generation not yet issued, same slot
      ((gen ^ 0x100000) << 16) | idx,       // a generation never issued, same slot
      (gen << 16) | (idx ^ 1),              // a slot never taken, with a live generation
      (gk_host_handle)(uintptr_t)k,         // the block's own address, as a swapped pointer would be
      (gk_host_handle)(uintptr_t)heap,      // a heap buffer the caller controls
      (gk_host_handle)(uintptr_t)none,      // an unmapped page
      (gk_host_handle)(uintptr_t)none | idx,
      ~(gk_host_handle)0, 1, 0x7fff0000,
      ((gen << 16) | idx) ^ ((gk_host_handle)1 << 63)};  // the live handle with its top bit flipped
  int garbage_ok = none != MAP_FAILED;
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    if (gk_host_slot_get(bad[i]) != NULL) garbage_ok = 0;
  gk_host_slot_free(bad[2]);  // refused: frees nothing
  gk_host_slot_free(bad[6]);
  int heap_intact = 1;
  for (int i = 0; i < 64; i++) if (heap[i] != 0xEE) heap_intact = 0;
  free(heap);
  munmap(none, 4096);
  int intact = 1;
  for (int i = 0; i < 32; i++) if (k[i] != (unsigned char)(0xC0 + i)) intact = 0;
  gk_get_stats(&s2);
  garbage_ok = garbage_ok && heap_intact && intact && gk_host_slot_get(h) == k &&
               s2.host_slots == s1.host_slots && s2.host_blocks == s1.host_blocks;

  // Freed: the handle resolves to nothing, the block is scrubbed and returned,
  // and a second free is refused. Recycled: the next allocation of that class
  // takes the slot and the block again, under a new handle; the old handle
  // still resolves to nothing while the new one resolves to the block, zeroed.
  gk_host_slot_free(h);
  gk_get_stats(&s3);
  int freed = gk_host_slot_get(h) == NULL && s3.host_slots == s0.host_slots &&
              s3.host_blocks == s0.host_blocks;
  gk_host_slot_free(h);
  gk_get_stats(&s4);
  freed = freed && s4.host_slots == s3.host_slots && s4.host_blocks == s3.host_blocks;
  gk_host_handle h2 = gk_host_slot_alloc(32);
  unsigned char *k2 = gk_host_slot_get(h2);
  int recycled = h2 != 0 && (h2 & 0xffff) == idx && h2 != h && k2 == k &&
                 gk_host_slot_get(h) == NULL;
  for (int i = 0; i < 32; i++) if (k2 && k2[i]) recycled = 0;
  gk_host_slot_free(h2);

  // Capacity: the table fills to GK_HOST_SLOTS exactly, the next allocation
  // reports 0, every handle names its own block (each block is tagged with its
  // slot's position and read back through the handle), and the registry grew
  // by at most the one region the blocks may have needed, not per slot. Freed
  // in an order that scrambles the free list, the table empties again.
  static gk_host_handle all[GK_HOST_SLOTS + 1];
  gk_get_stats(&s4);
  int n = 0;
  for (; n <= GK_HOST_SLOTS; n++) {
    all[n] = gk_host_slot_alloc(16);
    if (!all[n]) break;
  }
  gk_host_handle over = n <= GK_HOST_SLOTS ? all[n] : gk_host_slot_alloc(16);
  gk_get_stats(&s5);
  int distinct = 1;
  for (int i = 0; i < n; i++) {
    int *p = gk_host_slot_get(all[i]);
    if (!p) { distinct = 0; break; }
    *p = i;
  }
  for (int i = 0; i < n && distinct; i++) {
    int *p = gk_host_slot_get(all[i]);
    if (!p || *p != i) distinct = 0;
  }
  for (int i = 0; i < n; i += 2) gk_host_slot_free(all[i]);
  for (int i = (n - 1) | 1; i >= 1; i -= 2) if (i < n) gk_host_slot_free(all[i]);
  gk_get_stats(&s6);
  int capacity_ok = n == GK_HOST_SLOTS - s4.host_slots && over == 0 &&
                    s5.host_slots == GK_HOST_SLOTS && distinct &&
                    s5.prot_ranges - s4.prot_ranges == s5.host_regions - s4.host_regions &&
                    s5.prot_ranges - s4.prot_ranges <= 1 && s6.host_slots == s4.host_slots &&
                    s6.host_blocks == s4.host_blocks && s6.prot_ranges == s5.prot_ranges &&
                    gk_host_slot_get(all[0]) == NULL && gk_host_slot_get(all[n - 1]) == NULL;
  // The table is reusable after that: an allocation succeeds and the handle
  // is fresh (a generation past every one issued above).
  gk_host_handle h3 = gk_host_slot_alloc(16);
  int fresh = h3 != 0 && (h3 >> 16) > (all[n - 1] >> 16) && gk_host_slot_get(h3) != NULL;
  gk_host_slot_free(h3);
  long cpl = gk_run_user(user_cpl, NULL);  // the process survived the faults above

  int ok = zero && resolves && walled && garbage_ok && freed && recycled && capacity_ok &&
           fresh && cpl == 3;
  printf("host slots: handle %#llx -> block @%p (zero-filled %s, host r/w %s); ring-3 read of "
         "the block -> %s (fault %#lx == %p), ring-3 write to the table -> %s (fault %#lx == "
         "%#lx); %zu garbage handles -> nothing (%s, caller's memory untouched %s, block intact "
         "%s); freed -> nothing %s; recycled as %#llx (same slot %s, same block %s, old handle "
         "-> nothing %s, zeroed %s); capacity %d: filled %d, next -> %#llx, all distinct %s, "
         "registry +%d for +%d region(s), emptied %s; reusable %s; ring 3 at CPL %ld [%s]\n",
         (unsigned long long)h, (void *)k, zero ? "yes" : "NO", resolves ? "ok" : "BROKEN",
         r3 == GK_EFAULT ? "FAULT" : "READ", f3, (void *)(k + 7),
         w3 == GK_EFAULT ? "FAULT" : "WROTE", fw3, table, sizeof bad / sizeof bad[0],
         garbage_ok ? "yes" : "NO", heap_intact ? "yes" : "NO", intact ? "yes" : "NO",
         freed ? "yes" : "NO", (unsigned long long)h2, (h2 & 0xffff) == idx ? "yes" : "no",
         k2 == k ? "yes" : "no", gk_host_slot_get(h) == NULL ? "yes" : "NO",
         recycled ? "yes" : "NO", GK_HOST_SLOTS, n, (unsigned long long)over,
         distinct ? "yes" : "NO", s5.prot_ranges - s4.prot_ranges,
         s5.host_regions - s4.host_regions, s6.host_slots == s4.host_slots ? "yes" : "NO",
         fresh ? "yes" : "NO", cpl, ok ? "OK" : "FAIL");
  return ok;
}

// ---- hostcalls ---------------------------------------------------------------
// A hostcall runs a registered host handler at the guest's request and hands
// the result back before the guest's next instruction. The guest selects the
// handler by an index checked against gk's table (in supervisor memory):
// an unregistered index, an index at or past the table, a garbage index --
// a pointer-shaped 64-bit value in RDI, all ones -- and any index while
// nothing is registered all return -ENOSYS with no handler called. Arguments
// cross through the thread's exchange region, which gk locates from its own
// record and copies into a host-memory frame before the handler runs, so the
// handler's view is exactly what the guest wrote and a length past the frame
// is refused unread; the region's mapping is the guest's to use but not to
// change. Pointer-shaped arguments reach the handler as numbers only. The
// same call works from ring 0 and from the host side, the table is
// unreachable from ring 3, and the round trip is timed.

// The handler's own record of what it was given. In host memory, so the
// guest cannot write it between the call and the check.
struct hc_rec {
  uint64_t args[4];
  uint64_t in_len;
  unsigned char first[16];
};
struct hc_seen {
  long calls;
  struct hc_rec rec[8];  // one per call, in order
};
enum { HC_IDX = 5, HC_IDX2 = GK_HOSTCALLS - 1, HC_UNREG = 6, HC_N = 20000 };

// Records the frame it was given, answers with every byte of buf incremented
// and the sum of the args. Allocates nothing: this runs under the guard build.
static long hc_handler(void *ctx, gk_hostcall_frame *f) {
  struct hc_seen *s = ctx;
  if (s->calls < 8) {
    struct hc_rec *r = &s->rec[s->calls];
    memcpy(r->args, f->args, sizeof r->args);
    r->in_len = f->in_len;
    memcpy(r->first, f->buf, sizeof r->first);
  }
  s->calls++;
  for (uint64_t i = 0; i < f->in_len; i++) f->buf[i] = (unsigned char)(f->buf[i] + 1);
  f->out_len = f->in_len;
  return (long)(f->args[0] + f->args[1] + f->args[2] + f->args[3]);
}

// A second handler, as cheap as one can be, for the timing loop.
static long hc_handler2(void *ctx, gk_hostcall_frame *f) {
  (void)ctx; (void)f;
  return 0x2222;
}

// A raw hostcall with a 64-bit RDI, past the C prototype's unsigned index: the
// values below are what a guest that ignores gk_hostcall can put there.
static long hc_raw(uint64_t rdi) {
  long ret;
#if defined(__x86_64__)
  __asm__ __volatile__("syscall" : "=a"(ret) : "a"(0xF4241L), "D"((long)rdi)
                       : "rcx", "r11", "memory");
#else
  register long x8 __asm__("x8") = 0xF4241L;
  register long x0 __asm__("x0") = (long)rdi;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  ret = x0;
#endif
  return ret;
}

struct hc_probe {
  gk_hostcall_frame *region;
  long r_ok, r_two, r_unreg, r_at, r_past, r_huge, r_ptr, r_max, r_over, r_all;
  uint64_t out_len_ok, out_len_max;
  unsigned char out[16];
  unsigned long none;  // a PROT_NONE page's address, used as every argument
};

// Runs in the guest (ring 3, and once at ring 0).
static long hc_guest(void *arg) {
  struct hc_probe *p = arg;
  gk_hostcall_frame *r = gk_hostcall_region();
  p->region = r;
  if (!r) return -1;
  // A registered handler, with arguments: the handler must see these.
  r->args[0] = 1; r->args[1] = 20; r->args[2] = 300; r->args[3] = 4000;
  r->in_len = 16;
  memcpy(r->buf, "0123456789abcdef", 16);
  p->r_ok = gk_hostcall(HC_IDX);
  p->out_len_ok = r->out_len;
  memcpy(p->out, r->buf, 16);
  p->r_two = gk_hostcall(HC_IDX2);
  // Indices that name nothing.
  p->r_unreg = gk_hostcall(HC_UNREG);
  p->r_at = gk_hostcall(GK_HOSTCALLS);
  p->r_past = gk_hostcall(GK_HOSTCALLS + 1);
  p->r_huge = hc_raw(~0ULL);
  p->r_ptr = hc_raw((uint64_t)(uintptr_t)r);
  // Lengths: the frame's full size is served; one more, and all ones, are
  // refused unread. The arguments and the buffer are the address of a page
  // nothing may touch, so a dereference anywhere would be a crash.
  for (int i = 0; i < 4; i++) r->args[i] = p->none;
  for (size_t i = 0; i + sizeof p->none <= GK_HOSTCALL_BYTES; i += sizeof p->none)
    memcpy(r->buf + i, &p->none, sizeof p->none);
  r->in_len = GK_HOSTCALL_BYTES;
  p->r_max = gk_hostcall(HC_IDX);
  p->out_len_max = r->out_len;
  r->in_len = GK_HOSTCALL_BYTES + 1;
  p->r_over = gk_hostcall(HC_IDX);
  r->in_len = ~0ULL;
  p->r_all = gk_hostcall(HC_IDX);
  return 0;
}

// Runs in the guest: HC_N calls of the cheap handler, for the timing.
static long hc_bench(void *arg) {
  (void)arg;
  long acc = 0;
  gk_hostcall_frame *r = gk_hostcall_region();
  r->in_len = 0;
  for (int i = 0; i < HC_N; i++) acc += gk_hostcall(HC_IDX2);
  return acc;
}
static long hc_empty(void *arg) { (void)arg; return 0; }

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int hostcall_check(void) {
  gk_stats s0, s1, s2;
  gk_get_stats(&s0);
  struct hc_seen *seen = gk_host_alloc(sizeof *seen);
  struct hc_probe *p = calloc(1, sizeof *p);
  unsigned char *none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!seen || !p || none == MAP_FAILED) { printf("hostcalls: setup failed [FAIL]\n"); return 0; }
  p->none = (unsigned long)(uintptr_t)none;

  // Nothing registered: every index fails without a handler to call.
  long n_a = gk_run_user(hc_guest, p);
  int none_ok = n_a == 0 && p->r_ok == -ENOSYS && p->r_two == -ENOSYS && p->r_at == -ENOSYS &&
                p->r_huge == -ENOSYS && p->r_max == -ENOSYS && s0.hostcall_handlers == 0;
  // Registration: bounds, a NULL handler, and a taken index are refused.
  int reg_bad = gk_hostcall_register(GK_HOSTCALLS, hc_handler, seen) == -EINVAL &&
                gk_hostcall_register(HC_IDX, NULL, seen) == -EINVAL;
  int reg = gk_hostcall_register(HC_IDX, hc_handler, seen) == 0 &&
            gk_hostcall_register(HC_IDX2, hc_handler2, NULL) == 0 &&
            gk_hostcall_register(HC_IDX, hc_handler2, NULL) == -EBUSY;
  gk_get_stats(&s1);

  // From ring 3, on a private guest stack and on the caller's stack.
  memset(p, 0, sizeof *p);
  p->none = (unsigned long)(uintptr_t)none;
  long ru = gk_run_user(hc_guest, p);
  int out_ok = 1;
  for (int i = 0; i < 16; i++) if (p->out[i] != (unsigned char)("0123456789abcdef"[i] + 1)) out_ok = 0;
  int call_ok = ru == 0 && p->r_ok == 4321 && p->out_len_ok == 16 && out_ok && p->r_two == 0x2222;
  // The handler ran twice in that run (the first call and the max-length
  // one): the refused indices and lengths added nothing.
  const struct hc_rec *c0 = &seen->rec[0], *c1 = &seen->rec[1];
  int seen_ok = seen->calls == 2 && c0->args[0] == 1 && c0->args[1] == 20 &&
                c0->args[2] == 300 && c0->args[3] == 4000 && c0->in_len == 16 &&
                memcmp(c0->first, "0123456789abcdef", 16) == 0;
  int refused_ok = p->r_unreg == -ENOSYS && p->r_at == -ENOSYS && p->r_past == -ENOSYS &&
                   p->r_huge == -ENOSYS && p->r_ptr == -ENOSYS;
  int len_ok = p->r_max == (long)(4 * p->none) && p->out_len_max == GK_HOSTCALL_BYTES &&
               c1->in_len == GK_HOSTCALL_BYTES && c1->args[0] == p->none &&
               c1->args[3] == p->none && memcmp(c1->first, &p->none, sizeof p->none) == 0 &&
               p->r_over == -E2BIG && p->r_all == -E2BIG;
  long calls_after_user = seen->calls;
  struct hc_probe *q = calloc(1, sizeof *q);
  q->none = p->none;
  long rh = gk_run_here_user(hc_guest, q);
  int here_ok = rh == 0 && q->r_ok == 4321 && q->r_two == 0x2222 && q->r_unreg == -ENOSYS &&
                q->r_huge == -ENOSYS && q->r_max == (long)(4 * q->none) && q->r_over == -E2BIG &&
                q->region == p->region && seen->calls == calls_after_user + 2;
  // From ring 0, and from the host side: the same function, the same table.
  struct hc_probe *k = calloc(1, sizeof *k);
  k->none = p->none;
  long r0 = gk_run(hc_guest, k);
  int ring0_ok = r0 == 0 && k->r_ok == 4321 && k->r_unreg == -ENOSYS && k->r_over == -E2BIG;
  struct hc_probe *h = calloc(1, sizeof *h);
  h->none = p->none;
  long rhost = hc_guest(h);
  int host_ok = rhost == 0 && h->r_ok == 4321 && h->r_two == 0x2222 && h->r_unreg == -ENOSYS &&
                h->r_at == -ENOSYS && h->r_over == -E2BIG && h->region == p->region &&
                h->r_huge == -ENOSYS;  // the raw syscall on the host reaches the kernel
  gk_get_stats(&s2);
  int stats_ok = s1.hostcall_handlers == s0.hostcall_handlers + 2 &&
                 s2.hostcalls - s0.hostcalls == 3 * 4;

  // The table is supervisor memory: a ring-3 write faults. The region is the
  // guest's to use, not to remap: the mapping policy refuses it, with the
  // guest's own page next to it still allowed.
  unsigned long table = gk_debug_ctl_addr(GK_CTL_HOSTCALLS);
  long w3 = gk_run_user(user_write_here, (void *)table);
  unsigned long fw3 = gk_fault_addr();
  unsigned char *own = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int walled = w3 == GK_EFAULT && fw3 == table &&
               !gk_mapping_change_ok((unsigned long)(uintptr_t)p->region, 4096) &&
               !gk_mapping_change_ok((unsigned long)(uintptr_t)p->region + 4096, 1) &&
               gk_mapping_change_ok((unsigned long)(uintptr_t)own, 4096);
  munmap(own, 4096);

  // Round-trip cost: HC_N calls of the cheap handler from ring 3 on the
  // caller's stack (the production path), less the cost of the entry itself.
  double t0 = now_ns();
  long acc = gk_run_here_user(hc_bench, NULL);
  double t1 = now_ns();
  gk_run_here_user(hc_empty, NULL);
  double t2 = now_ns();
  double per_call = ((t1 - t0) - (t2 - t1)) / HC_N;
  int bench_ok = acc == (long)HC_N * 0x2222 && per_call > 0;

  // Unregistered: the index fails again, the other handler still answers, and
  // a second unregister reports nothing bound.
  int unreg = gk_hostcall_unregister(HC_IDX) == 0;
  struct hc_probe *u = calloc(1, sizeof *u);
  u->none = p->none;
  long rund = gk_run_user(hc_guest, u);
  int after_ok = unreg && rund == 0 && u->r_ok == -ENOSYS && u->r_max == -ENOSYS &&
                 u->r_two == 0x2222 && seen->calls == calls_after_user + 6 &&
                 gk_hostcall_unregister(HC_IDX) == -ENOSYS &&
                 gk_hostcall_unregister(GK_HOSTCALLS) == -EINVAL;
  gk_hostcall_unregister(HC_IDX2);
  long cpl = gk_run_user(user_cpl, NULL);

  int ok = none_ok && reg_bad && reg && call_ok && seen_ok && refused_ok && len_ok && here_ok &&
           ring0_ok && host_ok && stats_ok && walled && bench_ok && after_ok && cpl == 3;
  printf("hostcalls: nothing registered -> all ENOSYS %s; register: past table/NULL/taken "
         "refused %s, two bound %s; ring 3: handler %u -> %ld (want 4321), out %s, %u -> %#lx; "
         "handler saw args %llu/%llu/%llu/%llu in_len %llu bytes %s; unregistered/at/past/"
         "all-ones/pointer index -> %ld/%ld/%ld/%ld/%ld (want all %d, handler calls unchanged "
         "%s); in_len %d -> %ld with %llu bytes back, %d -> %ld, ~0 -> %ld (want %d); caller's "
         "stack %s; ring 0 %s; host side %s; %ld calls counted %s; ring-3 write to table -> %s "
         "(fault %#lx == %#lx), region unmappable by the guest %s; %.0f ns per round trip "
         "(%d calls); after unregister -> %ld, other %#lx, re-unregister %s; CPL %ld [%s]\n",
         none_ok ? "yes" : "NO", reg_bad ? "yes" : "NO", reg ? "yes" : "NO", HC_IDX, p->r_ok,
         out_ok ? "ok" : "WRONG", HC_IDX2, p->r_two, (unsigned long long)c0->args[0],
         (unsigned long long)c0->args[1], (unsigned long long)c0->args[2],
         (unsigned long long)c0->args[3], (unsigned long long)c0->in_len,
         seen_ok ? "as passed" : "DIFFER", p->r_unreg, p->r_at, p->r_past, p->r_huge,
         p->r_ptr, -ENOSYS, refused_ok ? "yes" : "NO", GK_HOSTCALL_BYTES, p->r_max,
         (unsigned long long)p->out_len_max, GK_HOSTCALL_BYTES + 1, p->r_over, p->r_all, -E2BIG,
         here_ok ? "ok" : "BROKEN", ring0_ok ? "ok" : "BROKEN", host_ok ? "ok" : "BROKEN",
         s2.hostcalls - s0.hostcalls, stats_ok ? "ok" : "WRONG",
         w3 == GK_EFAULT ? "FAULT" : "WROTE", fw3, table, walled ? "yes" : "NO", per_call, HC_N,
         u->r_ok, u->r_two, after_ok ? "ok" : "BROKEN", cpl, ok ? "OK" : "FAIL");
  free(p); free(q); free(k); free(h); free(u);
  munmap(none, 4096);
  gk_host_free(seen, sizeof *seen);
  return ok;
}

// ---- ring-0 resolve ------------------------------------------------------------
// A ring-0 resolve answers a ring-3 handle lookup against gk's table in
// supervisor memory, inside the guest, with no VM exit (see gk_resolve in
// gk.h). Checks: a registered entry resolves from ring 3 (on both kinds of
// stack), from ring 0 and from the host; a zero handle, an index at and past
// capacity (which decomposes to a generation never issued, since the index
// is masked), generations never issued, a slot never taken, the pointer itself,
// a heap address, a PROT_NONE page's address and all-ones each resolve to
// nothing with nothing read through them; a handle to an unregistered entry
// resolves to nothing, and still does once the slot is recycled under a new
// generation; a ring-3 read and write of the table fault at the table; the
// table fills to GK_RESOLVE_SLOTS exactly, the next registration is refused
// with 0, every entry resolves to its own pointer, and the table empties and
// is reusable; and the round trip is timed in sub-runs around which KVM's
// own per-vCPU exit counter is read, so a kept sub-run is one that took
// exactly the one exit that ends the turn, however many resolves it made.
// What the table will hold is checked for each of the three ways an entry
// comes to be owned: an arena's own memory is admitted for that arena and
// for no other root, and gk's supervisor and refused pages, another live
// arena's memory and NULL are refused.
// The guard build runs the host-side path with allocation forbidden; the
// ring-0 path is asm.

enum { RS_N = 8000, RS_SUBRUNS = 64, RS_BAD = 13 };
#define RS_BENCH_TYPE 0x5e5e5e5e5e5e5e5eULL
#define RS_OWN_TYPE 0x0a0a0a0a0a0a0a0aULL

struct rs_probe {
  gk_resolve_handle h, h2;   // a live handle, and the recycled slot's new one
  gk_resolve_handle bad[RS_BAD];
  void *r_h, *r_h2, *r_zero;
  void *r_bad[RS_BAD];
};

// Runs in the guest (ring 3, and once at ring 0) and on the host: resolves
// every handle in the probe.
static long rs_guest(void *arg) {
  struct rs_probe *p = arg;
  p->r_h = gk_resolve(p->h);
  p->r_h2 = gk_resolve(p->h2);
  p->r_zero = gk_resolve(0);
  for (int i = 0; i < RS_BAD; i++) p->r_bad[i] = gk_resolve(p->bad[i]);
  return 0;
}

// Runs in the guest (ring 3) and on the host: resolves two handles with their
// type words, which the trampoline returns beside the pointer.
struct rs_type { gk_resolve_handle h[2]; void *p[2]; uint64_t t[2]; };
static long rs_resolve_types(void *arg) {
  struct rs_type *r = arg;
  for (int i = 0; i < 2; i++) r->p[i] = gk_resolve_typed(r->h[i], &r->t[i]);
  return 0;
}

// Runs in the guest (ring 3): RS_N resolves of one live handle, counting the
// ones that answered with the registered pointer.
// The resolve as the runtime makes it: the pointer and the entry's type word
// back from the trampoline, both compared (hostMemory::resolveOfType), with
// the type in a stack slot as it is for a caller in another object.
struct rs_bench { gk_resolve_handle h; void *want; uint64_t type; };
static long rs_bench(void *arg) {
  struct rs_bench *b = arg;
  long acc = 0;
  for (int i = 0; i < RS_N; i++) {
    uint64_t type = 0;
    acc += gk_resolve_typed(b->h, &type) == b->want && type == b->type;
  }
  return acc;
}
static long rs_empty(void *arg) { (void)arg; return 0; }

// Runs in the guest (ring 3): resolves every handle of a full table and
// returns how many did not answer with their own pointer.
struct rs_all { gk_resolve_handle *h; void **want; int n; };
static long rs_resolve_all(void *arg) {
  struct rs_all *a = arg;
  long bad = 0;
  for (int i = 0; i < a->n; i++) bad += gk_resolve(a->h[i]) != a->want[i];
  return bad;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static int resolve_check(void) {
  gk_stats s0, s1, s2, s3, s4, s5;
  gk_get_stats(&s0);
  static int obj, obj2;
  int reg_bad = gk_resolve_register(NULL, 0) == 0;
  gk_resolve_handle h = gk_resolve_register(&obj, RS_BENCH_TYPE);
  if (!h) { printf("ring-0 resolve: gk_resolve_register failed [FAIL]\n"); return 0; }
  gk_get_stats(&s1);
  int resolves = gk_resolve(h) == &obj && s1.resolve_entries == s0.resolve_entries + 1;

  // Handles gk did not issue. The PROT_NONE page is the check that resolution
  // reads nothing through the caller's value, at either ring.
  uint64_t idx = h & 0xfffff, gen = h >> 20;  // the handle's split (GK_RESOLVE_INDEX_BITS)
  unsigned char *none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char *heap = malloc(64);
  if (none == MAP_FAILED || !heap) { printf("ring-0 resolve: setup failed [FAIL]\n"); return 0; }
  memset(heap, 0xEE, 64);
  struct rs_probe *p = calloc(1, sizeof *p);
  p->h = h;
  gk_resolve_handle bad[RS_BAD] = {
      ((gen ^ 0x200000) << 20) | GK_RESOLVE_SLOTS | idx,  // the capacity bit set: the mask discards it, the generation names nothing
      ((gen ^ 0x200000) << 20) | 0xfffff,   // the last slot, with a generation never issued
      ((gen + 1) << 20) | idx,              // a generation not yet issued, same slot
      ((gen ^ 0x100000) << 20) | idx,       // a generation never issued, same slot
      (gen << 20) | (idx ^ 1),              // a slot never taken, with a live generation
      (gk_resolve_handle)(uintptr_t)&obj,   // the object's own address, as a swapped pointer would be
      (gk_resolve_handle)(uintptr_t)heap,   // a heap buffer the caller controls
      (gk_resolve_handle)(uintptr_t)none,   // an unmapped page
      (gk_resolve_handle)(uintptr_t)none | idx,
      ~(gk_resolve_handle)0, 1, 0x7fff0000,
      ((gen << 20) | idx) ^ ((gk_resolve_handle)1 << 63)};  // the live handle with its top bit flipped
  memcpy(p->bad, bad, sizeof bad);

  // From ring 3 on a private guest stack, ring 3 on the caller's stack, ring
  // 0, and the host: the live handle resolves, everything else to nothing.
  long ru = gk_run_user(rs_guest, p);
  int user_ok = ru == 0 && p->r_h == &obj && p->r_zero == NULL && p->r_h2 == NULL;
  for (int i = 0; i < RS_BAD; i++) if (p->r_bad[i] != NULL) user_ok = 0;
  memset(p->r_bad, 0x11, sizeof p->r_bad); p->r_h = NULL;
  long rh = gk_run_here_user(rs_guest, p);
  int here_ok = rh == 0 && p->r_h == &obj && p->r_zero == NULL;
  for (int i = 0; i < RS_BAD; i++) if (p->r_bad[i] != NULL) here_ok = 0;
  memset(p->r_bad, 0x11, sizeof p->r_bad); p->r_h = NULL;
  long r0 = gk_run(rs_guest, p);
  int ring0_ok = r0 == 0 && p->r_h == &obj && p->r_zero == NULL;
  for (int i = 0; i < RS_BAD; i++) if (p->r_bad[i] != NULL) ring0_ok = 0;
  memset(p->r_bad, 0x11, sizeof p->r_bad); p->r_h = NULL;
  long rhost = rs_guest(p);
  int host_ok = rhost == 0 && p->r_h == &obj && p->r_zero == NULL;
  for (int i = 0; i < RS_BAD; i++) if (p->r_bad[i] != NULL) host_ok = 0;
  int heap_intact = 1;
  for (int i = 0; i < 64; i++) if (heap[i] != 0xEE) heap_intact = 0;
  gk_get_stats(&s2);
  int garbage_ok = heap_intact && s2.resolve_entries == s1.resolve_entries;

  // The table is supervisor memory: a ring-3 read and a ring-3 write fault
  // at the table.
  unsigned long table = gk_debug_ctl_addr(GK_CTL_RESOLVE);
  long r3 = gk_run_user(read_byte, (void *)table);
  unsigned long fr3 = gk_fault_addr();
  long w3 = gk_run_user(user_write_here, (void *)table);
  unsigned long fw3 = gk_fault_addr();
  int walled = r3 == GK_EFAULT && fr3 == table && w3 == GK_EFAULT && fw3 == table;

  // Round trip and the no-exit proof. Sub-runs of RS_N resolves from ring 3
  // on the caller's stack (the production path), each bracketed by a read of
  // the vCPU's exit counter. The counter moves by the one exit that ends the
  // turn (the exit syscall); a sub-run during which it moved by more took a
  // host interrupt or a demand fault and is discarded, so a kept sub-run
  // provably ran its RS_N resolves with no VM exit. The empty turn's exits
  // and time are measured the same way, and subtracted. Timing is the
  // median over kept sub-runs. A resident vCPU (GK_RUNNER=resident) ends a
  // turn with up to two exits the kernel services without returning to
  // gk, an ioeventfd OUT when the owner has gone to sleep and the HLT after
  // it, and a turn long enough for the owner to sleep takes both where the
  // empty turn takes neither, so a sub-run is allowed that many more; the
  // proof then bounds the resolves' own exits at two per sub-run of RS_N.
  gk_stats rs_st;
  gk_get_stats(&rs_st);
  long e_allow = rs_st.resident_vcpus > 0 ? 2 : 0;
  struct rs_bench b = {h, &obj, RS_BENCH_TYPE};
  gk_run_here_user(rs_bench, &b);  // warm up: pages, the vCPU's TLB
  long e_empty = -1;
  double t_empty = 0;
  int empty_runs = 0;
  for (int i = 0; i < 16; i++) {
    long e0 = gk_debug_vcpu_stat("exits");
    double t0 = now_ns();
    gk_run_here_user(rs_empty, NULL);
    double t1 = now_ns();
    long e1 = gk_debug_vcpu_stat("exits");
    if (e0 < 0 || e1 < 0) break;
    if (e_empty < 0 || e1 - e0 < e_empty) { e_empty = e1 - e0; t_empty = t1 - t0; empty_runs = 1; }
    else if (e1 - e0 == e_empty) { t_empty += t1 - t0; empty_runs++; }
  }
  if (empty_runs) t_empty /= empty_runs;
  static double kept_ns[RS_SUBRUNS];
  int kept = 0, discarded = 0, wrong = 0;
  long extra_exits = 0, irq0 = gk_debug_vcpu_stat("irq_exits");
  for (int i = 0; i < RS_SUBRUNS; i++) {
    long e0 = gk_debug_vcpu_stat("exits");
    double t0 = now_ns();
    long acc = gk_run_here_user(rs_bench, &b);
    double t1 = now_ns();
    long e1 = gk_debug_vcpu_stat("exits");
    if (acc != RS_N) wrong++;
    if (e0 >= 0 && e1 >= 0 && e1 - e0 <= e_empty + e_allow) kept_ns[kept++] = (t1 - t0 - t_empty) / RS_N;
    else { discarded++; extra_exits += (e1 - e0) - e_empty; }
  }
  long irq_delta = gk_debug_vcpu_stat("irq_exits") - irq0;
  qsort(kept_ns, (size_t)kept, sizeof kept_ns[0], cmp_double);
  double per_call = kept ? kept_ns[kept / 2] : -1;
  int bench_ok = e_empty >= 0 && kept >= RS_SUBRUNS / 4 && wrong == 0 && per_call > 0;

  // Unregistered: the handle resolves to nothing from ring 3 and the host; a
  // second unregister and the garbage handles are refused with nothing
  // changed. Recycled: the next registration takes the slot under a new
  // generation; the old handle still resolves to nothing from ring 3 while
  // the new one resolves to the new object.
  int unreg = gk_resolve_unregister(h) == 0;
  gk_get_stats(&s3);
  memset(p->r_bad, 0x11, sizeof p->r_bad); p->r_h = &obj;
  long rf = gk_run_user(rs_guest, p);
  int freed = unreg && rf == 0 && p->r_h == NULL && gk_resolve(h) == NULL &&
              s3.resolve_entries == s0.resolve_entries && gk_resolve_unregister(h) == -EINVAL;
  for (int i = 0; i < RS_BAD; i++) if (gk_resolve_unregister(bad[i]) != -EINVAL) freed = 0;
  gk_get_stats(&s3);
  freed = freed && s3.resolve_entries == s0.resolve_entries;
  gk_resolve_handle h2 = gk_resolve_register(&obj2, 0);
  p->h2 = h2;
  memset(p->r_bad, 0x11, sizeof p->r_bad); p->r_h = &obj; p->r_h2 = NULL;
  long rr = gk_run_user(rs_guest, p);
  int recycled = h2 != 0 && (h2 & 0xfffff) == idx && h2 != h && rr == 0 && p->r_h == NULL &&
                 p->r_h2 == &obj2 && gk_resolve(h) == NULL && gk_resolve(h2) == &obj2;
  gk_resolve_unregister(h2);

  // Capacity: the table fills to GK_RESOLVE_SLOTS exactly, the next
  // registration reports 0, every handle resolves from ring 3 to its own
  // pointer, and, unregistered in an order that scrambles the free list, the
  // table empties and is reusable under a fresh generation.
  static gk_resolve_handle all[GK_RESOLVE_SLOTS + 1];
  static void *want[GK_RESOLVE_SLOTS + 1];
  static char objs[GK_RESOLVE_SLOTS + 1];
  gk_get_stats(&s3);
  int n = 0;
  double t_fill = now_ns();
  for (; n <= GK_RESOLVE_SLOTS; n++) {
    want[n] = &objs[n];
    all[n] = gk_resolve_register(want[n], 0);
    if (!all[n]) break;
  }
  double ns_register = (now_ns() - t_fill) / (n ? n : 1);
  gk_resolve_handle over = n <= GK_RESOLVE_SLOTS ? all[n] : gk_resolve_register(&objs[0], 0);
  gk_get_stats(&s4);
  struct rs_all ra = {all, want, n};
  long mismatched = gk_run_user(rs_resolve_all, &ra);
  double t_drain = now_ns();
  for (int i = 0; i < n; i += 2) gk_resolve_unregister(all[i]);
  for (int i = (n - 1) | 1; i >= 1; i -= 2) if (i < n) gk_resolve_unregister(all[i]);
  double ns_unregister = (now_ns() - t_drain) / (n ? n : 1);
  gk_get_stats(&s5);
  int capacity_ok = n == GK_RESOLVE_SLOTS - s3.resolve_entries && over == 0 &&
                    s4.resolve_entries == GK_RESOLVE_SLOTS && mismatched == 0 &&
                    s5.resolve_entries == s3.resolve_entries && gk_resolve(all[0]) == NULL &&
                    gk_resolve(all[n - 1]) == NULL;
  gk_resolve_handle h3 = gk_resolve_register(&obj, 0);
  int fresh = h3 != 0 && (h3 >> 20) > (all[n - 1] >> 20) && gk_resolve(h3) == &obj;
  gk_resolve_unregister(h3);

  // What register and retire cost, on the base root above and inside an
  // arena here: one fill of the table and one drain of it in an order that
  // scrambles the free list, per operation. An arena's registrations are the
  // ones the runtime makes from a turn, and the ones that go on the arena's
  // chain, so the two numbers bound what the chain adds.
  gk_stats s6;
  gk_arena *carena = gk_arena_create(1UL << 20);
  double ns_register_arena = 0, ns_unregister_arena = 0;
  int cost_ok = carena != NULL;
  if (carena) {
    gk_arena_enter(carena);
    int m = 0;
    double t0 = now_ns();
    for (; m <= GK_RESOLVE_SLOTS; m++) {
      all[m] = gk_resolve_register(&objs[m], 0);
      if (!all[m]) break;
    }
    ns_register_arena = (now_ns() - t0) / (m ? m : 1);
    t0 = now_ns();
    for (int i = 0; i < m; i += 2) gk_resolve_unregister(all[i]);
    for (int i = (m - 1) | 1; i >= 1; i -= 2) if (i < m) gk_resolve_unregister(all[i]);
    ns_unregister_arena = (now_ns() - t0) / (m ? m : 1);
    gk_arena_enter(NULL);
    gk_get_stats(&s6);
    cost_ok = m == n && s6.resolve_entries == s3.resolve_entries;
    gk_arena_destroy(carena);
  }

  // Reservation: reserved entries resolve to nothing from ring 3 and the host until committed,
  // a commit takes once and refuses a committed, retired or never-issued handle, and the table
  // refuses a pointer it will not hold: NULL, gk's supervisor and refused pages, and an arena,
  // from both commit and register.
  static int objs3[3];
  gk_resolve_handle rsv[3] = {0, 0, 0};
  gk_get_stats(&s3);
  int reserved = gk_resolve_reserve(rsv, 3);
  gk_get_stats(&s4);
  struct rs_all rra = {rsv, (void *[]){NULL, NULL, NULL}, 3};
  long rsv_unresolved = gk_run_user(rs_resolve_all, &rra);  // 0: every one answered NULL
  int reserve_ok = reserved == 3 && rsv[0] && rsv[1] && rsv[2] && rsv_unresolved == 0 &&
                   gk_resolve(rsv[0]) == NULL && s4.resolve_entries == s3.resolve_entries + 3;
  unsigned long super_addr = 0, refuse_addr = 0;
  gk_debug_control_addrs(&super_addr, &refuse_addr);
  gk_arena *rarena = gk_arena_create(1UL << 20);
  void *arena_ptr = rarena ? gk_arena_base(rarena) : NULL;
  int commit_ok = gk_resolve_commit(rsv[0], &objs3[0], 0) == 0 && gk_resolve(rsv[0]) == &objs3[0] &&
                  gk_resolve_commit(rsv[0], &objs3[1], 0) == -EINVAL &&   // committed once
                  gk_resolve(rsv[0]) == &objs3[0] &&
                  gk_resolve_commit(rsv[1], NULL, 0) == -EINVAL &&
                  gk_resolve_commit(rsv[1], (void *)super_addr, 0) == -EINVAL &&
                  gk_resolve_commit(rsv[1], (void *)refuse_addr, 0) == -EINVAL &&
                  (!arena_ptr || gk_resolve_commit(rsv[1], arena_ptr, 0) == -EINVAL) &&
                  gk_resolve(rsv[1]) == NULL &&                          // still reserved
                  gk_resolve_commit(h3, &objs3[2], 0) == -EINVAL &&         // retired
                  gk_resolve_commit(bad[3], &objs3[2], 0) == -EINVAL &&     // never issued
                  gk_resolve_register((void *)super_addr, 0) == 0 &&
                  gk_resolve_register((void *)refuse_addr, 0) == 0 &&
                  (!arena_ptr || gk_resolve_register(arena_ptr, 0) == 0) &&
                  gk_resolve_commit(rsv[1], &objs3[1], 0) == 0 && gk_resolve(rsv[1]) == &objs3[1];
  struct rs_all rrb = {rsv, (void *[]){&objs3[0], &objs3[1], NULL}, 3};
  long committed_wrong = gk_run_user(rs_resolve_all, &rrb);  // ring 3 sees the two commits
  for (int i = 0; i < 3; i++) gk_resolve_unregister(rsv[i]);
  gk_get_stats(&s5);
  int reserve_freed = s5.resolve_entries == s3.resolve_entries && gk_resolve(rsv[0]) == NULL &&
                      gk_resolve(rsv[2]) == NULL;

  // What the table holds, and for whom. An entry's pointer must be memory the
  // arena that comes to own the entry can already reach, which includes that
  // arena's own: an isolate's objects are allocated under its lock and so lie
  // inside its arena, and a rule of "outside every arena" would refuse every
  // registration a turn makes. Each of the three ways an entry comes to be
  // owned decides that arena differently and all three are checked here --
  // gk_resolve_register takes the registering thread's active arena,
  // gk_resolve_register_for takes the arena the caller names, and
  // gk_resolve_commit takes the arena of the thread that ran the turn, where
  // the commit runs. SA's own page is admitted by all three and resolves at
  // SA's ring 3 and at no other; another live arena's page, gk's supervisor
  // and refused pages, the resolve table itself and NULL are refused by all
  // three; and SA's page is refused for the base root, which owns an entry
  // registered with no arena active.
  gk_arena *SA = gk_arena_create(1UL << 20), *SB = gk_arena_create(1UL << 20);
  void *sa_ptr = SA ? gk_arena_base(SA) : NULL, *sb_ptr = SB ? gk_arena_base(SB) : NULL;
  gk_resolve_handle hsa = 0, hsa_for = 0, hsa_rsv[1] = {0};
  long own_in_a = -1, own_in_b = -1;
  int own_ok = SA != NULL && SB != NULL && sa_ptr != NULL && sb_ptr != NULL;
  if (own_ok) {
    gk_resolve_reserve(hsa_rsv, 1);
    hsa_for = gk_resolve_register_for(SA, sa_ptr, RS_OWN_TYPE);
    own_ok = hsa_rsv[0] != 0 && hsa_for != 0 && gk_resolve(hsa_for) == sa_ptr &&
             gk_resolve_owned_by(SA, hsa_for) == 1 &&
             // On no arena, where the base root would own the entry, an
             // arena's memory is refused however the registration is made.
             gk_resolve_register(sa_ptr, 0) == 0 &&
             // Named for one arena, a pointer inside another is refused.
             gk_resolve_register_for(SB, sa_ptr, 0) == 0 &&
             gk_resolve_register_for(SA, sb_ptr, 0) == 0 &&
             gk_resolve_register_for(SA, (void *)super_addr, 0) == 0 &&
             gk_resolve_register_for(SA, (void *)refuse_addr, 0) == 0 &&
             gk_resolve_register_for(SA, (void *)table, 0) == 0 &&
             gk_resolve_register_for(SA, NULL, 0) == 0;
    gk_arena_enter(SA);
    hsa = gk_resolve_register(sa_ptr, RS_OWN_TYPE);
    own_ok = own_ok && hsa != 0 && gk_resolve(hsa) == sa_ptr &&
             gk_resolve_owned_by(SA, hsa) == 1 && gk_resolve_owned_by(SB, hsa) == 0 &&
             // From inside SA, everything but SA's own memory is still refused.
             gk_resolve_register(sb_ptr, 0) == 0 &&
             gk_resolve_register((void *)super_addr, 0) == 0 &&
             gk_resolve_register((void *)refuse_addr, 0) == 0 &&
             gk_resolve_register((void *)table, 0) == 0 &&
             gk_resolve_register(NULL, 0) == 0 &&
             // The commit, which runs after the turn and on its thread, is
             // held to the same rule against the same arena.
             gk_resolve_commit(hsa_rsv[0], sb_ptr, 0) == -EINVAL &&
             gk_resolve_commit(hsa_rsv[0], (void *)super_addr, 0) == -EINVAL &&
             gk_resolve_commit(hsa_rsv[0], (void *)refuse_addr, 0) == -EINVAL &&
             gk_resolve_commit(hsa_rsv[0], (void *)table, 0) == -EINVAL &&
             gk_resolve_commit(hsa_rsv[0], NULL, 0) == -EINVAL &&
             gk_resolve(hsa_rsv[0]) == NULL &&  // still reserved
             gk_resolve_commit(hsa_rsv[0], sa_ptr, RS_OWN_TYPE) == 0 &&
             gk_resolve_owned_by(SA, hsa_rsv[0]) == 1;
    {
      gk_resolve_handle three[3] = {hsa, hsa_for, hsa_rsv[0]};
      void *in_a[3] = {sa_ptr, sa_ptr, sa_ptr};
      void *in_b[3] = {NULL, NULL, NULL};
      struct rs_all ra_sa = {three, in_a, 3}, ra_sb = {three, in_b, 3};
      own_in_a = gk_run_here_user(rs_resolve_all, &ra_sa);  // 0: SA's ring 3 sees all three
      gk_arena_enter(SB);
      own_in_b = gk_run_here_user(rs_resolve_all, &ra_sb);  // 0: SB's ring 3 sees none
    }
    gk_arena_enter(NULL);
    own_ok = own_ok && own_in_a == 0 && own_in_b == 0;
    gk_resolve_unregister(hsa);
    gk_resolve_unregister(hsa_for);
    gk_resolve_unregister(hsa_rsv[0]);
  }
  if (SA) gk_arena_destroy(SA);
  if (SB) gk_arena_destroy(SB);

  // The owner: an entry resolves at ring 3 only under the page-table root it
  // was registered under. Two arenas and the base root, one registration
  // made under each, and every combination resolved from ring 3 on the
  // caller's stack -- the production path -- so that each handle answers in
  // its own arena and nothing anywhere else. The host resolves all three
  // from no arena at all: the owner names which turn may resolve the entry,
  // not which code may, and host code serves every context. A turn that
  // rewrites its handle to another isolate's live handle is the ring-3
  // off-diagonal here.
  static int oa, ob, obase;
  gk_arena *OA = gk_arena_create(1UL << 20), *OB = gk_arena_create(1UL << 20);
  gk_resolve_handle ha = 0, hb = 0, hbase = 0;
  long wrong_in_a = -1, wrong_in_b = -1, wrong_on_base = -1;
  int owner_ok = OA != NULL && OB != NULL;
  if (owner_ok) {
    gk_arena_enter(OA);
    ha = gk_resolve_register(&oa, 0);
    gk_arena_enter(OB);
    hb = gk_resolve_register(&ob, 0);
    gk_arena_enter(NULL);
    hbase = gk_resolve_register(&obase, 0);
    gk_resolve_handle three[3] = {ha, hb, hbase};
    void *in_a[3] = {&oa, NULL, NULL};
    void *in_b[3] = {NULL, &ob, NULL};
    void *on_base[3] = {NULL, NULL, &obase};
    struct rs_all ra_a = {three, in_a, 3}, ra_b = {three, in_b, 3},
                  ra_base = {three, on_base, 3};
    gk_arena_enter(OA);
    wrong_in_a = gk_run_here_user(rs_resolve_all, &ra_a);
    gk_arena_enter(OB);
    wrong_in_b = gk_run_here_user(rs_resolve_all, &ra_b);
    gk_arena_enter(NULL);
    wrong_on_base = gk_run_here_user(rs_resolve_all, &ra_base);
    owner_ok = ha != 0 && hb != 0 && hbase != 0 && wrong_in_a == 0 && wrong_in_b == 0 &&
               wrong_on_base == 0 && gk_resolve(ha) == &oa && gk_resolve(hb) == &ob &&
               gk_resolve(hbase) == &obase;
    gk_resolve_unregister(ha);
    gk_resolve_unregister(hb);
    gk_resolve_unregister(hbase);
  }
  if (OA) gk_arena_destroy(OA);
  if (OB) gk_arena_destroy(OB);

  // An entry must not outlive the arena it is tagged with. A destroyed
  // arena's root is the next arena's -- the root is gk_arena_create's first
  // allocation and the page-table allocator's free list is LIFO -- so an
  // entry left behind would carry a tag that is the new isolate's CR3, and
  // its handle would resolve at that isolate's ring 3: the cross-isolate
  // read the tag refuses, with no race in it. One entry registered under
  // arena RA, RA destroyed with the entry deliberately left registered, RB
  // created: RB's root is checked to be RA's recycled page, so the case is
  // the collision and not two arenas that happened to differ, and the stale
  // handles are resolved from RB's ring 3 on the caller's stack and from the
  // host. The destroy reports the entries and retires them, so both answer
  // nothing. Two entries, one from each way an entry comes to belong to an
  // arena: registered with the arena active, as a turn's own is, and
  // registered for the arena by name from a thread on no arena, as the
  // runtime registers a context's entry on the event loop.
  static int oc, oc2;
  gk_stats sd0, sd1;
  gk_arena *RA = gk_arena_create(1UL << 20);
  gk_resolve_handle hstale = 0, hstale_for = 0;
  unsigned long root_a = 0, root_b = 0;
  long stale_in_b = -1;
  int recycle_ok = RA != NULL;
  if (RA) {
    gk_arena_enter(RA);
    hstale = gk_resolve_register(&oc, 0);
    gk_arena_enter(NULL);
    hstale_for = gk_resolve_register_for(RA, &oc2, 0);
    root_a = gk_debug_arena_root(RA);
    gk_get_stats(&sd0);
    gk_arena_destroy(RA);  // the entry is left registered on purpose
    gk_get_stats(&sd1);
    gk_arena *RB = gk_arena_create(1UL << 20);
    root_b = gk_debug_arena_root(RB);
    if (RB) {
      gk_resolve_handle both[2] = {hstale, hstale_for};
      void *none_wanted[2] = {NULL, NULL};
      struct rs_all rs = {both, none_wanted, 2};
      gk_arena_enter(RB);
      stale_in_b = gk_run_here_user(rs_resolve_all, &rs);  // 0: it answered NULL
      gk_arena_enter(NULL);
      gk_arena_destroy(RB);
    }
    recycle_ok = hstale != 0 && hstale_for != 0 && root_a != 0 && root_b == root_a &&
                 stale_in_b == 0 && gk_resolve(hstale) == NULL &&
                 gk_resolve(hstale_for) == NULL && gk_resolve_unregister(hstale) == -EINVAL &&
                 gk_resolve_unregister(hstale_for) == -EINVAL &&
                 sd1.resolve_entries == sd0.resolve_entries - 2 &&
                 sd1.resolve_orphans == sd0.resolve_orphans + 2;
  }

  // The owner check host code makes about a handle a turn supplied. The
  // ring-3 comparison cannot serve here: the host's own lookup answers for
  // every context it serves, so a host path that retires an entry on one
  // isolate's behalf has to name that isolate. One entry under arena XA, one
  // on the base root and one reserved, asked about from both arenas and from
  // no arena, and a retire of XA's entry attempted as XB's.
  static int ox, oxb;
  static int ta, tb;
  enum { RS_TYPE_A = 0xA10000A1, RS_TYPE_B = 0xB20000B2 };
  gk_arena *XA = gk_arena_create(1UL << 20), *XB = gk_arena_create(1UL << 20);
  gk_resolve_handle hx = 0, hxb = 0, xrsv[1] = {0};
  gk_arena *cur_in = NULL, *cur_out = NULL;
  int owned_ok = XA != NULL && XB != NULL;
  if (owned_ok) {
    gk_arena_enter(XA);
    hx = gk_resolve_register(&ox, 0);
    cur_in = gk_arena_current();
    gk_arena_enter(NULL);
    cur_out = gk_arena_current();
    hxb = gk_resolve_register(&oxb, 0);  // the base root's
    gk_resolve_reserve(xrsv, 1);
    owned_ok = hx != 0 && hxb != 0 && xrsv[0] != 0 && cur_in == XA && cur_out == NULL &&
               gk_resolve_owned_by(XA, hx) == 1 &&      // its own arena
               gk_resolve_owned_by(XB, hx) == 0 &&      // another isolate's
               gk_resolve_owned_by(NULL, hx) == 0 &&    // the base root is a root like any other
               gk_resolve_owned_by(NULL, hxb) == 1 && gk_resolve_owned_by(XA, hxb) == 0 &&
               gk_resolve_owned_by(NULL, xrsv[0]) == 0 &&  // reserved: owned by no root yet
               gk_resolve_owned_by(XA, 0) == 0 && gk_resolve_owned_by(XA, bad[3]) == 0 &&
               gk_resolve_unregister_for(XB, hx) == -EINVAL &&   // the cross-isolate retire
               gk_resolve(hx) == &ox &&                          // which retired nothing
               gk_resolve_unregister_for(NULL, hx) == -EINVAL && gk_resolve(hx) == &ox &&
               gk_resolve_unregister_for(XA, hxb) == -EINVAL && gk_resolve(hxb) == &oxb &&
               gk_resolve_unregister_for(XA, hx) == 0 &&         // its owner's retire
               gk_resolve(hx) == NULL && gk_resolve_unregister_for(XA, hx) == -EINVAL;
    gk_resolve_unregister(hxb);
    gk_resolve_unregister(xrsv[0]);
  }
  if (XA) gk_arena_destroy(XA);
  if (XB) gk_arena_destroy(XB);

  // Re-owning. The runtime creates objects for one isolate inside turns of
  // another -- a subrequest builds the callee's IoContext inside the caller's
  // turn -- and an entry registered that way is owned by the wrong arena,
  // with nothing a turn supplies able to say which arena is right. So an
  // entry is disowned, where it resolves for no ring 3 at all, and adopted
  // later by the arena the adopting thread is on, which gk takes from its own
  // record. One entry registered under arena NA, disowned, resolved from NA's
  // ring 3 and from the host, then adopted under NB and resolved from both
  // rings again. The pointer is what keeps one caller from re-owning
  // another's entry, so a wrong pointer, a wrong type and a dead handle are
  // tried too, and must leave the owner where it was.
  static int on, onb;
  gk_arena *NA = gk_arena_create(1UL << 20), *NB = gk_arena_create(1UL << 20);
  gk_resolve_handle hn = 0;
  double ns_reown = 0;
  long disowned_in_a = -1, adopted_in_b = -1, adopted_in_a = -1;
  int reown_ok = NA != NULL && NB != NULL;
  if (reown_ok) {
    gk_arena_enter(NA);
    hn = gk_resolve_register(&on, RS_TYPE_A);
    gk_arena_enter(NULL);
    reown_ok = hn != 0 && gk_resolve_owned_by(NA, hn) == 1 &&
        // A wrong pointer or type is refused, and the entry stays NA's.
        gk_resolve_disown(hn, &onb, RS_TYPE_A) == -EINVAL &&
        gk_resolve_disown(hn, &on, RS_TYPE_B) == -EINVAL &&
        gk_resolve_adopt(hn, &onb, RS_TYPE_A) == -EINVAL &&
        gk_resolve_adopt(0, &on, RS_TYPE_A) == -EINVAL &&
        gk_resolve_adopt(bad[3], &on, RS_TYPE_A) == -EINVAL &&
        gk_resolve_owned_by(NA, hn) == 1 && gk_resolve(hn) == &on &&
        // Disowned: no arena owns it, the host still resolves it.
        gk_resolve_disown(hn, &on, RS_TYPE_A) == 0 && gk_resolve_owned_by(NA, hn) == 0 &&
        gk_resolve_owned_by(NB, hn) == 0 && gk_resolve_owned_by(NULL, hn) == 1 &&
        gk_resolve(hn) == &on;
    {
      gk_resolve_handle one[1] = {hn};
      void *none_wanted[1] = {NULL};
      void *on_wanted[1] = {&on};
      struct rs_all rs = {one, none_wanted, 1};
      gk_arena_enter(NA);
      disowned_in_a = gk_run_here_user(rs_resolve_all, &rs);  // 0: it answered NULL
      gk_arena_enter(NULL);
      gk_arena_enter(NB);
      reown_ok = reown_ok && gk_resolve_adopt(hn, &on, RS_TYPE_A) == 0;
      rs.want = on_wanted;
      adopted_in_b = gk_run_here_user(rs_resolve_all, &rs);  // 0: NB's ring 3 sees it
      gk_arena_enter(NULL);
      gk_arena_enter(NA);
      rs.want = none_wanted;
      adopted_in_a = gk_run_here_user(rs_resolve_all, &rs);  // 0: NA's no longer does
      gk_arena_enter(NULL);
    }
    // What a disown and an adopt cost, which the runtime pays once each per
    // context built inside another isolate's turn: a pair per iteration, the
    // arena entered as the adopting thread has it.
    {
      int m = 0;
      gk_arena_enter(NB);
      double t0 = now_ns();
      for (; m < 20000; m++) {
        if (gk_resolve_disown(hn, &on, RS_TYPE_A) != 0) break;
        if (gk_resolve_adopt(hn, &on, RS_TYPE_A) != 0) break;
      }
      ns_reown = (now_ns() - t0) / (m ? 2 * m : 1);
      gk_arena_enter(NULL);
      reown_ok = reown_ok && m == 20000;
    }
    reown_ok = reown_ok && disowned_in_a == 0 && adopted_in_b == 0 && adopted_in_a == 0 &&
        gk_resolve_owned_by(NB, hn) == 1 && gk_resolve_owned_by(NA, hn) == 0 &&
        gk_resolve_unregister_for(NA, hn) == -EINVAL &&
        gk_resolve_unregister_for(NB, hn) == 0 && gk_resolve(hn) == NULL &&
        gk_resolve_adopt(hn, &on, RS_TYPE_A) == -EINVAL;
  }
  if (NA) gk_arena_destroy(NA);
  if (NB) gk_arena_destroy(NB);

  // The type word. The owner says whose entry it is and nothing about what
  // it is, so an entry carries a word the registrant chooses and the lookup
  // hands back beside the pointer, for the caller to compare against what it
  // expected (see the type word in gk.h). Two entries of different types, on
  // the base root, resolved from the host and from ring 3: each answers with
  // its own word, a retired handle and a reserved one answer 0, and a commit
  // sets the word the committer gives.
  gk_resolve_handle hta = gk_resolve_register(&ta, RS_TYPE_A);
  gk_resolve_handle htb = gk_resolve_register(&tb, RS_TYPE_B);
  gk_resolve_handle trsv[1] = {0};
  gk_resolve_reserve(trsv, 1);
  uint64_t t1 = ~0ULL, t2 = ~0ULL, t3 = ~0ULL, t4 = ~0ULL;
  int type_ok = hta != 0 && htb != 0 && trsv[0] != 0 &&
                gk_resolve_typed(hta, &t1) == &ta && t1 == RS_TYPE_A &&
                gk_resolve_typed(htb, &t2) == &tb && t2 == RS_TYPE_B &&
                gk_resolve_typed(trsv[0], &t3) == NULL && t3 == 0 &&  // reserved: no type yet
                gk_resolve_commit(trsv[0], &objs3[0], RS_TYPE_B) == 0 &&
                gk_resolve_typed(trsv[0], &t3) == &objs3[0] && t3 == RS_TYPE_B;
  // From ring 3, where the trampoline returns the word in RDX.
  static struct rs_type guest_types;
  guest_types.h[0] = hta;
  guest_types.h[1] = htb;
  gk_arena_enter(NULL);
  gk_run_here_user(rs_resolve_types, &guest_types);
  type_ok = type_ok && guest_types.p[0] == &ta && guest_types.t[0] == RS_TYPE_A &&
            guest_types.p[1] == &tb && guest_types.t[1] == RS_TYPE_B;
  // Retired: the handle resolves to nothing and carries no type.
  gk_resolve_unregister(hta);
  type_ok = type_ok && gk_resolve_typed(hta, &t4) == NULL && t4 == 0;
  gk_resolve_unregister(htb);
  gk_resolve_unregister(trsv[0]);

  if (rarena) gk_arena_destroy(rarena);
  long cpl = gk_run_user(user_cpl, NULL);  // the process survived the faults above

  int ok = reg_bad && resolves && user_ok && here_ok && ring0_ok && host_ok && garbage_ok &&
           walled && bench_ok && freed && recycled && capacity_ok && fresh && reserve_ok &&
           commit_ok && committed_wrong == 0 && reserve_freed && own_ok && owner_ok &&
           recycle_ok && owned_ok && reown_ok && type_ok && cost_ok && cpl == 3;
  printf("ring-0 resolve: NULL refused %s; handle %#llx -> %p (host %s); ring 3 %s, caller's "
         "stack %s, ring 0 %s, host %s; %d garbage handles and 0 -> nothing at every ring (%s, "
         "caller's memory untouched %s); ring-3 read of the table -> %s (fault %#lx == %#lx), "
         "write -> %s (fault %#lx == %#lx); %d sub-runs of %d ring-3 resolves: %d kept with "
         "at most the %ld exit(s) of an empty turn plus %ld allowed, %d discarded (%ld extra exits, %ld of all "
         "irq_exits in the period), %d answered wrong; %.1f ns per resolve (median of kept, "
         "empty turn %.0f ns subtracted); unregistered -> nothing %s; recycled as %#llx (same "
         "slot %s, old handle -> nothing %s, new -> %p %s); capacity %d: filled %d, next -> "
         "%#llx, %ld mismatched from ring 3, emptied %s; reusable %s; reserved 3 -> nothing "
         "until committed %s, commits once and refuses NULL, supervisor, refused and arena "
         "pointers %s (%ld wrong from ring 3), released %s; an arena's own page is "
         "registered under it, registered for it by name and committed into a reservation "
         "on its thread, and resolves %ld times wrongly from its ring 3 and %ld from "
         "another arena's, while another arena's page, gk's supervisor, refused and table "
         "pages and NULL are refused by all three and its own page is refused for the base "
         "root %s; three entries registered under "
         "arena A, arena B and the base root: %ld, %ld and %ld of the three resolved wrongly "
         "from ring 3 under A, under B and on the base root, all three resolve host-side %s; "
         "two entries left registered on a destroyed arena, one made under it and one "
         "made for it by name, are retired by the destroy (root %#lx recycled into the next "
         "arena %s, stale handles resolved %ld times from its ring 3, host-side %s) %s; %.0f ns per register and %.0f ns per retire on the base root, "
         "%.0f and %.0f in an arena %s; an entry is owned by its own arena and by no other, "
         "and a retire for another arena, for the base root or of a base-root entry is refused "
         "%s; an entry of one arena, disowned, resolves at no ring 3 and still resolves "
         "host-side, and adopted under another arena resolves at that arena's ring 3 and not "
         "the first's, while a wrong pointer, type or handle re-owns nothing %s (%.0f ns per "
         "re-owning); an entry's "
         "type word comes back with the pointer, host-side and at ring 3, and "
         "a reserved, committed or retired entry carries what it should %s; CPL %ld [%s]\n",
         reg_bad ? "yes" : "NO", (unsigned long long)h, (void *)&obj, resolves ? "ok" : "BROKEN",
         user_ok ? "ok" : "BROKEN", here_ok ? "ok" : "BROKEN", ring0_ok ? "ok" : "BROKEN",
         host_ok ? "ok" : "BROKEN", RS_BAD, garbage_ok && user_ok && host_ok ? "yes" : "NO",
         heap_intact ? "yes" : "NO", r3 == GK_EFAULT ? "FAULT" : "READ", fr3, table,
         w3 == GK_EFAULT ? "FAULT" : "WROTE", fw3, table, RS_SUBRUNS, RS_N, kept, e_empty,
         e_allow, discarded, extra_exits, irq_delta, wrong, per_call, t_empty, freed ? "yes" : "NO",
         (unsigned long long)h2, (h2 & 0xfffff) == idx ? "yes" : "no",
         p->r_h == NULL ? "yes" : "NO", p->r_h2, p->r_h2 == &obj2 ? "ok" : "WRONG",
         GK_RESOLVE_SLOTS, n, (unsigned long long)over, mismatched,
         s5.resolve_entries == s3.resolve_entries ? "yes" : "NO", fresh ? "yes" : "NO",
         reserve_ok ? "yes" : "NO", commit_ok ? "yes" : "NO", committed_wrong,
         reserve_freed ? "yes" : "NO", own_in_a, own_in_b, own_ok ? "yes" : "NO",
         wrong_in_a, wrong_in_b, wrong_on_base,
         owner_ok ? "yes" : "NO", root_a, root_b == root_a ? "yes" : "NO", stale_in_b,
         gk_resolve(hstale) == NULL && gk_resolve(hstale_for) == NULL ? "nothing" : "AN OBJECT",
         recycle_ok ? "yes" : "NO",
         ns_register, ns_unregister, ns_register_arena,
         ns_unregister_arena, cost_ok ? "ok" : "BROKEN", owned_ok ? "yes" : "NO",
         reown_ok ? "yes" : "NO", ns_reown, type_ok ? "yes" : "NO", cpl,
         ok ? "OK" : "FAIL");
  free(p);
  free(heap);
  munmap(none, 4096);
  return ok;
}

// ---- signal fields -----------------------------------------------------------
// A signal field is a 4-byte word in an arena that the host writes from a
// signal handler, through a descriptor the host validated and keeps in
// supervisor memory (see gk_signal_field_register in gk.h). Checks: a field a
// ring-3 turn registers through a hostcall handler is found and written by
// gk_signal_field_write, including from a real signal handler that interrupts
// the turn; a buffer outside the arena, a field past the buffer, a length
// that overflows, and a bad kind are each refused host-side with nothing
// recorded; an unregister from another arena, from no arena, or with a
// garbage handle is refused; destroying an arena drops its fields; the table
// is unreachable from ring 3; the write path survives a CPU-timer signal
// landing inside registrations and unregistrations; the write is timed and,
// under the guard build, shown to allocate nothing.

enum { SF_IDX = 9, SF_REGISTER = 1, SF_UNREGISTER = 2, SF_KIND = 2, SF_N = 200000, SF_STRESS = 20000 };

// The hostcall handler a runtime would register: validates nothing itself,
// since gk validates against the thread's arena. Allocates nothing.
static long sf_handler(void *ctx, gk_hostcall_frame *f) {
  (void)ctx;
  if (f->args[0] == SF_REGISTER) {
    gk_signal_handle h = 0;
    int r = gk_signal_field_register((uintptr_t)f->args[1], (size_t)f->args[2],
                                     (uint32_t)f->args[3], (unsigned)(f->args[3] >> 32), &h);
    memcpy(f->buf, &h, sizeof h);
    f->out_len = sizeof h;
    return r;
  }
  if (f->args[0] == SF_UNREGISTER) return gk_signal_field_unregister(f->args[1]);
  return -EINVAL;
}

struct sf_call { uint64_t offset, len, field, kind; long r; gk_signal_handle h; };

// Runs in the guest (ring 3): one registration hostcall per entry of `arg`.
static long sf_guest_register(void *arg) {
  struct sf_call *c = arg;
  gk_hostcall_frame *r = gk_hostcall_region();
  if (!r) return -1;
  for (; c->kind != ~0ULL; c++) {
    r->args[0] = SF_REGISTER; r->args[1] = c->offset; r->args[2] = c->len;
    r->args[3] = c->field | (c->kind << 32);
    r->in_len = 0;
    c->r = gk_hostcall(SF_IDX);
    c->h = 0;
    if (r->out_len == sizeof c->h) memcpy(&c->h, r->buf, sizeof c->h);
  }
  return 0;
}

static long sf_guest_unregister(void *arg) {
  gk_hostcall_frame *r = gk_hostcall_region();
  r->args[0] = SF_UNREGISTER; r->args[1] = *(gk_signal_handle *)arg; r->in_len = 0;
  return gk_hostcall(SF_IDX);
}

static volatile long sf_signals;  // handler runs, and the total of fields written
static volatile long sf_written;
static void sf_sigaction(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si; (void)uc;
  long n = gk_signal_field_write(SF_KIND, 24);
  sf_signals++;
  if (n > 0) sf_written += n;
}

// Runs in the guest (ring 3): raises the signal at itself (a forwarded
// tgkill: the handler runs host-side when the syscall returns) and then reads
// the field, which the handler must have written by then. Returns what it
// read.
struct sf_raise { volatile uint32_t *field; long before, after; };
static long sf_guest_raise(void *arg) {
  struct sf_raise *p = arg;
  p->before = *p->field;
  syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1);
  p->after = *p->field;
  return p->after;
}

// Runs in the guest (ring 3) under a CPU-time timer whose handler writes the
// fields: per round, registers two fields through the hostcall, zeroes them,
// reads them for a while and unregisters them. A field reads 0 or the value
// the handler writes, never anything else; every hostcall succeeds. Returns
// the number of rounds that saw otherwise.
struct sf_stress { unsigned char *buf; uint64_t offset, len; long rounds; };
static long sf_guest_stress(void *arg) {
  struct sf_stress *p = arg;
  gk_hostcall_frame *r = gk_hostcall_region();
  volatile uint32_t *w = (volatile uint32_t *)p->buf;
  long bad = 0;
  for (long i = 0; i < p->rounds; i++) {
    gk_signal_handle h[2];
    for (int k = 0; k < 2; k++) {
      r->args[0] = SF_REGISTER; r->args[1] = p->offset; r->args[2] = p->len;
      r->args[3] = (uint64_t)(4 * k) | ((uint64_t)SF_KIND << 32);
      r->in_len = 0;
      if (gk_hostcall(SF_IDX) != 0 || r->out_len != sizeof h[k]) { bad++; return bad; }
      memcpy(&h[k], r->buf, sizeof h[k]);
    }
    w[0] = 0; w[1] = 0;
    for (int n = 0; n < 64; n++) {
      uint32_t a = w[0], b = w[1];
      if ((a != 0 && a != 24) || (b != 0 && b != 24)) bad++;
    }
    for (int k = 1; k >= 0; k--) {
      r->args[0] = SF_UNREGISTER; r->args[1] = h[k]; r->in_len = 0;
      if (gk_hostcall(SF_IDX) != 0) bad++;
    }
  }
  return bad;
}

static int signal_field_check(void) {
  gk_stats s0, s1, s2, s3, s4;
  gk_get_stats(&s0);
  gk_arena *a = gk_arena_create(1UL << 20), *b = gk_arena_create(4096);
  if (!a || !b || gk_hostcall_register(SF_IDX, sf_handler, NULL) != 0) {
    printf("signal fields: setup failed [FAIL]\n");
    return 0;
  }
  unsigned char *base = gk_arena_base(a);
  size_t span = ((size_t)gk_arena_size(a) + (1UL << 39) - 1) & ~((1UL << 39) - 1);  // the reservation
  unsigned char *buf = base + 3 * 4096;  // a 4 KiB buffer in the arena
  enum { BUF = 4096, FIELD = 100 };      // the field at an unaligned offset
  if (mprotect(buf, BUF, PROT_READ | PROT_WRITE) != 0) { printf("signal fields: mprotect [FAIL]\n"); return 0; }
  memset(buf, 0, BUF);
  struct sigaction sa = {0}, old;
  sa.sa_sigaction = sf_sigaction;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGUSR1, &sa, &old);

  // 1. Registered from ring 3 through the handler, for the thread's arena,
  //    then written from the host: the word in the arena carries the value,
  //    the count is one, and a second kind writes nothing.
  gk_arena_enter(a);
  struct sf_call *reg = calloc(2, sizeof *reg);
  reg[0] = (struct sf_call){(uint64_t)(buf - base), BUF, FIELD, SF_KIND, 0, 0};
  reg[1].kind = ~0ULL;
  long rr = gk_run_here_user(sf_guest_register, reg);
  gk_get_stats(&s1);
  uint32_t word = 0;
  memcpy(&word, buf + FIELD, 4);
  long w1 = gk_signal_field_write(SF_KIND, 24);
  uint32_t w1v = 0;
  memcpy(&w1v, buf + FIELD, 4);
  long w2 = gk_signal_field_write(SF_KIND + 1, 99);
  uint32_t w2v = 0;
  memcpy(&w2v, buf + FIELD, 4);
  int untouched = 1;
  for (int i = 0; i < BUF; i++) if (i < FIELD || i >= FIELD + 4) if (buf[i]) untouched = 0;
  int reg_ok = rr == 0 && reg[0].r == 0 && reg[0].h != 0 && s1.signal_fields == s0.signal_fields + 1 &&
               word == 0 && w1 == 1 && w1v == 24 && w2 == 0 && w2v == 24 && untouched;

  // 2. From a real signal handler interrupting the ring-3 turn: the guest
  //    raises SIGUSR1 at itself and reads the field on return.
  uint32_t zero = 0;
  memcpy(buf + FIELD, &zero, 4);
  sf_signals = sf_written = 0;
  struct sf_raise *rs = calloc(1, sizeof *rs);
  rs->field = (volatile uint32_t *)(buf + FIELD);
  long raised = gk_run_here_user(sf_guest_raise, rs);
  long raise_signals = sf_signals, raise_written = sf_written;
  int sig_ok = raised == 24 && rs->before == 0 && rs->after == 24 && raise_signals == 1 &&
               raise_written == 1;

  // 3. Refused host-side, with nothing recorded: a buffer past the arena's
  //    reservation, one that reaches past its end, a field past the buffer,
  //    a field whose end overflows the buffer, a length that wraps the
  //    address space, a buffer too short for a field, a kind at the limit.
  struct sf_call *bad = calloc(8, sizeof *bad);
  bad[0] = (struct sf_call){span, BUF, 0, SF_KIND, 0, 0};                    // outside the arena
  bad[1] = (struct sf_call){span - 4096, 8192, 0, SF_KIND, 0, 0};            // past its end
  bad[2] = (struct sf_call){(uint64_t)(buf - base), BUF, BUF, SF_KIND, 0, 0};  // field at the end
  bad[3] = (struct sf_call){(uint64_t)(buf - base), BUF, BUF - 3, SF_KIND, 0, 0};  // 3 bytes left
  bad[4] = (struct sf_call){(uint64_t)(buf - base), ~0ULL, 0, SF_KIND, 0, 0};  // length wraps
  bad[5] = (struct sf_call){(uint64_t)(buf - base), 3, 0, SF_KIND, 0, 0};      // too short
  bad[6] = (struct sf_call){(uint64_t)(buf - base), BUF, 0, GK_SIGNAL_KINDS, 0, 0};  // kind
  bad[7].kind = ~0ULL;
  long rb = gk_run_here_user(sf_guest_register, bad);
  gk_get_stats(&s2);
  int refused = rb == 0 && s2.signal_fields == s1.signal_fields;
  for (int i = 0; i < 7; i++) if (bad[i].r != -EINVAL || bad[i].h != 0) refused = 0;

  // 4. Unregister: refused from another arena (b) and from no arena, and for
  //    handles gk did not issue; accepted from the owner, after which the
  //    write finds nothing and a second unregister is refused.
  gk_signal_handle h = reg[0].h;
  gk_arena_enter(b);
  long u_other = gk_run_here_user(sf_guest_unregister, &h);
  gk_arena_leave();
  gk_arena_enter(NULL);
  long u_none = gk_signal_field_unregister(h);
  gk_arena_enter(a);
  gk_signal_handle garbage[] = {h ^ 1, h + (1ULL << 16), (gk_signal_handle)(uintptr_t)buf, ~0ULL, 0,
                                (h & 0xffff) | ((uint64_t)GK_SIGNAL_FIELDS << 16)};
  int u_garbage = 1;
  for (size_t i = 0; i < sizeof garbage / sizeof garbage[0]; i++)
    if (gk_run_here_user(sf_guest_unregister, &garbage[i]) != -EINVAL) u_garbage = 0;
  gk_get_stats(&s3);
  memcpy(buf + FIELD, &zero, 4);
  long u_own = gk_run_here_user(sf_guest_unregister, &h);
  long w3 = gk_signal_field_write(SF_KIND, 24);
  uint32_t w3v = 0;
  memcpy(&w3v, buf + FIELD, 4);
  long u_again = gk_run_here_user(sf_guest_unregister, &h);
  gk_get_stats(&s4);
  int unreg_ok = u_other == -EINVAL && u_none == -ESRCH && u_garbage &&
                 s3.signal_fields == s1.signal_fields && u_own == 0 && w3 == 0 && w3v == 0 &&
                 u_again == -EINVAL && s4.signal_fields == s0.signal_fields;

  // 5. No active arena, bad kind: nothing written, an error returned.
  gk_arena_enter(NULL);
  long w_none = gk_signal_field_write(SF_KIND, 24);
  gk_arena_enter(a);
  long w_kind = gk_signal_field_write(GK_SIGNAL_KINDS, 24);
  int errs_ok = w_none == -ESRCH && w_kind == -EINVAL;

  // 6. The table is supervisor memory: a ring-3 read and write both fault.
  unsigned long table = gk_debug_ctl_addr(GK_CTL_SIGNAL_FIELDS);
  long r3 = gk_run_user(read_byte, (void *)table);
  unsigned long fr3 = gk_fault_addr();
  long w3t = gk_run_user(user_write_here, (void *)table);
  unsigned long fw3 = gk_fault_addr();
  int walled = r3 == GK_EFAULT && fr3 == table && w3t == GK_EFAULT && fw3 == table;

  // 7. Capacity: the table fills to GK_SIGNAL_FIELDS, the next is refused
  //    with -ENOMEM, every handle unregisters, and the count returns.
  static struct sf_call all[GK_SIGNAL_FIELDS + 2];
  int n = GK_SIGNAL_FIELDS - s4.signal_fields + 1;
  for (int i = 0; i < n; i++) all[i] = (struct sf_call){(uint64_t)(buf - base), BUF, 0, SF_KIND, 0, 0};
  all[n].kind = ~0ULL;
  gk_run_here_user(sf_guest_register, all);
  gk_stats s5;
  gk_get_stats(&s5);
  long wall = gk_signal_field_write(SF_KIND, 7);
  int cap_ok = s5.signal_fields == GK_SIGNAL_FIELDS && all[n - 1].r == -ENOMEM && all[n - 1].h == 0 &&
               wall == n - 1;
  for (int i = 0; i < n - 1; i++) {
    if (all[i].r != 0 || all[i].h == 0) cap_ok = 0;
    long ur = gk_run_here_user(sf_guest_unregister, &all[i].h);
    if (ur != 0) {
      if (cap_ok) {
        unsigned long fa = gk_fault_addr();
        fprintf(stderr, "signal fields: unregister of handle %d (%#llx) from ring 3 -> %ld (fault %#lx; "
                "region %p; exit tramp %p, syscall tramp %p, stack %p)\n",
                i, (unsigned long long)all[i].h, ur, fa, (void *)gk_hostcall_region(),
                (void *)&gk_user_exit_tramp, (void *)&GK_SYSCALL_ENTRY, (void *)&i);
        FILE *m = fopen("/proc/self/maps", "r");
        char line[512];
        while (m && fgets(line, sizeof line, m)) {
          unsigned long lo, hi;
          if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && fa >= lo - 0x1000000 && fa < hi + 0x1000000)
            fprintf(stderr, "  maps near the fault: %s", line);
        }
        if (m) fclose(m);
      }
      cap_ok = 0;
    }
  }
  gk_stats s6;
  gk_get_stats(&s6);
  cap_ok = cap_ok && s6.signal_fields == s4.signal_fields;

  // 8. A destroyed arena takes its fields with it.
  struct sf_call *db = calloc(2, sizeof *db);
  db[0] = (struct sf_call){(uint64_t)(buf - base), BUF, 8, SF_KIND, 0, 0};
  db[1].kind = ~0ULL;
  gk_run_here_user(sf_guest_register, db);
  gk_stats s7;
  gk_get_stats(&s7);
  gk_arena_enter(NULL);
  gk_arena_destroy(a);
  gk_stats s8;
  gk_get_stats(&s8);
  int destroy_ok = db[0].r == 0 && s7.signal_fields == s6.signal_fields + 1 &&
                   s8.signal_fields == s6.signal_fields;

  // 9. Cost, and a signal landing inside the mutations. A fresh arena with
  //    eight fields registered: SF_N writes from the host, timed. Then a
  //    CPU-time timer fires SIGUSR1 at this thread every 100 us while the
  //    thread registers two fields, writes them and unregisters them,
  //    SF_STRESS times: the handler's write runs inside gk's lock at random
  //    points of those mutations and must find a consistent list each time.
  a = gk_arena_create(1UL << 20);
  base = gk_arena_base(a);
  buf = base + 4096;
  mprotect(buf, BUF, PROT_READ | PROT_WRITE);
  memset(buf, 0, BUF);
  gk_arena_enter(a);
  struct sf_call *eight = calloc(9, sizeof *eight);
  for (int i = 0; i < 8; i++) eight[i] = (struct sf_call){(uint64_t)(buf - base), BUF, 16 * i, SF_KIND, 0, 0};
  eight[8].kind = ~0ULL;
  gk_run_here_user(sf_guest_register, eight);
  double t0 = now_ns();
  long acc = 0;
  for (int i = 0; i < SF_N; i++) acc += gk_signal_field_write(SF_KIND, (uint32_t)i);
  double t1 = now_ns();
  double per_write = (t1 - t0) / SF_N;
  for (int i = 0; i < 8; i++) gk_run_here_user(sf_guest_unregister, &eight[i].h);
  gk_signal_handle one;
  gk_signal_field_register((uintptr_t)(buf - base), BUF, 0, SF_KIND, &one);
  t0 = now_ns();
  for (int i = 0; i < SF_N; i++) acc += gk_signal_field_write(SF_KIND, (uint32_t)i);
  t1 = now_ns();
  double per_write1 = (t1 - t0) / SF_N;
  gk_signal_field_unregister(one);
  int bench_ok = acc == 8L * SF_N + SF_N;

  timer_t timer;
  struct sigevent sev = {0};
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGUSR1;
  sev._sigev_un._tid = (int)syscall(SYS_gettid);
  struct itimerspec its = {{0, 100000}, {0, 100000}};
  sf_signals = sf_written = 0;
  int timer_ok = timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer) == 0 &&
                 timer_settime(timer, 0, &its, NULL) == 0;
  int stress_ok = timer_ok;
  struct sf_stress *st = calloc(1, sizeof *st);
  st->buf = buf; st->offset = (uint64_t)(buf - base); st->len = BUF; st->rounds = SF_STRESS;
  long guest_bad = gk_run_here_user(sf_guest_stress, st);
  long guest_signals = sf_signals;
  if (guest_bad != 0) stress_ok = 0;
  sf_signals = 0;
  for (int i = 0; i < SF_STRESS && stress_ok; i++) {
    gk_signal_handle h1 = 0, h2 = 0;
    if (gk_signal_field_register((uintptr_t)(buf - base), BUF, 0, SF_KIND, &h1) != 0) stress_ok = 0;
    if (gk_signal_field_register((uintptr_t)(buf - base), BUF, 4, SF_KIND, &h2) != 0) stress_ok = 0;
    uint32_t v = 0;
    memcpy(buf, &v, 4); memcpy(buf + 4, &v, 4);
    long k = gk_signal_field_write(SF_KIND, 24);
    uint32_t v1, v2;
    memcpy(&v1, buf, 4); memcpy(&v2, buf + 4, 4);
    if (k != 2 || v1 != 24 || v2 != 24) stress_ok = 0;
    if (gk_signal_field_unregister(h2) != 0 || gk_signal_field_unregister(h1) != 0) stress_ok = 0;
  }
  if (timer_ok) timer_delete(timer);
  long stress_signals = sf_signals;
  gk_stats s9;
  gk_get_stats(&s9);
  stress_ok = stress_ok && guest_signals > 0 && stress_signals > 0 &&
              s9.signal_fields == s6.signal_fields;
  gk_arena_enter(NULL);
  gk_arena_destroy(a);
  gk_arena_destroy(b);
  gk_hostcall_unregister(SF_IDX);
  sigaction(SIGUSR1, &old, NULL);
  long cpl = gk_run_user(user_cpl, NULL);

  int ok = reg_ok && sig_ok && refused && unreg_ok && errs_ok && walled && cap_ok && destroy_ok &&
           bench_ok && stress_ok && cpl == 3;
  printf("signal fields: registered from ring 3 -> handle %#llx (field at %p, arena offset %#lx+%d), "
         "host write -> %ld field(s), word %u (want 24), other kind -> %ld, rest of buffer intact "
         "%s [%s]; from a signal handler interrupting the turn: before %ld, after %ld, %ld "
         "signal(s) wrote %ld [%s]; refused host-side: outside arena %ld, past its end %ld, field "
         "at end %ld, 3 bytes left %ld, length wraps %ld, too short %ld, kind %ld (want all %d; "
         "count unchanged %s) [%s]; unregister from other arena %ld, from no arena %ld, %zu "
         "garbage handles refused %s, by owner %ld then write -> %ld word %u, again %ld [%s]; "
         "write with no arena %ld, bad kind %ld [%s]; ring-3 read of the table -> %s (fault "
         "%#lx == %#lx), write -> %s (fault %#lx) [%s]; capacity %d: filled, next -> %ld, write "
         "hit %ld, emptied %s [%s]; destroyed arena dropped its field %s [%s]; %.0f ns per write "
         "with 8 fields, %.0f with 1 (%d writes each) [%s]; under a 100 us CPU timer, %d "
         "register/read/unregister rounds from ring 3 through the hostcall: %ld signals landed, "
         "%ld inconsistent reads, and %d rounds from the host: %ld signals landed, consistent %s "
         "[%s]; CPL %ld [%s]\n",
         (unsigned long long)reg[0].h, (void *)(buf + FIELD), (unsigned long)(buf - base), FIELD,
         w1, w1v, w2, untouched ? "yes" : "NO", reg_ok ? "ok" : "FAIL", rs->before, rs->after,
         raise_signals, raise_written, sig_ok ? "ok" : "FAIL", bad[0].r, bad[1].r, bad[2].r,
         bad[3].r, bad[4].r, bad[5].r, bad[6].r, -EINVAL,
         s2.signal_fields == s1.signal_fields ? "yes" : "NO", refused ? "ok" : "FAIL", u_other,
         u_none, sizeof garbage / sizeof garbage[0], u_garbage ? "yes" : "NO", u_own, w3, w3v,
         u_again, unreg_ok ? "ok" : "FAIL", w_none, w_kind, errs_ok ? "ok" : "FAIL",
         r3 == GK_EFAULT ? "FAULT" : "READ", fr3, table, w3t == GK_EFAULT ? "FAULT" : "WROTE", fw3,
         walled ? "ok" : "FAIL", GK_SIGNAL_FIELDS, all[n - 1].r, wall,
         s6.signal_fields == s4.signal_fields ? "yes" : "NO", cap_ok ? "ok" : "FAIL",
         s8.signal_fields == s6.signal_fields ? "yes" : "NO", destroy_ok ? "ok" : "FAIL",
         per_write, per_write1, SF_N, bench_ok ? "ok" : "FAIL", SF_STRESS, guest_signals,
         guest_bad, SF_STRESS, stress_signals, stress_ok ? "yes" : "NO", stress_ok ? "ok" : "FAIL",
         cpl, ok ? "OK" : "FAIL");
  free(reg); free(rs); free(bad); free(db); free(eight); free(st);
  return ok;
}

#if defined(__x86_64__)
// ---- AVX-512 state in the guest ----------------------------------------------
// The guest CPUID is the host's, so glibc's ifunc resolvers and any JIT pick
// AVX-512 code paths on a CPU that has them; the guest's XCR0 must then have
// the opmask, ZMM_Hi256 and Hi16_ZMM components enabled, or the first such
// instruction raises #UD, which gk reports as GK_EFAULT. Runs an AVX-512
// instruction that needs all three components at ring 3 and at ring 0 and
// checks its result, so the state is shown enabled rather than merely not
// reached. Skipped on a CPU without AVX-512.

// Host-side probe: AVX-512F in CPUID leaf 7 and its three state components in
// the host's XCR0. (The same probe inside the guest is the thing under test.)
static int host_has_avx512(uint64_t *host_xcr0) {
  unsigned eax, ebx, ecx, edx, lo, hi;
  __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7u), "c"(0u));
  __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
  uint64_t xcr0 = ((uint64_t)hi << 32) | lo;
  if (host_xcr0) *host_xcr0 = xcr0;
  return (ebx & (1u << 16)) != 0 && (xcr0 & 0xe0) == 0xe0;
}

struct avx512_probe { uint64_t xcr0; int32_t out[16]; };

// Runs in the guest. Records the guest's XCR0 (xgetbv is unprivileged under
// CR4.OSXSAVE), then adds two 16-lane vectors held in zmm16 and zmm17 under
// opmask k1 with zeroing, which needs all three AVX-512 components: k1 (the
// opmask state), the upper 256 bits of the registers (ZMM_Hi256) and registers
// 16-31 (Hi16_ZMM). Lanes 0-7 get a[i]+b[i] and lanes 8-15 zero. Returns the
// number of lanes that came out wrong.
static long __attribute__((noinline)) guest_avx512(void *arg) {
  struct avx512_probe *p = arg;
  unsigned lo, hi;
  __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
  p->xcr0 = ((uint64_t)hi << 32) | lo;
  _Alignas(64) int32_t a[16], b[16];
  for (int i = 0; i < 16; i++) { a[i] = 1000 + i; b[i] = 7 * i; }
  __asm__ __volatile__("kmovw %k[mask], %%k1\n\t"
                       "vmovdqa32 (%[a]), %%zmm16\n\t"
                       "vmovdqa32 (%[b]), %%zmm17\n\t"
                       "vpaddd %%zmm17, %%zmm16, %%zmm18%{%%k1%}%{z%}\n\t"
                       "vmovdqu32 %%zmm18, (%[out])\n\t"
                       "vzeroupper"
                       :
                       : [a] "r"(a), [b] "r"(b), [out] "r"(p->out), [mask] "r"(0x00ffu)
                       : "memory");  // zmm16-18 and k1 are not registers the
                                      // compiler uses or accepts as clobbers
                                      // without -mavx512f
  long bad = 0;
  for (int i = 0; i < 16; i++) bad += p->out[i] != (i < 8 ? a[i] + b[i] : 0);
  return bad;
}

// Formats a guest_avx512 result: a fault (the #UD of a disabled component),
// or the count of wrong lanes.
static const char *avx512_result(long r, unsigned long fault, char *buf, size_t n) {
  if (r == GK_EFAULT) snprintf(buf, n, "FAULT (fault %#lx)", fault);
  else snprintf(buf, n, "%ld lanes wrong", r);
  return buf;
}

static long avx512_check(void) {
  uint64_t host_xcr0 = 0;
  if (!host_has_avx512(&host_xcr0)) {
    printf("avx512 in guest: host CPU lacks AVX-512 (host XCR0 %#llx); skipped [OK]\n",
           (unsigned long long)host_xcr0);
    return 1;
  }
  struct avx512_probe u = {0}, k = {0};
  long ru = gk_run_user(guest_avx512, &u);
  unsigned long fu = gk_fault_addr();
  long rk = gk_run(guest_avx512, &k);
  unsigned long fk = gk_fault_addr();
  int ok = ru == 0 && rk == 0 && (u.xcr0 & 0xe7) == 0xe7 && (k.xcr0 & 0xe7) == 0xe7;
  char bu[64], bk[64];
  printf("avx512 in guest: host XCR0 %#llx; ring 3: XCR0 %#llx, masked zmm16+zmm17 -> %s "
         "(lanes 0/7/8: %d %d %d); ring 0: XCR0 %#llx, -> %s [%s]\n",
         (unsigned long long)host_xcr0, (unsigned long long)u.xcr0,
         avx512_result(ru, fu, bu, sizeof bu), u.out[0], u.out[7], u.out[8],
         (unsigned long long)k.xcr0, avx512_result(rk, fk, bk, sizeof bk), ok ? "OK" : "FAIL");
  return ok;
}


#endif  // __x86_64__

// ---- thread words --------------------------------------------------------------
// GK_THREAD_WORDS words per thread in the supervisor control block (see
// gk_thread_words_set in gk.h). Checks: a range is validated before anything
// is read; words set on the host read back, as a range and from a signal
// handler interrupting a ring-3 turn; a thread with no record reads -ESRCH
// and a fresh record reads zeroes; the words are unreachable from ring 3 and
// reachable from ring 0; the read is timed.

static volatile long tw_sig_r;
static volatile uint64_t tw_sig_v[2];
static void tw_sigaction(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si; (void)uc;
  uint64_t v[2] = {0, 0};
  tw_sig_r = gk_thread_words_get(1, 2, v);
  tw_sig_v[0] = v[0]; tw_sig_v[1] = v[1];
}

// Runs in the guest (ring 3): raises SIGUSR1 at itself; the handler runs
// host-side on the forwarded tgkill's return and reads the words.
static long tw_guest_raise(void *arg) {
  (void)arg;
  syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1);
  return 0;
}

// Runs in the guest (ring 0): reads the word at the address given.
static long tw_read_word(void *arg) {
  return (long)*(volatile uint64_t *)arg;
}

static void *tw_thread(void *arg) {
  (void)arg;
  uint64_t v[1] = {0x1234};
  long r_none = gk_thread_words_get(0, 1, v);        // no record yet
  long r_set = gk_thread_words_set(0, 1, v);         // takes one
  uint64_t w[GK_THREAD_WORDS];
  memset(w, 0xff, sizeof w);
  long r_get = gk_thread_words_get(0, GK_THREAD_WORDS, w);
  int zeroed = w[0] == 0x1234;
  for (int i = 1; i < GK_THREAD_WORDS; i++) if (w[i] != 0) zeroed = 0;
  return (void *)(uintptr_t)(r_none == -ESRCH && r_set == 0 && r_get == 0 && zeroed ? 1 : 0);
}

enum { TW_N = 200000 };

static int thread_word_check(void) {
  uint64_t v[3] = {0x1111, 0x2222, 0x3333}, r[3] = {0, 0, 0}, one = 0;
  // Bounds: a range at or past the table, or running past it, is refused unread.
  int bounds = gk_thread_words_set(GK_THREAD_WORDS, 1, v) == -EINVAL &&
               gk_thread_words_set(GK_THREAD_WORDS - 1, 2, v) == -EINVAL &&
               gk_thread_words_set(0, 0, v) == -EINVAL &&
               gk_thread_words_set(0, 1, NULL) == -EINVAL &&
               gk_thread_words_get(GK_THREAD_WORDS, 1, r) == -EINVAL &&
               gk_thread_words_get(1, GK_THREAD_WORDS, r) == -EINVAL &&
               gk_thread_words_get(0, 1, NULL) == -EINVAL && r[0] == 0;
  // A range set reads back as a range and one word at a time.
  int set = gk_thread_words_set(1, 3, v) == 0 && gk_thread_words_get(1, 3, r) == 0 &&
            r[0] == 0x1111 && r[1] == 0x2222 && r[2] == 0x3333 &&
            gk_thread_words_get(3, 1, &one) == 0 && one == 0x3333;
  // From a signal handler interrupting a ring-3 turn.
  struct sigaction sa = {0}, old;
  sa.sa_sigaction = tw_sigaction;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR1, &sa, &old);
  tw_sig_r = -1; tw_sig_v[0] = tw_sig_v[1] = 0;
  long rr = gk_run_here_user(tw_guest_raise, NULL);
  sigaction(SIGUSR1, &old, NULL);
  int sig_ok = rr == 0 && tw_sig_r == 0 && tw_sig_v[0] == 0x1111 && tw_sig_v[1] == 0x2222;
  // A fresh thread: no record is -ESRCH, a new record reads zeroes.
  pthread_t th; void *rv = NULL;
  pthread_create(&th, NULL, tw_thread, NULL);
  pthread_join(th, &rv);
  int fresh_ok = (uintptr_t)rv == 1;
  // Unreachable from ring 3, both ways; readable from ring 0.
  unsigned long words = gk_debug_ctl_addr(GK_CTL_THREAD_WORDS);
  long w3 = gk_run_user(user_write_here, (void *)(words + 8));
  unsigned long fw3 = gk_fault_addr();
  long r3 = gk_run_user(read_byte, (void *)(words + 8));
  unsigned long fr3 = gk_fault_addr();
  long r0 = gk_run(tw_read_word, (void *)(words + 8));
  int walled = words != 0 && w3 == GK_EFAULT && fw3 == words + 8 && r3 == GK_EFAULT &&
               fr3 == words + 8 && r0 == 0x1111 &&
               gk_thread_words_get(1, 1, &one) == 0 && one == 0x1111;  // the write did not land
  // Cost of a read on the host.
  double t0 = now_ns();
  long acc = 0;
  for (int i = 0; i < TW_N; i++) { gk_thread_words_get(1, 3, r); acc += (long)r[0]; }
  double per = (now_ns() - t0) / TW_N;
  int bench_ok = acc == (long)TW_N * 0x1111 && per > 0;
  uint64_t zero[3] = {0, 0, 0};
  gk_thread_words_set(1, 3, zero);

  int ok = bounds && set && sig_ok && fresh_ok && walled && bench_ok;
  printf("thread words: bad ranges refused %s; set/get %s; from a SIGUSR1 handler under a ring-3 "
         "turn %s (r=%ld, %#llx %#llx); fresh thread: ESRCH then zeroes %s; ring-3 write -> %ld at "
         "%#lx, read -> %ld, ring-0 read %#lx %s; get of 3 words %.0f ns [%s]\n",
         bounds ? "yes" : "NO", set ? "yes" : "NO", sig_ok ? "yes" : "NO", tw_sig_r,
         (unsigned long long)tw_sig_v[0], (unsigned long long)tw_sig_v[1], fresh_ok ? "yes" : "NO",
         w3, fw3, r3, (unsigned long)r0, walled ? "walled" : "NOT WALLED", per, ok ? "OK" : "FAIL");
  return ok;
}

// ---- the handler flag ------------------------------------------------------------
// Whether the embedder's hostcall handler is running, from the thread's record
// rather than from anything ring 3 can write (see gk_in_handler in gk.h).
// Checks: a handler called from the host and a handler called from a ring-3
// turn both see it set; a bracketed call made from inside one leaves it set,
// so an inner call does not unguard the rest of the handler; host code outside
// a handler sees it clear, and so does the handler's caller afterwards; and a
// ring-3 turn is answered 0 without the record being touched.

enum { HF_IDX = GK_HOSTCALLS - 2 };
static volatile int hf_inside, hf_after_inner;
static long hf_handler(void *ctx, gk_hostcall_frame *f) {
  (void)ctx; (void)f;
  hf_inside = gk_in_handler();
  // A host-side resolve takes a bracket of its own; the flag must still say
  // handler once it returns.
  gk_resolve(0);
  hf_after_inner = gk_in_handler();
  return 0;
}
// Runs in the guest (ring 3): the flag here, then a hostcall whose handler
// reads it on the host side of the exit.
static long hf_guest(void *arg) {
  (void)arg;
  long here = gk_in_handler();
  gk_hostcall(HF_IDX);
  return here;
}

static int handler_flag_check(void) {
  int reg = gk_hostcall_register(HF_IDX, hf_handler, NULL) == 0;
  hf_inside = hf_after_inner = -1;
  long host_call = gk_hostcall(HF_IDX);
  int from_host = hf_inside == 1 && hf_after_inner == 1;
  int outside = gk_in_handler() == 0;
  hf_inside = hf_after_inner = -1;
  long r3 = gk_run_here_user(hf_guest, NULL);
  int from_guest = hf_inside == 1 && hf_after_inner == 1;
  int clear_after = gk_in_handler() == 0;
  gk_hostcall_unregister(HF_IDX);

  int ok = reg && host_call == 0 && from_host && outside && r3 == 0 && from_guest && clear_after;
  printf("handler flag: a handler called from the host sees it set %s, one called from a ring-3 "
         "turn %s, and an inner bracketed call leaves it set %s; outside one %s; from ring 3 "
         "-> %ld %s [%s]\n",
         from_host ? "yes" : "NO", from_guest ? "yes" : "NO", hf_after_inner == 1 ? "yes" : "NO",
         outside && clear_after ? "clear" : "NOT CLEAR", r3, r3 == 0 ? "ok" : "WRONG",
         ok ? "OK" : "FAIL");
  return ok;
}

// ---- the report budget ----------------------------------------------------------
// What a refusal path may say, per site and per second (see gk_log_budget in
// gk.h). Checks: the first report at a site passes with nothing before it; a
// burst is capped; the reports held back are counted and handed to the next
// one that passes; a site gk does not know passes rather than goes silent;
// one arena's budget is not another's, so a turn spending its own takes no
// other isolate's report with it; and a call from a ring-3 turn decides
// nothing and answers -EPERM.

// Runs in the guest (ring 3): asks for a report budget, which the caller's
// ring refuses before the block is touched.
static long lb_guest(void *arg) {
  return gk_log_budget((unsigned)(uintptr_t)arg);
}

static int log_budget_check(void) {
  // Two sites nothing else uses, so the counts are this test's alone.
  const unsigned site = GK_LOG_SITES - 1, other = GK_LOG_SITES - 2;
  // The first report at a site passes with no history behind it.
  int first = gk_log_budget(site) == 1;
  // The rest of the burst passes and the HELD after it do not. A window
  // turning over in the middle starts the burst again and hands the held
  // ones over early, so the run is retried; either way a whole second holds
  // GK_LOG_BURST reports, which is the bound that matters.
  enum { HELD = 20 };
  int passed = 1, capped = 0;
  long held = 0;
  for (int attempt = 0; attempt < 4 && !capped; attempt++) {
    held = 0;
    for (int i = 1; i < GK_LOG_BURST; i++) if (gk_log_budget(site) <= 0) passed = 0;
    for (int i = 0; i < HELD; i++) {
      if (gk_log_budget(site) == 0) held++;
      else held = 0;  // a new window: what was held went out with its first report
    }
    capped = held == HELD;
  }
  // The ones held back are counted, and the first report of the next window
  // carries them: 1 plus the number left unsaid.
  usleep(1100000);  // past the window, without asking again and adding to the count
  long next = gk_log_budget(site);
  int counted = next == held + 1;
  // A site gk does not know is a caller's mistake, and the answer to one is
  // the report: an id out of range must never be silence.
  int unknown = gk_log_budget(GK_LOG_SITES) == 1 && gk_log_budget(~0u) == 1;
  // Sites are counted apart: the one above is spent and this one is not.
  int apart = gk_log_budget(other) >= 1;
  // So are arenas. A thread with an arena active counts in that arena's
  // buckets, so a turn that spends a site's burst leaves another isolate's
  // first report at that site to pass as it always would.
  const unsigned shared = GK_LOG_SITES - 3;
  gk_arena *a1 = gk_arena_create(1 << 20), *a2 = gk_arena_create(1 << 20);
  int arenas = 0;
  if (a1 && a2 && gk_arena_enter(a1) == 0) {
    int mine = gk_log_budget(shared) == 1;
    for (int i = 0; i < 4 * GK_LOG_BURST; i++) gk_log_budget(shared);
    int spent = gk_log_budget(shared) == 0;
    gk_arena_leave();
    int theirs = gk_arena_enter(a2) == 0 && gk_log_budget(shared) == 1;
    gk_arena_leave();
    // And the block's own buckets, which a thread with no arena uses, are
    // untouched by either.
    int neither = gk_log_budget(shared) == 1;
    arenas = mine && spent && theirs && neither;
  }
  if (a1) gk_arena_destroy(a1);
  if (a2) gk_arena_destroy(a2);
  // From a ring-3 turn: no decision and no fault -- a turn that faulted would
  // have ended as GK_EFAULT rather than returning gk's refusal.
  long r3 = gk_run_here_user(lb_guest, (void *)(uintptr_t)other);
  int walled = r3 == -1;  // -GK_EPERM
  // Cost of the host-side call on a path that is refusing something.
  enum { LB_N = 200000 };
  double t1 = now_ns();
  long acc = 0;
  for (int i = 0; i < LB_N; i++) acc += gk_log_budget(site);
  double per = (now_ns() - t1) / LB_N;

  int ok = first && passed && capped && counted && unknown && apart && arenas && walled &&
           per > 0;
  printf("report budget: first report passes %s; burst of %d capped %s; %ld held back came back "
         "as %ld %s; unknown site passes %s; sites counted apart %s; arenas counted apart %s; "
         "from a ring-3 turn -> %ld %s; host-side call %.0f ns (%ld passed of %d) [%s]\n",
         first ? "yes" : "NO", GK_LOG_BURST, capped ? "yes" : "NO", held, next,
         counted ? "yes" : "NO", unknown ? "yes" : "NO", apart ? "yes" : "NO",
         arenas ? "yes" : "NO", r3, walled ? "refused" : "NOT REFUSED", per, acc, LB_N,
         ok ? "OK" : "FAIL");
  return ok;
}

// ---- the turn directory ---------------------------------------------------------
// With gk_confine_paths on, a ring-3 turn's path syscalls resolve beneath the
// directory a hostcall handler set for it, or are refused (see
// gk_confine_paths in gk.h). Checks: with no directory everything is refused;
// with one, openat and newfstatat under it work and files land there, and
// unlinkat, mkdirat, readlinkat and faccessat2 act on a name under it, while a
// dirfd for another directory, AT_FDCWD, an absolute path, a "..", a symbolic
// link out (opened, unlinked, made a directory under, or access-checked
// through), a direct openat2, access and fchdir are each refused; the
// directory's descriptor closed and reused for another directory is refused
// by identity; a second entry starts with no directory; a turn cannot set the
// directory itself; the policy off forwards as before; the cost of a confined
// open is measured.

enum { TD_IDX = 11, TD_SET_A = 1, TD_SET_B = 2, TD_SET_NONE = 3, TD_N = 20000 };

// A raw syscall, returning the kernel's value (-errno on failure) rather than
// glibc's -1 with errno, so the guest code below can record what gk answered.
static long td_raw(long nr, long a1, long a2, long a3, long a4, long a5) {
  long ret;
#if defined(__x86_64__)
  register long r10 __asm__("r10") = a4, r8 __asm__("r8") = a5, r9 __asm__("r9") = 0;
  __asm__ __volatile__("syscall"
                       : "=a"(ret)
                       : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                       : "rcx", "r11", "memory");
#else
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a1, x1 __asm__("x1") = a2, x2 __asm__("x2") = a3;
  register long x3 __asm__("x3") = a4, x4 __asm__("x4") = a5;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                       : "memory");
  ret = x0;
#endif
  return ret;
}
struct td_dirs { int a, b; };

// The handler a runtime would register: the directory comes from the context
// (host memory), the guest only chooses which.
static long td_handler(void *ctx, gk_hostcall_frame *f) {
  struct td_dirs *d = ctx;
  if (f->args[0] == TD_SET_A) return gk_turn_dir_set(d->a);
  if (f->args[0] == TD_SET_B) return gk_turn_dir_set(d->b);
  if (f->args[0] == TD_SET_NONE) return gk_turn_dir_set(GK_TURN_DIR_NONE);
  return -EINVAL;
}

struct td_probe {
  int a, b;
  long before, open_a, open_b, cwd, abs, dotdot, link, stat_a, stat_b, stat_dir, statx_a,
      access, unlink, unlink_b, unlink_link, unlink_dotdot, unlink_abs, mkdir, mkdir_link, rmdir,
      readlink, readlink_b, faccess, faccess_link, faccess_through, faccess_dotdot, openat2,
      fchdir, swapped, open_b2, none, self_set;
  long st_size, st_isdir, stx_size;
  char target[16];
};

static long td_hostcall(unsigned op) {
  gk_hostcall_frame *r = gk_hostcall_region();
  r->args[0] = op; r->in_len = 0;
  return gk_hostcall(TD_IDX);
}

static long td_sys_open(int dir, const char *path, int flags) {
  return td_raw(SYS_openat, dir, (long)path, flags, 0644, 0);
}

// Runs in the guest (ring 3).
static long td_guest(void *arg) {
  struct td_probe *p = arg;
  struct stat st;
  p->before = td_sys_open(p->a, "f0", O_CREAT | O_WRONLY);
  if (td_hostcall(TD_SET_A) != 0) return -1;
  p->open_a = td_sys_open(p->a, "f1", O_CREAT | O_WRONLY);
  if (p->open_a >= 0) { td_raw(SYS_write, p->open_a, (long)"hi", 2, 0, 0); td_raw(SYS_close, p->open_a, 0, 0, 0, 0); }
  p->open_b = td_sys_open(p->b, "f2", O_CREAT | O_WRONLY);
  p->cwd = td_sys_open(AT_FDCWD, "/etc/passwd", O_RDONLY);
  p->abs = td_sys_open(p->a, "/etc/passwd", O_RDONLY);
  p->dotdot = td_sys_open(p->a, "../escape", O_CREAT | O_WRONLY);
  p->link = td_sys_open(p->a, "out/passwd", O_RDONLY);
  p->stat_a = td_raw(SYS_newfstatat, p->a, (long)"f1", (long)&st, 0, 0);
  p->st_size = st.st_size;
  p->stat_b = td_raw(SYS_newfstatat, p->b, (long)"f1", (long)&st, 0, 0);
  p->stat_dir = td_raw(SYS_newfstatat, p->a, (long)"", (long)&st, AT_EMPTY_PATH, 0);
  p->st_isdir = S_ISDIR(st.st_mode);
  struct { unsigned char raw[256]; } stx;
  p->statx_a = td_raw(SYS_statx, p->a, (long)"f1", 0, 0x7ff, (long)&stx);
  memcpy(&p->stx_size, stx.raw + 40, 8);  // stx_size
#if defined(__x86_64__)
  p->access = td_raw(SYS_access, (long)"/etc/passwd", R_OK, 0, 0, 0);
#else  // no SYS_access on arm64: the same check by its *at form, through AT_FDCWD
  p->access = td_raw(SYS_faccessat, AT_FDCWD, (long)"/etc/passwd", R_OK, 0, 0);
#endif
  // The name-acting syscalls: under the directory they work on the final
  // component through its parent; elsewhere, through a symbolic link or a
  // "..", or on ".." itself, they are refused.
  long u = td_sys_open(p->a, "u1", O_CREAT | O_WRONLY);
  if (u >= 0) td_raw(SYS_close, u, 0, 0, 0, 0);
  p->unlink = td_raw(SYS_unlinkat, p->a, (long)"u1", 0, 0, 0);
  p->unlink_b = td_raw(SYS_unlinkat, p->b, (long)"f2", 0, 0, 0);
  p->unlink_link = td_raw(SYS_unlinkat, p->a, (long)"out/passwd", 0, 0, 0);
  p->unlink_dotdot = td_raw(SYS_unlinkat, p->a, (long)"../escape", 0, 0, 0);
  p->unlink_abs = td_raw(SYS_unlinkat, p->a, (long)"/etc/passwd", 0, 0, 0);
  p->mkdir = td_raw(SYS_mkdirat, p->a, (long)"d", 0755, 0, 0);
  p->mkdir_link = td_raw(SYS_mkdirat, p->a, (long)"out/gk-td-escape", 0755, 0, 0);
  p->rmdir = td_raw(SYS_unlinkat, p->a, (long)"d", AT_REMOVEDIR, 0, 0);
  memset(p->target, 0, sizeof p->target);
  p->readlink = td_raw(SYS_readlinkat, p->a, (long)"out", (long)p->target, sizeof p->target - 1, 0);
  p->readlink_b = td_raw(SYS_readlinkat, p->b, (long)"out", (long)p->target, sizeof p->target - 1, 0);
  p->faccess = td_raw(439, p->a, (long)"f1", R_OK, 0, 0);  // faccessat2
  p->faccess_link = td_raw(SYS_faccessat, p->a, (long)"out", R_OK, 0, 0);
  p->faccess_through = td_raw(439, p->a, (long)"out/passwd", R_OK, 0, 0);
  p->faccess_dotdot = td_raw(439, p->a, (long)"..", R_OK, 0, 0);
  struct { uint64_t flags, mode, resolve; } how = {O_RDONLY, 0, 0};
  p->openat2 = td_raw(437, p->a, (long)"f1", (long)&how, sizeof how, 0);
  p->fchdir = td_raw(SYS_fchdir, p->a, 0, 0, 0, 0);
  // The descriptor closed and its number reused for the other directory.
  int dupb = (int)td_raw(SYS_fcntl, p->b, F_DUPFD, 0, 0, 0);
  td_raw(SYS_close, p->a, 0, 0, 0, 0);
  int reused = (int)td_raw(SYS_fcntl, dupb, F_DUPFD, p->a, 0, 0);
  p->swapped = reused == p->a ? td_sys_open(p->a, "f3", O_CREAT | O_WRONLY) : -9999;
  td_raw(SYS_close, dupb, 0, 0, 0, 0);
  // Another directory, chosen by the handler.
  if (td_hostcall(TD_SET_B) != 0) return -2;
  p->open_b2 = td_sys_open(p->b, "g1", O_CREAT | O_WRONLY);
  if (p->open_b2 >= 0) td_raw(SYS_close, p->open_b2, 0, 0, 0, 0);
  if (td_hostcall(TD_SET_NONE) != 0) return -3;
  p->none = td_sys_open(p->b, "g2", O_CREAT | O_WRONLY);
  return 0;
}

// Runs in the guest (ring 3): a second entry has no directory; and the turn
// cannot set one itself (the record is supervisor memory).
static long td_guest_again(void *arg) {
  struct td_probe *p = arg;
  p->before = td_sys_open(p->b, "g3", O_CREAT | O_WRONLY);
  return 0;
}
static long td_guest_self_set(void *arg) {
  struct td_probe *p = arg;
  return gk_turn_dir_set(p->b);
}

// Runs in the guest (ring 3): TD_N opens and closes of f1 under the directory.
static long td_bench(void *arg) {
  struct td_probe *p = arg;
  long n = 0;
  for (int i = 0; i < TD_N; i++) {
    long fd = td_sys_open(p->a, "f1", O_RDONLY);
    if (fd >= 0) { td_raw(SYS_close, fd, 0, 0, 0, 0); n++; }
  }
  return n;
}
static long td_bench_set(void *arg) {
  if (td_hostcall(TD_SET_A) != 0) return -1;
  return td_bench(arg);
}

// Whether `dirpath/name` exists, checked by absolute path so it does not depend on a descriptor
// the guest may have closed.
static int exists_in(const char *dirpath, const char *name) {
  char path[256];
  snprintf(path, sizeof path, "%s/%s", dirpath, name);
  struct stat st;
  return lstat(path, &st) == 0;
}

static int turn_dir_check(void) {
  char ta[] = "/tmp/gk-td-a-XXXXXX", tb[] = "/tmp/gk-td-b-XXXXXX";
  if (!mkdtemp(ta) || !mkdtemp(tb)) { printf("turn directory: mkdtemp failed [FAIL]\n"); return 0; }
  struct td_dirs *d = gk_host_alloc(sizeof *d);
  d->a = open(ta, O_RDONLY | O_DIRECTORY);
  d->b = open(tb, O_RDONLY | O_DIRECTORY);
  char lnk[64]; snprintf(lnk, sizeof lnk, "%s/out", ta);
  if (symlink("/etc", lnk) != 0) { printf("turn directory: symlink failed [FAIL]\n"); return 0; }
  if (d->a < 0 || d->b < 0 || gk_hostcall_register(TD_IDX, td_handler, d) != 0) {
    printf("turn directory: setup failed [FAIL]\n");
    return 0;
  }
  // Setting a non-directory, or on the host before confinement matters, is
  // validated.
  int fdfile = open("/etc/passwd", O_RDONLY);
  int set_bad = gk_turn_dir_set(fdfile) == -EINVAL && gk_turn_dir_set(-7) == -EINVAL &&
                gk_turn_dir_set(d->a) == 0 && gk_turn_dir_set(GK_TURN_DIR_NONE) == 0;
  close(fdfile);
  gk_stats s0, s1;
  gk_get_stats(&s0);
  gk_confine_paths(1);
  struct td_probe *p = calloc(1, sizeof *p);
  p->a = d->a; p->b = d->b;
  long r = gk_run_user(td_guest, p);
  gk_get_stats(&s1);
  long first_before = p->before;
  int before_ok = p->before == -EPERM;
  int open_ok = p->open_a >= 0 && exists_in(ta, "f1") && p->stat_a == 0 && p->st_size == 2 &&
                p->stat_dir == 0 && p->st_isdir && p->statx_a == 0 && p->stx_size == 2;
  int refused = p->open_b == -EPERM && !exists_in(tb, "f2") && p->cwd == -EPERM &&
                p->abs == -EXDEV && p->dotdot == -EXDEV && !exists_in(tb, "escape") &&
                p->link == -EXDEV && p->stat_b == -EPERM && p->access == -EPERM &&
                p->openat2 == -EPERM && p->fchdir == -EPERM;
  // The name-acting syscalls: unlink, mkdir, rmdir, readlink and access beneath
  // the directory work; the same through another dirfd are refused, through
  // the symbolic link or a ".." fail to resolve beneath, and ".." itself is
  // refused. The link is read as itself (its target text), not followed.
  int names_ok = p->unlink == 0 && !exists_in(ta, "u1") && p->unlink_b == -EPERM &&
                 exists_in(tb, "f2") == 0 && p->unlink_link == -EXDEV && exists_in("/etc", "passwd") &&
                 p->unlink_dotdot == -EXDEV && p->unlink_abs == -EXDEV && p->mkdir == 0 &&
                 p->rmdir == 0 && !exists_in(ta, "d") && p->mkdir_link == -EXDEV &&
                 !exists_in("/etc", "gk-td-escape") && p->readlink == 4 &&
                 strcmp(p->target, "/etc") == 0 && p->readlink_b == -EPERM && p->faccess == 0 &&
                 p->faccess_link == 0 && p->faccess_through == -EXDEV &&
                 p->faccess_dotdot == -EPERM;
  int swapped_ok = p->swapped == -EPERM && !exists_in(tb, "f3") && !exists_in(ta, "f3");
  int other_ok = p->open_b2 >= 0 && exists_in(tb, "g1") && p->none == -EPERM &&
                 !exists_in(tb, "g2");
  // The descriptor the guest closed: reopen the directory for the rest.
  d->a = open(ta, O_RDONLY | O_DIRECTORY);
  p->a = d->a;
  long r2 = gk_run_user(td_guest_again, p);
  int again_ok = r2 == 0 && p->before == -EPERM && !exists_in(tb, "g3");
  long r3 = gk_run_user(td_guest_self_set, p);
  int self_ok = r3 == GK_EFAULT;
  long refusals = s1.path_refusals - s0.path_refusals;
  // Cost: confined opens against unconfined ones.
  double t0 = now_ns();
  long nb = gk_run_user(td_bench_set, p);
  double t1 = now_ns();
  gk_confine_paths(0);
  long nu = gk_run_user(td_bench, p);
  double t2 = now_ns();
  double per_c = (t1 - t0) / TD_N, per_u = (t2 - t1) / TD_N;
  int bench_ok = nb == TD_N && nu == TD_N;
  // Policy off: the other directory is reachable again, as before.
  long r4 = gk_run_user(td_guest_again, p);
  int off_ok = r4 == 0 && p->before >= 0 && exists_in(tb, "g3");
  if (p->before >= 0) close(p->before);
  gk_hostcall_unregister(TD_IDX);

  int ok = set_bad && r == 0 && before_ok && open_ok && refused && names_ok && swapped_ok &&
           other_ok && again_ok && self_ok && refusals == 12 && bench_ok && off_ok;
  if (!ok)
    printf("turn directory: set_bad %d r %ld before %d open %d (isdir %ld stx_size %ld) refused %d "
           "names %d swapped %d other %d again %d self %d refusals %ld bench %d (%ld %ld) off %d\n",
           set_bad, r, before_ok, open_ok, p->st_isdir, p->stx_size, refused, names_ok, swapped_ok,
           other_ok, again_ok, self_ok, refusals, bench_ok, nb, nu, off_ok);
  printf("turn directory: bad descriptors refused %s; no directory -> %ld; under it: open %s stat "
         "%ld (size %ld) dir %ld statx %ld; other dirfd %ld, AT_FDCWD %ld, absolute %ld, .. %ld, "
         "symlink out %ld, stat other %ld, access %ld, openat2 %ld, fchdir %ld; by name under it: "
         "unlinkat %ld mkdirat %ld rmdir %ld readlinkat %ld (\"%s\") faccessat2 %ld, on the link "
         "itself %ld; other dirfd: unlinkat %ld readlinkat %ld; through the link: unlinkat %ld "
         "mkdirat %ld faccessat2 %ld; .. %ld, absolute %ld, on .. itself %ld; descriptor reused "
         "for another directory %ld; handler-chosen other dir %s, none %ld; next entry %ld; set "
         "from ring 3 -> %ld; %ld refusals counted; confined open %.2f us vs %.2f us; policy off "
         "%s [%s]\n",
         set_bad ? "yes" : "NO", first_before, p->open_a >= 0 ? "ok" : "NO", p->stat_a, p->st_size,
         p->stat_dir, p->statx_a, p->open_b, p->cwd, p->abs, p->dotdot, p->link, p->stat_b,
         p->access, p->openat2, p->fchdir, p->unlink, p->mkdir, p->rmdir, p->readlink, p->target,
         p->faccess, p->faccess_link, p->unlink_b, p->readlink_b, p->unlink_link, p->mkdir_link,
         p->faccess_through, p->unlink_dotdot, p->unlink_abs, p->faccess_dotdot, p->swapped,
         p->open_b2 >= 0 ? "ok" : "NO", p->none, again_ok ? -EPERM : 0L, r3, refusals,
         per_c / 1000, per_u / 1000, off_ok ? "forwards" : "NO", ok ? "OK" : "FAIL");
  return ok;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  hpage = (uintptr_t)sysconf(_SC_PAGESIZE);
  if (gk_init() != 0) {
    fprintf(stderr, "gk_init failed: %s\n", gk_last_error());
    return 1;
  }
  fprintf(stderr, "gk_init ok: process is now a guest-capable VM\n");

  long want = 0;
  for (long i = 0; i < 1000; i++) want += i * i;
  long got = gk_run(test_compute, (void *)1000);
  int ok1 = (got == want);
  printf("compute in guest: got %ld, want %ld [%s]\n", got, want,
         ok1 ? "OK" : "FAIL");

  long rw = gk_run(test_raw_write, NULL);
  printf("raw write in guest returned %ld [%s]\n", rw, rw > 0 ? "OK" : "FAIL");

  long r = gk_run(test_libc, NULL);
  int ok2 = (r > 0);
  printf("libc in guest: checksum %ld [%s]\n", r, ok2 ? "OK" : "FAIL");

  // Arena isolation with demand paging: two isolates, each a PROT_NONE
  // reservation paged into its own root only while active. A is larger than
  // two 512GiB PML4 slots (as V8's largest sandbox reservation is), so it
  // spans three; B is small. Nothing is touched until the guest commits a page.
  const size_t SLOT = 1UL << 39;
  gk_arena *A = gk_arena_create(2 * SLOT + (64UL << 20));
  gk_arena *B = gk_arena_create(1UL << 20);
  int ok3 = 0;
  if (A && B) {
    unsigned char *ab = gk_arena_base(A), *bb = gk_arena_base(B);
    unsigned char *a2 = ab + SLOT + hpage;         // a page in A's second slot
    unsigned char *a3 = ab + 2 * SLOT + 2 * hpage; // and one in its third
    int layout = gk_arena_size(A) >= 2 * SLOT + (64UL << 20) && ((uintptr_t)ab % SLOT) == 0 &&
                 ((uintptr_t)bb % SLOT) == 0 && (bb < ab || bb >= ab + 3 * SLOT);
    struct ap wa = {ab, 0xA1}, wa2 = {a2, 0xA2}, wa3 = {a3, 0xA4}, wb = {bb, 0xB1},
              wa_re = {ab, 0xA3};
    gk_arena_enter(A);
    long ra = gk_run(commit_write, &wa);     // commit, write, read a page of A
    long ra2 = gk_run(commit_write, &wa2);   // same, in A's second slot
    long ra3 = gk_run_here(commit_write, &wa3);  // and its third, on the caller's stack
    long ra_b = gk_run(read_byte, bb);       // B's memory under A: must fault
    unsigned long fa_b = gk_fault_addr();
    gk_arena_enter(B);
    long rb = gk_run(commit_write, &wb);     // B commits and uses its own page
    long rb_a = gk_run(read_byte, ab);       // A's (committed) page under B: must fault
    unsigned long fa_a = gk_fault_addr();
    gk_arena_enter(NULL);
    long r0_a = gk_run(read_byte, ab);       // base root: neither arena is reachable
    unsigned long f0_a = gk_fault_addr();
    long r0_b = gk_run(read_byte, bb);
    unsigned long f0_b = gk_fault_addr();
    gk_arena_enter(A);
    long ra_again = gk_run(read_byte, ab);   // A's pages are back, in all three slots
    long ra2_again = gk_run(read_byte, a2);
    long ra3_again = gk_run(read_byte, a3);
    long rdec = gk_run(decommit_read, &wa);  // decommit is reflected into A's root
    unsigned long fdec = gk_fault_addr();
    long rrec = gk_run(commit_write, &wa_re);  // recommit: usable again
    gk_arena_enter(NULL);
    int host_sees = ab[0] == 0xA3 && a2[0] == 0xA2 && a3[0] == 0xA4 && bb[0] == 0xB1;  // same memory on the host
    printf("arenas: A = [%p, +%zu) over 3 slots, B = [%p, +%zu)\n", (void *)ab, gk_arena_size(A),
           (void *)bb, gk_arena_size(B));
    printf("  under A: commit+write A -> %#lx, A's 2nd slot -> %#lx, 3rd slot -> %#lx, "
           "read B -> %s (fault %#lx)\n", ra, ra2, ra3, ra_b == GK_EFAULT ? "FAULT" : "readable", fa_b);
    printf("  under B: commit+write B -> %#lx, read A -> %s (fault %#lx)\n", rb,
           rb_a == GK_EFAULT ? "FAULT" : "readable", fa_a);
    printf("  base root: read A -> %s (fault %#lx), read B -> %s (fault %#lx)\n",
           r0_a == GK_EFAULT ? "FAULT" : "readable", f0_a, r0_b == GK_EFAULT ? "FAULT" : "readable",
           f0_b);
    printf("  back in A: read A -> %#lx, 2nd slot -> %#lx, 3rd slot -> %#lx, decommit+read -> %s "
           "(fault %#lx), recommit+write -> %#lx, host sees %s\n", ra_again, ra2_again, ra3_again,
           rdec == GK_EFAULT ? "FAULT" : "readable", fdec, rrec, host_sees ? "same bytes" : "WRONG");
    ok3 = layout && ra == 0xA1 && ra2 == 0xA2 && ra3 == 0xA4 && ra_b == GK_EFAULT &&
          fa_b == (uintptr_t)bb && rb == 0xB1 && rb_a == GK_EFAULT && fa_a == (uintptr_t)ab &&
          r0_a == GK_EFAULT && f0_a == (uintptr_t)ab && r0_b == GK_EFAULT && f0_b == (uintptr_t)bb &&
          ra_again == 0xA1 && ra2_again == 0xA2 && ra3_again == 0xA4 && rdec == GK_EFAULT &&
          fdec == (uintptr_t)ab && rrec == 0xA3 && host_sees;
  } else {
    printf("arenas: create failed (A=%p B=%p)\n", (void *)A, (void *)B);
  }
  printf("arena isolation, demand-paged, multi-slot [%s]\n", ok3 ? "OK" : "FAIL");

  // Multi-vCPU concurrency: N threads each run in the guest at once.
  enum { NT = 4 };
  pthread_t th[NT];
  struct work w[NT];
  for (int i = 0; i < NT; i++) { w[i].id = i + 1; w[i].n = 200000; }
  for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, thread_main, &w[i]);
  for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
  int ok4 = 1;
  for (int i = 0; i < NT; i++) if (!w[i].ok) ok4 = 0;
  printf("multi-vCPU: %d threads ran in the guest concurrently [%s]\n", NT,
         ok4 ? "OK" : "FAIL");

  // Per-thread arenas running concurrently, each isolated from the other.
  // The host commits and fills one page of each; each thread's first guest
  // access demand-pages it into that thread's arena root.
  gk_arena *X = gk_arena_create(4096), *Y = gk_arena_create(4096);
  int ok5 = 0;
  if (X && Y && mprotect(gk_arena_base(X), 4096, PROT_READ | PROT_WRITE) == 0 &&
      mprotect(gk_arena_base(Y), 4096, PROT_READ | PROT_WRITE) == 0) {
    memset(gk_arena_base(X), 0x11, 16);
    memset(gk_arena_base(Y), 0x22, 16);
    pthread_barrier_t bar;
    pthread_barrier_init(&bar, NULL, 2);
    struct aw a0 = {.own = X, .peer = Y, .b = &bar};
    struct aw a1 = {.own = Y, .peer = X, .b = &bar};
    pthread_t t0, t1;
    pthread_create(&t0, NULL, arena_thread, &a0);
    pthread_create(&t1, NULL, arena_thread, &a1);
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
    pthread_barrier_destroy(&bar);
    ok5 = a0.own_ok && a0.peer_faulted && a1.own_ok && a1.peer_faulted;
    printf("per-thread arenas: two threads, each isolated from the other (own %#lx/%#lx, peer "
           "%ld/%ld, faults %#lx/%#lx) [%s]\n", a0.r_own, a1.r_own, a0.r_peer, a1.r_peer, a0.fa,
           a1.fa, ok5 ? "OK" : "FAIL");
  }

  // Syscall policy: with a filter denying write, the guest's write returns
  // -EPERM instead of being forwarded.
  gk_set_syscall_filter(deny_write);
  long denied = gk_run(guest_try_write, NULL);
  gk_set_syscall_filter(NULL);
  int ok6 = (denied == -1);  // -EPERM
  printf("syscall policy: denied write returned %ld (want -1/-EPERM) [%s]\n",
         denied, ok6 ? "OK" : "FAIL");

#if GK_TEST_GUEST_THREADS
  // Threads created from inside the guest enter the guest as their own vCPUs.
  struct gt ts[3];
  for (int i = 0; i < 3; i++) { ts[i].id = i + 1; ts[i].n = 100000; ts[i].cr0 = 0; ts[i].sum = -1; }
  long spawned = gk_run(spawn_in_guest, ts);
  int ok7 = (spawned == 1);
  for (int i = 0; i < 3; i++) {
    long want7 = 0;
    for (long j = 0; j < ts[i].n; j++) want7 += (j * 3 + ts[i].id) & 0xff;
    if (!PRIV_PROBE_OK(ts[i].cr0) || ts[i].sum != want7) ok7 = 0;
  }
  printf("guest-created threads (ring-0 creator): 3 threads spawned in the guest ran in ring 0 "
         "(gk_run=%ld, cr0=%#lx) [%s]\n", spawned, ts[0].cr0, ok7 ? "OK" : "FAIL");

  // Thread churn must not grow the vCPU count: 200 iterations of 3 guest-
  // spawned threads may add at most 3 vCPUs (peak concurrency), and 20
  // sequential host threads entering via gk_run may add at most 1.
  // Each guest thread's host stack is registered in the supervisor/refuse
  // registry while it lives, so the registry must not grow with the churn
  // either: at most two ranges (kvm_run, exception stack) per vCPU added,
  // three when each vCPU has a runner (its stack is registered too).
  const char *runner_env = getenv("GK_RUNNER");
  int ranges_per_vcpu = runner_env && *runner_env && strcmp(runner_env, "0") != 0 ? 3 : 2;
  int before = gk_vcpu_count();
  gk_stats sp1 = {0}, sp2 = {0};
  gk_get_stats(&sp1);
  struct churn ch = {.iters = 200, .per_iter = 3, .bad = 0};
  long churned = gk_run(churn_in_guest, &ch);
  int after_guest = gk_vcpu_count();
  gk_get_stats(&sp2);
  int prot_bounded = sp2.prot_ranges - sp1.prot_ranges <= ranges_per_vcpu * (after_guest - before);
  int ok8_host = 1;
  for (int i = 0; i < 20; i++) {
    pthread_t t;
    void *rv;
    pthread_create(&t, NULL, host_churn_thread, (void *)1000);
    pthread_join(t, &rv);
    if ((long)(uintptr_t)rv != want) ok8_host = 0;
  }
  int after_host = gk_vcpu_count();
  int ok8 = (churned == 0) && ch.bad == 0 && (after_guest - before) <= 3 &&
            (after_host - after_guest) <= 1 && ok8_host && prot_bounded;
  printf("vCPU pooling: %d guest threads churned, vCPUs %d -> %d, registry ranges %d -> %d; "
         "20 host threads churned, vCPUs -> %d [%s]\n", ch.iters * ch.per_iter, before,
         after_guest, sp1.prot_ranges, sp2.prot_ranges, after_host, ok8 ? "OK" : "FAIL");
#else
  int ok7 = 1;
  printf("guest-created threads (ring-0 creator): not supported on macOS; skipped [OK]\n");
  // Host-side churn alone: 20 sequential host threads entering via gk_run may
  // add at most 1 vCPU id, as the pool hands the ended thread's on.
  int before = gk_vcpu_count(), ok8 = 1;
  for (int i = 0; i < 20; i++) {
    pthread_t t;
    void *rv;
    pthread_create(&t, NULL, host_churn_thread, (void *)1000);
    pthread_join(t, &rv);
    if ((long)(uintptr_t)rv != want) ok8 = 0;
  }
  int after_host = gk_vcpu_count();
  if (after_host - before > 1) ok8 = 0;
  printf("vCPU pooling: 20 host threads churned, vCPUs %d -> %d [%s]\n", before, after_host,
         ok8 ? "OK" : "FAIL");
#endif

  // Memory protection keys, enforced by the guest CPU against the guest PKRU.
#if defined(__x86_64__)
  struct pk pk = {0};
  long pk_rights = gk_run(pk_setup, &pk);          // WD from pkey_alloc
  long pk_r1 = gk_run(pk_read, &pk);               // readable under WD
  long pk_w1 = gk_run(pk_write, &pk);              // write denied under WD
  unsigned long pk_wfault = gk_fault_addr();
  long pk_r2 = gk_run(pk_disable_access_read, &pk);  // read denied under AD
  unsigned long pk_rfault = gk_fault_addr();
  long pk_r3 = gk_run(pk_enable_write_read, &pk);  // all allowed again
  long pk_inh = gk_run(pk_inherit, &pk);           // child sees WD
  long pk_end = gk_run(pk_teardown, &pk);
  int ok9 = pk_rights == PKEY_DISABLE_WRITE && pk_r1 == 0x5a &&
            pk_w1 == GK_EFAULT && pk_wfault == (uintptr_t)pk.page &&
            pk_r2 == GK_EFAULT && pk_rfault == (uintptr_t)pk.page &&
            pk_r3 == 0x77 && pk_inh == PKEY_DISABLE_WRITE && pk_end == 0x77;
  printf("protection keys: key %d rights after alloc=%ld, read=%#lx, write->%s, "
         "read with access disabled->%s, re-enabled rw=%#lx, child inherits=%ld, "
         "teardown=%#lx [%s]\n", pk.key, pk_rights, pk_r1,
         pk_w1 == GK_EFAULT ? "FAULT" : "allowed", pk_r2 == GK_EFAULT ? "FAULT" : "allowed",
         pk_r3, pk_inh, pk_end, ok9 ? "OK" : "FAIL");
#else
  int ok9 = 1;
  printf("protection keys: no protection keys on arm64; skipped [OK]\n");
#endif

  // gk_run_here runs the guest on the calling thread's stack: on the main
  // thread (kernel-grown stack) and on a pthread (fixed mapping). gk_run, by
  // contrast, runs it on a private stack outside the thread's stack range.
  int ok10 = here_check("main thread") == 1;
  {
    pthread_t t; void *rv = NULL;
    pthread_create(&t, NULL, here_thread, NULL);
    pthread_join(t, &rv);
    if ((uintptr_t)rv != 1) ok10 = 0;
  }
  {
    uintptr_t lo, hi;
    struct here h = {0};
    long r = gk_run(here_fn, &h);
    int ok = thread_stack(&lo, &hi) && r > 0 && !(h.frame >= lo && h.frame < hi);
    printf("gk_run: guest frame %#lx is outside the thread stack [%#lx,%#lx) [%s]\n",
           (unsigned long)h.frame, (unsigned long)lo, (unsigned long)hi, ok ? "OK" : "FAIL");
    if (!ok) ok10 = 0;
  }

  // Arena churn: create, use, destroy, then create and use a FRESH arena, many
  // times over, alternating small (one-slot) and three-slot arenas. Every
  // destroyed arena's slots, memslots and page-table pages must be recycled
  // (bounded growth, and each new arena lands on the freed slot), the freed
  // span must be a plain fault from the base root (no stale mapping), and an
  // arena that lived through it all must be intact.
  int ok11 = 1;
  {
    enum { CHURN = 50, PAGES = 4 };
    gk_arena *keep = gk_arena_create(4096);  // lives across the churn
    if (!keep || mprotect(gk_arena_base(keep), 4096, PROT_READ | PROT_WRITE) != 0) ok11 = 0;
    else memset(gk_arena_base(keep), 0x33, 16);
    gk_stats s1 = {0}, s2 = {0};
    unsigned char *first_base = NULL;
    int reused = 0, fresh_ok = 0, freed_faults = 0;
    for (int it = 0; it < CHURN && ok11; it++) {
      // Two arenas per iteration: one destroyed after use, then a fresh one
      // (the guest entry into the fresh arena is what used to spin forever).
      for (int k = 0; k < 2 && ok11; k++) {
        int big = (it % 5 == 4);
        size_t size = big ? 2 * SLOT + (64UL << 20) : (32UL << 20);
        gk_arena *a = gk_arena_create(size);
        if (!a) { printf("  churn %d.%d: create failed\n", it, k); ok11 = 0; break; }
        unsigned char *base = gk_arena_base(a);
        if (!first_base) first_base = base;
        if (base == first_base) reused++;
        // Pages of a big arena land in each of its three slots; of a small one,
        // in separate 2MiB backing windows.
        struct churn_arena c = {base, big ? (2 * SLOT / 3) & ~(hpage - 1) : (2UL << 20),
                                PAGES, (unsigned)(it * 2 + k)};
        gk_arena_enter(a);
        long r = (it & 1) ? gk_run_here(churn_use, &c) : gk_run(churn_use, &c);
        if (it % 7 != 3) gk_arena_leave();  // sometimes destroy while still entered
        if (r != churn_want(&c)) {
          printf("  churn %d.%d: arena %p run -> %ld, want %ld (fault %#lx)\n", it, k,
                 (void *)base, r, churn_want(&c), gk_fault_addr());
          ok11 = 0;
        } else if (k == 1) {
          fresh_ok++;
        }
        gk_arena_destroy(a);
        if (it % 7 == 3) gk_arena_leave();
        long rf = gk_run(read_byte, base);  // the freed span: a plain demand fault
        if (rf != GK_EFAULT || gk_fault_addr() != (uintptr_t)base) {
          printf("  churn %d.%d: freed span read -> %ld (fault %#lx), want GK_EFAULT at %p\n",
                 it, k, rf, gk_fault_addr(), (void *)base);
          ok11 = 0;
        } else {
          freed_faults++;
        }
        if (it == 0 && k == 0) gk_get_stats(&s1);
      }
    }
    gk_get_stats(&s2);
    gk_arena_enter(keep);
    long rk = gk_run(read_byte, gk_arena_base(keep));
    gk_arena_enter(NULL);
    int bounded = s2.memslots <= s1.memslots + 4 && s2.memslot_ids <= s1.memslot_ids + 4 &&
                  s2.pt_pages_used <= s1.pt_pages_used + 4 &&
                  s2.pt_pages_total <= s1.pt_pages_total + 8 && s2.arenas == s1.arenas;
    printf("arena churn: %d arenas created+destroyed, fresh arenas ran %d/%d, freed spans "
           "faulted %d/%d, slot reused %d/%d, bystander reads %#lx; memslots %d -> %d, "
           "memslot ids %d -> %d, page-table pages %ld -> %ld (free %ld, total %ld -> %ld) [%s]\n",
           2 * CHURN, fresh_ok, CHURN, freed_faults, 2 * CHURN, reused, 2 * CHURN, rk,
           s1.memslots, s2.memslots, s1.memslot_ids, s2.memslot_ids, s1.pt_pages_used,
           s2.pt_pages_used, s2.pt_pages_free, s1.pt_pages_total, s2.pt_pages_total,
           ok11 && bounded && reused == 2 * CHURN && rk == 0x33 ? "OK" : "FAIL");
    if (!bounded || reused != 2 * CHURN || rk != 0x33) ok11 = 0;

    // Destroying an arena another thread still has active must not recycle
    // its tables (that thread's vCPU would run under someone else's), but the
    // arena is gone all the same, and later arenas are unaffected.
    gk_arena *z = gk_arena_create(4096);
    pthread_barrier_t zb;
    pthread_barrier_init(&zb, NULL, 2);
    struct holder h = {z, &zb, -1};
    pthread_t zt;
    if (z && mprotect(gk_arena_base(z), 4096, PROT_READ | PROT_WRITE) == 0 &&
        pthread_create(&zt, NULL, hold_arena, &h) == 0) {
      pthread_barrier_wait(&zb);
      gk_stats before, after;
      gk_get_stats(&before);
      fprintf(stderr, "(expected gk message follows: destroying an arena a thread still uses)\n");
      gk_arena_destroy(z);
      gk_get_stats(&after);
      pthread_barrier_wait(&zb);
      pthread_join(zt, NULL);
      pthread_barrier_destroy(&zb);
      gk_arena *z2 = gk_arena_create(4096);
      struct churn_arena c = {gk_arena_base(z2), 2UL << 20, 1, 77};
      gk_arena_enter(z2);
      long rz = gk_run(churn_use, &c);
      gk_arena_enter(NULL);
      gk_arena_destroy(z2);
      int okz = h.r == 0 && after.arenas == before.arenas - 1 &&
                after.pt_pages_used == before.pt_pages_used && rz == churn_want(&c);
      printf("arena destroyed while another thread uses it: tables kept (%ld -> %ld), "
             "arenas %d -> %d, next arena runs -> %ld [%s]\n", before.pt_pages_used,
             after.pt_pages_used, before.arenas, after.arenas, rz, okz ? "OK" : "FAIL");
      if (!okz) ok11 = 0;
    } else {
      ok11 = 0;
    }
    gk_arena_destroy(keep);
  }

  // ---- guest ring-3 privilege split -----------------------------------------
  // The four core properties of gk_run_user. After each faulting case the
  // process must survive to run the next assertion.
  int ok12 = 1;
  {
    // 1. A trivial fn runs at ring 3 and returns its value.
    unsigned long cs = 0;
    long cpl = gk_run_user(user_cpl, &cs);
    int p1 = (cpl == 3 && (cs & 3) == 3);
    printf("  1. ring-3 execution: fn ran at CPL %ld (CS %#lx) [%s]\n", cpl, cs,
           p1 ? "OK" : "FAIL");

    // 2. A privileged instruction (mov %%cr3) #GPs -> GK_EFAULT, process survives.
    long priv = gk_run_user(user_read_cr3, NULL);
    unsigned long priv_fault = gk_fault_addr();
    int p2 = (priv == GK_EFAULT);
    printf("  2. ring-3 privileged mov %%cr3: %s (faulting rip %#lx), process survived [%s]\n",
           priv == GK_EFAULT ? "#GP -> GK_EFAULT" : "DID NOT FAULT", priv_fault,
           p2 ? "OK" : "FAIL");

    // 3. Supervisor and refused addresses fault; a normal user page works.
    unsigned long super = 0, refuse = 0;
    gk_debug_control_addrs(&super, &refuse);
    long ws = gk_run_user(user_write_here, (void *)super);
    unsigned long fs = gk_fault_addr();
    long wr = gk_run_user(user_write_here, (void *)refuse);
    unsigned long fr = gk_fault_addr();
    unsigned char *upage = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long wu = (upage != MAP_FAILED) ? gk_run_user(user_write_here, upage) : -1;
    int p3 = ws == GK_EFAULT && fs == super && wr == GK_EFAULT && fr == refuse && wu == 0x5a;
    printf("  3. ring-3 supervisor write @%#lx -> %s (fault %#lx); refused write @%#lx -> %s "
           "(fault %#lx); own user page -> %#lx [%s]\n", super,
           ws == GK_EFAULT ? "FAULT" : "WROTE", fs, refuse,
           wr == GK_EFAULT ? "FAULT" : "WROTE", fr, wu, p3 ? "OK" : "FAIL");
    if (upage != MAP_FAILED) munmap(upage, 4096);

    // 4. A real syscall from ring 3 is forwarded and SYSRETs back with the result.
    long pid = gk_run_user(user_getpid, NULL);
    int p4 = (pid == (long)getpid());
    printf("  4. ring-3 forwarded getpid: guest saw %ld, host getpid %ld [%s]\n", pid,
           (long)getpid(), p4 ? "OK" : "FAIL");

    ok12 = p1 && p2 && p3 && p4;
  }
  printf("guest ring-3 privilege split [%s]\n", ok12 ? "OK" : "FAIL");

  // Ring 3 under an active arena: an arena page committed and used at ring 3
  // works (the mprotect is forwarded and the vCPU SYSRETs back), while gk's own
  // page tables remain unreachable.
  int ok13 = 1;
  {
    gk_arena *ua = gk_arena_create(4096);
    unsigned long refuse = 0;
    gk_debug_control_addrs(NULL, &refuse);
    if (!ua) {
      ok13 = 0;
      printf("ring-3 under arena: arena create failed [FAIL]\n");
    } else {
      unsigned char *base = gk_arena_base(ua);
      struct ap w = {base, 0xC7};
      gk_arena_enter(ua);
      long rc = gk_run_user(commit_write, &w);                 // commit+write+read at ring 3
      long rp = gk_run_user(user_write_here, (void *)refuse);  // gk page tables: must fault
      unsigned long fp = gk_fault_addr();
      gk_arena_enter(NULL);
      gk_arena_destroy(ua);
      ok13 = rc == 0xC7 && rp == GK_EFAULT && fp == refuse;
      printf("ring-3 under arena: commit+write arena page -> %#lx, touch gk page tables -> %s "
             "(fault %#lx) [%s]\n", rc, rp == GK_EFAULT ? "FAULT" : "REACHED", fp,
             ok13 ? "OK" : "FAIL");
    }
  }

  // ---- guest-created threads at ring 3 --------------------------------------
  // Threads spawned by a ring-3 context run at ring 3 themselves. Each faulting
  // case ends only the thread that faulted: its spawner's join returns and the
  // process survives to run the next check.
  int ok15 = 1;
#if !GK_TEST_GUEST_THREADS
  printf("guest-created threads (ring-3 creator): not supported on macOS; skipped [OK]\n");
#else
  {
    // 1. Three threads spawned at once from ring 3 run at CPL 3 (CS 0x2b), as
    //    do the grandchildren they spawn; each forwards a getpid (3.) and
    //    writes its own user page.
    unsigned char *upages = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct ugt us[3];
    memset(us, 0, sizeof us);
    for (int i = 0; i < 3; i++) {
      us[i].id = i + 1;
      us[i].page = upages == MAP_FAILED ? NULL : upages + i * 4096;
    }
    long spawned_u = gk_run_user(spawn_users_in_guest, us);
    int p1 = spawned_u == 1 && upages != MAP_FAILED, p3 = 1;
    for (int i = 0; i < 3; i++) {
      if (us[i].cs != USER_CS || us[i].child_cs != USER_CS) p1 = 0;
      if (us[i].pid != (long)getpid()) p3 = 0;
      if (us[i].wrote != 0x50 + us[i].id || upages[i * 4096] != 0x50 + us[i].id) p1 = 0;
    }
    printf("  1. ring-3 spawned threads: spawn -> %ld, CS %#lx/%#lx/%#lx, grandchildren CS "
           "%#lx/%#lx/%#lx, user-page writes %#lx/%#lx/%#lx [%s]\n", spawned_u, us[0].cs,
           us[1].cs, us[2].cs, us[0].child_cs, us[1].child_cs, us[2].child_cs, us[0].wrote,
           us[1].wrote, us[2].wrote, p1 ? "OK" : "FAIL");
    if (upages != MAP_FAILED) munmap(upages, 3 * 4096);

    // 2. A privileged instruction in a ring-3 thread #GPs; the thread ends at
    //    that instruction, the spawner joins it, the process survives.
    struct ugt pv = {0};
    struct spawn sp = {user_thread_priv, &pv};
    long jp = gk_run_user(spawn_one_in_guest, &sp);
    unsigned long fp = gk_fault_addr();
    int p2 = jp == 1 && pv.before == 1 && pv.after == 0 && fp == pv.addr && pv.addr != 0;
    printf("  2. ring-3 thread mov %%cr3 @%#lx: %s (fault %#lx), spawner joined -> %ld, "
           "process survived [%s]\n", pv.addr,
           pv.after ? "EXECUTED" : pv.before ? "#GP, thread ended" : "NOT REACHED", fp, jp,
           p2 ? "OK" : "FAIL");

    // 3. (checked above) a forwarded getpid returned to ring 3 with the pid.
    printf("  3. ring-3 thread forwarded getpid: %ld/%ld/%ld, host %ld [%s]\n", us[0].pid,
           us[1].pid, us[2].pid, (long)getpid(), p3 ? "OK" : "FAIL");

    // 4. Under an arena, a ring-3 thread writes the arena's own committed page
    //    but faults on gk's page tables and on another arena's page. The
    //    faulted threads drop out of the arena, so it is destroyed cleanly
    //    afterwards: its slot is released (a fresh arena lands on it again)
    //    rather than leaked as still in use.
    int p4 = 0;
    gk_arena *ua = gk_arena_create(4096), *ub = gk_arena_create(4096);
    unsigned long refuse = 0;
    gk_debug_control_addrs(NULL, &refuse);
    if (ua && ub && mprotect(gk_arena_base(ua), 4096, PROT_READ | PROT_WRITE) == 0 &&
        mprotect(gk_arena_base(ub), 4096, PROT_READ | PROT_WRITE) == 0) {
      unsigned char *abase = gk_arena_base(ua), *bbase = gk_arena_base(ub);
      struct ugt own = {.id = 7, .page = abase};
      struct ugt tpt = {.addr = refuse};
      struct ugt tpeer = {.addr = (unsigned long)bbase};
      struct spawn s_own = {user_thread_body, &own};
      struct spawn s_pt = {user_thread_touch, &tpt};
      struct spawn s_peer = {user_thread_touch, &tpeer};
      gk_arena_enter(ua);
      long j_own = gk_run_user(spawn_one_in_guest, &s_own);
      long j_pt = gk_run_user(spawn_one_in_guest, &s_pt);
      unsigned long f_pt = gk_fault_addr();
      long j_peer = gk_run_user(spawn_one_in_guest, &s_peer);
      unsigned long f_peer = gk_fault_addr();
      gk_arena_enter(NULL);
      int host_sees = abase[0] == 0x57 && bbase[0] != 0x5a;  // before the reservations go
      gk_arena_destroy(ua);
      gk_arena_destroy(ub);
      // ua had the lowest free slot; a fresh arena lands there again only if
      // the destroy released it, i.e. no faulted thread still counted as a user.
      gk_arena *uc = gk_arena_create(4096);
      int slot_reused = uc && gk_arena_base(uc) == abase;
      gk_arena_destroy(uc);
      p4 = j_own == 1 && own.cs == USER_CS && own.wrote == 0x57 && host_sees &&
           j_pt == 1 && tpt.before && !tpt.after && f_pt == refuse &&
           j_peer == 1 && tpeer.before && !tpeer.after && f_peer == (unsigned long)bbase &&
           slot_reused;
      printf("  4. ring-3 thread under arena: own page write -> %#lx (join %ld); gk page tables "
             "@%#lx -> %s (fault %#lx, join %ld); other arena @%p -> %s (fault %#lx, join %ld); "
             "arena slot %s after destroy [%s]\n", own.wrote, j_own, refuse,
             tpt.after ? "WROTE" : "FAULT, thread ended", f_pt, j_pt, (void *)bbase,
             tpeer.after ? "WROTE" : "FAULT, thread ended", f_peer, j_peer,
             slot_reused ? "released" : "LEAKED", p4 ? "OK" : "FAIL");
    } else {
      printf("  4. ring-3 thread under arena: setup failed [FAIL]\n");
    }
    // The process is intact: ring 3 and a ring-0 guest thread still work.
    long cpl_after = gk_run_user(user_cpl, NULL);
    struct churn c1 = {.iters = 1, .per_iter = 2, .bad = 0};
    long r0_after = gk_run(churn_in_guest, &c1);
    ok15 = p1 && p2 && p3 && p4 && cpl_after == 3 && r0_after == 0 && c1.bad == 0;
    printf("guest-created threads (ring-3 creator): run at ring 3, faults end the thread only; "
           "afterwards ring 3 at CPL %ld, ring-0 threads %s [%s]\n", cpl_after,
           r0_after == 0 && c1.bad == 0 ? "ok" : "BROKEN", ok15 ? "OK" : "FAIL");
  }
#endif

  // ---- gk's control data is supervisor memory --------------------------------
  // The structures that decide which root a thread runs under (the arena
  // registry and structs, the page-table allocator, the memslot tree and its
  // node pool, the supervisor/refuse registry, the thread records, the vCPU
  // pool, ...) must be unreachable from ring 3 even though the host has the
  // memory, or a ring-3 escape with an arbitrary write could rewrite them.
  // Each is written from ring 3, on the base root and under an arena, expecting
  // a fault at that address with the process surviving; then the scratch word
  // is written from ring 0 and read by the host, showing ring 0 keeps access.
  int ok14 = 1;
  {
    static const char *const names[GK_CTL_COUNT] = {
        "scratch word", "arena registry", "arena structs", "supervisor/refuse registry",
        "memslot tree root", "memslot node pool", "page-table allocator", "thread records",
        "vCPU pool", "protection-key ranges", "base root pointer", "syscall filter",
        "host slot table"};
    int faulted = 0;
    for (int i = 0; i < GK_CTL_COUNT; i++) {
      unsigned long a = gk_debug_ctl_addr(i);
      long w = gk_run_user(user_write_here, (void *)a);
      unsigned long f = gk_fault_addr();
      if (a != 0 && w == GK_EFAULT && f == a) {
        faulted++;
      } else {
        printf("  ring-3 write to %s @%#lx -> %s (fault %#lx) [FAIL]\n", names[i], a,
               w == GK_EFAULT ? "FAULT" : "WROTE", f);
        ok14 = 0;
      }
    }
    // The same under an active arena, from ring 3 on the caller's stack (the
    // production path), against the arena structs.
    gk_arena *ca = gk_arena_create(4096);
    unsigned long pool = gk_debug_ctl_addr(GK_CTL_ARENA_POOL);
    long wa = -1;
    unsigned long fa = 0;
    if (ca) {
      gk_arena_enter(ca);
      wa = gk_run_here_user(user_write_here, (void *)pool);
      fa = gk_fault_addr();
      gk_arena_enter(NULL);
      gk_arena_destroy(ca);
    }
    // Ring 0 (trusted code under gk_run_here / gk_run) reads and writes it.
    volatile unsigned char *scratch = (void *)gk_debug_ctl_addr(GK_CTL_SCRATCH);
    scratch[0] = 0;
    long r0w = gk_run_here(user_write_here, (void *)scratch);  // writes 0x5a
    int host_sees = scratch[0] == 0x5a;
    long r0r = gk_run(read_byte, (void *)scratch);
    // The process survived every fault: ring 3 still runs normally.
    long cpl = gk_run_user(user_cpl, NULL);
    ok14 = ok14 && ca && wa == GK_EFAULT && fa == pool && r0w == 0x5a && host_sees &&
           r0r == 0x5a && cpl == 3;
    printf("gk control data: ring-3 writes faulted %d/%d (base root), under arena -> %s "
           "(fault %#lx == arena structs %#lx); ring-0 write -> %#lx, host sees %s, ring-0 "
           "read -> %#lx; ring 3 afterwards at CPL %ld [%s]\n", faulted, GK_CTL_COUNT,
           wa == GK_EFAULT ? "FAULT" : "WROTE", fa, pool, r0w, host_sees ? "0x5a" : "WRONG", r0r,
           cpl, ok14 ? "OK" : "FAIL");
  }

  // ---- the host-frame wall ---------------------------------------------------
  // A ring-3 turn on the caller's stack cannot write the host frames above its
  // entry (it faults, the process survives), while it can still read them,
  // write TLS and use its own stack; ring 0 is unaffected. On the main thread
  // and on a pthread, whose TLS block the wall must stop below.
  int ok16 = wall_check("main thread") == 1;
  {
    pthread_t t; void *rv = NULL;
    pthread_create(&t, NULL, wall_thread, NULL);
    pthread_join(t, &rv);
    if ((uintptr_t)rv != 1) ok16 = 0;
  }

  // ---- the wall's flush, skipped when provably unnecessary ------------------
  // Consecutive same-depth ring-3 turns raise the wall without a TLB flush,
  // and it holds regardless; a deeper turn or a guest run in between makes the
  // next raise flush again (see wall_skip_check). On the main thread and on a
  // pthread, whose vCPU is fresh or reused and must flush first.
  int ok19 = wall_skip_check("main thread") == 1;
  {
    pthread_t t; void *rv = NULL;
    pthread_create(&t, NULL, wall_skip_thread, NULL);
    pthread_join(t, &rv);
    if ((uintptr_t)rv != 1) ok19 = 0;
  }

  // ---- a host-side decommit under an installed PTE ---------------------------
  // A page whose backing the host pulls without the guest seeing it faults at
  // the page and leaves the vCPU usable for the next turn (see
  // host_decommit_check). On the main thread and on a pthread.
#if GK_TEST_HOST_DECOMMIT
  int ok17 = host_decommit_check("main thread") == 1;
  {
    pthread_t t; void *rv = NULL;
    pthread_create(&t, NULL, host_decommit_thread, NULL);
    pthread_join(t, &rv);
    if ((uintptr_t)rv != 1) ok17 = 0;
  }
#else
  int ok17 = 1;
  printf("host-side decommit: a stage-2 page does not follow a host-side remap on macOS; "
         "skipped [OK]\n");
#endif

  // ---- an mprotect reflected in place ----------------------------------------
  // A forwarded mprotect that leaves a committed range readable rewrites the
  // PTEs the guest already has for it rather than dropping them: the new
  // protection takes effect (a page made read-only faults on write; made
  // writable again, it takes the write) with no demand fault to re-derive
  // what the syscall itself just set. The first run commits and touches the
  // page; every run after it must add no demand fault, except the write to
  // the read-only page, which is a genuine fault.
  int ok18 = 0;
  {
    gk_arena *m = gk_arena_create(4096);
    struct ap p = {gk_arena_base(m), 0x5a};
    gk_stats s0, s1, s2, s3, s4;
    gk_arena_enter(m);
    long r0 = gk_run(commit_write, &p);   // RW commit, first touch: faults in
    gk_get_stats(&s0);
    long r1 = gk_run(commit_write, &p);   // RW -> RW: the PTE stays
    gk_get_stats(&s1);
    long r2 = gk_run(protect_read, &p);   // RW -> R: the read hits the rewritten PTE
    gk_get_stats(&s2);
    p.val = 0x3c;
    long r3 = gk_run(write_byte, &p);     // the read-only page holds
    unsigned long f3 = gk_fault_addr();
    gk_get_stats(&s3);
    long r4 = gk_run(commit_write, &p);   // R -> RW: the write hits in place
    gk_get_stats(&s4);
    gk_arena_enter(NULL);
    gk_arena_destroy(m);
    long d1 = s1.demand_faults - s0.demand_faults, d2 = s2.demand_faults - s1.demand_faults,
         d4 = s4.demand_faults - s3.demand_faults;
    ok18 = r0 == 0x5a && r1 == 0x5a && d1 == 0 && r2 == 0x5a && d2 == 0 &&
           r3 == GK_EFAULT && f3 == (unsigned long)p.page && r4 == 0x3c && d4 == 0;
    printf("mprotect in place: RW->RW %ld (%ld faults), RW->R read %ld (%ld faults), "
           "write to R -> %ld at %#lx, R->RW write %ld (%ld faults) [%s]\n",
           r1, d1, r2, d2, r3, f3, r4, d4, ok18 ? "OK" : "FAIL");
  }

  // ---- global pages across a root switch -------------------------------------
  // Alternating turns for two arenas on one thread stay isolated, a shared
  // runtime page's translation survives the switch while an arena page's does
  // not, and gk's own flush drops global entries too (see global_check).
  int ok20 = global_check() == 1;

  // ---- demand-fault neighborhoods --------------------------------------------
  // One fault maps the aligned 64 KiB window around its page; window, VMA,
  // arena and class boundaries hold (see neighborhood_check).
  int ok21 = neighborhood_check() == 1;

  // ---- host-allocated protection keys ----------------------------------------
  // A key pkey_alloc'ed on the host, unseen by gk, is virtualized and enforced
  // like one allocated in the guest (see host_key_check).
  int ok22 = host_key_check() == 1;

  // ---- transparent huge pages behind an arena --------------------------------
  // A range committed in one piece is host-backed by 2MB pages on its first
  // touch from the guest, and stays eligible across a V8-style decommit and
  // recommit (see thp_check).
  int ok23 = thp_check() == 1;

  // ---- AVX-512 state in the guest --------------------------------------------
  // An instruction that needs every AVX-512 state component runs at ring 3
  // and ring 0 and computes the right result, on a CPU that has AVX-512 (see
  // avx512_check).
#if defined(__x86_64__)
  int ok24 = avx512_check() == 1;
#else
  int ok24 = 1;  // XCR0 and AVX-512 are x86-64 state; nothing to check here
#endif

  // ---- arena nesting and the restore path ------------------------------------
  // gk_arena_leave restores the arena gk recorded; a bogus or stale pointer
  // cannot select a root, and the nesting bound holds (see nesting_check).
  int ok25 = nesting_check() == 1;

  // ---- mapping-change policy --------------------------------------------------
  // With a filter applying gk_mapping_syscall_ok, a ring-3 turn cannot change
  // the mappings of the runtime image, another arena or gk's control memory,
  // while its own arena's pages still commit and decommit (see
  // mapping_guard_check).
  int ok26 = mapping_guard_check() == 1;

  // ---- mremap and the guest's translations ------------------------------------
  // A range the guest moves with mremap leaves no translation behind at its old
  // address (see mremap_reflect_check).
  int ok27 = mremap_reflect_check() == 1;

  // ---- host memory ------------------------------------------------------------
  // Memory from gk_host_alloc is the host's to use and faults for the guest at
  // every ring, inside and outside an arena, and the allocator recycles blocks
  // without growing the supervisor/refuse registry (see host_memory_check).
  int ok28 = host_memory_check() == 1;

  // ---- host slots -------------------------------------------------------------
  // A handle from gk's host slot table resolves to its block only while the
  // slot is live under that generation; anything else resolves to nothing with
  // nothing read through it, and the table's capacity bound holds (see
  // host_slot_check).
  int ok29 = host_slot_check() == 1;

  // ---- hostcalls --------------------------------------------------------------
  // A ring-3 guest calls a registered host handler by index and gets its
  // result synchronously; an index that names no handler fails with nothing
  // called; arguments cross through a region gk owns, bounded by gk, and
  // never as a pointer the handler dereferences (see hostcall_check).
  int ok30 = hostcall_check() == 1;

  // ---- signal fields ----------------------------------------------------------
  // A 4-byte word in an arena that a ring-3 turn registers through a hostcall
  // is written from the host, and from a signal handler interrupting the
  // turn, through a descriptor validated host-side and kept in supervisor
  // memory; bad descriptors are refused with nothing recorded, and the write
  // path takes no lock and allocates nothing (see signal_field_check).
  int ok31 = signal_field_check() == 1;

  // ---- thread words -----------------------------------------------------------
  // Per-thread words in supervisor memory for state the host or a signal
  // handler must read from memory the guest cannot write: validated keys, set
  // and read on the host and from a signal handler interrupting a ring-3
  // turn, unreachable from ring 3 (see thread_word_check).
  int ok32 = thread_word_check() == 1;

  // ---- the handler flag -------------------------------------------------------
  // The embedder's handler runs with the flag set, in the thread's record and
  // not where ring 3 could clear it; a nested bracketed call leaves it set,
  // and a ring-3 turn is answered no (see handler_flag_check).
  int ok37 = handler_flag_check() == 1;

  // ---- the report budget ------------------------------------------------------
  // What a refusal path may say is capped per site and per second, counted in
  // the control block where ring 3 cannot reset it; the first report at a
  // site always passes and the ones held back are handed to the next one that
  // does; a call from a ring-3 turn decides nothing (see log_budget_check).
  int ok36 = log_budget_check() == 1;

  // ---- the turn directory ----------------------------------------------------
  // A ring-3 turn's path syscalls resolve beneath the directory a hostcall
  // handler set for it, or are refused; a descriptor reused for another
  // directory is refused by identity; a new entry starts with none (see
  // turn_dir_check).
  int ok33 = turn_dir_check() == 1;

  // ---- ring-0 resolve ---------------------------------------------------------
  // A ring-3 turn resolves a handle against gk's table in supervisor memory
  // at ring 0 inside the guest, with no VM exit, in tens of nanoseconds; a
  // handle gk did not issue, or issued for an entry since unregistered or
  // recycled, resolves to nothing with nothing read through it; the table is
  // unreachable from ring 3 and its capacity bound holds (see resolve_check).
  int ok34 = resolve_check() == 1;

  // ---- vCPU runners -----------------------------------------------------------
  // With GK_RUNNER set, every KVM_RUN above went through a runner thread and
  // nothing above could tell the difference; without it, no runner exists.
  // The gk_stats counts say which happened.
  {
    gk_stats st;
    gk_get_stats(&st);
    const char *rv = getenv("GK_RUNNER");
    int want = rv && *rv && strcmp(rv, "0") != 0;
    int resident = rv && strcmp(rv, "resident") == 0;
    int ok35 = want ? (st.runners >= 1 && st.runner_runs > 0) : (st.runners == 0 && st.runner_runs == 0);
    // GK_RUNNER=resident: the vCPUs parked inside KVM_RUN between turns and
    // turns ended there rather than through a return to userspace (the ones
    // that faulted had to return, and so did every demand fault and
    // forwarded syscall inside a turn, which is where most of the KVM_RUNs
    // above come from); without it, no turn parked.
    if (resident) ok35 = ok35 && st.resident_vcpus >= 1 && st.park_turns > 1000;
    else ok35 = ok35 && st.resident_vcpus == 0 && st.park_turns == 0 && st.park_kicks == 0;
    printf("vCPU runners: %s, %d started, %ld KVM_RUNs through them; %d resident vCPUs, %ld turns "
           "ended in a park loop, %ld kicks [%s]\n",
           want ? (resident ? "resident" : "requested") : "not requested", st.runners,
           st.runner_runs, st.resident_vcpus, st.park_turns, st.park_kicks, ok35 ? "OK" : "FAIL");
    ok34 = ok34 && ok35;
  }

  int all = ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && ok10 && ok11 &&
            ok12 && ok13 && ok14 && ok15 && ok16 && ok17 && ok18 && ok19 && ok20 && ok21 &&
            ok22 && ok23 && ok24 && ok25 && ok26 && ok27 && ok28 && ok29 && ok30 && ok31 &&
            ok32 && ok33 && ok34 && ok36 && ok37;
  printf("\n%s\n", all ? "PASS" : "FAIL");
  return all ? 0 : 1;
}
