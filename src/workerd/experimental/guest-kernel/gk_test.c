// Tests for the gk library.
#define _GNU_SOURCE
#include "gk.h"

#include <stdint.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

// Reads the first byte at the address given as arg, and returns it. Used to
// probe arena isolation: reachable memory returns its byte; unreachable memory
// faults and gk_run returns GK_EFAULT.
static long read_byte(void *arg) {
  volatile unsigned char *p = arg;
  return (long)*p;
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
// Unallocated keys are rejected as the kernel rejects them; the freed key's
// page, reassigned the default key, is plainly accessible again.
static long pk_teardown(void *arg) {
  struct pk *p = arg;
  int bad = 15;
  while (bad > 0 && bad == p->key) bad--;
  if (pkey_mprotect((void *)p->page, 4096, PROT_READ | PROT_WRITE, bad) == 0 || errno != EINVAL)
    return -1;
  if (pkey_mprotect((void *)p->page, 4096, PROT_READ | PROT_WRITE, 0) != 0) return -2;
  if (pkey_free(p->key) != 0) return -3;
  pkey_set(p->key, PKEY_DISABLE_ACCESS);  // the page no longer has this key
  long v = p->page[0];
  munmap((void *)p->page, 4096);
  return v;
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
  int just_below = h.frame < caller && caller - h.frame < 4096;
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

  // Arena isolation: two isolates, each with a private cage.
  gk_arena *A = gk_arena_create(4096);
  gk_arena *B = gk_arena_create(4096);
  int ok3 = 0;
  if (A && B) {
    memset(gk_arena_base(A), 0xAA, 16);  // host fills each cage
    memset(gk_arena_base(B), 0xBB, 16);
    gk_arena_enter(A);
    long ra = gk_run(read_byte, gk_arena_base(A));  // A reads its own cage
    long rb = gk_run(read_byte, gk_arena_base(B));  // A reaches for B's cage
    gk_arena_enter(NULL);
    printf("under arena A: read A's cage -> 0x%lx, read B's cage -> %s\n",
           ra, rb == GK_EFAULT ? "FAULT" : "readable");
    if (rb == GK_EFAULT)
      printf("  (hardware blocked it; fault at 0x%lx == B's base 0x%lx)\n",
             gk_fault_addr(), (unsigned long)(uintptr_t)gk_arena_base(B));
    ok3 = (ra == 0xAA) && (rb == GK_EFAULT) &&
          (gk_fault_addr() == (uintptr_t)gk_arena_base(B));
  }
  printf("arena isolation [%s]\n", ok3 ? "OK" : "FAIL");

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
  gk_arena *X = gk_arena_create(4096), *Y = gk_arena_create(4096);
  int ok5 = 0;
  if (X && Y) {
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
  printf("guest-created threads: 3 threads spawned in the guest ran in ring 0 "
         "(gk_run=%ld, cr0=%#lx) [%s]\n", spawned, ts[0].cr0, ok7 ? "OK" : "FAIL");

  // Thread churn must not grow the vCPU count: 200 iterations of 3 guest-
  // spawned threads may add at most 3 vCPUs (peak concurrency), and 20
  // sequential host threads entering via gk_run may add at most 1.
  int before = gk_vcpu_count();
  struct churn ch = {.iters = 200, .per_iter = 3, .bad = 0};
  long churned = gk_run(churn_in_guest, &ch);
  int after_guest = gk_vcpu_count();
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
            (after_host - after_guest) <= 1 && ok8_host;
  printf("vCPU pooling: %d guest threads churned, vCPUs %d -> %d; 20 host threads "
         "churned, vCPUs -> %d [%s]\n", ch.iters * ch.per_iter, before, after_guest,
         after_host, ok8 ? "OK" : "FAIL");

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

  int all = ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && ok10;
  printf("\n%s\n", all ? "PASS" : "FAIL");
  return all ? 0 : 1;
}
