// gk: the Linux host: raw syscalls, /proc/self/maps, guest thread creation and
// syscall forwarding.
#include "gk_internal.h"
#if !GK_HVF
#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <linux/stat.h>  // struct statx

// Copies `n` bytes from a guest-supplied address into host memory, without
// faulting the host if the address is unmapped or not readable: the kernel
// does the access and reports failure instead. Returns 0, or -1 if any byte
// could not be read.
static int copy_in(void *dst, uintptr_t src, size_t n) {
  struct iovec local = {dst, n}, remote = {(void *)src, n};
  long pid = host_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
  long r = host_syscall(SYS_process_vm_readv, pid, (long)&local, 1, (long)&remote, 1, 0);
  return r == (long)n ? 0 : -1;
}

// Parse one /proc/self/maps line ("start-end perms ...") into its bounds and
// protection bits (bit0=r, bit1=w, bit2=x). Returns 0 if the line is malformed.
static int parse_maps_line(const char *l, const char *end, uintptr_t *s,
                           uintptr_t *e, int *perms) {
  uintptr_t v[2] = {0, 0};
  for (int k = 0; k < 2; k++) {
    int n = 0;
    while (l < end) {
      char c = *l;
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else break;
      v[k] = (v[k] << 4) | (uintptr_t)d;
      l++; n++;
    }
    if (n == 0 || l >= end || *l != (k == 0 ? '-' : ' ')) return 0;
    l++;
  }
  if (l + 3 > end) return 0;
  int m = 0;
  if (l[0] == 'r') m |= 1;
  if (l[1] == 'w') m |= 2;
  if (l[2] == 'x') m |= 4;
  *s = v[0]; *e = v[1]; *perms = m;
  return 1;
}


// Look up the host mapping containing `page` in /proc/self/maps. V8 reserves
// huge PROT_NONE regions and commits sub-ranges; only committed pages may be
// mapped. On success sets [*rs, *re) to the mapping's bounds and *perms to its
// protection (bit0=r, bit1=w, bit2=x) and returns 1; returns 0 if `page` is
// not mapped.
//
// This runs on the fault path, so it uses raw syscalls and a stack buffer only:
// stdio would call malloc, and the faulting guest thread may be inside malloc
// holding its arena lock, which the same host thread could then never take.
static int host_region(uintptr_t page, uintptr_t *rs, uintptr_t *re, int *perms) {
  long fd = host_syscall(SYS_openat, AT_FDCWD, (long)"/proc/self/maps", O_RDONLY | O_CLOEXEC,
                         0, 0, 0);
  if (fd < 0) return 0;
  char buf[8192];
  size_t have = 0;
  int found = 0, eof = 0;
  while (!found && !eof) {
    long n = host_syscall(SYS_read, fd, (long)(buf + have), (long)(sizeof buf - have),
                          0, 0, 0);
    if (n <= 0) eof = 1;
    else have += (size_t)n;
    // Process every complete line in the buffer.
    size_t pos = 0;
    while (pos < have) {
      char *nl = memchr(buf + pos, '\n', have - pos);
      if (!nl) {
        if (!eof && pos == 0 && have == sizeof buf) have = 0;  // oversize line: drop it
        break;
      }
      uintptr_t s, e; int m;
      if (parse_maps_line(buf + pos, nl, &s, &e, &m) && page >= s && page < e) {
        *rs = s; *re = e; *perms = m;
        found = 1;
        break;
      }
      pos = (size_t)(nl - buf) + 1;
    }
    if (!found) {
      memmove(buf, buf + pos, have - pos);
      have -= pos;
    }
  }
  host_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
  return found;
}
static long gettid_raw(void) { return host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0); }


// The record of the host thread whose pthread key destructor runs, through
// tls_rec as on every other entry (thread_cur); the key's value is unused.
static gk_thread *thread_ending(void *p) {
  (void)p;
  return thread_cur();
}

// The page may lie just below a stack the kernel grows on demand (the main
// thread's, which gk_run_here runs the guest on). Natively the thread's own
// access would grow it; a guest access never reaches the host's fault
// handler, so ask the kernel to fault the page here. rt_sigprocmask with a
// NULL `set` changes nothing, but writing the old mask to `oldset` = page
// makes the kernel's copy_to_user grow a VM_GROWSDOWN stack exactly as a
// native write would (and return EFAULT, not a signal, if it cannot).
// Ordinary unmapped addresses just fail the probe. Returns 1: look again.
static int host_stack_grow(uintptr_t page) {
  host_syscall(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)page, 8, 0, 0);
  return 1;
}

// ---- huge host pages for arenas ----------------------------------------------
// The host backs the arena's pages when KVM's nested-page-
// fault handler first touches them (get_user_pages on the host VMA), and
// every such fault is a VM exit. A range the owner commits in one piece of
// GK_THP_MIN or more is advised MADV_HUGEPAGE by forward_syscall as the
// commit is reflected: the host then backs each aligned 2MB of it at once,
// and KVM installs one 2MB nested entry for it, so the range costs one exit
// per 2MB rather than one per 4KB. Such commits are ArrayBuffer backing
// stores, large object pages and semispaces. Smaller commits, in particular
// the 256KB chunks V8 grows its regular heap by, are left alone on purpose:
// a chunk-sized VMA could not take a huge page at its first touch anyway (it
// is smaller than 2MB when it faults), and the advice would only put the
// range on khugepaged's list, which collapses sparsely used 2MB ranges into
// huge pages later and inflates RSS across many isolates. For the same
// reason the reservation itself is not advised. The guest side is unchanged:
// the arena's PTEs stay 4KB, and isolation is enforced by the arena's
// private root, not by the host page size.
//
// The advice is the VMA's: it survives the mprotect splits later commits and
// decommits make, and a fixed mmap (V8's decommit, whether forwarded from the
// guest or issued by a host thread gk never sees, such as V8's background
// sweeper freeing a backing store) replaces the VMA and drops it, which is
// why every large commit is advised rather than the range once. A commit
// after such a decommit is the allocating JS thread's, hence forwarded.
// Re-advising a VMA that already carries the advice changes nothing.
// GK_NO_THP=1 in the environment disables the advice. It is a hint: it
// requires the kernel's transparent_hugepage/enabled to be "madvise" or
// "always" and is ignored otherwise (EINVAL on a kernel without THP is
// ignored too).
#define GK_THP_MIN (2UL << 20)  // the smallest commit advised: one huge page
static void arena_thp_advise(uintptr_t s, uintptr_t e) {
  if (!G->thp || e <= s || e - s < GK_THP_MIN || !arena_containing(s)) return;
  host_syscall(SYS_madvise, (long)s, (long)(e - s), MADV_HUGEPAGE, 0, 0, 0);
}

// ---- guest thread creation -------------------------------------------------
// glibc's pthread_create issues clone3 (or clone) with CLONE_VM|CLONE_THREAD|
// CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID: the kernel starts the
// child with a copy of the parent's registers, rax = 0, rsp = the new stack and
// FS base = the TLS argument. Forwarding that verbatim would start the child on
// the host, outside the guest. Instead the clone is re-issued on the host with
// the same flags, tid pointers and TLS but a private host stack and gk's own
// entry point (child_entry), so the kernel still delivers the TID to the
// parent, clears it on exit for pthread_join, and installs the TLS; only where
// the child's code runs differs: child_entry brings up a vCPU and resumes the
// guest child with exactly the register state the kernel would have given it.
//
// The child's host-side gk loop runs on the private host stack, and the guest
// child runs on the stack its clone named. The thread ends when the guest calls
// exit (see forward_syscall); the CLONE_CHILD_CLEARTID futex wake then comes
// from the kernel as usual.
#define GK_CLONE_VM 0x100UL
#define GK_CLONE_THREAD 0x10000UL
#define GK_CLONE_PARENT_SETTID 0x00100000UL
#define GK_CLONE_CHILD_CLEARTID 0x00200000UL
#define GK_CLONE_CHILD_SETTID 0x01000000UL
#define GK_SYS_clone3 435
#define GK_HOST_STACK (512UL << 10)
struct gk_clone_args {  // struct clone_args from <linux/sched.h>
  uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size,
      tls, set_tid, set_tid_size, cgroup;
};

// End the guest-created thread `t` belongs to, from its host side (which runs
// on the thread's private host stack): park its vCPU for reuse, return the
// record to the pool, release the host stack, then exit the host thread so the
// kernel's CLONE_CHILD_CLEARTID wakes joiners. `code` is the thread's exit
// status.
static void guest_thread_exit(gk_thread *t, long code) __attribute__((noreturn));
static void guest_thread_exit(gk_thread *t, long code) {
  void *hs = t->host_stack;
  size_t hs_size = t->host_stack_size;
  thread_release(t);  // t is the pool's again from here
  pthread_mutex_lock(&G->lock);
  prot_remove((uintptr_t)hs, (uintptr_t)hs + hs_size);
  pthread_mutex_unlock(&G->lock);
  gk_host_unmapself_exit(hs, hs_size, code);
}

// The child's first host-side code. `arg` is the record its parent prepared
// (clone_thread), read off the child's refused host stack, so it cannot have
// been tampered with; the record is checked to be one of the pool's all the
// same, then claimed for this thread by tid.
//
// This is the child's ring-0 side: it claims the record, brings the vCPU up
// under the parent's root and PKRU, and only then hands control to the child's
// start routine. The child runs at the privilege its creator had (see
// child_launch): a thread spawned by untrusted code thus never gains ring 0,
// and from ring 3 its syscalls, faults and privileged instructions take the
// same paths as the top-level ring-3 function's (see enter_guest).
static long child_entry(void *arg) {
  uintptr_t off = (uintptr_t)arg - (uintptr_t)G->threads;
  if ((uintptr_t)arg < (uintptr_t)G->threads || off % sizeof(gk_thread) != 0 ||
      off / sizeof(gk_thread) >= GK_MAX_VCPUS)
    gk_fatal("guest thread started with a record outside the pool");
  gk_thread *t = arg;
  t->tid = gettid_raw();
  thread_bind(t, 0);
  // The parent counted this thread as a user of its arena (see clone_thread);
  // it already holds the arena and root, so no arena_enter here.
  // The kernel already applied CLONE_SETTLS to this host thread, so vcpu_init
  // reads the guest child's TLS base straight from FS.
  if (vcpu_init(t, 0) < 0) {
    fprintf(stderr, "[gk] guest thread: %s failed\n", G->err);
    host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  }
  t->user_mode = t->start.user;  // vcpu_init resets it; the vCPU may be a reused one
  // Eager-map the whole host mapping holding the child's stack (glibc's stack
  // block, with the thread descriptor and static TLS at its top). Exception
  // delivery pushes a frame on the current stack, so the stack must be present
  // before the first fault or that push double-faults.
  uintptr_t rs, re; int perms;
  pthread_mutex_lock(&G->lock);
  int ok = host_region(GK_REGS_SP(t->start.regs) - 1, &rs, &re, &perms) && (perms & 2) &&
           mmu_map_range(rs, re, perms) == 0;
  pthread_mutex_unlock(&G->lock);
  if (!ok) {
    fprintf(stderr, "[gk] guest thread: cannot map child stack at %#llx\n",
            (unsigned long long)GK_REGS_SP(t->start.regs));
    host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  }
  // The child's vCPU comes up on the base root; a parent in an arena needs
  // its root loaded, which is done from the host here (a full flush of a
  // fresh or reused vCPU's TLB, which costs nothing worth keeping).
  uint64_t cr3;
  if (sync_cr3(t, &cr3) < 0 || (cr3 && cr3_load_host(t, cr3) < 0))
    host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  host_pkru_allow_all();
  // A clone's child starts with its parent's PKRU.
  if (guest_pkru_update(t, 0, t->start.pkru, NULL) < 0)
    host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  t->pkru_ready = 1;
  child_launch(t);
  tls_in_guest = 1;  // the thread only ever leaves the guest by exiting
  long r = run_vcpu(t);
  // A guest thread only leaves through exit/exit_group, handled in
  // forward_syscall; reaching here means it faulted or the VM shut down. The
  // fault ends this thread alone, as gk_run's caller sees GK_EFAULT for its own
  // thread: the record and vCPU are recycled and the process goes on. The
  // faulting address is left in gk_fault_addr for the host to read. Joiners
  // are woken by the kernel as for any thread exit, but the thread's start
  // routine never returned, so no return value reaches them.
  fprintf(stderr, "[gk] vcpu %d: guest thread tid %ld (ring %d) died: r=%ld fault=%#lx; "
          "thread terminated\n", t->id, t->tid, t->user_mode ? 3 : 0, r, G->fault_addr);
  guest_thread_exit(t, 0);
}

