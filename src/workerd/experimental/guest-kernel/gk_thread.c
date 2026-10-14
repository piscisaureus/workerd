// gk: thread records and the vCPU pool.
#include "gk_internal.h"

// ---- thread records ----------------------------------------------------------

// The calling thread's record, or NULL if it has none yet. tls_rec is user
// memory, so the record it names is accepted only if it belongs to this thread
// by kernel tid; anything else is tampering and fatal. Called at every host-side
// entry into gk (a gk_run, an arena switch, a thread ending); the KVM-exit
// path passes the record along instead.
static gk_thread *thread_cur(void) {
  int i = tls_rec;
  if (i == 0) return NULL;
  if (i < 1 || i > GK_MAX_VCPUS || G->threads[i - 1].tid != gettid_raw())
    gk_fatal("thread record does not belong to the calling thread");
  return &G->threads[i - 1];
}

// Take a record from the pool for the calling thread (caller holds G->lock).
// NULL if the pool is exhausted.
static gk_thread *thread_alloc_locked(long tid) {
  gk_thread *t = G->thread_free;
  if (t) G->thread_free = t->next_free;
  else if (G->thread_n < GK_MAX_VCPUS) {
    // A record taken for the first time gets its hostcall frame and region
    // now (see the hostcalls section); both stay with it when it is recycled.
    t = &G->threads[G->thread_n];
    if (hostcall_record_init_locked(t) < 0) return NULL;
    G->thread_n++;
  } else return NULL;
  gk_hostcall_frame *frame = t->hostcall_frame;
  memset(t, 0, sizeof *t);
  t->hostcall_frame = frame;
  t->tid = tid;
  t->active_pml4 = G->pml4;
  return t;
}

// Bind `t` (taken from the pool) to the calling thread. `host` says the thread
// is one that ends through pthread exit (and so gets the key destructor), as
// opposed to a guest-created thread, which ends through its exit syscall.
static void thread_bind(gk_thread *t, int host) {
  tls_rec = (int)(t - G->threads) + 1;
  tls_hostcall_region = hostcall_region_of(t);
  if (host) pthread_setspecific(G->thread_key, t);
}


// The calling thread's record, created if it has none. NULL if the pool is
// exhausted.
static gk_thread *thread_get(void) {
  gk_thread *t = thread_cur();
  if (t) return t;
  pthread_mutex_lock(&G->lock);
  t = thread_alloc_locked(gettid_raw());
  pthread_mutex_unlock(&G->lock);
  if (!t) {
    if (G->dbg) fprintf(stderr, "[gk] thread record pool exhausted\n");
    return NULL;
  }
  thread_bind(t, 1);
#if GK_HVF
  stack_refresh();
#endif
  return t;
}

// Select `a` (NULL: the base root) as the active arena of the thread `t`
// belongs to. `a` is NULL or an arena the caller has established as live: the
// public entry points validate (arena_live_locked), the history restores what
// was validated when it was pushed, and the teardown paths pass NULL.
static void arena_select(gk_thread *t, gk_arena *a) {
  gk_arena *prev = t->active_arena;
  if (prev == a) return;
  if (a) __atomic_fetch_add(&a->active_threads, 1, __ATOMIC_SEQ_CST);
  t->active_arena = a;
  t->active_pml4 = a ? a->pml4 : G->pml4;
  if (prev) __atomic_fetch_sub(&prev->active_threads, 1, __ATOMIC_SEQ_CST);
}

// Whether `a` names a live arena: one of the structs in gk's own pool, at a
// slot the pool has handed out, that is currently registered. The pool lives in
// the supervisor control block, so the pointer is checked against the pool's
// bounds before anything is read through it: a pointer into memory gk does not
// own is refused unread, and a pool struct that is free, or belongs to an arena
// that has been destroyed, is refused by its flag. Caller holds G->lock.
static int arena_live_locked(const gk_arena *a) {
  uintptr_t p = (uintptr_t)a, pool = (uintptr_t)&G->arena_pool[0];
  if (p < pool || p >= pool + sizeof G->arena_pool) return 0;
  if ((p - pool) % sizeof(gk_arena) != 0) return 0;
  if ((p - pool) / sizeof(gk_arena) >= (size_t)G->arena_pool_n) return 0;
  return a->registered;
}

