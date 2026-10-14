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
    case SYS_eventfd2: case SYS_exit: case SYS_fsync:
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
  // the PTEs must pick up too) -- clears them (see unmap_range_all), so the
  // next access re-faults and demand-maps with the new host state. Together
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
  // The kernel applies the call to whole host pages (see the page-sizes section).
  uintptr_t rs = HOST_FLOOR((uintptr_t)a1);
  uintptr_t re = HOST_CEIL((uintptr_t)a1 + (uintptr_t)a2);
  if (reflect) pthread_mutex_lock(&G->lock);
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
  if (reflect) {
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

#endif  // !GK_HVF