// Parent side of an intercepted thread-creating clone/clone3. `t` is the
// parent's record, `r` its register snapshot at the syscall and `args` the
// syscall's number and arguments (see syscall_args). Returns what the guest
// sees as the clone result.
static long clone_thread(gk_thread *t, gk_regs *r, const long args[7]) {
  struct gk_clone_args ca;
  uint64_t child_sp;
  size_t ca_size = 0;
  if (args[0] == GK_SYS_clone3) {
    ca_size = (size_t)args[2];
    // Need at least the fields through `tls` (CLONE_ARGS_SIZE_VER0).
    if (ca_size < 64 || args[1] == 0) return -GK_EINVAL;
    memset(&ca, 0, sizeof ca);
    if (copy_in(&ca, (uintptr_t)args[1], ca_size < sizeof ca ? ca_size : sizeof ca) < 0)
      return -GK_EADDR;
    child_sp = ca.stack + ca.stack_size;
  } else {
    child_sp = (uint64_t)args[2];
  }
  if (child_sp == 0) return -GK_EINVAL;

  // The child inherits the parent's PKRU, read from the parent vCPU now.
  uint32_t pkru = 0;
  if (guest_pkru_update(t, ~0u, 0, &pkru) < 0) return -GK_EINVAL;
  syscall_stub_ran(r);

  // The child's host-side gk-loop stack. It is refused, like the pooled side
  // stacks, and unregistered again when the thread exits (forward_syscall) since
  // it is unmapped then. It is mapped PROT_NONE until registered, so no vCPU can
  // fault it in as user memory in between, and the [arg, fn] words the child
  // pops off it below cannot be changed by a guest before it does.
  void *hs = mmap(NULL, GK_HOST_STACK, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (hs == MAP_FAILED) return -errno;
  pthread_mutex_lock(&G->lock);
  prot_add((uintptr_t)hs, (uintptr_t)hs + GK_HOST_STACK, GK_PROT_REFUSE);
  gk_thread *c = thread_alloc_locked(0);  // claimed by the child, by tid, when it starts
  pthread_mutex_unlock(&G->lock);
  if (!c || mprotect(hs, GK_HOST_STACK, PROT_READ | PROT_WRITE) != 0) {
    pthread_mutex_lock(&G->lock);
    prot_remove((uintptr_t)hs, (uintptr_t)hs + GK_HOST_STACK);
    if (c) { c->next_free = G->thread_free; G->thread_free = c; }
    pthread_mutex_unlock(&G->lock);
    munmap(hs, GK_HOST_STACK);
    return -GK_ENOMEM;
  }
  c->guest_thread = 1;
  c->host_stack = hs;
  c->host_stack_size = GK_HOST_STACK;
  c->start.regs = *r;
  child_regs_init(&c->start.regs, child_sp);
  c->start.parent_id = t->id;
  c->start.pkru = pkru;
  c->start.user = t->user_mode;   // the child runs at its creator's privilege
  // The child inherits the parent's arena and counts as a user of it from now
  // (so a destroy in between cannot recycle the tables it is about to run
  // under); child_entry finds it in place.
  gk_arena *a = t->active_arena;
  if (a) __atomic_fetch_add(&a->active_threads, 1, __ATOMIC_SEQ_CST);
  c->active_arena = a;
  c->active_pml4 = a ? a->pml4 : G->pml4;
  // gk_host_clone_raw's child pops [arg, fn] off its initial stack.
  uint64_t top = (uint64_t)(uintptr_t)hs + GK_HOST_STACK - 16;
  ((uint64_t *)(uintptr_t)top)[0] = (uint64_t)(uintptr_t)c;
  ((uint64_t *)(uintptr_t)top)[1] = (uint64_t)(uintptr_t)&child_entry;

  long tid;
  if (args[0] == GK_SYS_clone3) {
    ca.stack = (uint64_t)(uintptr_t)hs;
    ca.stack_size = GK_HOST_STACK - 16;
    tid = gk_host_clone_raw(GK_SYS_clone3, (long)&ca, (long)ca_size, 0, 0, 0);
  } else {
    // clone(flags, stack, parent_tid, child_tid or tls, tls or child_tid): the
    // order of the last two differs between architectures, but they pass through.
    tid = gk_host_clone_raw(SYS_clone, args[1], (long)top, args[3], args[4], args[5]);
  }
  if (tid < 0) {
    if (a) __atomic_fetch_sub(&a->active_threads, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&G->lock);
    prot_remove((uintptr_t)hs, (uintptr_t)hs + GK_HOST_STACK);
    c->next_free = G->thread_free;
    G->thread_free = c;
    pthread_mutex_unlock(&G->lock);
    munmap(hs, GK_HOST_STACK);
  }
  return tid;
}

// ---- hostcalls: dispatch ------------------------------------------------------
// The call side of the hostcalls section above; placed here because it runs
// under the allocation guard and on the syscall exit, beside forward_syscall.

// Runs handler `idx` for the calling thread's record `t`, from the guest's
// syscall exit or from gk_hostcall on the host side. Returns what the guest
// sees in RAX.
static long hostcall_dispatch(gk_thread *t, uint64_t idx) {
  if (idx >= GK_HOSTCALLS) return -GK_ENOSYS;
  pthread_mutex_lock(&G->lock);
  gk_hostcall_entry e = G->hostcalls[idx];
  pthread_mutex_unlock(&G->lock);
  if (!e.fn) return -GK_ENOSYS;
  int guard = GK_HANDLER_ENTER();
  // What gk_in_handler answers for the embedder's code from here on. Restored
  // rather than cleared, as the guard's bracket is: gk refuses a hostcall
  // made from inside a handler, but a leave that cleared would be wrong the
  // moment that stops being true.
  int was_handler = t->in_hostcall;
  t->in_hostcall = 1;
  // The region is the guest's; every field is read once, into the host frame,
  // and the copy is bounded by the frame, not by anything the guest wrote.
  volatile gk_hostcall_frame *g = hostcall_region_of(t);
  gk_hostcall_frame *h = t->hostcall_frame;
  uint64_t in_len = g->in_len;
  if (in_len > GK_HOSTCALL_BYTES) {
    t->in_hostcall = was_handler;
    GK_HANDLER_LEAVE(guard);
    return -GK_E2BIG;
  }
  for (int i = 0; i < 4; i++) h->args[i] = g->args[i];
  h->in_len = in_len;
  h->out_len = 0;
  memcpy(h->buf, (const void *)g->buf, in_len);
  atomic_fetch_add_explicit(&G->hostcall_calls, 1, memory_order_relaxed);
  long r = e.fn(e.ctx, h);
  uint64_t out_len = h->out_len;
  if (out_len > GK_HOSTCALL_BYTES) {
    write_stderr("[gk] hostcall: handler set out_len past the frame; the guest gets EINVAL "
                 "and no bytes\n");
    r = -GK_EINVAL;
    out_len = 0;
  }
  memcpy((void *)g->buf, h->buf, out_len);
  g->out_len = out_len;
  // The exchanged bytes leave the host frame with the call; `state` stays.
  memset(h->buf, 0, in_len > out_len ? in_len : out_len);
  memset(h->args, 0, sizeof h->args);
  h->in_len = h->out_len = 0;
  __asm__ __volatile__("" : : "r"(h) : "memory");
  t->in_hostcall = was_handler;
  GK_HANDLER_LEAVE(guard);
  return r;
}

int gk_hostcall_register(unsigned idx, gk_hostcall_fn fn, void *ctx) {
  if (idx >= GK_HOSTCALLS || !fn) return -GK_EINVAL;
  pthread_mutex_lock(&G->lock);
  if (G->hostcalls[idx].fn) { pthread_mutex_unlock(&G->lock); return -GK_EBUSY; }
  G->hostcalls[idx] = (gk_hostcall_entry){fn, ctx};
  G->hostcall_n++;
  pthread_mutex_unlock(&G->lock);
  return 0;
}

int gk_hostcall_unregister(unsigned idx) {
  if (idx >= GK_HOSTCALLS) return -GK_EINVAL;
  pthread_mutex_lock(&G->lock);
  int bound = G->hostcalls[idx].fn != NULL;
  G->hostcalls[idx] = (gk_hostcall_entry){NULL, NULL};
  if (bound) G->hostcall_n--;
  pthread_mutex_unlock(&G->lock);
  return bound ? 0 : -GK_ENOSYS;
}

gk_hostcall_frame *gk_hostcall_region(void) {
  if (tls_in_guest) return tls_hostcall_region;
  gk_thread *t = thread_get();
  return t ? hostcall_region_of(t) : NULL;
}

long gk_hostcall(unsigned idx) {
  if (tls_in_guest) {
    long ret;
#if defined(__x86_64__)
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"((long)GK_HOSTCALL_SYSCALL), "D"((long)idx)
                         : "rcx", "r11", "memory");
#else
    register long x8 __asm__("x8") = GK_HOSTCALL_SYSCALL;
    register long x0 __asm__("x0") = (long)idx;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    ret = x0;
#endif
    return ret;
  }
  gk_thread *t = thread_get();
  if (!t) return -GK_ENOMEM;
  return hostcall_dispatch(t, idx);
}

// ---- ring-0 resolve ------------------------------------------------------------
// The host side of the resolve table (see gk_resolve in gk.h and
// gk_resolve_tramp in gk_asm.S): registration and unregistration under
// G->lock, and the direct lookup that gk_resolve takes when the caller is not
// at guest ring 3. The lookup is the trampoline's, in C: the same bounds
// check, the same generation read on both sides of the pointer, nothing read
// through the handle, no lock, no write, no allocation (it runs under the
// allocation guard so the guard build checks the last).

// The arena a registration made on this thread belongs to, or NULL when the
// thread has no active arena and the base root owns the entry. Caller holds
// G->lock.
static gk_arena *resolve_arena_now(void) {
  gk_thread *t = thread_cur();
  return t ? t->active_arena : NULL;
}

// The root an entry of `a` is tagged with: the arena's root, or the base root
// for an entry no arena owns. It is the value the guest's CR3 holds while a
// turn of that arena runs (see sync_cr3), which is what the trampoline
// compares against. Caller holds G->lock.
static uint64_t resolve_root_of(const gk_arena *a) {
  return (uint64_t)(uintptr_t)(a ? a->pml4 : G->pml4);
}

// The arena an entry belongs to, from its chain word, or NULL for one the base
// root owns and for a reserved one. Caller holds G->lock.
static gk_arena *resolve_arena_of(const gk_resolve_entry *e) {
  return e->aidx ? &G->arena_pool[e->aidx - 1] : NULL;
}

// Puts entry `idx` on arena `a`'s list, or on no list when the base root owns
// it. Caller holds G->lock.
static void resolve_chain_add(int idx, gk_arena *a) {
  if (!a) return;
  gk_resolve_entry *e = &gk_resolve_table.e[idx];
  e->aidx = (uint64_t)(a - G->arena_pool) + 1;
  e->prev = 0;
  e->next = (uint64_t)a->resolve_head;
  if (a->resolve_head) gk_resolve_table.e[a->resolve_head - 1].prev = (uint64_t)idx + 1;
  a->resolve_head = idx + 1;
  a->resolve_live++;
}

// Takes entry `idx` off its arena's list, if it is on one. The list is doubly
// linked because entries are retired in any order, not the order they were
// taken in, so the cost must not depend on how many the arena holds. Caller
// holds G->lock.
static void resolve_chain_remove(int idx) {
  gk_resolve_entry *e = &gk_resolve_table.e[idx];
  gk_arena *a = resolve_arena_of(e);
  if (!a) return;
  if (e->prev) gk_resolve_table.e[e->prev - 1].next = e->next;
  else a->resolve_head = (int)e->next;
  if (e->next) gk_resolve_table.e[e->next - 1].prev = e->prev;
  e->aidx = e->prev = e->next = 0;
  a->resolve_live--;
}

// The lookup the host and guest ring 0 take. It does not compare the owner:
// the tag says which arena's ring 3 may resolve the entry, and host code
// resolves on behalf of whichever context it is serving -- the delete queue's
// drain, a teardown walk, a hostcall handler -- from threads that are often on
// no arena at all.
static void *resolve_lookup(gk_resolve_handle h, uint64_t *type) {
  if (type) *type = 0;
  uint64_t idx = h & ((1ULL << GK_RESOLVE_INDEX_BITS) - 1), gen = h >> GK_RESOLVE_INDEX_BITS;
  if (gen == 0) return NULL;
  gk_resolve_entry *e = &gk_resolve_table.e[idx];  // the mask bounds idx (see the table)
  // Acquire loads: the compiler may not reorder them, and the CPU keeps loads
  // in order, which is what makes the generation on both sides of the pointer
  // a check that the pointer was the entry's under that generation.
  uint64_t g1 = __atomic_load_n(&e->gen, __ATOMIC_ACQUIRE);
  void *p = __atomic_load_n(&e->ptr, __ATOMIC_ACQUIRE);
  uint64_t t = __atomic_load_n(&e->type, __ATOMIC_ACQUIRE);
  uint64_t g2 = __atomic_load_n(&e->gen, __ATOMIC_ACQUIRE);
#ifdef GK_NEG_RESOLVE
  (void)g1; (void)g2; (void)gen;
  if (type) *type = t;
  return p;
#else
  if (g1 != gen || g2 != gen) return NULL;
  if (type) *type = t;
  return p;
#endif
}