static void runner_stop(struct gk_runner *r);

// Return the thread's vCPU (and guest stack, if any) to the pool.
static void park_leave(gk_thread *t);
static void vcpu_park(gk_thread *t) {
  if (!t->inited) return;
  // A pooled vCPU is out of KVM_RUN: the next thread to take it programs it
  // by ioctl, which would wait behind a run in progress.
  park_leave(t);
#if GK_HVF
  // An HVF vCPU belongs to the thread that created it and cannot be run or
  // destroyed by another, so it goes with the thread; its id, vcb and stacks
  // are pooled and the next thread creates a vCPU around them (vcpu_init).
  hv_vcpu_destroy(t->vcpu);
#endif
  pthread_mutex_lock(&G->lock);
  if (G->parked_n < GK_MAX_VCPUS) {
    gk_parked_vcpu *p = &G->parked[G->parked_n++];
#if !GK_HVF
    p->fd = t->fd;
#endif
    p->id = t->id;
#if !GK_HVF
    p->run = t->run;
#endif
    p->stack_top = t->stack_top;
    p->ist = t->ist;
    p->side_stack = t->side_stack;
    p->runner = t->runner;
    p->park = t->park;
    p->efd = t->efd;
    p->stats_fd = t->stats_fd;
  } else {  // cannot happen with KVM_CAP_MAX_VCPUS <= GK_MAX_VCPUS; be safe
    if (t->runner) runner_stop(t->runner);
    if (t->efd >= 0) close(t->efd);
    if (t->stats_fd >= 0) close(t->stats_fd);
#if !GK_HVF
    munmap(t->run, G->run_size);
    close(t->fd);
#endif
  }
  pthread_mutex_unlock(&G->lock);
  if (G->dbg) fprintf(stderr, "[gk] vcpu %d parked\n", t->id);
  t->inited = 0;
  t->runner = NULL;
  t->park = NULL;
  t->efd = t->stats_fd = -1;
}

// The thread `t` belongs to is ending: park its vCPU, drop it from its active
// arena's user count, and return the record to the pool.
static void thread_release(gk_thread *t) {
  vcpu_park(t);
  arena_select(t, NULL);
  t->arena_depth = 0;
  tls_rec = 0;
  tls_hostcall_region = NULL;
  // The next thread to take the record starts with an empty region and an
  // empty host frame: no argument, result or handler state carries over.
  memset(hostcall_region_of(t), 0, sizeof(gk_hostcall_frame));
  memset(t->hostcall_frame, 0, sizeof *t->hostcall_frame);
  __asm__ __volatile__("" : : "r"(t->hostcall_frame) : "memory");
#if GK_HVF
  stack_refresh();
#endif
  pthread_mutex_lock(&G->lock);
  t->tid = 0;
  t->next_free = G->thread_free;
  G->thread_free = t;
  pthread_mutex_unlock(&G->lock);
}

// pthread key destructor: a host thread with a record is ending. (Guest-created
// threads end through the exit syscall in forward_syscall instead, which
// releases explicitly.) The key's value is not trusted; the record is found and
// checked the usual way.
static void thread_key_dtor(void *p) {
#if GK_HVF
  // macOS may have finalized the thread's __thread storage (tls_rec) before
  // this destructor runs, so the key's value is checked the same way instead:
  // a record in the pool that this thread, by kernel tid, owns.
  gk_thread *t = (gk_thread *)p;
  if (t < G->threads || t >= G->threads + GK_MAX_VCPUS ||
      ((uintptr_t)t - (uintptr_t)G->threads) % sizeof *t != 0 || t->tid != gettid_raw())
    gk_fatal("thread record does not belong to the ending thread");
  thread_release(t);
#else
  (void)p;
  gk_thread *t = thread_cur();
  if (t) thread_release(t);
#endif
}

