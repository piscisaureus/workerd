// Tests for the gk library.
#define _GNU_SOURCE
#include "gk.h"

#include <stdint.h>

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>

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
  long ret;
  register long r10 __asm__("r10") = 0, r8 __asm__("r8") = 0, r9 __asm__("r9") = 0;
  __asm__ __volatile__("syscall"
                       : "=a"(ret)
                       : "a"(1L), "D"(1L), "S"(msg), "d"((long)sizeof(msg) - 1),
                         "r"(r10), "r"(r8), "r"(r9)
                       : "rcx", "r11", "memory");
  return ret;
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
  return c->base + (size_t)i * c->stride + 4096 * (size_t)i;
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
  long ret;
  register long r10 __asm__("r10") = 0, r8 __asm__("r8") = 0, r9 __asm__("r9") = 0;
  __asm__ __volatile__("syscall"
                       : "=a"(ret)
                       : "a"(1L), "D"(1L), "S"(msg), "d"((long)sizeof(msg) - 1),
                         "r"(r10), "r"(r8), "r"(r9)
                       : "rcx", "r11", "memory");
  return ret;
}

// ---- guest-created threads --------------------------------------------------
// Threads created *from inside the guest* (pthread_create -> clone3/clone) must
// enter the guest themselves. The thread body reads CR0, which only works in
// ring 0: on the host it would SIGSEGV, in the guest it returns CR0 with PG set.
struct gt { int id; long n; unsigned long cr0; long sum; };

static void *guest_thread_body(void *arg) {
  struct gt *t = arg;
  unsigned long cr0;
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
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
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
  return (void *)(uintptr_t)((cr0 & (1UL << 31)) ? (uintptr_t)arg : 0);
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

// Host-side churn: a host thread enters the guest via gk_run and then ends.
static void *host_churn_thread(void *arg) {
  return (void *)(uintptr_t)gk_run(test_compute, arg);
}

// Filter: deny write (nr 1), allow everything else.
static int deny_write(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
  return nr != 1;  // 0 = deny
}

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
  pthread_attr_t a;
  void *addr; size_t size;
  if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
  int ok = pthread_attr_getstack(&a, &addr, &size) == 0;
  pthread_attr_destroy(&a);
  *lo = (uintptr_t)addr; *hi = (uintptr_t)addr + size;
  return ok;
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
  // The entry rsp is two words above the frame pointer (return address, saved
  // rbp); the wall starts at the first page boundary at or above it.
  uintptr_t entry = frame + 16;
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
  const char *stub = b.flush_invpcid ? "invpcid" : "cr4.pge toggle";

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
  mmap(page, 4096, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
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
  unsigned char *p3 = gk_arena_base(a), *p0 = p3 + 4096;
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
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
  if (arg) *(unsigned long *)arg = cs;
  return (long)(cs & 3);
}

// Reads CR3 -- a privileged instruction, so #GP at ring 3. (A read, not a
// write, so if isolation were broken and it ran at ring 0 it would be
// harmless.)
static long user_read_cr3(void *arg) {
  (void)arg;
  unsigned long v;
  __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
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
  long pid;
  register long r10 __asm__("r10") = 0, r8 __asm__("r8") = 0, r9 __asm__("r9") = 0;
  __asm__ __volatile__("syscall" : "=a"(pid)
                       : "a"((long)SYS_getpid), "r"(r10), "r"(r8), "r"(r9)
                       : "rcx", "r11", "memory");
  return pid;
}

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
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
  *(unsigned long *)arg = cs;
  return NULL;
}