// Whether `ptr` may be placed in the table under an entry arena `owner` will
// own, NULL for the base root (see gk_resolve_commit in gk.h): memory that
// owner's ring 3 can already reach, which the host may therefore dereference
// on its behalf without reaching anything it could not reach itself -- so not
// NULL, not in a supervisor or refused range, and not inside an arena other
// than `owner`. The owner's own arena is admitted because that is where the
// objects it registers live; what keeps the admission from handing one
// isolate another's memory is that the entry resolves at ring 3 only under
// `owner`'s root (gk_resolve_tramp) and is retired with the arena
// (gk_arena_destroy). Caller holds G->lock.
static int resolve_ptr_ok(const void *ptr, const gk_arena *owner) {
  uintptr_t a = (uintptr_t)ptr;
  if (a == 0 || prot_class(a) != GK_PROT_KEEP) return 0;
  const gk_arena *in = arena_containing(a);
#ifdef GK_NEG_RESOLVE_ARENA
  // GK_NEG_RESOLVE_ARENA (a negative-test build, never shipped) refuses every
  // arena-resident pointer, the owner's own included, so an isolate cannot
  // register an object of its own heap and the corresponding gk_test case
  // fails.
  (void)owner;
  return in == NULL;
#else
  return in == NULL || in == owner;
#endif
}

// Takes a free entry, gives it a fresh generation and publishes it with `ptr`
// (NULL for a reservation), on arena `a`'s list unless the base root owns it.
// Returns the handle, or 0 when the table is full. A registration is owned by
// the root it is made under; a reservation is owned by no root until
// gk_resolve_commit places a pointer in it, and resolves to nothing meanwhile.
// Caller holds G->lock.
static gk_resolve_handle resolve_take(void *ptr, uint64_t owner, gk_arena *a, uint64_t type) {
  int idx;
  if (G->resolve_free) {
    idx = G->resolve_free - 1;
    G->resolve_free = (int)(uintptr_t)gk_resolve_table.e[idx].ptr;  // the link (see the table)
  } else if (G->resolve_n < GK_RESOLVE_SLOTS) {
    idx = G->resolve_n++;
  } else {
    return 0;  // the table is full: reported, never fatal
  }
  // A generation is never 0. The counter has 44 bits and wraps back to 1, so
  // generations do repeat after 2^44 registrations (see gk_resolve in gk.h).
  uint64_t gen = ++G->resolve_gen;
  if (gen >> GK_RESOLVE_GEN_BITS) gen = G->resolve_gen = 1;
  gk_resolve_entry *e = &gk_resolve_table.e[idx];
  // Publish: the type, the owner and the pointer first, the generation last
  // (see gk_resolve_tramp).
  __atomic_store_n(&e->type, type, __ATOMIC_RELEASE);
  __atomic_store_n(&e->owner, owner, __ATOMIC_RELEASE);
  __atomic_store_n(&e->ptr, ptr, __ATOMIC_RELEASE);
  __atomic_store_n(&e->gen, gen, __ATOMIC_RELEASE);
  resolve_chain_add(idx, a);
  G->resolve_live++;
  return (gen << GK_RESOLVE_INDEX_BITS) | (uint64_t)idx;
}

// The body of both registrations: the pointer check and a taken entry owned by
// arena `a`, or by the base root when `a` is NULL. Caller holds G->lock.
static gk_resolve_handle resolve_register_locked(void *ptr, gk_arena *a, uint64_t type,
                                                const char *who) {
  if (resolve_ptr_ok(ptr, a)) return resolve_take(ptr, resolve_root_of(a), a, type);
  gk_dbuf d;
  int rep = log_report(&d, GK_LOG_RESOLVE_REGISTER, who);
  if (rep) {
    db_hex(&d, (uint64_t)(uintptr_t)ptr);
    db_str(&d, " is not memory the entry's owner can reach; nothing is registered");
    log_report_end(&d, rep);
  }
  return 0;
}

gk_resolve_handle gk_resolve_register(void *ptr, uint64_t type) {
  if (!ptr || !G->initialized) return 0;
  pthread_mutex_lock(&G->lock);
  gk_resolve_handle h =
      resolve_register_locked(ptr, resolve_arena_now(), type, "gk_resolve_register");
  pthread_mutex_unlock(&G->lock);
  return h;
}

gk_resolve_handle gk_resolve_register_for(gk_arena *a, void *ptr, uint64_t type) {
  if (!ptr || !G->initialized) return 0;
  pthread_mutex_lock(&G->lock);
  gk_resolve_handle h = 0;
  if (a != NULL && !arena_live_locked(a)) {
    // As gk_arena_enter does: the pointer comes from an ordinary heap object,
    // so it is checked against gk's own pool before anything is read through it.
    gk_dbuf d;
    int rep = log_report(&d, GK_LOG_RESOLVE_REGISTER_FOR, "gk_resolve_register_for");
    if (rep) {
      db_hex(&d, (uint64_t)(uintptr_t)a);
      db_str(&d, " is not a live arena; nothing is registered");
      log_report_end(&d, rep);
    }
  } else {
    h = resolve_register_locked(ptr, a, type, "gk_resolve_register_for");
  }
  pthread_mutex_unlock(&G->lock);
  return h;
}

int gk_resolve_reserve(gk_resolve_handle *handles, int n) {
  if (!handles || n < 0 || !G->initialized) return 0;
  pthread_mutex_lock(&G->lock);
  int got = 0;
  while (got < n) {
    gk_resolve_handle h = resolve_take(NULL, 0, NULL, 0);
    if (!h) break;
    handles[got++] = h;
  }
  pthread_mutex_unlock(&G->lock);
  return got;
}

int gk_resolve_commit(gk_resolve_handle h, void *ptr, uint64_t type) {
  uint64_t idx = h & ((1ULL << GK_RESOLVE_INDEX_BITS) - 1), gen = h >> GK_RESOLVE_INDEX_BITS;
  pthread_mutex_lock(&G->lock);
  gk_resolve_entry *e = &gk_resolve_table.e[idx];  // the mask bounds idx (see the table)
  if (gen == 0 || e->gen != gen || e->ptr != NULL) {
    pthread_mutex_unlock(&G->lock);
    gk_dbuf d;
    int rep = log_report(&d, GK_LOG_RESOLVE_COMMIT, "gk_resolve_commit");
    if (rep) {
      db_hex(&d, h);
      db_str(&d, " is not the handle of a reserved resolve entry; nothing is committed");
      log_report_end(&d, rep);
    }
    return -GK_EINVAL;
  }
  // The arena the entry comes to be owned by, which is also the one the
  // pointer is admitted against: a commit runs after the turn but on the
  // thread that ran it, still holding the isolate's lock and so still on its
  // arena (ResolveTurn in util/host-memory.h).
  gk_arena *a = resolve_arena_now();
  if (!resolve_ptr_ok(ptr, a)) {
    pthread_mutex_unlock(&G->lock);
    gk_dbuf d;
    int rep = log_report(&d, GK_LOG_RESOLVE_COMMIT_PTR, "gk_resolve_commit");
    if (rep) {
      db_hex(&d, (uint64_t)(uintptr_t)ptr);
      db_str(&d, " is not memory the entry's owner can reach; ");
      db_hex(&d, h);
      db_str(&d, " stays unresolvable");
      log_report_end(&d, rep);
    }
    return -GK_EINVAL;
  }
  // The generation is already published; the owner goes first and the pointer
  // becomes visible with the store after it, which is the word the trampoline
  // gates the owner comparison on, so a lookup that saw the pointer saw the
  // owner too. A lookup that read NULL before either answered nothing.
  __atomic_store_n(&e->type, type, __ATOMIC_RELEASE);
  __atomic_store_n(&e->owner, resolve_root_of(a), __ATOMIC_RELEASE);
  __atomic_store_n(&e->ptr, ptr, __ATOMIC_RELEASE);
  resolve_chain_add((int)idx, a);
  pthread_mutex_unlock(&G->lock);
  return 0;
}

// Retires entry `idx`: it leaves its arena's list, then the generation goes
// first, so a lookup in flight fails its second read, then the pointer word,
// which becomes the free-list link (see gk_resolve_tramp and the table). The
// owner is left as it was; a free entry's generation is 0, which refuses every
// handle, and the next registration to take the entry writes its own. Caller
// holds G->lock.
static void resolve_retire_locked(int idx) {
  gk_resolve_entry *e = &gk_resolve_table.e[idx];
  resolve_chain_remove(idx);
  __atomic_store_n(&e->gen, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&e->ptr, (void *)(uintptr_t)G->resolve_free, __ATOMIC_RELEASE);
  G->resolve_free = idx + 1;
  G->resolve_live--;
}

// Retires every entry an arena being destroyed still owns. Nothing should be
// left: the runtime retires an isolate's entries with the objects that hold
// them, and the isolate outlives those. But the owner tag is a page-table root
// and the next arena is handed this one's, so an entry left behind would
// resolve at that isolate's ring 3 under a tag that is no longer its owner's
// (see gk_resolve in gk.h). Caller holds G->lock.
static void resolve_drop_locked(gk_arena *a) {
  if (!a->resolve_head) return;
#ifdef GK_NEG_RESOLVE_ORPHAN
  // GK_NEG_RESOLVE_ORPHAN (a negative-test build, never shipped) forgets the
  // arena's entries instead of retiring them, leaving them live under a root
  // that is about to be recycled, so the corresponding gk_test case fails.
  while (a->resolve_head) resolve_chain_remove(a->resolve_head - 1);
#else
  int n = 0;
  while (a->resolve_head) {
    int idx = a->resolve_head - 1;
    if (resolve_arena_of(&gk_resolve_table.e[idx]) != a) break;  // cannot happen; be safe
    resolve_retire_locked(idx);
    n++;
  }
  a->resolve_head = 0;
  a->resolve_live = 0;
  G->resolve_orphans += n;
  fprintf(stderr, "[gk] gk_arena_destroy: arena [%#lx,%#lx) still owned %d resolve entr%s; "
          "retired, so that no handle of it resolves under the arena that takes its root\n",
          (unsigned long)a->base, (unsigned long)a->end, n, n == 1 ? "y" : "ies");
#endif
}

int gk_resolve_unregister(gk_resolve_handle h) {
  uint64_t idx = h & ((1ULL << GK_RESOLVE_INDEX_BITS) - 1), gen = h >> GK_RESOLVE_INDEX_BITS;
  pthread_mutex_lock(&G->lock);
  gk_resolve_entry *e = &gk_resolve_table.e[idx];  // the mask bounds idx (see the table)
  if (gen == 0 || e->gen != gen) {
    pthread_mutex_unlock(&G->lock);
    if (h) {
      gk_dbuf d;
      int rep = log_report(&d, GK_LOG_RESOLVE_UNREGISTER, "gk_resolve_unregister");
      if (rep) {
        db_hex(&d, h);
        db_str(&d, " is not the handle of a live resolve entry; nothing is unregistered");
        log_report_end(&d, rep);
      }
    }
    return -GK_EINVAL;
  }
  resolve_retire_locked((int)idx);
  pthread_mutex_unlock(&G->lock);
  return 0;
}

// The owner check host code makes about a handle a turn supplied (see
// gk_resolve in gk.h): whether the entry is live, committed, and arena `a`'s.
// A reserved entry is owned by no root and answers no.
int gk_resolve_owned_by(gk_arena *a, gk_resolve_handle h) {
  if (!G->initialized) return 0;
  uint64_t idx = h & ((1ULL << GK_RESOLVE_INDEX_BITS) - 1), gen = h >> GK_RESOLVE_INDEX_BITS;
  pthread_mutex_lock(&G->lock);
  int ok = 0;
  if (a != NULL && !arena_live_locked(a)) {
    // As gk_arena_enter does: an arena pointer from an ordinary heap object is
    // checked against gk's pool before anything is read through it.
    gk_dbuf d;
    int rep = log_report(&d, GK_LOG_RESOLVE_OWNED_BY, "gk_resolve_owned_by");
    if (rep) {
      db_hex(&d, (uint64_t)(uintptr_t)a);
      db_str(&d, " is not a live arena; no entry is its");
      log_report_end(&d, rep);
    }
  } else if (gen != 0) {
    gk_resolve_entry *e = &gk_resolve_table.e[idx];  // the mask bounds idx (see the table)
    ok = e->gen == gen && e->ptr != NULL && e->owner == resolve_root_of(a);
  }
  pthread_mutex_unlock(&G->lock);
  return ok;
}