#if !GK_HVF
// ---- vCPU runners ----------------------------------------------------------
// The thread that returns from KVM_RUN pays for the return. The kernel's
// VMSCAPE mitigation ("IBPB before exit to userspace", the default on an
// affected CPU) issues an IBPB on the first return to userspace after a VM
// exit on that CPU, which drops the CPU's branch predictions: every branch
// site the code after it runs through mispredicts until it is trained again,
// and with the predictor cold the front end fetches past taken branches and
// misses the instruction caches as well. A turn ends in exactly such a
// return, so on the thread that enters the guest the host side of a request
// (the response, the next request, the dispatch into the next turn) runs on
// a cold front end every time: measured on an EPYC 9255, at a quarter of its
// native instruction rate (see measure/findings.md).
//
// A runner takes the return. It is a host thread that does nothing but the
// KVM_RUN ioctl for one vCPU: the thread that owns the vCPU hands it the run,
// waits, and gets the result and errno back, exactly as if it had made the
// ioctl itself (KVM lets any thread of the process run a vCPU; the guest's
// registers, stack and TLS are the owning thread's whatever thread issues
// the ioctl, and the exit is handled by the owner as before). The IBPB then
// lands on the runner's CPU, after the exit, where it costs the runner's few
// dozen handoff instructions; the owner, on its own CPU, keeps its
// predictions across the turn. The guest itself still enters cold -- its
// predictions were on the runner's CPU, flushed at the previous return --
// so the runner recovers the host side's share of the cost, not the guest's.
//
// The handoff is a pair of sequence words: the owner bumps `go`, the runner
// echoes it in `done` when the ioctl has returned. Each side waits for the
// other's word to change by spinning G->runner_spin iterations and then
// sleeping on the word's futex; the other side wakes it only if it said it
// was sleeping (the flag and the word are both sequentially consistent, so
// a sleeper that checked the word after setting its flag is seen by a waker
// that checked the flag after writing the word). With an unbounded spin the
// owner's CPU is busy for the whole turn, and the runner's between turns;
// with none, each handoff costs a scheduler wakeup.
//
// GK_RUNNER=1 in the environment gives every vCPU a runner (GK_RUNNER_SPIN:
// the spin budget per wait, -1 for unbounded, default 0; GK_RUNNER_PIN:
// "sibling" pins the owner to the CPU it is on when its vCPU is created and
// the runner to that CPU's SMT sibling, "other" pins the owner likewise and
// the runner to the next CPU on another core that shares the owner's
// last-level cache, a number pins the runner to that CPU). The owner and the
// vCPU hand the turn's working set to each other through the cache, so the
// two must share one: across last-level caches (the CCDs of an EPYC) the
// same hello request costs 50 us against 28 (see measure/findings.md). The
// runner and its record follow the vCPU through the pool, and
// stop when the vCPU is closed. The records live in refused memory, which
// no root maps, so a turn cannot rewrite the words its owner waits on.
//
// Resident vCPUs. GK_RUNNER=resident keeps the runner inside KVM_RUN across
// turns, so that no return to userspace, and so no IBPB, happens on the
// vCPU's CPU at all on the turn's own path; the guest's predictions survive
// from one turn to the next as the owner's do. Between turns the vCPU runs
// gk's park loop (gk_park_loop in gk_asm.S) at ring 0, with GS pointing at
// its park frame: a gk_park in supervisor memory (the first page of the
// vCPU's IST region) that the host writes and ring 3 cannot reach. A turn
// starts with the owner writing the frame -- the root to load, the entry
// stack, function, argument and ring, exactly the values enter_guest puts
// in kvm_run otherwise -- and bumping `go`; the loop sees the change,
// loads the root, and SYSRETs (or jumps) into the turn. A turn ends with
// the exit sentinel reaching the vCPU's own SYSCALL entry
// (gk_park_syscall_tramp), which stores the result in the frame and echoes
// `go` into `done` instead of leaving the VM; a ring-0 turn returns into
// gk_park_exit_tramp, which does the same. The register exchange that
// kvm_run carried is therefore the frame's, and kvm_run is only read and
// written when a run has returned (regs_get and sregs_get stay correct: they
// are reached only from exit handling, with the vCPU out).
//
// Waiting. Each side spins G->runner_spin iterations before it sleeps. The
// loop sleeps with STI;HLT, which KVM services in the kernel (the VM has an
// in-kernel LAPIC for this, a split irqchip enabled at init), and raises
// `halted` first with a locked exchange; the owner, having bumped `go`,
// kicks a vCPU that says it halted with KVM_SIGNAL_MSI in NMI delivery mode
// -- an NMI wakes HLT whatever IF is, needs no EOI and leaves no interrupt
// priority state behind, and gk_exc_nmi is a bare IRETQ, so a kick that
// lands after the loop already launched (the two sides race, and either
// may win) costs the turn one return and nothing else. The owner sleeps on
// the vCPU's eventfd, and raises `owner_sleeping` first the same way; the
// loop ends a turn with an OUT to the frame's port when it sees the flag,
// which an ioeventfd turns into a signal on that eventfd without leaving
// the kernel, and the runner writes the eventfd when a run returns. So on a
// turn that makes no forwarded syscall, hostcall or demand fault, nothing
// returns to userspace on the vCPU's CPU; the exits that remain are those,
// one per event, and they are served exactly as before: the runner's ioctl
// returns, the owner handles it, the runner runs the vCPU again.
//
// What leaves the loop. A fault that ends the turn returns to userspace as
// ever (the handlers report by port), and the vCPU is then out of KVM_RUN
// with nothing parked; the next entry enters it through gk_park_entry, which
// resets GS from a register and parks from a fresh state, with the root
// the turn wants loaded through kvm_run first. A run that returns while the
// vCPU is parked (a signal) leaves it the same way: parked means the loop
// is running inside KVM_RUN, and a vCPU found returned is treated as out.
// A pooled vCPU must be out too (the next thread programs it by ioctl), as
// must one a stub is to run on: park_leave asks the loop to exit (`leave`,
// port 0xF8) and waits for the run to return. A between-turn TLB flush does
// not: it is deferred to the loop's launch path through `flush`, where it
// runs before the root load. CR3 cannot be read back from kvm_run while the
// vCPU is parked, so the host keeps the root it last had the loop load
// (cr3_shadow).
//
// The loop runs under the last turn's root, and walks it on every TLB
// miss, so that root must stay valid while the loop runs: gk_arena_destroy
// asks every parked vCPU still on the arena's root to load the base root
// before it frees the tables (`reload`, see park_reload_root), and waits
// for the loop to say it has. A vCPU out of KVM_RUN walks nothing, and is
// given its root afresh at its next entry.
//
// Limits. A vCPU whose id is above 254 gets no NMI by xAPIC physical
// address (255 is the broadcast id), and KVM's PIO bus holds at most 1000
// ioeventfds; such a vCPU falls back to a plain runner. The frame is host-written supervisor
// memory and the loop trusts it as enter_guest's register values are
// trusted today: ring-0 guest code could rewrite it, and could also load
// CR3 directly, so the loop adds no capability ring 0 does not have.
#define GK_RUNNER_STACK (256UL << 10)
#define GK_RUNNER_PIN_NONE (-1)
#define GK_RUNNER_PIN_SIBLING (-2)
#define GK_RUNNER_PIN_OTHER (-3)
#define GK_RUNNER_PIN_CPU 0  // + the CPU number
typedef struct gk_runner {
  pthread_t thread;
  int fd;                      // the vCPU it runs
  int cpu;                     // the CPU it is pinned to, or -1
  _Atomic uint32_t go, done;   // the owner bumps go; the runner echoes it in done after the ioctl
  _Atomic int owner_sleeping, runner_sleeping;
  _Atomic int stop;
  int ret, err;                // the ioctl's result and errno, valid once done == go
  void *stack;
  struct gk_park *park;        // the vCPU's park frame, if it is resident
  int efd;                     // its eventfd (see resident_wait)
  _Atomic long tid;            // the runner's kernel tid, set by the runner as it starts
} gk_runner;