// The well-behaved ring-3 thread: records its CS, forwards a getpid, uses libc,
// writes a user page, and spawns a grandchild that records its own CS.
static void *user_thread_body(void *arg) {
  struct ugt *u = arg;
  unsigned short cs;
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
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
  __asm__ __volatile__("leaq 1f(%%rip), %0\n\t"
                       "movq %0, %1\n\t"
                       "1: movq %%cr3, %0"
                       : "=&r"(v), "=m"(u->addr));
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
  cpu_set_t old, one;
  int pinned = sched_getaffinity(0, sizeof old, &old) == 0;
  CPU_ZERO(&one);
  CPU_SET(sched_getcpu(), &one);
  if (pinned) sched_setaffinity(0, sizeof one, &one);
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
  int cached = 0;
  for (int attempt = 0; attempt < 3 && !cached; attempt++) {
    gk_arena_enter(A);
    gk_run(read_byte, pa);
    gk_debug_drop_pte((unsigned long)pa);
    gk_get_stats(&s0);
    long v = gk_run(read_byte, pa);
    gk_get_stats(&s1);
    cached = v == 0xAA && s1.demand_faults == s0.demand_faults;
  }
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
  if (pinned) sched_setaffinity(0, sizeof old, &old);

  int ok = alt_ok && sw_ok && cached && arena_ok && survived && flush_ok && globals > 0;
  printf("global pages: %d/%d alternating turns isolated (%ld root switches); arena page cached "
         "on the same root %s, after a switch away and back %s (%ld fault, unreachable in "
         "between %s); shared page after a switch %s (%ld faults, %d attempt%s); after gk's "
         "flush %s (%ld fault, warm-up %ld); %ld global PTEs installed [%s]\n",
         rounds_ok, ROUNDS, switches, cached ? "ok" : "NOT CACHED",
         arena_ok ? "re-walked" : "STALE", arena_faults,
         xb == GK_EFAULT && fxb == (uintptr_t)pa ? "faults" : "READABLE",
         survived ? "survived" : "DROPPED", shared_faults, shared_attempts,
         shared_attempts == 1 ? "" : "s", flush_ok ? "dropped" : "SURVIVED", flush_faults,
         warm_faults, globals, ok ? "OK" : "FAIL");
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
#define NB_PAGE 4096UL
#define NB_WIN (256UL << 10)
#define NB_LEN (8 * NB_WIN + 2 * NB_PAGE)  // nb_region's mapping, guards included
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
  size_t len = usable + 2 * NB_PAGE;
  unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return NULL;
  if (mprotect(m, NB_PAGE, PROT_NONE) != 0 ||
      mprotect(m + NB_PAGE + usable, NB_PAGE, PROT_NONE) != 0) {
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
  *win = (unsigned char *)(((uintptr_t)m + NB_PAGE + 2 * NB_WIN - 1) & ~(NB_WIN - 1));
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
  int registered = s1.prot_ranges == s0.prot_ranges + 1 && s1.host_regions == 1 &&
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

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
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
    unsigned char *a2 = ab + SLOT + 4096;      // a page in A's second slot
    unsigned char *a3 = ab + 2 * SLOT + 8192;  // and one in its third
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
    printf("per-thread arenas: two threads, each isolated from the other [%s]\n",
           ok5 ? "OK" : "FAIL");
  }

  // Syscall policy: with a filter denying write, the guest's write returns
  // -EPERM instead of being forwarded.
  gk_set_syscall_filter(deny_write);
  long denied = gk_run(guest_try_write, NULL);
  gk_set_syscall_filter(NULL);
  int ok6 = (denied == -1);  // -EPERM
  printf("syscall policy: denied write returned %ld (want -1/-EPERM) [%s]\n",
         denied, ok6 ? "OK" : "FAIL");

  // Threads created from inside the guest enter the guest as their own vCPUs.
  struct gt ts[3];
  for (int i = 0; i < 3; i++) { ts[i].id = i + 1; ts[i].n = 100000; ts[i].cr0 = 0; ts[i].sum = -1; }
  long spawned = gk_run(spawn_in_guest, ts);
  int ok7 = (spawned == 1);
  for (int i = 0; i < 3; i++) {
    long want7 = 0;
    for (long j = 0; j < ts[i].n; j++) want7 += (j * 3 + ts[i].id) & 0xff;
    if (!(ts[i].cr0 & (1UL << 31)) || ts[i].sum != want7) ok7 = 0;
  }
  printf("guest-created threads (ring-0 creator): 3 threads spawned in the guest ran in ring 0 "
         "(gk_run=%ld, cr0=%#lx) [%s]\n", spawned, ts[0].cr0, ok7 ? "OK" : "FAIL");

  // Thread churn must not grow the vCPU count: 200 iterations of 3 guest-
  // spawned threads may add at most 3 vCPUs (peak concurrency), and 20
  // sequential host threads entering via gk_run may add at most 1.
  // Each guest thread's host stack is registered in the supervisor/refuse
  // registry while it lives, so the registry must not grow with the churn
  // either: at most two ranges (kvm_run, exception stack) per vCPU added.
  int before = gk_vcpu_count();
  gk_stats sp1 = {0}, sp2 = {0};
  gk_get_stats(&sp1);
  struct churn ch = {.iters = 200, .per_iter = 3, .bad = 0};
  long churned = gk_run(churn_in_guest, &ch);
  int after_guest = gk_vcpu_count();
  gk_get_stats(&sp2);
  int prot_bounded = sp2.prot_ranges - sp1.prot_ranges <= 2 * (after_guest - before);
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

  // Memory protection keys, enforced by the guest CPU against the guest PKRU.
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
        struct churn_arena c = {base, big ? (2 * SLOT / 3) & ~(size_t)0xfff : (2UL << 20),
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
      if (us[i].cs != 0x2b || us[i].child_cs != 0x2b) p1 = 0;
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
      p4 = j_own == 1 && own.cs == 0x2b && own.wrote == 0x57 && host_sees &&
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
  int ok17 = host_decommit_check("main thread") == 1;
  {
    pthread_t t; void *rv = NULL;
    pthread_create(&t, NULL, host_decommit_thread, NULL);
    pthread_join(t, &rv);
    if ((uintptr_t)rv != 1) ok17 = 0;
  }

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
  int ok24 = avx512_check() == 1;

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

  int all = ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && ok10 && ok11 &&
            ok12 && ok13 && ok14 && ok15 && ok16 && ok17 && ok18 && ok19 && ok20 && ok21 &&
            ok22 && ok23 && ok24 && ok25 && ok26 && ok27 && ok28 && ok29;
  printf("\n%s\n", all ? "PASS" : "FAIL");
  return all ? 0 : 1;
}
