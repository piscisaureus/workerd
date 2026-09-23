// gk implementation. See gk.h.
#define _GNU_SOURCE
#include "gk.h"
#include <errno.h>
#include <string.h>

#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// ---- x86-64 paging and control-register bits -------------------------------
#define PTE_P (1UL << 0)
#define PTE_W (1UL << 1)
#define PTE_U (1UL << 2)
#define PTE_NX (1UL << 63)
#define CR0_PE (1UL << 0)
#define CR0_PG (1UL << 31)
#define CR4_PAE (1UL << 5)
#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)
#define CR4_OSXSAVE (1UL << 18)
#define CR4_PKE (1UL << 22)
#define EFER_LME (1UL << 8)
#define EFER_LMA (1UL << 10)
#define EFER_SCE (1UL << 0)
#define EFER_NXE (1UL << 11)

#define MSR_EFER 0xc0000080
#define MSR_STAR 0xc0000081
#define MSR_LSTAR 0xc0000082
#define MSR_SYSCALL_MASK 0xc0000084

// Hypercall ports.
#define PORT_SYSCALL 0x11
#define PORT_EXIT 0xFF
#define PORT_FAULT 0xFE
#define PORT_UDRIP 0xFD
#define PORT_DEMAND 0xFC

// ---- global platform state (set once by gk_init) ---------------------------
static int g_kvm = -1, g_vmfd = -1;
static uint64_t *g_pml4;             // base page-table root
static uint8_t *g_pt_next, *g_pt_end;
static uintptr_t g_pt_base;
static size_t g_pt_bytes;
static uint64_t g_gdt_va, g_idt_va;
static int g_next_slot;
static int g_run_size;
static struct kvm_cpuid2 *g_cpuid;   // host CPUID, applied to every vCPU
static const char *g_err;
static unsigned long g_fault_addr;
static int g_dbg;
static gk_syscall_filter g_filter;
static int g_next_cage_idx = 64;
// Registry of arena cage regions, so demand paging can refuse faults into any
// arena (each arena's own cage is pre-mapped in its root, so a fault at an
// arena address is always a cross-arena access that must stay isolated).
#define GK_MAX_ARENAS 4096
static struct { uintptr_t base, end; } g_arenas[GK_MAX_ARENAS];
static int g_arena_n;
static int addr_in_any_arena(uintptr_t a) {
  for (int i = 0; i < g_arena_n; i++)
    if (a >= g_arenas[i].base && a < g_arenas[i].end) return 1;
  return 0;
}
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_demand_ok;

// ---- vCPU pool -------------------------------------------------------------
// KVM never destroys a vCPU before the VM (closing its fd only drops a
// reference) and rejects a vcpu id that was already created, so a vCPU per
// thread ever created would exhaust KVM_CAP_MAX_VCPUS under thread churn.
// Instead a vCPU whose thread ends is parked here and handed to the next thread
// that needs one; a vCPU may be driven by a different host thread on each
// KVM_RUN. Live vCPUs are therefore bounded by peak concurrent guest threads.
// The pool holds the eager-mapped guest stack too, when the vCPU had one, so
// stacks are recycled rather than leaked. Guarded by g_lock.
#define GK_MAX_VCPUS 4096
#define GK_IST_BYTES (64UL << 10)   // per-vCPU exception stack, TSS in its last page
typedef struct {
  int fd, id;
  struct kvm_run *run;
  uint64_t stack_top;   // 0 if the vCPU has no private guest stack
  uint64_t ist;         // its exception stack + TSS region (see vcpu_init)
} gk_parked_vcpu;
static gk_parked_vcpu g_parked[GK_MAX_VCPUS];
static int g_parked_n;
static atomic_int g_vcpus_created;   // vcpu ids handed out, ever
static pthread_key_t g_vcpu_key;     // releases a gk_run thread's vCPU at exit

// Per-thread guest state: each host thread that enters the guest is a vCPU.
typedef struct {
  int inited;
  int id;
  int fd;
  struct kvm_run *run;
  uint64_t stack_top;
  uint64_t ist;            // exception stack + TSS region of this vCPU
  uint64_t loaded_cr3;
  uint64_t last_fault;
  int fault_repeat;
  uint64_t *active_pml4;   // root this thread runs under
  gk_arena *active_arena;
  // Set for a thread the guest created via clone (see clone_thread): it runs
  // its gk loop on this private host stack and leaves only through the guest's
  // exit syscall.
  int guest_thread;
  void *host_stack;
  size_t host_stack_size;
} gk_vcpu;
static __thread gk_vcpu tls;

struct gk_arena {
  void *base;
  size_t size;
  uint64_t *pml4;
  int cage_idx;         // PML4 index owned privately by this arena
  uint64_t cage_entry;  // its private cage subtree entry
};

// Trampolines and exception handlers (gk_asm.S), in this binary's mapped text.
extern void gk_syscall_tramp(void);
extern void gk_exit_tramp(void);
extern void gk_exc_de(void), gk_exc_ud(void), gk_exc_df(void), gk_exc_gp(void),
    gk_exc_pf(void);
// Host-side helpers for guest thread creation (gk_asm.S); never run in the guest.
extern long gk_host_clone_raw(long nr, long a1, long a2, long a3, long a4,
                              long a5);
extern void gk_host_unmapself_exit(void *addr, size_t len, long code)
    __attribute__((noreturn));

// ---- helpers ---------------------------------------------------------------
static long host_syscall(long nr, long a1, long a2, long a3, long a4, long a5,
                         long a6) {
  long ret;
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ __volatile__("syscall"
                       : "=a"(ret)
                       : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8),
                         "r"(r9)
                       : "rcx", "r11", "memory");
  return ret;
}

// Page-table page allocator over a fixed arena. Caller holds g_lock (except in
// single-threaded gk_init).
static uint64_t *alloc_table(void) {
  if (g_pt_next + 0x1000 > g_pt_end) return NULL;
  uint64_t *p = (uint64_t *)g_pt_next;
  memset(p, 0, 0x1000);
  g_pt_next += 0x1000;
  return p;
}

static uint64_t *next_table(uint64_t *tbl, int idx) {
  if (!(tbl[idx] & PTE_P)) {
    uint64_t *n = alloc_table();
    if (!n) return NULL;
    tbl[idx] = ((uint64_t)(uintptr_t)n) | PTE_P | PTE_W | PTE_U;
  }
  return (uint64_t *)(uintptr_t)(tbl[idx] & ~0xfffULL);
}