// A resident vCPU's park frame (see above). The layout is gk_park_loop's in
// gk_asm.S; the asserts below pin it. All words are 8 bytes so that the loop
// reads and writes whole words.
typedef struct gk_park {
  _Atomic uint64_t go;              // host: bumped to launch the turn described below
  _Atomic uint64_t done;            // guest: go, echoed when that turn has ended
  uint64_t result;                  // guest: the turn's result
  uint64_t cr3;                     // host: the root to load first, or 0
  uint64_t rsp, fn, arg, user;      // host: the turn's entry (see enter_guest)
  uint64_t flush;                   // host: a full TLB flush to run before the turn (see flush_run)
  int64_t spin;                     // host: the loop's spin budget before it halts (G->runner_spin)
  _Atomic uint64_t halted;          // guest: about to HLT; a kick is needed
  _Atomic uint64_t owner_sleeping;  // host: sleeping on the eventfd; signal the port
  _Atomic uint64_t leave;           // host: exit KVM_RUN through PORT_PARK_LEAVE
  uint64_t port;                    // host: the ioeventfd port
  uint64_t user_gs;                 // host: the turn's GS base
  uint64_t stack;                   // host: the loop's ring-0 stack top
  _Atomic uint64_t reload;          // host: a root to load at once (see park_reload_root); guest: cleared when done
  _Atomic uint64_t cur_cr3;         // guest: the root the loop is under, written at every load it makes
  _Atomic uint64_t in_turn;         // guest: set at a launch, cleared before `done` is echoed
} gk_park;
_Static_assert(offsetof(gk_park, done) == 8 && offsetof(gk_park, result) == 16 &&
               offsetof(gk_park, cr3) == 24 && offsetof(gk_park, rsp) == 32 &&
               offsetof(gk_park, fn) == 40 && offsetof(gk_park, arg) == 48 &&
               offsetof(gk_park, user) == 56 && offsetof(gk_park, flush) == 64 &&
               offsetof(gk_park, spin) == 72 && offsetof(gk_park, halted) == 80 &&
               offsetof(gk_park, owner_sleeping) == 88 && offsetof(gk_park, leave) == 96 &&
               offsetof(gk_park, port) == 104 && offsetof(gk_park, user_gs) == 112 &&
               offsetof(gk_park, stack) == 120 && offsetof(gk_park, reload) == 128 &&
               offsetof(gk_park, cur_cr3) == 136 && offsetof(gk_park, in_turn) == 144,
               "gk_park layout is gk_park_loop's");
