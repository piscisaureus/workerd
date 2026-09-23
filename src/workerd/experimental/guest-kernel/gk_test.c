// Tests for the gk library.
#define _GNU_SOURCE
#include "gk.h"

#include <stdint.h>

#include <errno.h>
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
        gk_arena *prev = gk_arena_enter(a);
        long r = (it & 1) ? gk_run_here(churn_use, &c) : gk_run(churn_use, &c);
        if (it % 7 != 3) gk_arena_enter(prev);  // sometimes destroy while still entered
        if (r != churn_want(&c)) {
          printf("  churn %d.%d: arena %p run -> %ld, want %ld (fault %#lx)\n", it, k,
                 (void *)base, r, churn_want(&c), gk_fault_addr());
          ok11 = 0;
        } else if (k == 1) {
          fresh_ok++;
        }
        gk_arena_destroy(a);
        if (it % 7 == 3) gk_arena_enter(prev);
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
        "vCPU pool", "protection-key ranges", "base root pointer", "syscall filter"};
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

  int all = ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && ok10 && ok11 &&
            ok12 && ok13 && ok14 && ok15;
  printf("\n%s\n", all ? "PASS" : "FAIL");
  return all ? 0 : 1;
}