// Four-level map of one page; guest-virtual == guest-physical == host-virtual.
static int map4k_root(uint64_t *root, uint64_t va, uint64_t flags) {
  uint64_t *pdpt = next_table(root, (va >> 39) & 0x1ff);
  if (!pdpt) return -1;
  uint64_t *pd = next_table(pdpt, (va >> 30) & 0x1ff);
  if (!pd) return -1;
  uint64_t *pt = next_table(pd, (va >> 21) & 0x1ff);
  if (!pt) return -1;
  // `flags` is a protection bitmask: 1=read, 2=write, 4=execute. Honor it so
  // W^X holds: code is mapped executable but not writable, data writable but
  // not executable. KVM also enforces the host VMA's real protection.
  uint64_t pte = (va & ~0xfffULL) | PTE_P | PTE_U;
  if (flags & 2) pte |= PTE_W;
  if (!(flags & 4)) pte |= PTE_NX;
  pt[(va >> 12) & 0x1ff] = pte;
  return 0;
}

// Clear one page's PTE in a root (used to reflect mprotect/munmap). Caller holds
// g_lock. Leaves intermediate tables in place.
static void unmap4k_root(uint64_t *root, uint64_t va) {
  if (!(root[(va >> 39) & 0x1ff] & PTE_P)) return;
  uint64_t *pdpt = (uint64_t *)(uintptr_t)(root[(va >> 39) & 0x1ff] & ~0xfffULL);
  if (!(pdpt[(va >> 30) & 0x1ff] & PTE_P)) return;
  uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[(va >> 30) & 0x1ff] & ~0xfffULL);
  if (!(pd[(va >> 21) & 0x1ff] & PTE_P)) return;
  uint64_t *pt = (uint64_t *)(uintptr_t)(pd[(va >> 21) & 0x1ff] & ~0xfffULL);
  pt[(va >> 12) & 0x1ff] = 0;
}

// ---- the MMU layer: interval-tree memslot manager --------------------------
// guest-physical == host-virtual everywhere, so a KVM memslot maps a range of
// guest-physical to the identical host-virtual range. Backed ranges ("regions")
// are tracked in an interval tree (a treap keyed by start) of non-overlapping
// [start, end) windows; every mapped PTE must have a backing memslot, or KVM
// takes the MMIO/emulation path and KVM_RUN returns EFAULT.
//
// Memory is backed one aligned GK_BACK_WIN window at a time (region_ensure
// rounds outward to the window), rather than one memslot per exact host mapping.
// This matters for correctness, not just speed: creating tight, exact-bounds
// memslots and extending them page by page as brk/mmap grow a live mapping under
// the running guest corrupts the memory being grown (glibc's heap, in practice).
// An aligned window absorbs a mapping's growth without touching the memslot that
// already backs it, and because everything is identity-mapped an oversized
// window that spills into an unmapped hole is harmless -- the guest never has a
// PTE there, so KVM never faults it in. Protection is not cached on the region;
// the PTE carries the live host protection, re-read on every fault.
//
// Memslots are created once and never deleted: an identity memslot validly backs
// its window whether or not the host currently has memory there, so mprotect and
// munmap only need to drop the guest PTEs (see forward_syscall), not the memslot.
//
// TODO: 2MB windows over-back sparsely committed reservations. A production MMU
// could track exact VMAs (cf. FreeBSD's vm_map RB tree, sys/vm, 2-clause BSD)
// once the exact-bounds growth path above is made safe, and issue cross-vCPU TLB
// shootdowns.
#define GK_BACK_WIN (2UL << 20)   // memslot backing granularity (2MB, aligned)
typedef struct gk_region {
  uintptr_t start, end;   // [start, end), GK_BACK_WIN-aligned
  int slot;               // KVM memslot id backing this region
  unsigned prio;          // treap heap priority
  struct gk_region *l, *r;
} gk_region;
static gk_region *g_regions;      // treap root
// Region nodes come from a static pool: region_add runs on the fault path, and
// the faulting guest thread may be inside malloc holding its arena lock, so the
// host side of that same thread must never call malloc there.
#define GK_MAX_REGIONS 65536
static gk_region g_region_pool[GK_MAX_REGIONS];
static int g_region_n;

// xorshift32 PRNG for treap priorities. Caller holds g_lock.
static unsigned gk_rand(void) {
  static unsigned s = 0x9e3779b9u;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

// Treap: BST ordered by `start`, max-heap ordered by `prio`.
static gk_region *rot_r(gk_region *n) {
  gk_region *l = n->l;
  n->l = l->r;
  l->r = n;
  return l;
}
static gk_region *rot_l(gk_region *n) {
  gk_region *r = n->r;
  n->r = r->l;
  r->l = n;
  return r;
}
static gk_region *treap_insert(gk_region *root, gk_region *node) {
  if (!root) return node;
  if (node->start < root->start) {
    root->l = treap_insert(root->l, node);
    if (root->l->prio > root->prio) root = rot_r(root);
  } else {
    root->r = treap_insert(root->r, node);
    if (root->r->prio > root->prio) root = rot_l(root);
  }
  return root;
}

// The region containing `addr`, or NULL. Caller holds g_lock.
static gk_region *region_find(uintptr_t addr) {
  gk_region *n = g_regions;
  while (n) {
    if (addr < n->start) n = n->l;
    else if (addr >= n->end) n = n->r;
    else return n;
  }
  return NULL;
}

// The region with the smallest start strictly greater than `s`, or NULL (the
// in-order successor of `s` in the start-ordered tree). Caller holds g_lock.
static gk_region *region_succ(uintptr_t s) {
  gk_region *n = g_regions, *best = NULL;
  while (n) {
    if (n->start > s) { best = n; n = n->l; }
    else n = n->r;
  }
  return best;
}

// Create one memslot-backed region for [s, e). Caller holds g_lock and has
// ensured [s, e) does not overlap any existing region.
static int region_add(uintptr_t s, uintptr_t e) {
  if (g_region_n >= GK_MAX_REGIONS) return -1;
  int slot = g_next_slot++;
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)slot,
                                          .guest_phys_addr = s,
                                          .memory_size = e - s,
                                          .userspace_addr = s};
  if (ioctl(g_vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (g_dbg)
      fprintf(stderr, "[gk] memslot %d [%#lx,%#lx) failed: %s\n", slot,
              (unsigned long)s, (unsigned long)e, strerror(errno));
    g_next_slot--;
    return -1;
  }
  gk_region *n = &g_region_pool[g_region_n++];
  memset(n, 0, sizeof *n);
  n->start = s;
  n->end = e;
  n->slot = slot;
  n->prio = gk_rand();
  g_regions = treap_insert(g_regions, n);
  return 0;
}