#define GK_PARK_PORT_BASE 0x1000  // ioeventfd ports: this + the vCPU id
#define GK_PARK_MAX_ID 254        // the largest vCPU id an xAPIC NMI can address (255 is the broadcast id)
// The IST region's layout for a resident vCPU (see vcpu_init): the frame in
// its first page, the loop's stack in the second, IST2 (the NMI) in the
// pages below the middle, IST1 (exceptions) in the pages below the TSS.
#define GK_PARK_STACK_TOP 0x2000
#define GK_IST2_TOP (32UL << 10)

static void futex_wait32(_Atomic uint32_t *w, uint32_t val) {
  host_syscall(SYS_futex, (long)w, FUTEX_WAIT_PRIVATE, (long)val, 0, 0, 0);
}

static void futex_wake32(_Atomic uint32_t *w) {
  host_syscall(SYS_futex, (long)w, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
}

// Wait until *w differs from `val`: spin the configured budget, then sleep on
// the word with `sleeping` raised for the waker to see.
static void runner_wait_change(_Atomic uint32_t *w, uint32_t val, _Atomic int *sleeping) {
  long spin = G->runner_spin;
  for (;;) {
    if (atomic_load_explicit(w, memory_order_seq_cst) != val) return;
    if (spin != 0) {
      if (spin > 0) spin--;
      cpu_relax();
      continue;
    }
    atomic_store_explicit(sleeping, 1, memory_order_seq_cst);
    if (atomic_load_explicit(w, memory_order_seq_cst) == val) futex_wait32(w, val);
    atomic_store_explicit(sleeping, 0, memory_order_seq_cst);
  }
}

// Publish a new value of *w and wake the other side if it sleeps on it.
static void runner_signal(_Atomic uint32_t *w, uint32_t val, _Atomic int *sleeping) {
  atomic_store_explicit(w, val, memory_order_seq_cst);
  if (atomic_load_explicit(sleeping, memory_order_seq_cst)) futex_wake32(w);
}

static void host_pkru_allow_all(void);

static void *runner_main(void *arg) {
  gk_runner *r = arg;
  atomic_store_explicit(&r->tid, host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0), memory_order_seq_cst);
  host_syscall(SYS_prctl, PR_SET_NAME, (long)"gk-vcpu-run", 0, 0, 0, 0);
  // KVM backs a guest page through the page tables of the thread that issues
  // KVM_RUN, under that thread's protection keys: a key the owner holds open
  // for its runs (see prepare_entry) must be open here too, or the first
  // touch of a keyed arena page fails the run with EFAULT.
  host_pkru_allow_all();
  if (r->cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(r->cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0 && G->dbg)
      fprintf(stderr, "[gk] runner for vcpu fd %d: cannot pin to CPU %d: %s\n", r->fd, r->cpu,
              strerror(errno));
  }
  uint32_t seen = 0;
  for (;;) {
    runner_wait_change(&r->go, seen, &r->runner_sleeping);
    seen = atomic_load_explicit(&r->go, memory_order_seq_cst);
    if (atomic_load_explicit(&r->stop, memory_order_seq_cst)) break;
    long ret = ioctl(r->fd, KVM_RUN, 0);
    r->err = ret < 0 ? errno : 0;
    r->ret = (int)ret;
    runner_signal(&r->done, seen, &r->owner_sleeping);
    // A resident vCPU's owner sleeps on the eventfd (see resident_wait).
    if (r->park && atomic_load_explicit(&r->park->owner_sleeping, memory_order_seq_cst)) {
      uint64_t one = 1;
      host_syscall(SYS_write, r->efd, (long)&one, sizeof one, 0, 0, 0);
    }
  }
  return NULL;
}