// The body of both re-ownings: the entry must be live and committed and must
// hold exactly `ptr` under `type`, which is what keeps a caller to entries
// naming an object it has in hand, since two live objects never share an
// address. The pointer is cleared across the owner store and put back after
// it, so a lookup in flight either sees no pointer, and answers nothing, or
// sees the pointer with the owner that goes with it (see resolve_take).
// Caller holds G->lock.
static int resolve_reown_locked(
    gk_resolve_handle h, void *ptr, uint64_t type, gk_arena *a, const char *who) {
  uint64_t idx = h & ((1ULL << GK_RESOLVE_INDEX_BITS) - 1), gen = h >> GK_RESOLVE_INDEX_BITS;
  gk_resolve_entry *e = &gk_resolve_table.e[idx];  // the mask bounds idx (see the table)
#ifdef GK_NEG_RESOLVE_REOWN
  // GK_NEG_RESOLVE_REOWN (a negative-test build, never shipped) re-owns a live
  // entry whatever it holds, so a caller can take an entry it was never given
  // by naming its handle, and the corresponding gk_test case fails.
  if (gen == 0 || e->gen != gen || e->ptr == NULL) {
#else
  if (gen == 0 || e->gen != gen || e->ptr == NULL || e->ptr != ptr || e->type != type) {
#endif
    gk_dbuf d;
    int rep = log_report(&d, GK_LOG_RESOLVE_REOWN, who);
    if (rep) {
      db_hex(&d, h);
      db_str(&d, " does not name a live resolve entry holding ");
      db_hex(&d, (uint64_t)(uintptr_t)ptr);
      db_str(&d, "; nothing is re-owned");
      log_report_end(&d, rep);
    }
    return -GK_EINVAL;
  }
#ifdef GK_NEG_RESOLVE_REOWN
  ptr = e->ptr;
#endif
  __atomic_store_n(&e->ptr, NULL, __ATOMIC_RELEASE);
  resolve_chain_remove((int)idx);
  __atomic_store_n(&e->owner, resolve_root_of(a), __ATOMIC_RELEASE);
  resolve_chain_add((int)idx, a);
  __atomic_store_n(&e->ptr, ptr, __ATOMIC_RELEASE);
  return 0;
}

int gk_resolve_disown(gk_resolve_handle h, void *ptr, uint64_t type) {
  if (!G->initialized) return -GK_EINVAL;
  pthread_mutex_lock(&G->lock);
  int r = resolve_reown_locked(h, ptr, type, NULL, "gk_resolve_disown");
  pthread_mutex_unlock(&G->lock);
  return r;
}

int gk_resolve_adopt(gk_resolve_handle h, void *ptr, uint64_t type) {
  if (!G->initialized) return -GK_EINVAL;
  pthread_mutex_lock(&G->lock);
  int r = resolve_reown_locked(h, ptr, type, resolve_arena_now(), "gk_resolve_adopt");
  pthread_mutex_unlock(&G->lock);
  return r;
}

int gk_resolve_unregister_for(gk_arena *a, gk_resolve_handle h) {
  uint64_t idx = h & ((1ULL << GK_RESOLVE_INDEX_BITS) - 1), gen = h >> GK_RESOLVE_INDEX_BITS;
  pthread_mutex_lock(&G->lock);
  if (a != NULL && !arena_live_locked(a)) {
    pthread_mutex_unlock(&G->lock);
    gk_dbuf d;
    int rep = log_report(&d, GK_LOG_RESOLVE_UNREG_ARENA, "gk_resolve_unregister_for");
    if (rep) {
      db_hex(&d, (uint64_t)(uintptr_t)a);
      db_str(&d, " is not a live arena; nothing is unregistered");
      log_report_end(&d, rep);
    }
    return -GK_EINVAL;
  }
  gk_resolve_entry *e = &gk_resolve_table.e[idx];  // the mask bounds idx (see the table)
  if (gen == 0 || e->gen != gen || e->ptr == NULL || e->owner != resolve_root_of(a)) {
    pthread_mutex_unlock(&G->lock);
    if (h) {
      gk_dbuf d;
      int rep = log_report(&d, GK_LOG_RESOLVE_UNREG_FOR, "gk_resolve_unregister_for");
      if (rep) {
        db_hex(&d, h);
        db_str(&d, " is not the handle of a live resolve entry of this arena; nothing is "
                   "unregistered");
        log_report_end(&d, rep);
      }
    }
    return -GK_EINVAL;
  }
  resolve_retire_locked((int)idx);
  pthread_mutex_unlock(&G->lock);
  return 0;
}

#if defined(__aarch64__)
// The lookup gk_resolve_tramp makes inside the guest on x86-64, in C, for
// the svc gk_resolve_typed issues on arm64 (see at_guest_ring3): answered by
// the host at the hypercall exit, with the pointer in x0 and the type in x1,
// 0 in both for a refused lookup. The same checks in the same order as the
// trampoline's: the generation on both sides of the pointer, the owner read
// inside that window, and a reserved entry refused by its missing pointer
// before the owner is looked at. The owner is compared against the root the
// calling thread's vCPU runs under, from the thread's record: supervisor
// memory, and the host alone decides which root a turn enters with, as CR3
// is on x86-64 the one piece of caller identity that is not memory. Under the
// allocation guard, as the direct lookup is.
static void resolve_answer(gk_thread *t, gk_frame *f) {
  gk_resolve_handle h = f->x[0];
  uint64_t idx = h & ((1ULL << GK_RESOLVE_INDEX_BITS) - 1), gen = h >> GK_RESOLVE_INDEX_BITS;
  void *p = NULL;
  uint64_t ty = 0;
  int guard = GK_HANDLER_ENTER();
  if (gen != 0) {
    gk_resolve_entry *e = &gk_resolve_table.e[idx];  // the mask bounds idx (see the table)
    uint64_t g1 = __atomic_load_n(&e->gen, __ATOMIC_ACQUIRE);
    p = __atomic_load_n(&e->ptr, __ATOMIC_ACQUIRE);
    uint64_t owner = __atomic_load_n(&e->owner, __ATOMIC_ACQUIRE);
    ty = __atomic_load_n(&e->type, __ATOMIC_ACQUIRE);
    uint64_t g2 = __atomic_load_n(&e->gen, __ATOMIC_ACQUIRE);
#ifndef GK_NEG_RESOLVE
    if (g1 != gen || g2 != gen) p = NULL;
#else
    (void)g1; (void)g2;
#endif
#ifndef GK_NEG_RESOLVE_OWNER
    if (p && owner != (uint64_t)(uintptr_t)t->active_pml4) p = NULL;
#else
    (void)owner;
#endif
  }
  GK_HANDLER_LEAVE(guard);
  f->x[0] = (uint64_t)(uintptr_t)p;
  f->x[1] = p ? ty : 0;
}
#endif

void *gk_resolve_typed(gk_resolve_handle h, uint64_t *type) {
  // The caller's ring (see at_guest_ring3): at guest ring 3 the lookup is
  // the trap. At guest ring 0 and on the host the table is read directly.
  if (at_guest_ring3()) {
    void *p;
#if defined(__x86_64__)
    uint64_t rdi = h, rdx;
    // The trampoline returns the entry's type in RDX, where it otherwise
    // returns 0; a refused lookup returns 0 in both.
    __asm__ __volatile__("syscall"
                         : "=a"(p), "+D"(rdi), "=d"(rdx)
                         : "a"((long)GK_RESOLVE_SYSCALL)
                         : "rcx", "r11", "rsi", "cc");
    if (type) *type = rdx;
#else
    // Answered by the host at the hypercall exit (see resolve_answer): the
    // pointer in x0 and the type in x1, 0 in both for a refused lookup.
    register uint64_t x8 __asm__("x8") = GK_RESOLVE_SYSCALL;
    register uint64_t x0 __asm__("x0") = h;
    register uint64_t x1 __asm__("x1") = 0;
    __asm__ __volatile__("svc #0" : "+r"(x0), "+r"(x1) : "r"(x8) : "memory");
    // An svc that reached the kernel instead (see at_guest_ring3) comes back
    // as -ENOSYS, which no pointer is: refused, like a handle to nothing.
    if (x0 >= (uint64_t)-4095) x0 = x1 = 0;
    p = (void *)(uintptr_t)x0;
    if (type) *type = x1;
#endif
    return p;
  }
  int guard = GK_HANDLER_ENTER();
  void *p = resolve_lookup(h, type);
  GK_HANDLER_LEAVE(guard);
  return p;
}

void *gk_resolve(gk_resolve_handle h) {
  return gk_resolve_typed(h, NULL);
}

// ---- signal fields -----------------------------------------------------------
// The table of 4-byte words in arenas that the host writes from a signal
// handler (see gk_signal_field_register in gk.h). Registration and
// unregistration are host-side and take G->lock; the write path takes no
// lock and allocates nothing, and is placed here, beside the hostcall
// dispatch, because it runs under the allocation guard too. The arena of
// every operation is the calling thread's active arena, from the thread's
// record: never a value the caller supplies.

// The calling thread's record for the lock-free path, or NULL. The thread-
// local index is a hint, accepted only if the record it names carries the
// calling thread's kernel tid; otherwise the records are scanned for the tid.
// Neither outcome is fatal: a signal handler has no caller to report to, and
// a rewritten hint must cost the writer nothing but the scan.
static gk_thread *thread_cur_lockfree(void) {
  long tid = gettid_raw();
  int i = tls_rec;
  if (i >= 1 && i <= GK_MAX_VCPUS && __atomic_load_n(&G->threads[i - 1].tid, __ATOMIC_RELAXED) == tid)
    return &G->threads[i - 1];
  int n = __atomic_load_n(&G->thread_n, __ATOMIC_ACQUIRE);
  if (n > GK_MAX_VCPUS) n = GK_MAX_VCPUS;
  for (int j = 0; j < n; j++)
    if (__atomic_load_n(&G->threads[j].tid, __ATOMIC_RELAXED) == tid) return &G->threads[j];
  return NULL;
}

int gk_in_handler(void) {
  // The caller's ring, as gk_resolve reads it. At guest ring 3 the record is
  // unreachable, and the answer there is no: a handler runs on the host side
  // of an exit, never in the guest.
  if (at_guest_ring3()) return 0;
  gk_thread *t = thread_cur_lockfree();
  return t ? t->in_hostcall : 0;
}

// The entry a handle names, or NULL if the handle is not one gk issued for a
// field that is still registered. Caller holds G->lock.
static gk_signal_field *signal_field_of_locked(gk_signal_handle h) {
  uint64_t idx = h & ((1ULL << GK_HOST_SLOT_INDEX_BITS) - 1), gen = h >> GK_HOST_SLOT_INDEX_BITS;
  if (idx >= (uint64_t)GK_SIGNAL_FIELDS) return NULL;
  gk_signal_field *f = &G->signal_fields[idx];
  if (!f->live || f->gen != gen) return NULL;
  return f;
}

// Retires entry `f` of arena `a`: clears its gate, unlinks it from the
// arena's list (the link it carries stays, see gk_signal_field), and returns
// it to the free list. Caller holds G->lock.
static void signal_field_retire_locked(gk_arena *a, gk_signal_field *f) {
  int idx = (int)(f - G->signal_fields);
  __atomic_store_n(&f->live, 0, __ATOMIC_RELEASE);
  int *prev = &a->field_head;
  for (int cur = *prev; cur; cur = *prev) {
    if (cur == idx + 1) { __atomic_store_n(prev, f->next, __ATOMIC_RELEASE); break; }
    prev = &G->signal_fields[cur - 1].next;
  }
  f->next_free = G->signal_field_free;
  G->signal_field_free = idx + 1;
  G->signal_field_live--;
}

// Unregisters every field of an arena being destroyed. Caller holds G->lock.
static void signal_fields_drop_locked(gk_arena *a) {
  while (a->field_head) {
    gk_signal_field *f = &G->signal_fields[a->field_head - 1];
    if (f->arena != a || !f->live) { a->field_head = 0; break; }  // cannot happen; be safe
    signal_field_retire_locked(a, f);
  }
}

int gk_signal_field_register(uintptr_t buffer_offset, size_t buffer_len, uint32_t field_offset,
                             unsigned kind, gk_signal_handle *handle) {
  if (handle) *handle = 0;
  if (!handle || kind >= GK_SIGNAL_KINDS || !G->initialized) return -GK_EINVAL;
  gk_thread *t = thread_cur();
  gk_arena *a = t ? t->active_arena : NULL;
  if (!a) return -GK_ESRCH;
  pthread_mutex_lock(&G->lock);
  // The buffer within the reservation, the field within the buffer, each
  // bound checked by subtraction so no sum can wrap.
  size_t span = a->end - a->base;
  if (buffer_offset > span || buffer_len > span - buffer_offset ||
      buffer_len < GK_SIGNAL_FIELD_BYTES || field_offset > buffer_len - GK_SIGNAL_FIELD_BYTES) {
    pthread_mutex_unlock(&G->lock);
    return -GK_EINVAL;
  }
  int idx;
  if (G->signal_field_free) {
    idx = G->signal_field_free - 1;
    G->signal_field_free = G->signal_fields[idx].next_free;
  } else if (G->signal_field_n < GK_SIGNAL_FIELDS) {
    idx = G->signal_field_n++;
  } else {
    pthread_mutex_unlock(&G->lock);
    return -GK_ENOMEM;  // the table is full: reported, never fatal
  }
  uint64_t gen = ++G->signal_field_gen;
  if (gen >> GK_HOST_SLOT_GEN_BITS) gen = G->signal_field_gen = 1;
  gk_signal_field *f = &G->signal_fields[idx];
  f->addr = a->base + buffer_offset + field_offset;
  f->arena = a;
  f->kind = kind;
  f->gen = gen;
  f->next = a->field_head;
  f->next_free = 0;
  // Published in this order: the entry is complete before its gate opens,
  // and reachable from the arena's list only after that.
  __atomic_store_n(&f->live, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&a->field_head, idx + 1, __ATOMIC_RELEASE);
  G->signal_field_live++;
  pthread_mutex_unlock(&G->lock);
  *handle = (gen << GK_HOST_SLOT_INDEX_BITS) | (uint64_t)idx;
  return 0;
}

int gk_signal_field_unregister(gk_signal_handle handle) {
  if (!G->initialized) return -GK_EINVAL;
  gk_thread *t = thread_cur();
  gk_arena *a = t ? t->active_arena : NULL;
  if (!a) return -GK_ESRCH;
  pthread_mutex_lock(&G->lock);
  gk_signal_field *f = signal_field_of_locked(handle);
  if (!f || f->arena != a) {
    pthread_mutex_unlock(&G->lock);
    gk_dbuf d;
    int rep = log_report(&d, GK_LOG_SIGNAL_FIELD_UNREG, "gk_signal_field_unregister");
    if (rep) {
      db_hex(&d, handle);
      db_str(&d, " is not the handle of a signal field of the calling thread's arena; nothing "
                 "is unregistered");
      log_report_end(&d, rep);
    }
    return -GK_EINVAL;
  }
  signal_field_retire_locked(a, f);
  pthread_mutex_unlock(&G->lock);
  return 0;
}

long gk_signal_field_write(unsigned kind, uint32_t value) {
  if (kind >= GK_SIGNAL_KINDS) return -GK_EINVAL;
  if (!G->initialized) return -GK_ESRCH;
  int guard = GK_HANDLER_ENTER();  // the signal may have interrupted a handler
  long n = -GK_ESRCH;
  gk_thread *t = thread_cur_lockfree();
  gk_arena *a = t ? __atomic_load_n(&t->active_arena, __ATOMIC_ACQUIRE) : NULL;
  if (a) {
    uintptr_t base = a->base, end = a->end;
    n = 0;
    int i = __atomic_load_n(&a->field_head, __ATOMIC_ACQUIRE);
    for (int steps = 0; i >= 1 && i <= GK_SIGNAL_FIELDS && steps < GK_SIGNAL_FIELDS; steps++) {
      gk_signal_field *f = &G->signal_fields[i - 1];
      int live = __atomic_load_n(&f->live, __ATOMIC_ACQUIRE);
      uintptr_t addr = __atomic_load_n(&f->addr, __ATOMIC_RELAXED);
      if (live && f->arena == a && f->kind == kind && addr >= base &&
          addr <= end - GK_SIGNAL_FIELD_BYTES) {
        memcpy((void *)addr, &value, sizeof value);  // the field need not be aligned
        n++;
      }
      i = __atomic_load_n(&f->next, __ATOMIC_ACQUIRE);
    }
  }
  GK_HANDLER_LEAVE(guard);
  return n;
}

// ---- thread words --------------------------------------------------------------
// GK_THREAD_WORDS 64-bit words per thread record (see gk.h). Both functions
// find the record lock-free, by the thread-local hint validated against the
// record's tid with a scan as the fallback (thread_cur_lockfree), so they are
// usable from a signal handler; a set on a thread that has no record yet takes
// one, which needs gk's lock, so the first set on a thread is not for a signal
// handler. The words are the owning thread's: only it and its signal handlers
// write them, so no lock is taken around the copy.
#ifdef GK_NEG_THREAD_WORDS
// Negative-test build only (never shipped): the words live in guest-writable thread-local
// storage rather than the supervisor record, so gk_test's thread_word_check shows the protection
// gone -- a ring-3 write of the words no longer faults, and a turn could change what a signal
// handler reads. The normal build keeps them in the record (below).
static __thread uint64_t neg_words[GK_THREAD_WORDS];
static uint64_t *thread_words_of(void) { return neg_words; }
#else
static uint64_t *thread_words_of(void) {
  gk_thread *t = thread_cur_lockfree();
  if (!t) t = thread_get();
  return t ? t->words : NULL;
}
#endif

int gk_thread_words_set(unsigned first, unsigned n, const uint64_t *values) {
  if (!values || n == 0 || first >= GK_THREAD_WORDS || n > GK_THREAD_WORDS - first)
    return -GK_EINVAL;
  if (!G->initialized) return -GK_ESRCH;
  uint64_t *w = thread_words_of();
  if (!w) return -GK_ENOMEM;
  for (unsigned i = 0; i < n; i++) __atomic_store_n(&w[first + i], values[i], __ATOMIC_RELAXED);
  atomic_signal_fence(memory_order_release);
  return 0;
}

int gk_thread_words_get(unsigned first, unsigned n, uint64_t *values) {
  if (!values || n == 0 || first >= GK_THREAD_WORDS || n > GK_THREAD_WORDS - first)
    return -GK_EINVAL;
  if (!G->initialized) return -GK_ESRCH;
#ifdef GK_NEG_THREAD_WORDS
  uint64_t *w = neg_words;
#else
  gk_thread *t = thread_cur_lockfree();
  if (!t) return -GK_ESRCH;
  uint64_t *w = t->words;
#endif
  atomic_signal_fence(memory_order_acquire);
  for (unsigned i = 0; i < n; i++) values[i] = __atomic_load_n(&w[first + i], __ATOMIC_RELAXED);
  return 0;
}

// ---- the turn directory ---------------------------------------------------------
// The policy gk_confine_paths turns on (see gk.h): a forwarded syscall that
// names a file by path resolves beneath the calling thread's turn directory
// or is refused. The directory is set host-side, by gk_turn_dir_set, and
// cleared at every guest entry (prepare_entry); what the guest passes as the
// dirfd must be that descriptor, and the descriptor must still name the
// directory it named when it was set (its device and inode are compared on
// every use, so a descriptor the guest closed and reused for something else
// is refused). The lookup is the kernel's: openat becomes openat2 with
// RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS, so the kernel refuses an absolute
// path, a ".." that would leave the directory, a symbolic link that would,
// and a /proc magic link, atomically with the lookup; a stat becomes an
// O_PATH open under the same rules followed by fstat or statx on the result.
// A syscall that acts on a name rather than opening it -- unlinkat, mkdirat,
// readlinkat, faccessat and faccessat2 -- has no beneath-only form, so it is
// split: its parent directory is opened beneath the turn directory under the
// same rules and the syscall is issued against that descriptor with the final
// component alone, which cannot traverse and is never followed. The split
// reads the guest's path into host memory once (confined_parent), since the
// bytes the kernel acts on must be the bytes the policy checked. Every other
// syscall gk knows to take a path is refused, as is fchdir, which would move
// the process's working directory. The policy is applied after the
// embedder's filter, which still decides what is forwarded at all; it never
// allocates.
#define GK_SYS_openat2 437
#define GK_SYS_faccessat2 439
#define GK_SYS_fchmodat2 452
#define GK_RESOLVE_NO_MAGICLINKS 0x02
#define GK_RESOLVE_BENEATH 0x08
#define GK_O_PATH 010000000
#define GK_AT_EMPTY_PATH 0x1000
#define GK_AT_SYMLINK_NOFOLLOW 0x100
#define GK_AT_EACCESS 0x200
#define GK_PATH_MAX 4096
#define GK_NAME_MAX 255
struct gk_open_how { uint64_t flags, mode, resolve; };

void gk_confine_paths(int on) { G->confine_paths = on != 0; }

int gk_turn_dir_set(int dirfd) {
  if (!G->initialized) return -GK_ESRCH;
  gk_thread *t = thread_get();
  if (!t) return -GK_ENOMEM;
  if (dirfd == GK_TURN_DIR_NONE) {
    t->turn_dir_set = 1;
    t->turn_dir = GK_TURN_DIR_NONE;
    return 0;
  }
  if (dirfd < 0) return -GK_EINVAL;
  struct stat st;
  if (host_syscall(SYS_fstat, dirfd, (long)&st, 0, 0, 0, 0) != 0) return -GK_EINVAL;
  if (!S_ISDIR(st.st_mode)) return -GK_EINVAL;
  t->turn_dir = dirfd;
  t->turn_dir_dev = st.st_dev;
  t->turn_dir_ino = st.st_ino;
  t->turn_dir_set = 1;
  return 0;
}

// Whether `nr` is a syscall that names a file by path (or changes the working
// directory) and that the policy does not rewrite: refused under confinement.
static int path_syscall_refused(long nr) {
  switch (nr) {
#if defined(__x86_64__)
    // The legacy calls that name a path outright; arm64 has only the *at forms.
    case SYS_open: case SYS_creat: case SYS_access: case SYS_stat: case SYS_lstat:
    case SYS_readlink: case SYS_rename: case SYS_unlink: case SYS_mkdir: case SYS_rmdir:
    case SYS_link: case SYS_symlink: case SYS_chmod: case SYS_chown: case SYS_lchown:
    case SYS_mknod: case SYS_utime: case SYS_utimes: case SYS_uselib: case SYS_futimesat:
#endif
    case SYS_truncate: case SYS_chdir: case SYS_fchdir: case SYS_chroot:
    case SYS_statfs: case SYS_execve:
    case SYS_acct: case SYS_swapon: case SYS_swapoff: case SYS_mount: case SYS_umount2:
    case SYS_pivot_root: case SYS_setxattr: case SYS_lsetxattr: case SYS_getxattr:
    case SYS_lgetxattr: case SYS_listxattr: case SYS_llistxattr: case SYS_removexattr:
    case SYS_lremovexattr: case SYS_quotactl: case SYS_inotify_add_watch:
    case SYS_fanotify_mark: case SYS_mknodat: case SYS_fchownat:
    case SYS_renameat: case SYS_linkat:
    case SYS_symlinkat: case SYS_fchmodat:
    case SYS_utimensat: case SYS_name_to_handle_at: case SYS_renameat2: case SYS_execveat:
    case GK_SYS_openat2: case GK_SYS_fchmodat2:
    case 428: case 429: case 430: case 433: case 442:  // open_tree, move_mount, fsopen, fspick, mount_setattr
      return 1;
    default:
      return 0;
  }
}

// The calling thread's turn directory if `dirfd` names it and it is still the
// directory it was when set, else -1 with *err the refusal.
static int turn_dir_for(gk_thread *t, long dirfd, long *err) {
  if (!t->turn_dir_set || t->turn_dir == GK_TURN_DIR_NONE || dirfd != t->turn_dir) {
    *err = -GK_EPERM;
    return -1;
  }
  struct stat st;
  if (host_syscall(SYS_fstat, t->turn_dir, (long)&st, 0, 0, 0, 0) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_dev != t->turn_dir_dev || st.st_ino != t->turn_dir_ino) {
    *err = -GK_EPERM;
    return -1;
  }
  return t->turn_dir;
}

// openat2 beneath `dir`, retried on the EAGAIN the kernel returns when a
// concurrent rename or mount made it unable to prove containment.
static long open_beneath(int dir, long path, uint64_t flags, uint64_t mode) {
  struct gk_open_how how = {flags, mode, GK_RESOLVE_BENEATH | GK_RESOLVE_NO_MAGICLINKS};
  long r = -GK_EINVAL;
  for (int i = 0; i < 8; i++) {
    r = host_syscall(GK_SYS_openat2, dir, path, (long)&how, sizeof how, 0, 0);
    if (r != -EAGAIN) break;
  }
  return r;
}

// The parent directory of the path at `path` in guest memory, opened beneath
// `dir` as an O_PATH descriptor, with the path's final component copied into
// `name` (GK_NAME_MAX + 1 bytes of host memory). The guest's string is memory
// another guest thread can rewrite under the call, so the path is read into
// host memory once -- with process_vm_readv, which answers EFAULT for an
// address the guest cannot read where a plain load would fault the host --
// and split there, and the kernel acts on the copy. A final component of ".."
// is refused with -EPERM (a syscall on it would act in the parent's parent,
// which for the turn directory itself lies outside); "." is allowed, naming
// the parent. Returns the parent descriptor, or a negative errno: the kernel's
// answer for the parent (EXDEV for one that would leave the directory), or
// -EPERM for the refused name.
static int confined_parent(int dir, long path, char *name) {
  char buf[GK_PATH_MAX];
  struct iovec local = {buf, sizeof buf}, remote = {(void *)path, sizeof buf};
  long n = host_syscall(SYS_process_vm_readv, getpid(), (long)&local, 1, (long)&remote, 1, 0);
  if (n < 0) return (int)n;
  if (n == 0) return -EFAULT;
  size_t len = strnlen(buf, (size_t)n);
  if (len == (size_t)n) return n == (long)sizeof buf ? -ENAMETOOLONG : -EFAULT;
  char *slash = strrchr(buf, '/');
  const char *parent, *base;
  if (!slash) { parent = "."; base = buf; }
  else if (slash == buf) { parent = "/"; base = buf + 1; }  // absolute: refused beneath
  else { *slash = '\0'; parent = buf; base = slash + 1; }
  size_t base_len = strlen(base);
  if (base_len == 0) return -ENOENT;
  if (base_len > GK_NAME_MAX) return -ENAMETOOLONG;
  if (base[0] == '.' && base[1] == '.' && base[2] == '\0') return -EPERM;
  memcpy(name, base, base_len + 1);
  return (int)open_beneath(dir, (long)parent, GK_O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
}

// Applies the policy to one forwarded syscall of a confined thread. Returns 1
// with *ret the result (a refusal or the rewritten syscall's result) when the
// policy decided the syscall, 0 when the syscall is not a path syscall and
// forwards as usual.
static int confined_path_syscall(gk_thread *t, long nr, long a1, long a2, long a3, long a4,
                                 long a5, long *ret) {
  long err;
  int dir;
  char name[GK_NAME_MAX + 1];
  switch (nr) {
    case SYS_openat: {
      dir = turn_dir_for(t, a1, &err);
      if (dir < 0) break;
      uint64_t flags = (uint64_t)a3;
      uint64_t mode = (flags & (O_CREAT | O_TMPFILE)) ? ((uint64_t)a4 & 07777) : 0;
      *ret = open_beneath(dir, a2, flags, mode);
      return 1;
    }
    case SYS_newfstatat: {
      dir = turn_dir_for(t, a1, &err);
      if (dir < 0) break;
      if (a4 & GK_AT_EMPTY_PATH) {
        *ret = host_syscall(SYS_fstat, dir, a3, 0, 0, 0, 0);
        return 1;
      }
      uint64_t flags = GK_O_PATH | O_CLOEXEC | ((a4 & GK_AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0);
      long fd = open_beneath(dir, a2, flags, 0);
      if (fd < 0) { *ret = fd; return 1; }
      *ret = host_syscall(SYS_fstat, fd, a3, 0, 0, 0, 0);
      host_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
      return 1;
    }
    case SYS_statx: {
      static const char empty[1] = "";
      dir = turn_dir_for(t, a1, &err);
      if (dir < 0) break;
      long sync = a3 & ~(long)(GK_AT_EMPTY_PATH | GK_AT_SYMLINK_NOFOLLOW);
      if (a3 & GK_AT_EMPTY_PATH) {
        *ret = host_syscall(SYS_statx, dir, (long)empty, sync | GK_AT_EMPTY_PATH, a4, a5, 0);
        return 1;
      }
      uint64_t flags = GK_O_PATH | O_CLOEXEC | ((a3 & GK_AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0);
      long fd = open_beneath(dir, a2, flags, 0);
      if (fd < 0) { *ret = fd; return 1; }
      *ret = host_syscall(SYS_statx, fd, (long)empty, sync | GK_AT_EMPTY_PATH, a4, a5, 0);
      host_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
      return 1;
    }
    case SYS_unlinkat:
    case SYS_mkdirat:
    case SYS_readlinkat:
    case SYS_faccessat:
    case GK_SYS_faccessat2: {
      dir = turn_dir_for(t, a1, &err);
      if (dir < 0) break;
      int pfd = confined_parent(dir, a2, name);
      if (pfd == -EPERM) { err = -GK_EPERM; break; }
      if (pfd < 0) { *ret = pfd; return 1; }
      switch (nr) {
        case SYS_unlinkat:
          *ret = host_syscall(SYS_unlinkat, pfd, (long)name, a3, 0, 0, 0);
          break;
        case SYS_mkdirat:
          *ret = host_syscall(SYS_mkdirat, pfd, (long)name, a3, 0, 0, 0);
          break;
        case SYS_readlinkat:
          *ret = host_syscall(SYS_readlinkat, pfd, (long)name, a3, a4, 0, 0);
          break;
        default: {
          // Both forms become faccessat2 with the final component unfollowed: a
          // symbolic link there is examined as itself rather than through its
          // target, which could lie outside the directory.
          long flags = (nr == GK_SYS_faccessat2 ? a4 & GK_AT_EACCESS : 0) | GK_AT_SYMLINK_NOFOLLOW;
          *ret = host_syscall(GK_SYS_faccessat2, pfd, (long)name, a3, flags, 0, 0);
          break;
        }
      }
      host_syscall(SYS_close, pfd, 0, 0, 0, 0, 0);
      return 1;
    }
    default:
      if (!path_syscall_refused(nr)) return 0;
      err = -GK_EPERM;
      break;
  }
  atomic_fetch_add_explicit(&G->path_refusals, 1, memory_order_relaxed);
  *ret = err;
  return 1;
}

// ---- the ring-3 syscall memory broker --------------------------------------
// A forwarded syscall runs on the host against the whole host address space,
// but the guest kernel confines ring-3 code to its own arena and the shared
// non-arena memory (see gk_run_user): a syscall it makes must not become a way
// to reach past that. Before forwarding a ring-3 thread's syscall, every
// pointer argument it carries -- including the ones nested in an iovec array or
// a msghdr -- is checked to name only memory the thread could reach itself (see
// guest_range_ok); a syscall whose arguments gk does not know how to check, or
// one that would act on host rather than guest state, is refused with EPERM.
// Ring-0 turns (gk_run) are gk's own trusted code and skip all of this.
//
// A pointer the host *kernel* dereferences for the syscall is safe from a bad
// address on its own: the kernel returns EFAULT, it does not fault the host
// process. The checks here are about reach, not crashes: they keep a syscall
// from reading or writing another arena, gk's control memory, or the caller's
// host-frame wall. (gk's own dereferences of guest pointers go through copy_in,
// which is fault-safe; see there.)
//
// Known gap: the array descriptors (iovec, msghdr) are read from guest memory
// both here and, moments later, by the kernel. A second guest thread in the
// same isolate could rewrite a descriptor's base/length in between, after the
// check but before the kernel reads it, to redirect a scatter/gather transfer.
// Closing this needs the descriptor marshalled into gk memory and the kernel
// pointed at that copy; the plain buffer syscalls (read, write, recvfrom, ...)
// are unaffected because their pointer and length are register arguments the
// kernel takes directly, with nothing in guest memory to race.
#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024
#endif

// A guest string argument (a path): readable for gk to scan, and bounded into
// reachable memory up to its NUL or PATH_MAX. A null pointer is refused.
static int g_str(gk_thread *t, uintptr_t s) {
  if (s == 0) return 0;
  for (uintptr_t off = 0; off <= 4096;) {
    uintptr_t p = s + off;
    uintptr_t page_end = (p & ~(GK_HOST_PAGE - 1)) + GK_HOST_PAGE;
    if (!guest_range_ok(t, p, page_end - p, 0)) return 0;
    char buf[256];
    while (p < page_end && off <= 4096) {
      size_t n = page_end - p;
      if (n > sizeof buf) n = sizeof buf;
      if (copy_in(buf, p, n) < 0) return 0;
      for (size_t i = 0; i < n; i++)
        if (buf[i] == 0) return 1;
      p += n;
      off += n;
    }
  }
  return 1;  // longer than PATH_MAX, but reachable; the kernel returns ENAMETOOLONG
}

// An iovec array and each buffer it points at. The buffers are written for a
// read-like call (`write` = 1) and read for a write-like one.
static int g_iov(gk_thread *t, uintptr_t iov, long cnt, int write) {
  if (cnt < 0 || cnt > UIO_MAXIOV) return 0;
  if (!guest_range_ok(t, iov, (uintptr_t)cnt * sizeof(struct iovec), 0)) return 0;
  for (long i = 0; i < cnt; i++) {
    struct iovec v;
    if (copy_in(&v, iov + (uintptr_t)i * sizeof v, sizeof v) < 0) return 0;
    if (!guest_range_ok(t, (uintptr_t)v.iov_base, v.iov_len, write)) return 0;
  }
  return 1;
}

// A msghdr and everything it points at (name, control, and the iovec).
// A msghdr and everything it points at. recvmsg writes the header fields
// (msg_*len, msg_flags) and the name/control/iov buffers; sendmsg only reads
// them. So `write` governs the header too, which keeps a recvmsg from writing a
// header placed in the host-frame wall.
static int g_msg(gk_thread *t, uintptr_t mp, int write) {
  struct msghdr m;
  if (!guest_range_ok(t, mp, sizeof m, write) || copy_in(&m, mp, sizeof m) < 0) return 0;
  if (m.msg_name && !guest_range_ok(t, (uintptr_t)m.msg_name, m.msg_namelen, write)) return 0;
  if (m.msg_control &&
      !guest_range_ok(t, (uintptr_t)m.msg_control, m.msg_controllen, write))
    return 0;
  return g_iov(t, (uintptr_t)m.msg_iov, (long)m.msg_iovlen, write);
}

// Whether ring-3 thread `t` may forward syscall `nr` with arguments `a[1..6]`:
// 1 if every pointer argument names reachable memory, 0 to refuse. `a[0]` is
// `nr`. Fail-closed: a syscall not named here is refused.
static int syscall_mem_ok(gk_thread *t, long nr, const long a[7]) {
#define RD(p, n) \
  do { if (!guest_range_ok(t, (uintptr_t)(p), (uintptr_t)(n), 0)) return 0; } while (0)
#define WR(p, n) \
  do { if (!guest_range_ok(t, (uintptr_t)(p), (uintptr_t)(n), 1)) return 0; } while (0)
#define STR(p) \
  do { if (!g_str(t, (uintptr_t)(p))) return 0; } while (0)
#define OPT_RD(p, n) \
  do { if ((p) && !guest_range_ok(t, (uintptr_t)(p), (uintptr_t)(n), 0)) return 0; } while (0)
#define OPT_WR(p, n) \
  do { if ((p) && !guest_range_ok(t, (uintptr_t)(p), (uintptr_t)(n), 1)) return 0; } while (0)
  switch (nr) {
    // Scalar-only: no pointer arguments, and no effect on another isolate.
    // (A guest thread's exit and a thread-creating clone are intercepted in
    // forward_syscall; a non-thread clone and exit_group are refused below.)
    case SYS_brk: case SYS_close: case SYS_dup: case SYS_dup3: case SYS_epoll_create1:
    case SYS_eventfd2: case SYS_exit: case SYS_fchdir: case SYS_fsync:
    case SYS_ftruncate: case SYS_getpid: case SYS_getppid: case SYS_gettid:
    case SYS_fdatasync: case SYS_listen: case SYS_lseek:
    case SYS_sched_getscheduler: case SYS_sched_yield: case SYS_shutdown:
    case SYS_socket: case SYS_tgkill: case SYS_restart_syscall:
      return 1;
    case SYS_fcntl:
      // Only the commands whose third argument is a scalar; the rest (F_GETLK,
      // F_SETLK, F_GETOWN_EX, ...) carry a pointer gk does not size.
      switch (a[2]) {
        case F_GETFD: case F_SETFD: case F_GETFL: case F_SETFL:
        case F_DUPFD: case F_DUPFD_CLOEXEC: case F_GETOWN: case F_SETOWN:
          return 1;
        default: return 0;
      }
    // The mapping calls act on a caller-named range; a range outside what this
    // turn can reach is refused (which also rejects reflecting it later).
    case SYS_madvise: case SYS_mprotect: case SYS_munmap: WR(a[1], a[2]); return 1;
    case SYS_pkey_mprotect: WR(a[1], a[2]); return 1;
    case SYS_mmap:
      if (a[4] & MAP_FIXED) WR(a[1], a[2]);  // a fixed map replaces that range
      return 1;
    case SYS_mremap:
      WR(a[1], a[2]);
      if (a[4] & MREMAP_FIXED) WR(a[5], a[3]);
      return 1;
    case SYS_read: case SYS_pread64: WR(a[2], a[3]); return 1;
    case SYS_write: case SYS_pwrite64: RD(a[2], a[3]); return 1;
    case SYS_readv: case SYS_preadv: case SYS_preadv2: return g_iov(t, a[2], a[3], 1);
    case SYS_writev: case SYS_pwritev: case SYS_pwritev2: return g_iov(t, a[2], a[3], 0);
    case SYS_getrandom: WR(a[1], a[2]); return 1;
    case SYS_getcwd: WR(a[1], a[2]); return 1;
    case SYS_getdents64: WR(a[2], a[3]); return 1;
    case SYS_recvfrom:
      WR(a[2], a[3]);  // the receive buffer
      if (a[6]) {      // the kernel writes the source length, then the address
        socklen_t sl = 0;
        WR(a[6], sizeof sl);
        if (copy_in(&sl, (uintptr_t)a[6], sizeof sl) < 0) return 0;
        WR(a[5], sl);
      }
      return 1;
    case SYS_sendto:
      RD(a[2], a[3]);
      OPT_RD(a[5], a[6]);  // dest address, length a6 by value
      return 1;
    case SYS_recvmsg: return g_msg(t, a[2], 1);
    case SYS_sendmsg: return g_msg(t, a[2], 0);
    case SYS_sendmmsg: case SYS_recvmmsg: {
      long n = a[3];
      int recv = nr == SYS_recvmmsg;
      if (n < 0 || n > UIO_MAXIOV) return 0;
      // struct mmsghdr is a msghdr followed by an unsigned msg_len the kernel
      // writes either way, so the array itself is written.
      struct mmsghdr { struct msghdr msg_hdr; unsigned msg_len; };
      WR(a[2], (uintptr_t)n * sizeof(struct mmsghdr));
      for (long i = 0; i < n; i++)
        if (!g_msg(t, a[2] + (uintptr_t)i * sizeof(struct mmsghdr), recv)) return 0;
      if (recv) OPT_RD(a[5], sizeof(struct timespec));  // timeout
      return 1;
    }
    case SYS_connect: case SYS_bind: RD(a[2], a[3]); return 1;
    case SYS_accept4:
      if (a[2]) {  // the kernel writes the peer length, then the address
        socklen_t sl = 0;
        WR(a[3], sizeof sl);
        if (copy_in(&sl, (uintptr_t)a[3], sizeof sl) < 0) return 0;
        WR(a[2], sl);
      }
      return 1;
    case SYS_getsockname: case SYS_getpeername:
      WR(a[3], sizeof(socklen_t));
      if (a[2]) {
        socklen_t sl = 0;
        if (copy_in(&sl, (uintptr_t)a[3], sizeof sl) < 0) return 0;
        WR(a[2], sl);
      }
      return 1;
    case SYS_setsockopt: RD(a[4], a[5]); return 1;
    case SYS_getsockopt:
      WR(a[5], sizeof(socklen_t));
      if (a[4]) {
        socklen_t sl = 0;
        if (copy_in(&sl, (uintptr_t)a[5], sizeof sl) < 0) return 0;
        WR(a[4], sl);
      }
      return 1;
    case SYS_openat: STR(a[2]); return 1;
    case GK_SYS_openat2: STR(a[2]); RD(a[3], a[4]); return 1;  // struct open_how, a[4] bytes
    case SYS_newfstatat: STR(a[2]); WR(a[3], sizeof(struct stat)); return 1;
    case SYS_statx: STR(a[2]); WR(a[5], sizeof(struct statx)); return 1;
    case SYS_fstat: WR(a[2], sizeof(struct stat)); return 1;
    case SYS_statfs: case SYS_fstatfs:
      if (nr == SYS_statfs) STR(a[1]);
      WR(a[2], sizeof(struct statfs));
      return 1;
    case SYS_readlinkat: STR(a[2]); WR(a[3], a[4]); return 1;
    case SYS_faccessat: case SYS_faccessat2: STR(a[2]); return 1;
    case SYS_renameat: case SYS_renameat2: STR(a[2]); STR(a[4]); return 1;
    case SYS_unlinkat: case SYS_mkdirat: STR(a[2]); return 1;
    case SYS_clock_gettime: case SYS_clock_getres: WR(a[2], sizeof(struct timespec)); return 1;
    case SYS_clock_nanosleep:
      RD(a[3], sizeof(struct timespec));
      OPT_WR(a[4], sizeof(struct timespec));
      return 1;
    case SYS_nanosleep: RD(a[1], sizeof(struct timespec)); OPT_WR(a[2], sizeof(struct timespec)); return 1;
    case SYS_epoll_ctl: OPT_RD(a[4], sizeof(struct epoll_event)); return 1;
    case SYS_epoll_pwait: case SYS_epoll_pwait2:
      WR(a[2], (uintptr_t)a[3] * sizeof(struct epoll_event));
      OPT_RD(a[5], sizeof(sigset_t));
      return 1;
    case SYS_ppoll:
      WR(a[1], (uintptr_t)a[2] * sizeof(struct pollfd));  // the kernel writes revents
      OPT_RD(a[3], sizeof(struct timespec));
      OPT_RD(a[4], sizeof(sigset_t));
      return 1;
    case SYS_prlimit64:
      if (a[3]) return 0;  // setting a process-wide limit affects every isolate
      OPT_WR(a[4], sizeof(struct rlimit));
      return 1;
    case SYS_getrusage: WR(a[2], sizeof(struct rusage)); return 1;
    case SYS_uname: WR(a[1], sizeof(struct utsname)); return 1;
    case SYS_sched_getaffinity: WR(a[3], a[2]); return 1;
    case SYS_sched_getparam: WR(a[2], sizeof(struct sched_param)); return 1;
    case SYS_futex:
      RD(a[1], sizeof(int));
      switch (a[2] & 0x7f) {
        case 0: case 9: OPT_RD(a[4], sizeof(struct timespec)); break;  // WAIT, WAIT_BITSET
        case 5: WR(a[5], sizeof(int)); break;                          // WAKE_OP writes uaddr2
        case 3: case 4: case 10: case 12: RD(a[5], sizeof(int)); break;  // REQUEUE, CMP_REQUEUE[_PI]
      }
      return 1;
    case SYS_set_robust_list: RD(a[1], a[2]); return 1;
    case SYS_rseq: WR(a[1], a[2]); return 1;
    case SYS_sched_setaffinity: RD(a[3], a[2]); return 1;
    case SYS_rt_sigprocmask: {
      // Changes only the calling vCPU thread's own mask; abseil also uses it
      // (with an invalid `how`, so no change) to probe whether an address is
      // readable, and needs the kernel's EFAULT/EINVAL rather than a refusal.
      size_t ss = (size_t)a[4];
      if (ss > 128) ss = 128;  // the kernel accepts only an 8-byte set; bound the walk
      OPT_RD(a[2], ss);
      OPT_WR(a[3], ss);
      return 1;
    }
    // Only a thread-creating clone (intercepted in forward_syscall) is allowed
    // from ring 3; anything else would fork a host process. The tid pointers
    // the kernel writes must be reachable.
    case GK_SYS_clone3: {
      struct gk_clone_args ca;
      size_t sz = (size_t)a[2];
      if (sz < 64 || a[1] == 0) return 0;
      RD(a[1], sz);
      memset(&ca, 0, sizeof ca);
      if (copy_in(&ca, (uintptr_t)a[1], sz < sizeof ca ? sz : sizeof ca) < 0) return 0;
      if ((ca.flags & (GK_CLONE_VM | GK_CLONE_THREAD)) != (GK_CLONE_VM | GK_CLONE_THREAD))
        return 0;
      if (ca.flags & GK_CLONE_PARENT_SETTID) WR(ca.parent_tid, sizeof(int));
      if (ca.flags & (GK_CLONE_CHILD_SETTID | GK_CLONE_CHILD_CLEARTID)) WR(ca.child_tid, sizeof(int));
      OPT_WR(ca.pidfd, sizeof(int));
      return 1;
    }
    case SYS_clone: {
      unsigned long flags = (unsigned long)a[1];
      if ((flags & (GK_CLONE_VM | GK_CLONE_THREAD)) != (GK_CLONE_VM | GK_CLONE_THREAD)) return 0;
      // clone(flags, stack, parent_tid, ...): parent_tid is a[3]; child_tid is
      // a[4] on x86-64 and a[5] on arm64 (tls and child_tid are swapped).
      if (flags & GK_CLONE_PARENT_SETTID) WR(a[3], sizeof(int));
#if defined(__x86_64__)
      if (flags & (GK_CLONE_CHILD_SETTID | GK_CLONE_CHILD_CLEARTID)) WR(a[4], sizeof(int));
#else
      if (flags & (GK_CLONE_CHILD_SETTID | GK_CLONE_CHILD_CLEARTID)) WR(a[5], sizeof(int));
#endif
      return 1;
    }
#if defined(__x86_64__)
    case SYS_open: STR(a[1]); return 1;
    case SYS_stat: case SYS_lstat: STR(a[1]); WR(a[2], sizeof(struct stat)); return 1;
    case SYS_access: STR(a[1]); return 1;
    case SYS_readlink: STR(a[1]); WR(a[2], a[3]); return 1;
    case SYS_rename: STR(a[1]); STR(a[2]); return 1;
    case SYS_unlink: case SYS_mkdir: case SYS_rmdir: STR(a[1]); return 1;
    case SYS_poll: WR(a[1], (uintptr_t)a[2] * sizeof(struct pollfd)); return 1;  // revents written
    case SYS_epoll_wait: WR(a[2], (uintptr_t)a[3] * sizeof(struct epoll_event)); return 1;
    case SYS_eventfd: case SYS_epoll_create: return 1;  // scalar
#endif
    // Signal disposition, the host thread's mask, a non-thread clone,
    // exit_group, kill and anything else acts on the host or on other isolates,
    // or carries pointers gk cannot size: refuse. The embedder's filter bounds
    // the set further.
    default:
      if (G->dbg)
        fprintf(stderr, "[gk] vcpu %d ring-3 syscall %ld has no memory descriptor: refused\n",
                t->id, nr);
      return 0;
  }
#undef RD
#undef WR
#undef STR
#undef OPT_RD
#undef OPT_WR
}

// ---- syscall forwarding ----------------------------------------------------
// Every syscall is re-issued on the host; guest and host share memory, so
// pointer arguments work directly. New mappings (mmap/brk/mprotect) are picked
// up lazily by demand paging on first access, so nothing special is needed here.
static long forward_syscall(gk_thread *t, gk_regs *r) {
  long args[7];
  syscall_args(r, args);
  long nr = args[0], a1 = args[1], a2 = args[2], a3 = args[3], a4 = args[4], a5 = args[5],
       a6 = args[6];
  if (G->filter && !G->filter(nr, a1, a2, a3, a4, a5, a6)) {
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d syscall %ld DENIED\n", t->id, nr);
    return -GK_EPERM;
  }
  // A ring-3 thread's syscall may only name memory it could reach itself (see
  // the ring-3 syscall memory broker). Ring-0 turns are gk's own code.
  if (t->user_mode && !syscall_mem_ok(t, nr, args)) return -GK_EPERM;
  if (G->confine_paths
#ifdef GK_NEG_TURN_DIR
      && 0  // negative-test build: confinement removed, every path syscall forwards unconfined
#endif
  ) {
    long pr;
    if (confined_path_syscall(t, nr, a1, a2, a3, a4, a5, &pr)) {
      if (G->dbg)
        fprintf(stderr, "[gk] vcpu %d syscall %ld(%#lx, %#lx, %#lx, %#lx) under the turn "
                "directory -> %ld\n", t->id, nr, (unsigned long)a1, (unsigned long)a2,
                (unsigned long)a3, (unsigned long)a4, pr);
      return pr;
    }
  }
  // Thread creation: the child must enter the guest too. Only true threads
  // (CLONE_VM|CLONE_THREAD) are intercepted; fork/vfork-style clones forward.
  if (nr == SYS_clone || nr == GK_SYS_clone3) {
    uint64_t flags = 0;
    if (nr == SYS_clone) flags = (uint64_t)a1;
    else if (a1 && (uint64_t)a2 >= 64 &&
             copy_in(&flags, (uintptr_t)a1 + offsetof(struct gk_clone_args, flags),
                     sizeof flags) < 0)
      return -GK_EADDR;
    if ((flags & (GK_CLONE_VM | GK_CLONE_THREAD)) == (GK_CLONE_VM | GK_CLONE_THREAD)) {
      long tid = clone_thread(t, r, args);
      if (G->dbg)
        fprintf(stderr, "[gk] vcpu %d syscall %ld (thread clone, flags=%#lx) -> %ld\n",
                t->id, nr, (unsigned long)flags, tid);
      return tid;
    }
  }
  // A guest-created thread ending (see guest_thread_exit).
  if (nr == SYS_exit && t->guest_thread) {
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d syscall %ld (thread exit %ld)\n", t->id, nr, a1);
    guest_thread_exit(t, a1);
  }
  // Protection keys (see gk_pkey.c). pkey_mprotect
  // records the key for the range and goes to the host as a plain mprotect,
  // so the key lives only in the guest PTEs; the PTE clearing below then makes
  // the range re-fault and pick the key up. pkey_alloc and pkey_free are
  // forwarded so the host hands out the key numbers, and the calling vCPU's
  // PKRU gets the new key's initial access rights, as the kernel would give the
  // calling thread. Any key that fits the PTE field is accepted, host-allocated
  // ones included (see the protection-keys section).
  //
  // Protection/mapping changes are reflected into the guest PTEs for the
  // affected range, in the base root and in every arena root the range
  // overlaps, then this vCPU's TLB is flushed. A plain mprotect that leaves
  // the range readable rewrites the PTEs the guest already has, in place, to
  // the protection it just set (see reprotect_range_all); everything else --
  // munmap, a fixed mmap, an mprotect to PROT_NONE, a pkey_mprotect (whose key
  // the PTEs must pick up too), an mremap (both of its ranges) -- clears them
  // (see unmap_range_all), so the next access re-faults and demand-maps with
  // the new host state. Together
  // this is what makes W^X, JIT code and V8's commit/decommit of sandbox pages
  // work. The host call and the PTE update happen under G->lock so a demand
  // fault on another vCPU cannot install a PTE with the old protection in
  // between. The memslot backing is left in place: guest-physical equals
  // host-virtual, so a memslot validly covers its range whether or not the host
  // currently has memory there, and if the range is later remapped at the same
  // address the same memslot backs it. Deleting the memslot here would be
  // correct but wasteful -- KVM invalidates every nested-page-table root on a
  // memslot removal, so we never delete to reflect a protection or unmap change.
  //
  // Other vCPUs' TLBs are not shot down: a stale entry there carries the old
  // protection or key until it is evicted or faults (a page fault invalidates
  // the entry, so the retry re-walks and sees the new PTE).
  long ret;
  int reflect = (nr == SYS_mprotect || nr == SYS_munmap ||
                 nr == SYS_pkey_mprotect ||
                 (nr == SYS_mmap && (a4 & MAP_FIXED))) && a2 > 0;
  // mremap is reflected on its own, as it names two ranges: the mapping may
  // move, so the PTEs of the old range have to go as well as those of the new
  // one. A PTE left behind at the old address keeps resolving, and guest
  // physical is host virtual, so it would reach whatever the host maps there
  // next without that mapping ever passing the demand fault's class check.
  int reflect_mremap = nr == SYS_mremap && a2 > 0;
  // The kernel applies the call to whole host pages (see the page-sizes section).
  uintptr_t rs = HOST_FLOOR((uintptr_t)a1);
  uintptr_t re = HOST_CEIL((uintptr_t)a1 + (uintptr_t)a2);
  if (reflect || reflect_mremap) pthread_mutex_lock(&G->lock);
  if (GK_HAS_PKEYS && nr == SYS_pkey_mprotect) {
    long pkey = a4;
    if (pkey != -1 && (pkey < 0 || pkey > 15 || !pkey_room()))
      ret = -GK_ENOMEM;  // never EINVAL: see the protection-keys section
    else {
      ret = host_syscall(SYS_mprotect, a1, a2, a3, 0, 0, 0);
      if (ret == 0 && pkey != -1 && a2 > 0) pkey_set_range(rs, re, (int)pkey);  // room checked
    }
  } else {
    ret = host_syscall(nr, a1, a2, a3, a4, a5, a6);
  }
  if (reflect_mremap) {
    if ((unsigned long)ret < (unsigned long)-4096) {
      uintptr_t ns = HOST_FLOOR((uintptr_t)ret);
      uintptr_t ne = HOST_CEIL((uintptr_t)ret + (uintptr_t)a3);
      if (pkey_set_range(rs, re, 0) < 0 && G->dbg)
        fprintf(stderr, "[gk] pkey table full; [%#lx,%#lx) keeps a stale key\n",
                (unsigned long)rs, (unsigned long)re);
      if (pkey_set_range(ns, ne, 0) < 0 && G->dbg)
        fprintf(stderr, "[gk] pkey table full; [%#lx,%#lx) keeps a stale key\n",
                (unsigned long)ns, (unsigned long)ne);
      unmap_range_all(rs, re);
      if (ns < rs || ne > re) unmap_range_all(ns, ne);
    }
    pthread_mutex_unlock(&G->lock);
    flush_tlb(t, r);
  } else if (reflect) {
    if (nr == SYS_mmap ? ret == a1 : ret == 0) {
      // A new or removed mapping has no key.
      if ((nr == SYS_munmap || nr == SYS_mmap) && pkey_set_range(rs, re, 0) < 0 && G->dbg)
        fprintf(stderr, "[gk] pkey table full; [%#lx,%#lx) keeps a stale key\n",
                (unsigned long)rs, (unsigned long)re);
      // A commit of a large range gets huge host pages (see arena_thp_advise);
      // for mmap too, a3 is the protection.
      if (nr != SYS_munmap && (a3 & 7)) arena_thp_advise(rs, re);
      // In place only for a prot made of PROT_READ|PROT_WRITE|PROT_EXEC with
      // PROT_READ set; a PROT_GROWSDOWN/GROWSUP prot covers more than [rs, re).
      if (nr == SYS_mprotect && (a3 & ~7L) == 0 && (a3 & PROT_READ))
        reprotect_range_all(t, rs, re, (int)a3);
      else
        unmap_range_all(rs, re);
    }
    pthread_mutex_unlock(&G->lock);
    flush_tlb(t, r);
  } else if (nr == SYS_mmap && a2 > 0 && (unsigned long)ret < (unsigned long)-4096) {
    // A fresh mapping, placed by the kernel: whatever key was last recorded
    // for those addresses belonged to a mapping that is gone.
    pthread_mutex_lock(&G->lock);
    if (pkey_set_range(HOST_FLOOR((uintptr_t)ret), HOST_CEIL((uintptr_t)ret + (uintptr_t)a2), 0) < 0 &&
        G->dbg)
      fprintf(stderr, "[gk] pkey table full; mapping at %#lx keeps a stale key\n",
              (unsigned long)ret);
    pthread_mutex_unlock(&G->lock);
  }
  if (GK_HAS_PKEYS && nr == SYS_pkey_alloc && ret >= 0 && ret <= 15) {
    // The kernel gave the calling *host* thread the key's initial rights; the
    // guest thread is this vCPU, so give them to its PKRU (2 bits per key:
    // bit 0 = access disable, bit 1 = write disable).
    uint32_t shift = 2u * (uint32_t)ret, pkru = 0;
    if (guest_pkru_update(t, ~(3u << shift), ((uint32_t)a2 & 3u) << shift, &pkru) == 0)
      syscall_stub_ran(r);
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d pkey_alloc -> key %ld, rights %#lx; guest PKRU now %#x\n",
              t->id, ret, (unsigned long)a2, pkru);
  }
  if (G->dbg) {
    fprintf(stderr, "[gk] vcpu %d syscall %ld(%#lx, %#lx, %#lx, %#lx) -> %ld\n",
            t->id, nr, (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
            (unsigned long)a4, ret);
    if (GK_HAS_PKEYS && nr == SYS_pkey_mprotect && ret == 0)
      fprintf(stderr, "[gk] vcpu %d pkey_mprotect [%#lx,%#lx) key %ld: host mprotect only, "
              "guest PTEs will carry key %d\n", t->id, (unsigned long)rs, (unsigned long)re,
              a4, a4 == -1 ? pkey_lookup(rs) : (int)a4);
  }
  return ret;
}

// ---- mapping-change policy ------------------------------------------------
// The memory syscalls a turn needs (V8 commits and decommits sandbox pages
// with mprotect and fixed mmap/munmap, JIT code moves between RW and RX, the
// process allocator grows and trims) reach the host with the guest's own
// arguments, so a syscall filter that admits them by number alone lets an
// escaped ring-3 turn rewrite or remove any mapping in the process. These two
// functions are the range check such a filter adds (see gk.h). They run on the
// syscall-forward path, so they allocate nothing and consult only the small
// arrays in the control block: the supervisor/refuse registry, the arena list
// and the runtime image, each checked by overlap rather than page by page,
// since the guest may name a range of any size.

// Whether [s, e) (page-aligned, s < e) is clear of every range the calling
// thread may not change. Caller holds G->lock.
static int range_clear_locked(uintptr_t s, uintptr_t e, const gk_arena *mine) {
  // The image is sorted and disjoint (image_compact), so as with the registry
  // below, the first span ending past s is the only candidate for an overlap.
  {
    int lo = 0, hi = G->image_n;
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (G->image[mid].end <= s) lo = mid + 1;
      else hi = mid;
    }
    if (lo < G->image_n && G->image[lo].start < e) return 0;
  }
  // The hostcall exchange regions: the host copies from and to a thread's
  // region on every hostcall, so the guest may not pull the mapping away.
  if (s < G->hostcall_area + GK_HOSTCALL_AREA_BYTES && G->hostcall_area < e) return 0;
  for (int i = 0; i < G->arena_n; i++) {
    const gk_arena *a = G->arenas[i];
    if (a != mine && s < a->end && a->base < e) return 0;
  }
  // The registry is sorted and disjoint: the first range ending past s is the
  // only candidate for an overlap.
  int lo = 0, hi = G->prot_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (G->prot[mid].end <= s) lo = mid + 1;
    else hi = mid;
  }
  return !(lo < G->prot_n && G->prot[lo].start < e);
}

int gk_mapping_change_ok(unsigned long addr, unsigned long len) {
  if (len == 0) return 1;  // names no page; the kernel's own answer stands
  // The kernel applies the call to whole host pages (see the page-sizes section).
  uintptr_t s = HOST_FLOOR((uintptr_t)addr);
  uintptr_t e = (uintptr_t)addr + (uintptr_t)len;
  if (e < (uintptr_t)addr) return 0;  // wraps around the address space
  e = HOST_CEIL(e);
  if (e <= s) return 0;  // the rounding wrapped
  // The thread's active arena comes from its own record, in supervisor
  // memory; thread_cur validates the record by kernel tid, so ring 3 cannot
  // point this thread at another thread's record. A thread with no record
  // (host code outside any turn) has no arena.
  gk_thread *t = thread_cur();
  const gk_arena *mine = t ? t->active_arena : NULL;
  // forward_syscall calls the filter before it takes G->lock for the
  // reflection, and nothing holds G->lock across a KVM_RUN, so taking it
  // here cannot deadlock against the thread's own forward path.
  pthread_mutex_lock(&G->lock);
  int ok = range_clear_locked(s, e, mine);
  pthread_mutex_unlock(&G->lock);
  return ok;
}

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif
int gk_mapping_syscall_ok(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
  (void)a6;
  switch (nr) {
    case SYS_mmap:
      // A mapping the kernel places itself can affect nothing that exists:
      // the kernel only ever puts it in free space. It stays allowed without
      // a range check, deliberately: the process allocator grows during a
      // turn, and refusing its growth would break the runtime for no
      // security gain. MAP_FIXED replaces whatever is at the address, so it
      // is checked; MAP_FIXED_NOREPLACE cannot replace anything but is
      // checked too, as the cheaper of the two choices.
      if ((a4 & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0) return 1;
      return gk_mapping_change_ok((unsigned long)a1, (unsigned long)a2);
    case SYS_mprotect:
    case SYS_pkey_mprotect:
    case SYS_munmap:
    case SYS_madvise:
      // PROT_GROWSDOWN/GROWSUP widen an mprotect only within a growsdown
      // mapping: the main thread's stack, refused in full, or one the guest
      // created for itself.
      return gk_mapping_change_ok((unsigned long)a1, (unsigned long)a2);
    case SYS_mremap: {
      // The old range, and the new size at the old address (a grow in place
      // extends over whatever follows); then the destination of a move the
      // guest places itself. A move the kernel places lands in free space.
      unsigned long old_len = (unsigned long)a2, new_len = (unsigned long)a3;
      if (!gk_mapping_change_ok((unsigned long)a1, old_len > new_len ? old_len : new_len))
        return 0;
      if (a4 & MREMAP_FIXED) return gk_mapping_change_ok((unsigned long)a5, new_len);
      return 1;
    }
    default:
      return 1;
  }
}

#endif  // !GK_HVF

long gk_debug_vcpu_stat(const char *name) {
#ifdef KVM_GET_STATS_FD
  gk_thread *t = thread_cur();
  if (!t || !t->inited) return -1;
  int fd = t->stats_fd;  // taken at creation (see vcpu_init); never a vCPU ioctl here
  if (fd < 0) return -1;
  long v = -1;
  struct kvm_stats_header hdr;
  // One descriptor at a time into a fixed buffer: the path allocates nothing.
  union { struct kvm_stats_desc d; char raw[sizeof(struct kvm_stats_desc) + 256]; } desc;
  if (pread(fd, &hdr, sizeof hdr, 0) == (ssize_t)sizeof hdr && hdr.name_size <= 256) {
    size_t dsz = sizeof(struct kvm_stats_desc) + hdr.name_size;
    for (uint32_t i = 0; i < hdr.num_desc; i++) {
      if (pread(fd, &desc, dsz, hdr.desc_offset + i * dsz) != (ssize_t)dsz) break;
      desc.raw[dsz - 1] = 0;
      if (strcmp(desc.d.name, name) != 0) continue;
      uint64_t val;
      if (pread(fd, &val, sizeof val, hdr.data_offset + desc.d.offset) == (ssize_t)sizeof val)
        v = (long)val;
      break;
    }
  }
  return v;
#else
  (void)name;
  return -1;
#endif
}