// Ensure memslots back all of [s, e), rounded outward to GK_BACK_WIN windows.
// Additive: existing regions are kept and only the gaps between them get fresh
// memslots. Caller holds g_lock.
static int region_ensure(uintptr_t s, uintptr_t e) {
  s &= ~(GK_BACK_WIN - 1);
  e = (e + GK_BACK_WIN - 1) & ~(GK_BACK_WIN - 1);
  while (s < e) {
    gk_region *have = region_find(s);
    if (have) {  // already backed; skip past it
      s = have->end;
      continue;
    }
    uintptr_t gap_end = e;
    gk_region *nx = region_succ(s);  // stop the new region before the next one
    if (nx && nx->start < gap_end) gap_end = nx->start;
    if (region_add(s, gap_end) < 0) return -1;
    s = gap_end;
  }
  return 0;
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
  long fd = host_syscall(SYS_open, (long)"/proc/self/maps", O_RDONLY | O_CLOEXEC,
                         0, 0, 0, 0);
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

// Eager-map [s, e) into the base root with PTE protection `perms`, backing the
// containing GK_BACK_WIN windows so later demand faults in the same region find
// an existing memslot. Caller holds g_lock.
static int mmu_map_range(uintptr_t s, uintptr_t e, int perms) {
  if (region_ensure(s, e) < 0) return -1;
  for (uintptr_t v = s & ~0xfffUL; v < e; v += 0x1000)
    if (map4k_root(g_pml4, v, (uint64_t)perms) < 0) return -1;
  return 0;
}

// Handle a guest page fault: if the faulting page is host-accessible, back it
// with a memslot and PTE so the guest can retry. Caller must not hold g_lock.
//
// The PTE always carries the page's *current* host protection, re-read from
// /proc/self/maps on every fault, so mprotect/mmap protection changes are
// honored even if a syscall reflection missed them; a stale read-only PTE on a
// page the host has made writable would silently drop guest writes. The host
// protection is read under g_lock, the same lock a protection-changing syscall
// holds across its host call and PTE clearing (see forward_syscall), so another
// vCPU cannot change a page's protection between this read and the PTE install.
static int demand_map(uintptr_t addr) {
  uintptr_t page = addr & ~0xfffUL;
  // A fault at an arena address is a cross-arena access (each arena's own cage
  // is fully mapped in its root); refuse it to preserve isolation.
  if (addr_in_any_arena(page)) return -1;
  uintptr_t rs, re; int perms;
  pthread_mutex_lock(&g_lock);
  if (!host_region(page, &rs, &re, &perms) || !(perms & 1)) {
    pthread_mutex_unlock(&g_lock);
    return -1;  // not host-readable: a genuine fault
  }
  if (!region_find(page) && region_ensure(page, page + 1) < 0) {
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  int r = map4k_root(g_pml4, page, (uint64_t)perms);  // honor R/W/X for W^X
  if (r >= 0) g_demand_ok++;
  pthread_mutex_unlock(&g_lock);
  return r;
}

// Every gate runs its handler on IST1, the vCPU's exception stack (see
// vcpu_init), never on the interrupted code's stack.
static void set_idt_gate(uint8_t *idt, int vec, uint64_t h) {
  uint8_t *e = idt + vec * 16;
  *(uint16_t *)(e + 0) = h & 0xffff;
  *(uint16_t *)(e + 2) = 0x08;
  e[4] = 1;  // IST index
  e[5] = 0x8e;
  *(uint16_t *)(e + 6) = (h >> 16) & 0xffff;
  *(uint32_t *)(e + 8) = (h >> 32);
}

// Return the calling thread's vCPU (and guest stack, if any) to the pool.
static void vcpu_park(void) {
  if (!tls.inited) return;
  pthread_mutex_lock(&g_lock);
  if (g_parked_n < GK_MAX_VCPUS) {
    gk_parked_vcpu *p = &g_parked[g_parked_n++];
    p->fd = tls.fd;
    p->id = tls.id;
    p->run = tls.run;
    p->stack_top = tls.stack_top;
    p->ist = tls.ist;
  } else {  // cannot happen with KVM_CAP_MAX_VCPUS <= GK_MAX_VCPUS; be safe
    munmap(tls.run, g_run_size);
    close(tls.fd);
  }
  pthread_mutex_unlock(&g_lock);
  if (g_dbg) fprintf(stderr, "[gk] vcpu %d parked\n", tls.id);
  tls.inited = 0;
}

// pthread key destructor: a host thread that entered the guest via gk_run is
// ending, so recycle its vCPU. (Guest-created threads end through the exit
// syscall in forward_syscall instead, which parks explicitly.)
static void vcpu_key_dtor(void *p) {
  (void)p;
  vcpu_park();
}

// Bring the calling thread's vCPU up. Each thread has its own vCPU, stack and
// TLS base, but they share the VM, memslots and page tables. A parked vCPU is
// reused when one is available (see the vCPU pool); otherwise a new one is
// created. Either way the segment bases, CR3 and FPU are programmed for the
// calling thread, so a reused vCPU carries nothing over from its last thread
// except its id. The guest stack is only needed for threads entering through
// gk_run; a thread the guest created itself already has the stack its clone
// named (see clone_thread).
static int vcpu_init(int with_stack) {
  if (tls.inited) return 0;
  int id, fd, reused = 0;
  struct kvm_run *run = NULL;
  uint64_t stack_top = 0, ist = 0;
  pthread_mutex_lock(&g_lock);
  if (g_parked_n > 0) {
    gk_parked_vcpu *p = &g_parked[--g_parked_n];
    fd = p->fd; id = p->id; run = p->run; stack_top = p->stack_top; ist = p->ist;
    reused = 1;
  }
  pthread_mutex_unlock(&g_lock);
  if (!reused) {
    id = atomic_fetch_add(&g_vcpus_created, 1);
    fd = ioctl(g_vmfd, KVM_CREATE_VCPU, id);
    if (fd < 0) { g_err = "KVM_CREATE_VCPU"; return -1; }
    if (ioctl(fd, KVM_SET_CPUID2, g_cpuid) < 0) { g_err = "KVM_SET_CPUID2"; return -1; }
    run = mmap(NULL, g_run_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (run == MAP_FAILED) { g_err = "mmap kvm_run"; return -1; }

    // Exception stack. The guest runs ordinary user-mode code, which keeps
    // locals in the 128-byte red zone below rsp; the ABI promises that zone
    // survives asynchronous events. A ring-0 exception without a stack switch
    // would push its frame right there, so every gate switches to this vCPU's
    // IST1 stack, selected through a per-vCPU TSS that lives in the last page
    // of the region. Eager-mapped: the switch itself must never fault.
    uint8_t *ir = mmap(NULL, GK_IST_BYTES, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ir == MAP_FAILED) { g_err = "mmap exception stack"; return -1; }
    pthread_mutex_lock(&g_lock);
    int irc = mmu_map_range((uintptr_t)ir, (uintptr_t)ir + GK_IST_BYTES, 1 | 2);
    pthread_mutex_unlock(&g_lock);
    if (irc < 0) { g_err = "map exception stack"; return -1; }
    ist = (uint64_t)(uintptr_t)ir;
    uint8_t *tss = ir + GK_IST_BYTES - 0x1000;
    *(uint64_t *)(tss + 36) = (uint64_t)(uintptr_t)tss;  // IST1: stack top is just below the TSS
    *(uint16_t *)(tss + 102) = 0x68;                     // I/O map base past the limit
  }

  // A private guest stack for this vCPU, eager-mapped so interrupt delivery
  // (which pushes a frame) never itself faults. A parked vCPU may bring one
  // along; it is kept (and parked again later) even if this thread has no use
  // for it, so stacks are neither leaked nor left with dangling PTEs.
  const size_t stksz = 2 * 1024 * 1024;
  if (with_stack && stack_top == 0) {
    uint8_t *stk = mmap(NULL, stksz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) { g_err = "mmap guest stack"; return -1; }
    pthread_mutex_lock(&g_lock);
    int mrc = mmu_map_range((uintptr_t)stk, (uintptr_t)stk + stksz, 1 | 2);  // rw, NX
    pthread_mutex_unlock(&g_lock);
    if (mrc < 0) { g_err = "map guest stack"; return -1; }
    stack_top = (uintptr_t)stk + stksz;
  }

  struct kvm_sregs s;
  ioctl(fd, KVM_GET_SREGS, &s);
  s.cr3 = (uint64_t)(uintptr_t)g_pml4;
  s.cr4 = CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE | CR4_PKE;
  s.cr0 = CR0_PE | CR0_PG;
  s.efer = EFER_LME | EFER_LMA | EFER_SCE | EFER_NXE;
  struct kvm_segment cs = {.base = 0, .limit = 0xffffffff, .selector = 0x08,
                           .type = 11, .present = 1, .s = 1, .l = 1, .g = 1};
  struct kvm_segment ds = {.base = 0, .limit = 0xffffffff, .selector = 0x10,
                           .type = 3, .present = 1, .s = 1, .db = 1, .g = 1};
  s.cs = cs;
  s.ds = s.es = s.ss = ds;
  uint64_t fsbase = 0, gsbase = 0;
  host_syscall(SYS_arch_prctl, 0x1003, (long)&fsbase, 0, 0, 0, 0);  // ARCH_GET_FS
  host_syscall(SYS_arch_prctl, 0x1004, (long)&gsbase, 0, 0, 0, 0);  // ARCH_GET_GS
  s.fs = ds; s.fs.base = fsbase;
  s.gs = ds; s.gs.base = gsbase;
  // TR comes from the vCPU's cached descriptor, set here; the GDT has no TSS
  // entry because the guest never executes ltr (iretq reloads only CS and SS).
  struct kvm_segment tr = {.base = ist + GK_IST_BYTES - 0x1000, .limit = 0x67,
                           .selector = 0x18, .type = 11, .present = 1};
  s.tr = tr;
  s.gdt.base = g_gdt_va; s.gdt.limit = 3 * 8 - 1;
  s.idt.base = g_idt_va; s.idt.limit = 256 * 16 - 1;
  if (ioctl(fd, KVM_SET_SREGS, &s) < 0) { g_err = "KVM_SET_SREGS"; return -1; }

  if (reused) {
    // Fresh x87/SSE control state, as a new thread would start with; the
    // previous thread's register contents are otherwise irrelevant because
    // the guest entry point sets every register it relies on.
    struct kvm_fpu f = {0};
    f.fcw = 0x37f;
    f.mxcsr = 0x1f80;
    if (ioctl(fd, KVM_SET_FPU, &f) < 0) { g_err = "KVM_SET_FPU"; return -1; }
    goto ready;
  }

  struct kvm_xcrs xcrs = {0};
  xcrs.nr_xcrs = 1; xcrs.xcrs[0].xcr = 0; xcrs.xcrs[0].value = 0x7;
  if (ioctl(fd, KVM_SET_XCRS, &xcrs) < 0) { g_err = "KVM_SET_XCRS"; return -1; }

  // KVM starts a new vCPU's TSC near zero, but the guest runs the host's vDSO,
  // whose clock_gettime derives time from rdtsc against the host's TSC. With an
  // offset TSC the vDSO sees the counter behind its last update and returns the
  // stale tick-granular base time. A zero TSC offset makes guest rdtsc equal
  // host rdtsc, so vDSO time is exact. Best effort: older kernels lack the attr.
  {
    int64_t zero = 0;
    struct kvm_device_attr ta = {.group = KVM_VCPU_TSC_CTRL,
                                 .attr = KVM_VCPU_TSC_OFFSET,
                                 .addr = (uint64_t)(uintptr_t)&zero};
    if (ioctl(fd, KVM_SET_DEVICE_ATTR, &ta) < 0 && g_dbg)
      fprintf(stderr, "[gk] vcpu %d: KVM_VCPU_TSC_OFFSET unsupported: %s\n", id,
              strerror(errno));
  }

  struct { struct kvm_msrs h; struct kvm_msr_entry e[4]; } m = {0};
  m.h.nmsrs = 4;
  m.e[0].index = MSR_EFER; m.e[0].data = EFER_LME | EFER_LMA | EFER_SCE | EFER_NXE;
  m.e[1].index = MSR_STAR; m.e[1].data = (uint64_t)0x08 << 32;
  m.e[2].index = MSR_LSTAR; m.e[2].data = (uintptr_t)&gk_syscall_tramp;
  m.e[3].index = MSR_SYSCALL_MASK; m.e[3].data = 0x3f7fd5;
  if (ioctl(fd, KVM_SET_MSRS, &m) < 4) { g_err = "KVM_SET_MSRS"; return -1; }

ready:
  tls.id = id;
  tls.fd = fd;
  tls.run = run;
  tls.stack_top = stack_top;
  tls.ist = ist;
  tls.loaded_cr3 = (uint64_t)(uintptr_t)g_pml4;
  tls.last_fault = 0;
  tls.fault_repeat = 0;
  if (!tls.active_pml4) tls.active_pml4 = g_pml4;
  tls.inited = 1;
  // A gk_run thread gives its vCPU back when it ends (see vcpu_key_dtor).
  if (with_stack) pthread_setspecific(g_vcpu_key, &tls);
  if (g_dbg)
    fprintf(stderr, "[gk] vcpu %d %s by tid %ld\n", id, reused ? "reused" : "created",
            host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0));
  return 0;
}

// Eager-map the pages spanning the interrupt handlers and trampolines. They
// must be present before any fault, or delivering the first fault would itself
// fault and triple-fault. All live together in gk_asm.S, so one small range
// covers them (the rest of the binary and V8 are demand-paged on execute).
static int map_handler_text(void) {
  uintptr_t hs[] = {
      (uintptr_t)&gk_exc_de, (uintptr_t)&gk_exc_ud, (uintptr_t)&gk_exc_df,
      (uintptr_t)&gk_exc_gp, (uintptr_t)&gk_exc_pf, (uintptr_t)&gk_syscall_tramp,
      (uintptr_t)&gk_exit_tramp};
  uintptr_t lo = hs[0], hi = hs[0];
  for (size_t i = 1; i < sizeof hs / sizeof hs[0]; i++) {
    if (hs[i] < lo) lo = hs[i];
    if (hs[i] > hi) hi = hs[i];
  }
  return mmu_map_range(lo & ~0xfffUL, (hi + 64 + 0xfff) & ~0xfffUL, 1 | 4);  // r-x
}

int gk_init(void) {
  g_dbg = getenv("GK_DEBUG") != NULL;
  g_kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (g_kvm < 0) { g_err = "open /dev/kvm"; return -1; }
  if (ioctl(g_kvm, KVM_GET_API_VERSION, 0) != 12) { g_err = "KVM API != 12"; return -1; }
  g_vmfd = ioctl(g_kvm, KVM_CREATE_VM, 0);
  if (g_vmfd < 0) { g_err = "KVM_CREATE_VM"; return -1; }

  g_pt_bytes = 64 * 1024 * 1024;
  uint8_t *pt = mmap(NULL, g_pt_bytes, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (pt == MAP_FAILED) { g_err = "mmap PT arena"; return -1; }
  g_pt_next = pt; g_pt_end = pt + g_pt_bytes; g_pt_base = (uintptr_t)pt;
  g_pml4 = alloc_table();

  uint8_t *tables = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (tables == MAP_FAILED) { g_err = "mmap gdt/idt"; return -1; }
  g_gdt_va = (uintptr_t)tables;
  g_idt_va = (uintptr_t)tables + 0x1000;
  uint64_t *gdt = (uint64_t *)(uintptr_t)g_gdt_va;
  gdt[0] = 0;
  gdt[1] = 0x00AF9A000000FFFFULL;
  gdt[2] = 0x00CF92000000FFFFULL;
  uint8_t *idt = (uint8_t *)(uintptr_t)g_idt_va;
  memset(idt, 0, 0x1000);
  set_idt_gate(idt, 0, (uintptr_t)&gk_exc_de);
  set_idt_gate(idt, 6, (uintptr_t)&gk_exc_ud);
  set_idt_gate(idt, 8, (uintptr_t)&gk_exc_df);
  set_idt_gate(idt, 13, (uintptr_t)&gk_exc_gp);
  set_idt_gate(idt, 14, (uintptr_t)&gk_exc_pf);

  // The page-table arena is read by the CPU page walker via guest-physical
  // addresses, so it needs memslots (but no PTEs of its own).
  if (region_ensure(g_pt_base, g_pt_base + g_pt_bytes) < 0) { g_err = "memslot for PT arena"; return -1; }
  // GDT/IDT and the handler text must be present before the first fault.
  if (mmu_map_range(g_gdt_va, g_gdt_va + 0x2000, 1 | 2) < 0) { g_err = "map gdt/idt"; return -1; }
  if (map_handler_text() < 0) { g_err = "map handler text"; return -1; }

  size_t nent = 128;
  g_cpuid = calloc(1, sizeof(*g_cpuid) + nent * sizeof(struct kvm_cpuid_entry2));
  g_cpuid->nent = nent;
  if (ioctl(g_kvm, KVM_GET_SUPPORTED_CPUID, g_cpuid) < 0) { g_err = "GET_SUPPORTED_CPUID"; return -1; }
  g_run_size = ioctl(g_kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  if (pthread_key_create(&g_vcpu_key, vcpu_key_dtor) != 0) { g_err = "pthread_key_create"; return -1; }

  return vcpu_init(1);  // bring up the calling thread's vCPU
}

int gk_vcpu_count(void) { return atomic_load(&g_vcpus_created); }

const char *gk_last_error(void) { return g_err; }
unsigned long gk_fault_addr(void) { return g_fault_addr; }
void gk_set_syscall_filter(gk_syscall_filter f) { g_filter = f; }

#define GK_EPERM 1
#define GK_EINVAL 22

// Reload CR3 on this vCPU, flushing its non-global TLB entries.
static void flush_tlb(void) {
  struct kvm_sregs s;
  ioctl(tls.fd, KVM_GET_SREGS, &s);
  ioctl(tls.fd, KVM_SET_SREGS, &s);
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
#define GK_SYS_clone3 435
#define GK_HOST_STACK (512UL << 10)
struct gk_clone_args {  // struct clone_args from <linux/sched.h>
  uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size,
      tls, set_tid, set_tid_size, cgroup;
};
typedef struct {
  struct kvm_regs regs;   // the guest child's initial registers
  uint64_t *active_pml4;
  gk_arena *active_arena;
  void *host_stack;
  size_t host_stack_size;
  int parent_id;
} gk_child;

static long run_vcpu(void);
static int sync_cr3(void);
static void host_pkru_allow_all(void);

static long child_entry(void *arg) {
  gk_child c = *(gk_child *)arg;
  free(arg);
  tls.guest_thread = 1;
  tls.host_stack = c.host_stack;
  tls.host_stack_size = c.host_stack_size;
  tls.active_pml4 = c.active_pml4;
  tls.active_arena = c.active_arena;
  // The kernel already applied CLONE_SETTLS to this host thread, so vcpu_init
  // reads the guest child's TLS base straight from FS.
  if (vcpu_init(0) < 0) {
    fprintf(stderr, "[gk] guest thread: %s failed\n", g_err);
    host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  }
  // Eager-map the whole host mapping holding the child's stack (glibc's stack
  // block, with the thread descriptor and static TLS at its top). Exception
  // delivery pushes a frame on the current stack, so the stack must be present
  // before the first fault or that push double-faults.
  uintptr_t rs, re; int perms;
  pthread_mutex_lock(&g_lock);
  int ok = host_region(c.regs.rsp - 1, &rs, &re, &perms) && (perms & 2) &&
           mmu_map_range(rs, re, perms) == 0;
  pthread_mutex_unlock(&g_lock);
  if (!ok) {
    fprintf(stderr, "[gk] guest thread: cannot map child stack at %#llx\n",
            (unsigned long long)c.regs.rsp);
    host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  }
  if (sync_cr3() < 0) host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  host_pkru_allow_all();
  ioctl(tls.fd, KVM_SET_REGS, &c.regs);
  if (g_dbg)
    fprintf(stderr, "[gk] vcpu %d: guest thread tid %ld (from vcpu %d) rip=%#llx rsp=%#llx\n",
            tls.id, host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0), c.parent_id,
            (unsigned long long)c.regs.rip, (unsigned long long)c.regs.rsp);
  long r = run_vcpu();
  // A guest thread only leaves through exit/exit_group, handled in
  // forward_syscall; reaching here means it faulted or the VM shut down.
  fprintf(stderr, "[gk] vcpu %d: guest thread died: r=%ld fault=%#lx\n", tls.id,
          r, g_fault_addr);
  host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  return r;
}

// Parent side of an intercepted thread-creating clone/clone3. `r` is the
// parent's register snapshot at the syscall trampoline (rip at the outb, rcx =
// the guest's return address). Returns what the guest sees as the clone result.
static long clone_thread(struct kvm_regs *r, long nr) {
  struct gk_clone_args ca;
  uint64_t child_sp;
  size_t ca_size = 0;
  if (nr == GK_SYS_clone3) {
    ca_size = r->rsi;
    // Need at least the fields through `tls` (CLONE_ARGS_SIZE_VER0).
    if (ca_size < 64 || r->rdi == 0) return -GK_EINVAL;
    memset(&ca, 0, sizeof ca);
    memcpy(&ca, (void *)(uintptr_t)r->rdi, ca_size < sizeof ca ? ca_size : sizeof ca);
    child_sp = ca.stack + ca.stack_size;
  } else {
    child_sp = r->rsi;
  }
  if (child_sp == 0) return -GK_EINVAL;

  void *hs = mmap(NULL, GK_HOST_STACK, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (hs == MAP_FAILED) return -errno;
  gk_child *c = calloc(1, sizeof *c);
  if (!c) { munmap(hs, GK_HOST_STACK); return -12; }  // ENOMEM
  c->regs = *r;
  c->regs.rax = 0;          // the child's clone return value
  c->regs.rsp = child_sp;   // the stack the clone named
  c->regs.rip = r->rcx;     // straight to the guest's return address
  c->active_pml4 = tls.active_pml4;
  c->active_arena = tls.active_arena;
  c->host_stack = hs;
  c->host_stack_size = GK_HOST_STACK;
  c->parent_id = tls.id;
  // gk_host_clone_raw's child pops [arg, fn] off its initial stack.
  uint64_t top = (uint64_t)(uintptr_t)hs + GK_HOST_STACK - 16;
  ((uint64_t *)(uintptr_t)top)[0] = (uint64_t)(uintptr_t)c;
  ((uint64_t *)(uintptr_t)top)[1] = (uint64_t)(uintptr_t)&child_entry;

  long tid;
  if (nr == GK_SYS_clone3) {
    ca.stack = (uint64_t)(uintptr_t)hs;
    ca.stack_size = GK_HOST_STACK - 16;
    tid = gk_host_clone_raw(GK_SYS_clone3, (long)&ca, (long)ca_size, 0, 0, 0);
  } else {
    // clone(flags, stack, parent_tid, child_tid, tls)
    tid = gk_host_clone_raw(SYS_clone, (long)r->rdi, (long)top, (long)r->rdx,
                            (long)r->r10, (long)r->r8);
  }
  if (tid < 0) {
    free(c);
    munmap(hs, GK_HOST_STACK);
  }
  return tid;
}

// ---- syscall forwarding ----------------------------------------------------
// Every syscall is re-issued on the host; guest and host share memory, so
// pointer arguments work directly. New mappings (mmap/brk/mprotect) are picked
// up lazily by demand paging on first access, so nothing special is needed here.
static long forward_syscall(struct kvm_regs *r) {
  long nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx, a4 = r->r10,
       a5 = r->r8, a6 = r->r9;
  if (g_filter && !g_filter(nr, a1, a2, a3, a4, a5, a6)) {
    if (g_dbg) fprintf(stderr, "[gk] vcpu %d syscall %ld DENIED\n", tls.id, nr);
    return -GK_EPERM;
  }
  // Thread creation: the child must enter the guest too. Only true threads
  // (CLONE_VM|CLONE_THREAD) are intercepted; fork/vfork-style clones forward.
  if (nr == SYS_clone || nr == GK_SYS_clone3) {
    uint64_t flags = 0;
    if (nr == SYS_clone) flags = (uint64_t)a1;
    else if (a1 && (uint64_t)a2 >= 64) flags = ((struct gk_clone_args *)a1)->flags;
    if ((flags & (GK_CLONE_VM | GK_CLONE_THREAD)) == (GK_CLONE_VM | GK_CLONE_THREAD)) {
      long tid = clone_thread(r, nr);
      if (g_dbg)
        fprintf(stderr, "[gk] vcpu %d syscall %ld (thread clone, flags=%#lx) -> %ld\n",
                tls.id, nr, (unsigned long)flags, tid);
      return tid;
    }
  }
  // A guest-created thread ending: park its vCPU for reuse, release its host
  // stack, then exit the host thread so the kernel's CLONE_CHILD_CLEARTID wakes
  // joiners.
  if (nr == SYS_exit && tls.guest_thread) {
    if (g_dbg) fprintf(stderr, "[gk] vcpu %d syscall %ld (thread exit %ld)\n", tls.id, nr, a1);
    vcpu_park();
    gk_host_unmapself_exit(tls.host_stack, tls.host_stack_size, a1);
  }
  // Isolates are separated by page-table roots, not host protection keys, so
  // reduce pkey_mprotect to a plain mprotect: the guest PTEs carry the R/W/X
  // perms and no host-side pkey needs to be assigned.
  //
  // Protection/mapping changes are reflected by clearing the guest PTEs for the
  // affected range so the next access re-faults and demand-maps with the new
  // host permissions (this is what makes W^X and JIT code work), then flushing
  // this vCPU's TLB. The host call and the PTE clearing happen under g_lock so
  // a demand fault on another vCPU cannot install a PTE with the old protection
  // in between. The memslot backing is left in place: guest-physical equals
  // host-virtual, so a memslot validly covers its range whether or not the host
  // currently has memory there, and if the range is later remapped at the same
  // address the same memslot backs it. Deleting and recreating memslots here
  // instead corrupts memory the guest is actively using.
  //
  // Other vCPUs' TLBs are not shot down: a stale entry there only carries the
  // old protection, and a fault against it is resolved by the retry path in
  // run_vcpu, which flushes that vCPU's TLB when a fault repeats.
  long ret;
  int reflect = (nr == SYS_mprotect || nr == SYS_munmap ||
                 nr == SYS_pkey_mprotect ||
                 (nr == SYS_mmap && (a4 & MAP_FIXED))) && a2 > 0;
  if (reflect) pthread_mutex_lock(&g_lock);
  if (nr == SYS_pkey_mprotect)
    ret = host_syscall(SYS_mprotect, a1, a2, a3, 0, 0, 0);
  else
    ret = host_syscall(nr, a1, a2, a3, a4, a5, a6);
  if (reflect) {
    if (nr == SYS_mmap ? ret == a1 : ret == 0) {
      for (uintptr_t v = (uintptr_t)a1 & ~0xfffUL;
           v < (uintptr_t)a1 + (uintptr_t)a2; v += 0x1000)
        unmap4k_root(g_pml4, v);
    }
    pthread_mutex_unlock(&g_lock);
    flush_tlb();
  }
  if (g_dbg)
    fprintf(stderr, "[gk] vcpu %d syscall %ld(%#lx, %#lx, %#lx, %#lx) -> %ld\n",
            tls.id, nr, (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
            (unsigned long)a4, ret);
  return ret;
}

// V8's sandbox write-protects its pointer tables with a memory protection key
// and flips PKRU only around controlled writes. In the guest those WRPKRUs set
// the guest PKRU, but KVM's host-side page backing (get_user_pages) uses the
// host thread's PKRU, which would still deny the write and fault. Clear the host
// PKRU so KVM can always back the guest's access.
// TODO: to keep in-guest pkey protections meaningful, reflect the guest PTE
// protection-key bits and guest PKRU into the host instead of disabling.
static void host_pkru_allow_all(void) {
  __asm__ __volatile__("xor %%ecx,%%ecx\n\t"
                       "xor %%edx,%%edx\n\t"
                       "xor %%eax,%%eax\n\t"
                       "wrpkru"
                       ::: "eax", "ecx", "edx");
}

// Point this vCPU's CR3 at the thread's active root. Refreshes the active
// arena's shared (non-cage) entries from the base root so it sees every mapping
// added since the arena was created, keeping its cage.
static int sync_cr3(void) {
  if (tls.active_arena) {
    gk_arena *a = tls.active_arena;
    pthread_mutex_lock(&g_lock);
    uint64_t saved = a->pml4[a->cage_idx];
    memcpy(a->pml4, g_pml4, 0x1000);
    a->pml4[a->cage_idx] = saved ? saved : a->cage_entry;
    pthread_mutex_unlock(&g_lock);
  }
  uint64_t want_cr3 = (uint64_t)(uintptr_t)tls.active_pml4;
  if (want_cr3 != tls.loaded_cr3) {
    struct kvm_sregs s;
    ioctl(tls.fd, KVM_GET_SREGS, &s);
    s.cr3 = want_cr3;
    if (ioctl(tls.fd, KVM_SET_SREGS, &s) < 0) return -1;
    tls.loaded_cr3 = want_cr3;
  }
  return 0;
}

long gk_run(long (*fn)(void *), void *arg) {
  if (vcpu_init(1) < 0) return -1;
  if (sync_cr3() < 0) return -1;
  host_pkru_allow_all();
  uint64_t sp = tls.stack_top - 8;
  *(uint64_t *)sp = (uintptr_t)&gk_exit_tramp;

  struct kvm_regs regs = {0};
  regs.rip = (uintptr_t)fn;
  regs.rsp = sp;
  regs.rdi = (uintptr_t)arg;
  regs.rflags = 0x2;
  ioctl(tls.fd, KVM_SET_REGS, &regs);
  return run_vcpu();
}

// Run this thread's vCPU from its current register state until the guest exits
// (via gk_exit_tramp) or faults.
static long run_vcpu(void) {
  for (;;) {
    if (ioctl(tls.fd, KVM_RUN, 0) < 0) {
      if (errno == EINTR) continue;
      if (g_dbg) {
        struct kvm_regs rr; struct kvm_sregs sr;
        ioctl(tls.fd, KVM_GET_REGS, &rr); ioctl(tls.fd, KVM_GET_SREGS, &sr);
        fprintf(stderr, "[gk] KVM_RUN errno=%d rip=%#llx cr2=%#llx (demand ok=%ld)\n",
                errno, (unsigned long long)rr.rip, (unsigned long long)sr.cr2,
                g_demand_ok);
      }
      return -1;
    }
    switch (tls.run->exit_reason) {
      case KVM_EXIT_IO: {
        if (tls.run->io.direction != KVM_EXIT_IO_OUT) break;
        uint16_t port = tls.run->io.port;
        if (port == PORT_SYSCALL) {
          struct kvm_regs r;
          ioctl(tls.fd, KVM_GET_REGS, &r);
          r.rax = (uint64_t)forward_syscall(&r);
          ioctl(tls.fd, KVM_SET_REGS, &r);
        } else if (port == PORT_EXIT) {
          struct kvm_regs r;
          ioctl(tls.fd, KVM_GET_REGS, &r);
          return (long)r.rax;
        } else if (port == PORT_UDRIP) {
          struct kvm_regs r;
          ioctl(tls.fd, KVM_GET_REGS, &r);
          g_fault_addr = r.rax;
          if (g_dbg) fprintf(stderr, "[gk] #UD at rip=%#llx\n", (unsigned long long)r.rax);
          return GK_EFAULT;
        } else if (port == PORT_DEMAND) {
          struct kvm_sregs s;
          ioctl(tls.fd, KVM_GET_SREGS, &s);
          uint64_t cr2 = s.cr2;
          int repeat = (cr2 == tls.last_fault);
          if (repeat) {
            if (++tls.fault_repeat > 2) { g_fault_addr = cr2; return GK_EFAULT; }
          } else { tls.last_fault = cr2; tls.fault_repeat = 0; }
          if (demand_map((uintptr_t)cr2) < 0) {
            if (g_dbg) {
              struct kvm_regs rr;
              ioctl(tls.fd, KVM_GET_REGS, &rr);
              // The #PF frame on the guest stack: [err, rip, cs, rflags, rsp].
              uint64_t *frame = (uint64_t *)(uintptr_t)rr.rsp;
              fprintf(stderr, "[gk] vcpu %d: unmappable fault cr2=%#llx rip=%#llx err=%#llx rsp=%#llx rax=%#llx\n",
                      tls.id, (unsigned long long)cr2, (unsigned long long)frame[1],
                      (unsigned long long)frame[0], (unsigned long long)frame[4],
                      (unsigned long long)rr.rax);
              // Best-effort frame-pointer walk of the guest stack (host memory,
              // identity mapped); stops when the chain leaves the stack.
              uint64_t bp = rr.rbp, lo = frame[4], hi = frame[4] + (8UL << 20);
              for (int i = 0; i < 16 && bp >= lo && bp + 16 <= hi; i++) {
                uint64_t *f = (uint64_t *)(uintptr_t)bp;
                fprintf(stderr, "[gk]   frame %2d: ret=%#llx\n", i, (unsigned long long)f[1]);
                if (f[0] <= bp) break;
                bp = f[0];
              }
            }
            g_fault_addr = cr2;
            return GK_EFAULT;
          }
          // A repeated fault on a page that is mapped means this vCPU's TLB
          // holds a stale entry (another vCPU changed the PTE and only flushed
          // its own TLB); drop it before retrying.
          if (repeat) flush_tlb();
        } else if (port == PORT_FAULT) {
          struct kvm_regs rr; struct kvm_sregs sr;
          ioctl(tls.fd, KVM_GET_REGS, &rr);
          ioctl(tls.fd, KVM_GET_SREGS, &sr);
          if (g_dbg) fprintf(stderr, "[gk] fatal exception, handler_rax=%#llx cr2=%#llx\n",
                             (unsigned long long)rr.rax, (unsigned long long)sr.cr2);
          g_fault_addr = rr.rax;
          return GK_EFAULT;
        }
        break;
      }
      case KVM_EXIT_HLT:
        return 0;
      case KVM_EXIT_SHUTDOWN:
        return GK_ESHUTDOWN;
      default:
        if (g_dbg) fprintf(stderr, "[gk] unexpected exit reason=%u\n", tls.run->exit_reason);
        return -1;
    }
  }
}

// ---- arenas ----------------------------------------------------------------
gk_arena *gk_arena_create(size_t size) {
  size = (size + 0xfff) & ~0xfffUL;
  if (size == 0 || size > (1UL << 39)) return NULL;
  pthread_mutex_lock(&g_lock);
  uint64_t va = (uint64_t)(g_next_cage_idx) << 39;
  void *mem = mmap((void *)(uintptr_t)va, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (mem == MAP_FAILED || (uintptr_t)mem != va) { pthread_mutex_unlock(&g_lock); return NULL; }
  g_next_cage_idx++;
  gk_arena *a = calloc(1, sizeof *a);
  if (!a) { munmap(mem, size); pthread_mutex_unlock(&g_lock); return NULL; }
  a->base = mem;
  a->size = size;
  a->pml4 = alloc_table();
  a->cage_idx = (int)((va >> 39) & 0x1ff);
  memcpy(a->pml4, g_pml4, 0x1000);  // share the base root's top-level entries
  if (region_ensure(va, va + size) < 0) { free(a); pthread_mutex_unlock(&g_lock); return NULL; }
  for (uint64_t v = va; v < va + size; v += 0x1000)
    if (map4k_root(a->pml4, v, 1 | 2) < 0) { free(a); pthread_mutex_unlock(&g_lock); return NULL; }
  a->cage_entry = a->pml4[a->cage_idx];  // remember the private cage subtree
  if (g_arena_n < GK_MAX_ARENAS) {
    g_arenas[g_arena_n].base = va;
    g_arenas[g_arena_n].end = va + size;
    g_arena_n++;
  }
  pthread_mutex_unlock(&g_lock);
  return a;
}

void *gk_arena_base(const gk_arena *a) { return a ? a->base : NULL; }
size_t gk_arena_size(const gk_arena *a) { return a ? a->size : 0; }

gk_arena *gk_arena_enter(gk_arena *a) {
  gk_arena *prev = tls.active_arena;
  tls.active_arena = a;
  tls.active_pml4 = a ? a->pml4 : g_pml4;
  return prev;
}

void gk_arena_destroy(gk_arena *a) {
  if (!a) return;
  if (tls.active_arena == a) gk_arena_enter(NULL);
  munmap(a->base, a->size);
  free(a);
}