// The SMT sibling of `cpu` from sysfs, or -1 if it has none that can be read.
static int sibling_cpu(int cpu) {
  char path[96], buf[64];
  snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  // "a,b", "a-b" or a single "a": take the first listed CPU that is not `cpu`.
  for (char *p = buf; *p;) {
    char *end;
    long a = strtol(p, &end, 10);
    if (end == p) break;
    long b = a;
    if (*end == '-') b = strtol(end + 1, &end, 10);
    for (long c = a; c <= b; c++)
      if (c != cpu) return (int)c;
    p = (*end == ',') ? end + 1 : end;
  }
  return -1;
}

// Whether CPUs `a` and `b` share a last-level cache, from sysfs: the
// shared_cpu_list of the highest cache index `a` lists. 1 when sysfs does
// not say, so that a pinning that needs the answer still picks a CPU.
static int shares_llc(int a, int b) {
  char path[128], buf[1024];
  ssize_t n = 0;
  for (int i = 0;; i++) {
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", a, i);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) break;
    ssize_t m = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (m <= 0) break;
    n = m;
  }
  if (n <= 0) return 1;
  buf[n] = 0;
  for (char *p = buf; *p;) {
    char *end;
    long lo = strtol(p, &end, 10);
    if (end == p) break;
    long hi = lo;
    if (*end == '-') hi = strtol(end + 1, &end, 10);
    if (b >= lo && b <= hi) return 1;
    p = (*end == ',') ? end + 1 : end;
  }
  return 0;
}

// Start a runner for the vCPU `fd`, on the calling (owning) thread's behalf.
// NULL, with G->err set, if it cannot be started.
static gk_runner *runner_start(int fd) {
  pthread_mutex_lock(&G->lock);
  gk_runner *r = G->runner_n < GK_MAX_VCPUS ? &G->runner_pool[G->runner_n++] : NULL;
  pthread_mutex_unlock(&G->lock);
  if (!r) { G->err = "runner pool exhausted"; return NULL; }
  memset(r, 0, sizeof *r);
  r->fd = fd;
  r->cpu = -1;
  if (G->runner_pin == GK_RUNNER_PIN_SIBLING) {
    int here = sched_getcpu();
    if (here >= 0) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(here, &set);
      sched_setaffinity(0, sizeof set, &set);  // the owner stays where it is
      r->cpu = sibling_cpu(here);
    }
  } else if (G->runner_pin == GK_RUNNER_PIN_OTHER) {
    int here = sched_getcpu();
    if (here >= 0) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(here, &set);
      sched_setaffinity(0, sizeof set, &set);
      // The next CPU up (wrapping at the top) that is not on this core and
      // shares its last-level cache; failing that, the next on any other
      // core.
      int n = (int)sysconf(_SC_NPROCESSORS_ONLN), sib = sibling_cpu(here), other = -1;
      for (int c = (here + 1) % n; c != here; c = (c + 1) % n) {
        if (c == sib) continue;
        if (other < 0) other = c;
        if (shares_llc(here, c)) { r->cpu = c; break; }
      }
      if (r->cpu < 0) r->cpu = other;
    }
  } else if (G->runner_pin >= GK_RUNNER_PIN_CPU) {
    r->cpu = G->runner_pin - GK_RUNNER_PIN_CPU;
  }
  // The runner's stack is host-only, like the gk-loop side stack: refused
  // while still PROT_NONE, so no vCPU can fault it in as user memory.
  void *stack = mmap(NULL, GK_RUNNER_STACK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (stack == MAP_FAILED) { G->err = "mmap runner stack"; return NULL; }
  pthread_mutex_lock(&G->lock);
  prot_add((uintptr_t)stack, (uintptr_t)stack + GK_RUNNER_STACK, GK_PROT_REFUSE);
  pthread_mutex_unlock(&G->lock);
  if (mprotect(stack, GK_RUNNER_STACK, PROT_READ | PROT_WRITE) != 0) {
    G->err = "mprotect runner stack";
    return NULL;
  }
  r->stack = stack;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, stack, GK_RUNNER_STACK);
  int rc = pthread_create(&r->thread, &attr, runner_main, r);
  pthread_attr_destroy(&attr);
  if (rc != 0) { G->err = "pthread_create runner"; return NULL; }
  // Its tid, for gk_turn_cpu_clockid: published by the runner as it starts.
  while (atomic_load_explicit(&r->tid, memory_order_seq_cst) == 0) cpu_relax();
  if (G->dbg)
    fprintf(stderr, "[gk] runner for vcpu fd %d started (pinned to CPU %d, spin %ld)\n", fd, r->cpu,
            G->runner_spin);
  return r;
}

// Stop and join the runner `r`. Its stack stays registered and mapped: the
// record is not recycled.
static void runner_stop(gk_runner *r) {
  atomic_store_explicit(&r->stop, 1, memory_order_seq_cst);
  runner_signal(&r->go, atomic_load_explicit(&r->go, memory_order_seq_cst) + 1, &r->runner_sleeping);
  pthread_join(r->thread, NULL);
}

// KVM_RUN on this thread's vCPU: by its runner when it has one, else here.
// Returns the ioctl's result with errno set as the ioctl would have left it.
static int vcpu_run_ioctl(gk_thread *t) {
  gk_runner *r = t->runner;
  if (!r) return ioctl(t->fd, KVM_RUN, 0);
  uint32_t seq = atomic_load_explicit(&r->go, memory_order_seq_cst) + 1;
  runner_signal(&r->go, seq, &r->runner_sleeping);
  runner_wait_change(&r->done, seq - 1, &r->owner_sleeping);
  atomic_fetch_add_explicit(&G->runner_runs, 1, memory_order_relaxed);
  if (r->ret < 0) errno = r->err;
  return r->ret;
}

// ---- resident vCPUs (see the runner section) --------------------------------
#if defined(__x86_64__)
// Hand the runner a KVM_RUN without waiting for it: the owner then waits in
// resident_wait for whichever comes first, the run's return or the turn's
// end in the park loop.
static void resident_run_start(gk_thread *t) {
  gk_runner *r = t->runner;
  t->run_seq = atomic_load_explicit(&r->go, memory_order_seq_cst) + 1;
  runner_signal(&r->go, t->run_seq, &r->runner_sleeping);
  atomic_fetch_add_explicit(&G->runner_runs, 1, memory_order_relaxed);
}
#endif

// Whether the run the owner last handed the runner has returned.
static int resident_run_returned(const gk_thread *t) {
  return atomic_load_explicit(&t->runner->done, memory_order_seq_cst) == t->run_seq;
}

// Kick a halted resident vCPU: an NMI through the in-kernel LAPIC, by the
// vCPU's xAPIC id. KVM_SIGNAL_MSI is a VM ioctl, so it does not wait behind
// the run in progress as a vCPU ioctl would.
static void park_kick(gk_thread *t) {
  struct kvm_msi m = {0};
  m.address_lo = 0xFEE00000u | ((uint32_t)t->id << 12);
  m.data = 4u << 8;  // delivery mode NMI
  long rc = ioctl(G->vmfd, KVM_SIGNAL_MSI, &m);
  atomic_fetch_add_explicit(&G->park_kicks, 1, memory_order_relaxed);
  // Never expected once the vCPU is a destination (see vcpu_init); a kick
  // that is not delivered leaves the vCPU halted and its owner waiting, so
  // it is reported whatever the debug setting.
  if (rc != 1)
    fprintf(stderr, "[gk] vcpu %d: KVM_SIGNAL_MSI (NMI kick) -> %ld: %s\n", t->id, rc,
            rc < 0 ? strerror(errno) : "not delivered");
}

#if defined(__x86_64__)
enum { GK_EV_RUN_RETURNED, GK_EV_TURN_ENDED };

// Wait for the park loop to end the turn (`done` reaches turn_seq) or for
// the runner's ioctl to return (a userspace exit, which the caller handles),
// whichever is first. Spins the budget, then sleeps on the eventfd with
// `owner_sleeping` raised: the loop and the runner both check the flag after
// their own store (the stores are locked exchanges, so neither side's store
// can be reordered past its load of the other's), so a sleeper that found
// nothing after raising the flag is signalled by whichever of them writes
// next. A signal that arrives after the flag goes down again stays in the
// eventfd and only ends the next sleep early.
static int resident_wait(gk_thread *t) {
  gk_park *p = t->park;
  long spin = G->runner_spin;
  for (;;) {
    if (atomic_load_explicit(&p->done, memory_order_seq_cst) == t->turn_seq) return GK_EV_TURN_ENDED;
    if (resident_run_returned(t)) return GK_EV_RUN_RETURNED;
    if (spin != 0) {
      if (spin > 0) spin--;
      cpu_relax();
      continue;
    }
    atomic_store_explicit(&p->owner_sleeping, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&p->done, memory_order_seq_cst) != t->turn_seq && !resident_run_returned(t)) {
      uint64_t v;
      host_syscall(SYS_read, t->efd, (long)&v, sizeof v, 0, 0, 0);
    }
    atomic_store_explicit(&p->owner_sleeping, 0, memory_order_seq_cst);
  }
}
#endif

// Take a parked vCPU out of KVM_RUN: ask the loop to leave and wait for the
// run to return. The vCPU is then stopped after the loop's OUT, and the next
// use programs its registers (every entry does). No-op unless parked.
static void park_leave(gk_thread *t) {
  if (!t->parked) return;
  gk_park *p = t->park;
  if (!resident_run_returned(t)) {
    atomic_store_explicit(&p->leave, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&p->halted, memory_order_seq_cst)) park_kick(t);
    runner_wait_change(&t->runner->done, t->run_seq - 1, &t->runner->owner_sleeping);
    atomic_store_explicit(&p->leave, 0, memory_order_seq_cst);
  }
  t->parked = 0;
  if (G->dbg) {
    unsigned reason = t->run->exit_reason;
    int port = reason == KVM_EXIT_IO ? t->run->io.port : -1;
    fprintf(stderr, "[gk] vcpu %d: left the park loop (exit reason %u, port %#x)\n", t->id, reason,
            port);
  }
}

// Have the park loop of `t`'s vCPU load `root` now, from any thread, and
// wait until it has: the loop takes the request at its next iteration, or
// its next launch supersedes it (a launch loads the root its turn wants and
// clears the request), and neither needs a lock, so the wait is a few
// microseconds at most and may be made with G->lock held. A run that has
// returned (a signal) is not waited for: that vCPU executes nothing until
// its owner runs it again, which gives it its root afresh (see
// enter_guest). The owner's record of the root (cr3_shadow) is set to a
// value no root has, so its next sync_cr3 loads whatever it wants rather
// than finding it already loaded; that word is the owner's, and 0 is the
// one value safe to write under it from here, since the owner only ever
// compares it with a live root.
static void park_reload_root(gk_thread *t, uint64_t root) {
  gk_park *p = t->park;
  atomic_store_explicit(&p->reload, root, memory_order_seq_cst);
  if (atomic_load_explicit(&p->halted, memory_order_seq_cst)) park_kick(t);
  while (atomic_load_explicit(&p->reload, memory_order_seq_cst) != 0 && !resident_run_returned(t))
    cpu_relax();
  t->cr3_shadow = 0;
}
#else
// Hypervisor.framework runs a vCPU only on the thread that created it, so
// there are no runners and no park loop (gk_init turns GK_RUNNER off), and
// the thread records' runner fields stay NULL.
static void runner_stop(struct gk_runner *r) { (void)r; }
static void park_leave(gk_thread *t) { (void)t; }
static void park_reload_root(gk_thread *t, uint64_t root) { (void)t; (void)root; }
#endif  // !GK_HVF
