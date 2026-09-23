// gk implementation. See gk.h.
#define _GNU_SOURCE
#include "gk.h"
#include <errno.h>
#include <string.h>

#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <signal.h>
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
#define PTE_PKEY_SHIFT 59        // bits 62:59 hold the page's protection key
#define PTE_PKEY_MASK (0xfUL << PTE_PKEY_SHIFT)
#define PF_ERR_PK (1UL << 5)     // #PF error code: protection-key violation
#define CR0_PE (1UL << 0)
#define CR0_WP (1UL << 16)
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
#define PORT_PKRU 0xFB
#define PORT_FLUSH 0xFA

// Reserved syscall number a ring-3 guest issues to leave the guest (see
// gk_user_exit_tramp in gk_asm.S). Ring-3 code cannot execute OUT, so it cannot
// use the PORT_EXIT hypercall the ring-0 path uses; it returns its result
// through this sentinel syscall instead. Well above any real Linux syscall
// number, so it never collides with a forwarded call. Keep in sync with the
// literal in gk_asm.S.
#define GK_EXIT_SYSCALL 0xF4240

// ---- global platform state (set once by gk_init) ---------------------------
static int g_kvm = -1, g_vmfd = -1;
static uint64_t *g_pml4;             // base page-table root
static uint8_t *g_pt_next, *g_pt_end;
static uintptr_t g_pt_base;
static size_t g_pt_bytes;
static uint64_t *g_pt_free;          // freed page-table pages, linked through their first word
static long g_pt_used, g_pt_free_n;  // pages handed out and not returned; pages on the free list
static uint64_t g_gdt_va, g_idt_va;
static int g_next_slot;              // KVM memslot ids handed out, ever (see the region pool)
static int g_run_size;
static struct kvm_cpuid2 *g_cpuid;   // host CPUID, applied to every vCPU
static const char *g_err;
static unsigned long g_fault_addr;
static int g_dbg;
static gk_syscall_filter g_filter;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_demand_ok;
// Bumped whenever page-table pages are returned to the allocator (an arena
// was destroyed). A vCPU whose root is unchanged since it last entered the
// guest may still hold TLB and paging-structure-cache entries derived from
// tables that have since been freed and reused, so it flushes at its next
// entry when it sees a new generation (see sync_cr3).
static unsigned g_root_gen = 1;

// ---- vCPU pool -------------------------------------------------------------
// KVM never destroys a vCPU before the VM (closing its fd only drops a
// reference) and rejects a vcpu id that was already created, so a vCPU per
// thread ever created would exhaust KVM_CAP_MAX_VCPUS under thread churn.
// Instead a vCPU whose thread ends is parked here and handed to the next thread
// that needs one; a vCPU may be driven by a different host thread on each
// KVM_RUN. Live vCPUs are therefore bounded by peak concurrent guest threads.
// The pool holds the eager-mapped guest stack and the gk_run_here side stack
// too, when the vCPU had them, so stacks are recycled rather than leaked.
// Guarded by g_lock.
#define GK_MAX_VCPUS 4096
#define GK_IST_BYTES (64UL << 10)   // per-vCPU exception stack, TSS in its last page
#define GK_SIDE_STACK (512UL << 10) // host-side stack for gk_run_here's gk loop
typedef struct {
  int fd, id;
  struct kvm_run *run;
  uint64_t stack_top;   // 0 if the vCPU has no private guest stack
  uint64_t ist;         // its exception stack + TSS region (see vcpu_init)
  void *side_stack;     // NULL if the vCPU has no gk_run_here side stack
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
  void *side_stack;        // host stack for gk_run_here's gk loop (see there)
  uint64_t loaded_cr3;
  unsigned root_gen;       // g_root_gen as of this vCPU's last TLB flush (0: never)
  uint64_t last_fault;
  int fault_repeat;
  int pkru_ready;          // guest PKRU has been given its initial value
  int user_mode;           // the current invocation runs fn at ring 3 (gk_run_user)
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

// An arena is a PROT_NONE host reservation spanning whole PML4 slots (512GiB
// each), demand-paged into its private page-table root only while it is a
// vCPU's active arena. The root shares the base root's top-level entries for
// everything outside the arena (refreshed at each guest entry, see sync_cr3),
// but each of the arena's slots points at a PDPT owned by this arena alone, so
// no other root can ever reach the arena's pages. Nothing is mapped up front:
// the owner (V8) commits sub-ranges with mprotect through forwarded syscalls,
// and the guest's first access to a committed page installs its PTE under the
// arena's private subtree with the page's current host protection.
#define GK_SLOT_BITS 39
#define GK_SLOT_BYTES (1UL << GK_SLOT_BITS)
#define GK_MAX_ARENA_SLOTS 8   // 4TiB per arena; V8's largest sandbox reservation is ~1.34TB
#define GK_USER_SLOTS 256      // PML4 slots 0..255 are the 47-bit user address space
#define GK_FIRST_CAGE_SLOT 64  // arenas are placed from here up
struct gk_arena {
  uintptr_t base;             // slot-aligned reservation start
  size_t size;                // usable size, as requested (page-rounded)
  uintptr_t end;              // base + nslots * GK_SLOT_BYTES: the whole reservation
  uint64_t *pml4;
  int slot0, nslots;          // the contiguous PML4 slots this arena owns
  uint64_t slot_entry[GK_MAX_ARENA_SLOTS];  // its private PDPT entry per slot
  // Threads that currently have this arena active (gk_arena_enter, and guest
  // threads that inherited it). gk_arena_destroy refuses to recycle the
  // arena's page tables, slots and struct while any remain, since their
  // vCPUs would otherwise run under tables that now belong to someone else.
  int active_threads;
};
// Registry of live arenas, so demand paging can tell a fault inside the active
// arena (backed, into that arena's root) from one inside any other arena (a
// cross-arena access, refused), and so protection changes can be reflected
// into every arena root they touch. Guarded by g_lock.
#define GK_MAX_ARENAS 4096
static gk_arena *g_arenas[GK_MAX_ARENAS];
static int g_arena_n;
// Which PML4 slots live arenas own. A destroyed arena's slots are reused by
// later arenas, so slot churn does not run through the user address space.
static unsigned char g_slot_used[GK_USER_SLOTS];
static gk_arena *arena_containing(uintptr_t a) {
  for (int i = 0; i < g_arena_n; i++)
    if (a >= g_arenas[i]->base && a < g_arenas[i]->end) return g_arenas[i];
  return NULL;
}

// Trampolines and exception handlers (gk_asm.S), in this binary's mapped text.
extern void gk_syscall_tramp(void);
extern void gk_syscall_tramp_resume(void);
extern void gk_user_syscall_resume(void);
extern void gk_user_launch(void);
extern void gk_exit_tramp(void);
extern void gk_user_exit_tramp(void);
extern void gk_exc_de(void), gk_exc_ud(void), gk_exc_df(void), gk_exc_gp(void),
    gk_exc_pf(void);
extern void gk_pkru_stub(void);
extern void gk_flush_stub(void);
// Host-side helpers for guest thread creation (gk_asm.S); never run in the guest.
extern long gk_host_clone_raw(long nr, long a1, long a2, long a3, long a4,
                              long a5);
extern void gk_host_unmapself_exit(void *addr, size_t len, long code)
    __attribute__((noreturn));
extern long gk_host_call_on_stack(void *stack_top,
                                  long (*f)(void *ctx, unsigned long caller_sp),
                                  void *ctx);

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

// Page-table page allocator over a fixed arena: a bump allocator with a free
// list in front of it, so the tables of a destroyed arena serve the next one.
// Freed pages are linked through their first word; the page is zeroed again
// when handed out. Caller holds g_lock (except in single-threaded gk_init).
static uint64_t *alloc_table(void) {
  uint64_t *p;
  if (g_pt_free) {
    p = g_pt_free;
    g_pt_free = *(uint64_t **)p;
    g_pt_free_n--;
  } else {
    if (g_pt_next + 0x1000 > g_pt_end) return NULL;
    p = (uint64_t *)g_pt_next;
    g_pt_next += 0x1000;
  }
  memset(p, 0, 0x1000);
  g_pt_used++;
  return p;
}

// Return a page-table page. The caller guarantees no root reaches it any more.
static void free_table(uint64_t *p) {
  *(uint64_t **)p = g_pt_free;
  g_pt_free = p;
  g_pt_free_n++;
  g_pt_used--;
}

// Free a table and every table beneath it. `level` is the table's paging
// level: 3 for a PDPT, 2 for a PD, 1 for a PT (whose entries are pages, not
// tables). gk maps only 4KiB pages, so every present entry above level 1
// points at a table. Caller holds g_lock.
static void free_subtree(uint64_t *tbl, int level) {
  if (level > 1)
    for (int i = 0; i < 512; i++)
      if (tbl[i] & PTE_P) free_subtree((uint64_t *)(uintptr_t)(tbl[i] & ~0xfffULL), level - 1);
  free_table(tbl);
}

static uint64_t *next_table(uint64_t *tbl, int idx) {
  if (!(tbl[idx] & PTE_P)) {
    uint64_t *n = alloc_table();
    if (!n) return NULL;
    tbl[idx] = ((uint64_t)(uintptr_t)n) | PTE_P | PTE_W | PTE_U;
  }
  return (uint64_t *)(uintptr_t)(tbl[idx] & ~0xfffULL);
}

// ---- the supervisor/refuse registry ----------------------------------------
// The guest's untrusted code runs in ring 3 (see gk_run_user), so it must not
// be able to reach gk's own control structures even with arbitrary code
// execution. Every guest page falls into one of three classes, decided by
// address here and honored by map4k_root (leaf U bit) and demand_map:
//
//  KEEP (default)  the page is user-accessible (PTE_U set): the isolate arena,
//                  the runtime .text/.data and glibc, the execution stack.
//  SUPERVISOR      mapped present but with U cleared, so ring-0 (gk's fault and
//                  syscall handlers, the CPU's descriptor-table walks) reaches
//                  it while ring 3 faults: the handler text, the GDT/IDT page,
//                  and each vCPU's IST/TSS/RSP0 region.
//  REFUSE          never mapped into any guest root, so a fault there is a
//                  genuine fault even though the host has the memory: the
//                  page-table arena, each vCPU's kvm_run mmap, and the gk-loop
//                  side and host stacks. Without this, ring-3 code could fault
//                  gk's own page tables in as writable user memory and rewrite
//                  the walls.
//
// A ring's access check ANDs the U bit down the whole path, so clearing U on
// the leaf alone makes a page supervisor even though intermediate tables (which
// user pages under the same 2MB/1GB share) keep U set. Ranges are disjoint and
// address-sorted; lookups run on the fault path, so this is a static array with
// a binary-search lookup and no allocation. Guarded by g_lock.
#define GK_PROT_KEEP 0
#define GK_PROT_SUPER 1
#define GK_PROT_REFUSE 2
#define GK_MAX_PROT_RANGES 8192
typedef struct { uintptr_t start, end; int kind; } gk_prot_range;
static gk_prot_range g_prot[GK_MAX_PROT_RANGES];
static int g_prot_n;

// The class of the page holding `addr` (GK_PROT_KEEP if unregistered). Caller
// holds g_lock.
static int prot_class(uintptr_t addr) {
  int lo = 0, hi = g_prot_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (addr < g_prot[mid].start) hi = mid;
    else if (addr >= g_prot[mid].end) lo = mid + 1;
    else return g_prot[mid].kind;
  }
  return GK_PROT_KEEP;
}

// Register [s, e) (page-aligned) as SUPER or REFUSE, keeping the array sorted by
// start. The regions registered are fixed platform structures (allocated once
// and, for pooled vCPUs, never freed), so ranges never overlap and are never
// removed. Caller holds g_lock (or is single-threaded gk_init).
static void prot_add(uintptr_t s, uintptr_t e, int kind) {
  if (g_prot_n >= GK_MAX_PROT_RANGES) {
    if (g_dbg) fprintf(stderr, "[gk] prot registry full; [%#lx,%#lx) unregistered\n",
                       (unsigned long)s, (unsigned long)e);
    return;
  }
  int i = 0;
  while (i < g_prot_n && g_prot[i].start < s) i++;
  memmove(&g_prot[i + 1], &g_prot[i], (size_t)(g_prot_n - i) * sizeof g_prot[0]);
  g_prot[i] = (gk_prot_range){s, e, kind};
  g_prot_n++;
}

// Four-level map of one page; guest-virtual == guest-physical == host-virtual.
// `pkey` (0..15) is the page's protection key, placed in PTE bits 62:59; the
// guest CPU checks it against the guest PKRU on every data access (with
// CR4.PKE and CR0.WP set, see vcpu_init), which is what makes the guest's own
// protection keys real. Key 0 is the default, unrestricted key.
//
// The leaf's U bit follows the supervisor/refuse registry: a SUPER page is
// mapped with U cleared (ring 3 cannot reach it), a REFUSE page is never mapped
// at all, and everything else is user-accessible. Caller holds g_lock.
static int map4k_root(uint64_t *root, uint64_t va, uint64_t flags, int pkey) {
  int cls = prot_class(va & ~0xfffULL);
  if (cls == GK_PROT_REFUSE) return -1;  // a control page: never reachable
  uint64_t *pdpt = next_table(root, (va >> 39) & 0x1ff);
  if (!pdpt) return -1;
  uint64_t *pd = next_table(pdpt, (va >> 30) & 0x1ff);
  if (!pd) return -1;
  uint64_t *pt = next_table(pd, (va >> 21) & 0x1ff);
  if (!pt) return -1;
  // `flags` is a protection bitmask: 1=read, 2=write, 4=execute. Honor it so
  // W^X holds: code is mapped executable but not writable, data writable but
  // not executable. KVM also enforces the host VMA's real protection.
  uint64_t pte = (va & ~0xfffULL) | PTE_P;
  if (cls != GK_PROT_SUPER) pte |= PTE_U;  // user pages only; supervisor clears U
  if (flags & 2) pte |= PTE_W;
  if (!(flags & 4)) pte |= PTE_NX;
  pte |= ((uint64_t)pkey << PTE_PKEY_SHIFT) & PTE_PKEY_MASK;
  pt[(va >> 12) & 0x1ff] = pte;
  return 0;
}

// Clear the PTEs of [s, e) (page-aligned) in a root, to reflect an mprotect,
// munmap or fixed mmap. Walks the hierarchy and skips a whole 512GiB, 1GiB or
// 2MiB range at once where no table exists beneath it, so reflecting a change
// over a large, sparsely committed reservation costs in proportion to what is
// mapped, not to the range. Leaves intermediate tables in place. Caller holds
// g_lock.
#define GK_NEXT_BOUNDARY(v, bits) ((((v) >> (bits)) + 1) << (bits))
static void unmap_range_root(uint64_t *root, uintptr_t s, uintptr_t e) {
  for (uintptr_t v = s; v < e;) {
    uint64_t pml4e = root[(v >> 39) & 0x1ff];
    if (!(pml4e & PTE_P)) { v = GK_NEXT_BOUNDARY(v, 39); continue; }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & ~0xfffULL);
    uint64_t pdpte = pdpt[(v >> 30) & 0x1ff];
    if (!(pdpte & PTE_P)) { v = GK_NEXT_BOUNDARY(v, 30); continue; }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & ~0xfffULL);
    uint64_t pde = pd[(v >> 21) & 0x1ff];
    if (!(pde & PTE_P)) { v = GK_NEXT_BOUNDARY(v, 21); continue; }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & ~0xfffULL);
    pt[(v >> 12) & 0x1ff] = 0;
    v += 0x1000;
  }
}

// Reflect a host protection or mapping change over [s, e) into every root that
// may hold PTEs for it: the base root (whose subtrees every arena root shares
// for addresses outside arenas) and each arena whose reservation the range
// overlaps (their private subtrees are reachable from no other root). A stale
// PTE over a page the host has decommitted would otherwise make the guest's
// next access an unrecoverable KVM_RUN EFAULT rather than a demand fault.
// Caller holds g_lock.
static void unmap_range_all(uintptr_t s, uintptr_t e) {
  unmap_range_root(g_pml4, s, e);
  for (int i = 0; i < g_arena_n; i++) {
    gk_arena *a = g_arenas[i];
    uintptr_t lo = s > a->base ? s : a->base, hi = e < a->end ? e : a->end;
    if (lo < hi) unmap_range_root(a->pml4, lo, hi);
  }
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
// This is a cost choice, not a correctness one. Exact per-page memslots are
// safe (given the malloc-free fault path -- see demand_map's signal-safety
// note), but each KVM_SET_USER_MEMORY_REGION costs on the order of 20us, and a
// V8-sized heap backed by 4K slots would blow past KVM's ~32764-memslot limit.
// A window amortizes both: one create backs 2MB, and a mapping's later growth
// usually lands in a window that already exists. Because everything is
// identity-mapped, an oversized window that spills into an unmapped hole is
// harmless -- the guest never has a PTE there, so KVM never faults it in.
// Protection is not cached on the region; the PTE carries the live host
// protection, re-read on every fault.
//
// Memslots are created once and deleted only when the arena they lie in is
// destroyed (see gk_arena_destroy). An identity memslot validly backs its
// window whether or not the host currently has memory there, so mprotect and
// munmap only need to drop the guest PTEs (see forward_syscall), not the
// memslot -- and a delete is not free: KVM invalidates every nested-page-table
// root when a memslot is removed, whereas adding an abutting memslot zaps
// nothing. An arena's teardown is the exception because its address space is
// given back for good: keeping its windows would let memslots grow without
// bound under isolate churn (KVM allows ~32K per VM), so they are deleted and
// their ids and pool nodes reused.
//
// TODO: 2MB windows over-back sparsely committed reservations. A production MMU
// could track exact VMAs (cf. FreeBSD's vm_map RB tree, sys/vm, 2-clause BSD)
// and issue cross-vCPU TLB shootdowns. Exact bounds are already safe, so this is
// a density/cost improvement, not a correctness fix.
#define GK_BACK_WIN (2UL << 20)   // memslot backing granularity (2MB, aligned)
typedef struct gk_region {
  uintptr_t start, end;   // [start, end), GK_BACK_WIN-aligned
  int slot;               // KVM memslot id backing this region
  unsigned prio;          // treap heap priority
  struct gk_region *l, *r;
} gk_region;
static gk_region *g_regions;      // treap root
// Region nodes come from a static pool because the fault path must not allocate.
// The host side of a guest fault runs on the SAME host thread whose guest half
// took the fault, and that thread may be in the middle of glibc malloc -- in
// fact the store that faults is often sysmalloc writing the freshly-grown heap
// top chunk's size field, so at that instant the arena's top chunk has size 0
// and its invariants are momentarily broken. If the fault handler then allocates
// it re-enters that same arena: single-threaded, glibc skips the arena lock and
// the nested malloc corrupts the arena (aborting later inside sysmalloc, at
// malloc.c's top-chunk assertion); multi-threaded, it deadlocks on the arena
// lock the guest half holds. So the fault path -- and the host side of any
// syscall malloc itself forwards, e.g. the brk/mmap sysmalloc issues -- must be
// async-signal-safe with respect to the guest thread: no malloc/free, no
// allocating stdio, no lock the guest may hold. That is why host_region reads
// /proc/self/maps with raw open/read rather than fopen, and region nodes come
// from this pool rather than calloc. (This corruption was once attributed to
// KVM's on-demand memslot creation; the memslot ioctl is not involved -- an
// added, abutting memslot invalidates nothing. See the backing-window note.)
#define GK_MAX_REGIONS 65536
static gk_region g_region_pool[GK_MAX_REGIONS];
static int g_region_n;            // pool nodes ever taken
static gk_region *g_region_free;  // nodes of deleted regions, linked through `l`
static int g_region_live;         // regions currently in the treap (= live memslots)

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

// Unlink `node` (which is in the tree) by rotating it down to a leaf, keeping
// the heap order among the others. Caller holds g_lock.
static gk_region *treap_remove(gk_region *root, gk_region *node) {
  if (!root) return NULL;
  if (node->start < root->start) {
    root->l = treap_remove(root->l, node);
  } else if (node->start > root->start) {
    root->r = treap_remove(root->r, node);
  } else {
    if (!root->l) return root->r;
    if (!root->r) return root->l;
    if (root->l->prio > root->r->prio) {
      root = rot_r(root);
      root->r = treap_remove(root->r, node);
    } else {
      root = rot_l(root);
      root->l = treap_remove(root->l, node);
    }
  }
  return root;
}

// Create one memslot-backed region for [s, e). Caller holds g_lock and has
// ensured [s, e) does not overlap any existing region. A pool node freed by a
// region deletion is reused first, with the KVM memslot id it was created
// with, so both are bounded by the peak number of live regions.
static int region_add(uintptr_t s, uintptr_t e) {
  gk_region *n;
  int slot, recycled = g_region_free != NULL;
  if (recycled) {
    n = g_region_free;
    slot = n->slot;
  } else {
    if (g_region_n >= GK_MAX_REGIONS) return -1;
    n = &g_region_pool[g_region_n];
    slot = g_next_slot;
  }
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)slot,
                                          .guest_phys_addr = s,
                                          .memory_size = e - s,
                                          .userspace_addr = s};
  if (ioctl(g_vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (g_dbg)
      fprintf(stderr, "[gk] memslot %d [%#lx,%#lx) failed: %s\n", slot,
              (unsigned long)s, (unsigned long)e, strerror(errno));
    return -1;  // the node stays where it was (free list or untouched pool tail)
  }
  if (recycled) {
    g_region_free = n->l;
  } else {
    g_region_n++;
    g_next_slot++;
  }
  memset(n, 0, sizeof *n);
  n->start = s;
  n->end = e;
  n->slot = slot;
  n->prio = gk_rand();
  g_regions = treap_insert(g_regions, n);
  g_region_live++;
  return 0;
}

// Delete a region's memslot and drop it from the tree; its node (and memslot
// id) go to the free list. Caller holds g_lock and has made sure no root maps
// a page in the region any more, so the guest cannot reach it. Returns the
// ioctl's result: on failure the region stays.
static int region_remove(gk_region *n) {
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)n->slot,
                                          .guest_phys_addr = n->start,
                                          .memory_size = 0,
                                          .userspace_addr = n->start};
  if (ioctl(g_vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (g_dbg)
      fprintf(stderr, "[gk] memslot %d [%#lx,%#lx) delete failed: %s\n", n->slot,
              (unsigned long)n->start, (unsigned long)n->end, strerror(errno));
    return -1;
  }
  g_regions = treap_remove(g_regions, n);
  g_region_live--;
  n->l = g_region_free;
  g_region_free = n;
  return 0;
}

// The region with the smallest start at or beyond `s`, or NULL. Caller holds
// g_lock.
static gk_region *region_lower_bound(uintptr_t s) {
  gk_region *n = g_regions, *best = NULL;
  while (n) {
    if (n->start >= s) { best = n; n = n->l; }
    else n = n->r;
  }
  return best;
}

// Delete every region lying within [s, e). Regions never straddle a 512GiB
// slot boundary (windows are created within host mappings and arena spans,
// both of which lie within slots), so for a whole-slot range this is every
// region that overlaps it; one that did straddle would be left alone. Returns
// how many were deleted. Caller holds g_lock.
static int region_remove_range(uintptr_t s, uintptr_t e) {
  int n = 0;
  gk_region *r = region_lower_bound(s);
  while (r && r->start < e) {
    gk_region *next = region_succ(r->start);
    if (r->end <= e && region_remove(r) == 0) n++;
    r = next;
  }
  return n;
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

// ---- protection keys ---------------------------------------------------------
// The guest's memory protection keys are virtualized, not emulated: the guest
// runs with CR4.PKE, so its rdpkru/wrpkru operate on the vCPU's own PKRU (KVM
// saves and restores it around every exit), and every guest PTE carries the
// key its page was assigned with pkey_mprotect (map4k_root). The guest CPU then
// enforces the key on every data access exactly as it would natively, and a
// violation arrives as a #PF with the PK error bit (see run_vcpu).
//
// The host side is deliberately kept out of it. A pkey_mprotect is issued on
// the host as a plain mprotect, so no host VMA ever carries a key and KVM's
// page backing (get_user_pages, which checks the host thread's PKRU against the
// host VMA's key) can never pkey-fault whatever the guest has put in its PKRU.
// Only the guest enforces; the host's PKRU is irrelevant to correctness (see
// host_pkru_allow_all).
//
// The key of each range is tracked here, in a sorted array of disjoint
// [start, end) ranges holding a nonzero key (key 0, the default, is implicit).
// Linux semantics are followed: pkey_mprotect with an explicit key sets it, a
// plain mprotect (or pkey -1) keeps the range's key, munmap and mmap drop it
// (a fresh mapping has key 0), and pkey_free leaves it in the PTEs. Lookups
// run on the fault path (demand_map), so the table is static and the lookup a
// binary search: no allocation there. Guarded by g_lock.
// Master switch for protection-key virtualization. gk isolates isolates by
// page-table root, so V8's host-side keys are not needed for security in this
// model; set this to 0 and the guest PTEs carry no key, so nothing is enforced
// and the keys become no-ops -- turning the whole scheme off in one line while
// leaving the rest (W^X, CR0.WP, the real TLB flush) intact.
#ifndef GK_VIRTUALIZE_PKEYS
#define GK_VIRTUALIZE_PKEYS 1
#endif

#define GK_MAX_PKEY_RANGES 4096
typedef struct { uintptr_t start, end; int pkey; } gk_pkey_range;
static gk_pkey_range g_pkeys[GK_MAX_PKEY_RANGES];
static int g_pkey_n;
// Keys the guest holds from pkey_alloc, as a bitmask; key 0 is always held.
// pkey_mprotect validates against this the way the kernel would (EINVAL).
static unsigned g_pkeys_held = 1;

// The key of the page holding `addr`, or 0. Caller holds g_lock.
static int pkey_lookup(uintptr_t addr) {
  if (!GK_VIRTUALIZE_PKEYS) return 0;  // keys not reflected into PTEs -> no-ops
  int lo = 0, hi = g_pkey_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (addr < g_pkeys[mid].start) hi = mid;
    else if (addr >= g_pkeys[mid].end) lo = mid + 1;
    else return g_pkeys[mid].pkey;
  }
  return 0;
}

// Index of the first range whose end is beyond `addr`. Caller holds g_lock.
static int pkey_lower_bound(uintptr_t addr) {
  int lo = 0, hi = g_pkey_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (g_pkeys[mid].end <= addr) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Set the key of [s, e) (page-aligned) to `pkey`, replacing whatever keys the
// range held. Existing ranges overlapping [s, e) are trimmed, split or removed;
// a nonzero key is then inserted and merged with equal-key neighbors. Fails
// only when the table is full (pkey_room() guarantees it is not); the table is
// then left unchanged. Caller holds g_lock.
static int pkey_room(void) { return g_pkey_n + 2 <= GK_MAX_PKEY_RANGES; }
static int pkey_set_range(uintptr_t s, uintptr_t e, int pkey) {
  if (!pkey_room()) return -1;
  int i = pkey_lower_bound(s);
  while (i < g_pkey_n && g_pkeys[i].start < e) {
    gk_pkey_range *r = &g_pkeys[i];
    if (r->start < s && r->end > e) {  // [s, e) is strictly inside r: split it
      memmove(&g_pkeys[i + 2], &g_pkeys[i + 1], (size_t)(g_pkey_n - i - 1) * sizeof *r);
      g_pkey_n++;
      g_pkeys[i + 1] = (gk_pkey_range){e, r->end, r->pkey};
      r->end = s;
      i++;
      break;
    } else if (r->start < s) {  // r sticks out to the left: keep that part
      r->end = s;
      i++;
    } else if (r->end > e) {  // r sticks out to the right: keep that part
      r->start = e;
      break;
    } else {  // r lies within [s, e): drop it
      memmove(r, r + 1, (size_t)(g_pkey_n - i - 1) * sizeof *r);
      g_pkey_n--;
    }
  }
  if (pkey == 0) return 0;
  // Now every range before index i ends at or before s, and every range from i
  // on starts at or after e. Insert [s, e), absorbing adjacent equal keys.
  if (i > 0 && g_pkeys[i - 1].end == s && g_pkeys[i - 1].pkey == pkey) {
    g_pkeys[i - 1].end = e;
    if (i < g_pkey_n && g_pkeys[i].start == e && g_pkeys[i].pkey == pkey) {
      g_pkeys[i - 1].end = g_pkeys[i].end;
      memmove(&g_pkeys[i], &g_pkeys[i + 1], (size_t)(g_pkey_n - i - 1) * sizeof g_pkeys[0]);
      g_pkey_n--;
    }
    return 0;
  }
  if (i < g_pkey_n && g_pkeys[i].start == e && g_pkeys[i].pkey == pkey) {
    g_pkeys[i].start = s;
    return 0;
  }
  memmove(&g_pkeys[i + 1], &g_pkeys[i], (size_t)(g_pkey_n - i) * sizeof g_pkeys[0]);
  g_pkeys[i] = (gk_pkey_range){s, e, pkey};
  g_pkey_n++;
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
    if (map4k_root(g_pml4, v, (uint64_t)perms, pkey_lookup(v)) < 0) return -1;
  return 0;
}

// Allocation guard for the fault path (off by default). The fault handler must
// not call malloc (see the region-pool note): the faulting guest thread may be
// mid-malloc with its arena in a transient state. Building with
// -DGK_GUARD_HANDLER_ALLOC and -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc
// makes any allocation between GK_HANDLER_ENTER/LEAVE abort loudly rather than
// silently corrupt glibc's arena, so a future stray malloc/fopen on this path is
// caught deterministically at any backing granularity. Shipping builds compile
// the macros to nothing and never wrap malloc.
#ifdef GK_GUARD_HANDLER_ALLOC
static __thread int g_in_handler;
#define GK_HANDLER_ENTER() (g_in_handler = 1)
#define GK_HANDLER_LEAVE() (g_in_handler = 0)
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
#else
#define GK_HANDLER_ENTER() ((void)0)
#define GK_HANDLER_LEAVE() ((void)0)
#endif

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
//
// Which root gets the PTE depends on where the page lies. Inside this vCPU's
// active arena, the PTE goes under that arena's private subtree, so the page
// is reachable from that root only; the host protection re-read below is what
// the arena's owner committed the page with (uncommitted pages are PROT_NONE
// and refused). Inside any other arena the fault is refused outright: that is
// a cross-arena access, and it must stay unaddressable even though the host
// could back it. Outside every arena the PTE goes into the base root, whose
// subtrees all arena roots share.
static int demand_map(uintptr_t addr) {
  uintptr_t page = addr & ~0xfffUL;
  uintptr_t rs, re; int perms;
  pthread_mutex_lock(&g_lock);
  uint64_t *root = g_pml4;
  gk_arena *in = arena_containing(page);
  if (in) {
    if (in != tls.active_arena) {
      pthread_mutex_unlock(&g_lock);
      return -1;  // another arena's memory: refuse, whatever the host has there
    }
    root = in->pml4;
  }
  // Consult the supervisor/refuse registry. A REFUSE page (gk's page tables,
  // kvm_run, the gk-loop stacks) is never mapped, so a fault there is genuine
  // even though the host has the memory: this is what stops ring-3 code from
  // faulting gk's own control memory in as ordinary user pages. A SUPER page
  // (handler text, GDT/IDT, IST/TSS) is mapped with U cleared by map4k_root.
  int cls = prot_class(page);
  if (cls == GK_PROT_REFUSE) {
    if (g_dbg)
      fprintf(stderr, "[gk] vcpu %d: REFUSE fault %#lx (gk control region)\n", tls.id,
              (unsigned long)page);
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  int mapped = host_region(page, &rs, &re, &perms);
  if (!mapped) {
    // The page may lie just below a stack the kernel grows on demand (the main
    // thread's, which gk_run_here runs the guest on). Natively the thread's own
    // access would grow it; a guest access never reaches the host's fault
    // handler, so ask the kernel to fault the page here. rt_sigprocmask with a
    // NULL `set` changes nothing, but writing the old mask to `oldset` = page
    // makes the kernel's copy_to_user grow a VM_GROWSDOWN stack exactly as a
    // native write would (and return EFAULT, not a signal, if it cannot).
    // Ordinary unmapped addresses just fail the probe.
    host_syscall(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)page, 8, 0, 0);
    mapped = host_region(page, &rs, &re, &perms);
  }
  if (!mapped || !(perms & 1)) {
    pthread_mutex_unlock(&g_lock);
    return -1;  // not host-readable: a genuine fault
  }
  if (!region_find(page) && region_ensure(page, page + 1) < 0) {
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  int pkey = pkey_lookup(page);
  int r = map4k_root(root, page, (uint64_t)perms, pkey);  // honor R/W/X for W^X
  if (r >= 0) {
    g_demand_ok++;
    // A base-root mapping that populated a fresh top-level entry is not yet in
    // the active arena's root (which copied the base root's entries at entry,
    // see sync_cr3); carry it over now so the retry does not fault again. The
    // index cannot be one of the arena's own slots: the page is outside every
    // arena.
    gk_arena *a = tls.active_arena;
    if (root == g_pml4 && a) {
      int idx = (int)((page >> 39) & 0x1ff);
      if (a->pml4[idx] != g_pml4[idx]) a->pml4[idx] = g_pml4[idx];
    }
  }
  pthread_mutex_unlock(&g_lock);
  if (g_dbg && cls == GK_PROT_SUPER)
    fprintf(stderr, "[gk] vcpu %d: SUPER demand-map %#lx perms=%d (U cleared)\n", tls.id,
            (unsigned long)page, perms);
  if (g_dbg && pkey != 0)
    fprintf(stderr, "[gk] vcpu %d demand-map %#lx perms=%d pkey=%d (PTE bits 62:59)\n",
            tls.id, (unsigned long)page, perms, pkey);
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
    p->side_stack = tls.side_stack;
  } else {  // cannot happen with KVM_CAP_MAX_VCPUS <= GK_MAX_VCPUS; be safe
    munmap(tls.run, g_run_size);
    close(tls.fd);
  }
  pthread_mutex_unlock(&g_lock);
  if (g_dbg) fprintf(stderr, "[gk] vcpu %d parked\n", tls.id);
  tls.inited = 0;
  // The thread is ending: it no longer counts as a user of its active arena.
  gk_arena_enter(NULL);
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
// except its id. `flags`: GK_VCPU_HOST for a host thread entering through
// gk_run or gk_run_here (its vCPU is parked when the thread ends), plus
// GK_VCPU_STACK when it needs the private guest stack (gk_run). A thread the
// guest created itself already has the stack its clone named (see
// clone_thread) and ends through the exit syscall, so it passes neither.
#define GK_VCPU_HOST 1
#define GK_VCPU_STACK 2
static int vcpu_init(int flags) {
  if (tls.inited) return 0;
  int id, fd, reused = 0;
  struct kvm_run *run = NULL;
  uint64_t stack_top = 0, ist = 0;
  void *side_stack = NULL;
  pthread_mutex_lock(&g_lock);
  if (g_parked_n > 0) {
    gk_parked_vcpu *p = &g_parked[--g_parked_n];
    fd = p->fd; id = p->id; run = p->run; stack_top = p->stack_top; ist = p->ist;
    side_stack = p->side_stack;
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
    // kvm_run is host/KVM shared state; the IST region holds the exception
    // stack, TSS and RSP0 stack. Ring 3 must reach neither: refuse the former,
    // map the latter supervisor (registered before mapping so U is cleared).
    prot_add((uintptr_t)run, (uintptr_t)run + g_run_size, GK_PROT_REFUSE);
    prot_add((uintptr_t)ir, (uintptr_t)ir + GK_IST_BYTES, GK_PROT_SUPER);
    int irc = mmu_map_range((uintptr_t)ir, (uintptr_t)ir + GK_IST_BYTES, 1 | 2);
    pthread_mutex_unlock(&g_lock);
    if (irc < 0) { g_err = "map exception stack"; return -1; }
    ist = (uint64_t)(uintptr_t)ir;
    uint8_t *tss = ir + GK_IST_BYTES - 0x1000;
    *(uint64_t *)(tss + 36) = (uint64_t)(uintptr_t)tss;  // IST1: stack top is just below the TSS
    // RSP0: the ring-0 stack the CPU switches to on a ring-3 -> ring-0 gate that
    // does not use an IST (all gk gates use IST1, so this is defensive). A
    // dedicated page at the bottom of the region, disjoint from IST1's downward
    // growth from just below the TSS.
    *(uint64_t *)(tss + 4) = (uint64_t)(uintptr_t)ir + 0x1000;
    *(uint16_t *)(tss + 102) = 0x68;                     // I/O map base past the limit
  }

  // A private guest stack for this vCPU, eager-mapped so interrupt delivery
  // (which pushes a frame) never itself faults. A parked vCPU may bring one
  // along (and a gk_run_here side stack); they are kept (and parked again
  // later) even if this thread has no use for them, so stacks are neither
  // leaked nor left with dangling PTEs.
  const size_t stksz = 2 * 1024 * 1024;
  if ((flags & GK_VCPU_STACK) && stack_top == 0) {
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
  // The guest runs in ring 0, where the CPU ignores a PTE's write-protect bit
  // and a protection key's write-disable bit unless CR0.WP is set. It is, so
  // read-only pages and write-disabled keys hold for the guest as they would
  // for a user-mode process.
  s.cr0 = CR0_PE | CR0_WP | CR0_PG;
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
  s.gdt.base = g_gdt_va; s.gdt.limit = 6 * 8 - 1;  // through the ring-3 descriptors (0x28)
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
  // STAR[47:32] = 0x08: SYSCALL enters ring-0 CS 0x08, SS 0x10.
  // STAR[63:48] = 0x1b: SYSRET adds 8 and 16 to this and uses the result
  // verbatim -- the RPL is not forced -- so the base must already carry RPL 3.
  // SS = 0x1b + 8 = 0x23 (GDT index 4, ring-3 data), CS = 0x1b + 16 = 0x2b
  // (GDT index 5, ring-3 code). A base of 0x18 (RPL 0) would return ring 3 with
  // RPL-0 selectors, and the IRETQ off a ring-3 fault would then #GP on SS.
  m.e[1].index = MSR_STAR; m.e[1].data = ((uint64_t)0x08 << 32) | ((uint64_t)0x1b << 48);
  m.e[2].index = MSR_LSTAR; m.e[2].data = (uintptr_t)&gk_syscall_tramp;
  m.e[3].index = MSR_SYSCALL_MASK; m.e[3].data = 0x3f7fd5;
  if (ioctl(fd, KVM_SET_MSRS, &m) < 4) { g_err = "KVM_SET_MSRS"; return -1; }

ready:
  tls.id = id;
  tls.fd = fd;
  tls.run = run;
  tls.stack_top = stack_top;
  tls.ist = ist;
  tls.side_stack = side_stack;
  tls.loaded_cr3 = (uint64_t)(uintptr_t)g_pml4;
  tls.root_gen = 0;    // a fresh or reused vCPU flushes its TLB at first entry
  tls.last_fault = 0;
  tls.fault_repeat = 0;
  tls.pkru_ready = 0;  // a fresh or reused vCPU gets its PKRU at first entry
  tls.user_mode = 0;   // ring 0 unless enter_guest is asked for ring 3; guest
                       // threads (child_entry) run at ring 0
  if (!tls.active_pml4) tls.active_pml4 = g_pml4;
  tls.inited = 1;
  // A gk_run thread gives its vCPU back when it ends (see vcpu_key_dtor).
  if (flags & GK_VCPU_HOST) pthread_setspecific(g_vcpu_key, &tls);
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
      (uintptr_t)&gk_exit_tramp, (uintptr_t)&gk_pkru_stub, (uintptr_t)&gk_flush_stub,
      (uintptr_t)&gk_user_syscall_resume, (uintptr_t)&gk_user_launch};
  // gk_user_exit_tramp is deliberately absent: it runs in ring 3, so it must
  // stay a user page (its own page-aligned section, demand-paged as user r-x).
  uintptr_t lo = hs[0], hi = hs[0];
  for (size_t i = 1; i < sizeof hs / sizeof hs[0]; i++) {
    if (hs[i] < lo) lo = hs[i];
    if (hs[i] > hi) hi = hs[i];
  }
  lo &= ~0xfffUL;
  hi = (hi + 64 + 0xfff) & ~0xfffUL;
  // The handlers run in ring 0 and must be unreadable to ring 3, so map them
  // supervisor r-x. Register before mapping so map4k_root clears U.
  prot_add(lo, hi, GK_PROT_SUPER);
  return mmu_map_range(lo, hi, 1 | 4);  // r-x
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
  // The page-table arena is walked by the CPU through guest-physical addresses
  // (it has memslots), never through guest-virtual PTEs. Refuse it so ring-3
  // code cannot fault it in and rewrite the tables that enforce the arenas.
  prot_add(g_pt_base, g_pt_base + g_pt_bytes, GK_PROT_REFUSE);
  g_pml4 = alloc_table();

  uint8_t *tables = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (tables == MAP_FAILED) { g_err = "mmap gdt/idt"; return -1; }
  g_gdt_va = (uintptr_t)tables;
  g_idt_va = (uintptr_t)tables + 0x1000;
  // The GDT and IDT are read by the CPU (a supervisor access) during exception
  // delivery and ring transitions; ring-3 code has no business touching them.
  prot_add(g_gdt_va, g_gdt_va + 0x2000, GK_PROT_SUPER);
  // GDT layout, in the order SYSCALL/SYSRET and the exception frames need:
  //   0x08 ring-0 code64   (SYSCALL loads CS from STAR[47:32] = 0x08)
  //   0x10 ring-0 data     (SYSCALL loads SS = 0x08 + 8)
  //   0x18 unused          (TR's nominal selector; TR uses a cached descriptor)
  //   0x20 ring-3 data     (SYSRET loads SS = STAR[63:48] + 8 = 0x1b + 8 = 0x23)
  //   0x28 ring-3 code64   (SYSRET loads CS = STAR[63:48] + 16 = 0x1b + 16 = 0x2b)
  // STAR[63:48] is 0x1b (RPL 3): SYSRET adds 8/16 and uses the RPL as-is, so
  // ring 3 runs with SS 0x23 and CS 0x2b. The CPU reloads both from the GDT on
  // the IRETQ that returns from a ring-3 fault, so the +8/+16 order, RPL and
  // DPL=3 must all be exact.
  uint64_t *gdt = (uint64_t *)(uintptr_t)g_gdt_va;
  gdt[0] = 0;
  gdt[1] = 0x00AF9A000000FFFFULL;  // ring-0 code64 (DPL 0, L=1)
  gdt[2] = 0x00CF92000000FFFFULL;  // ring-0 data   (DPL 0)
  gdt[3] = 0;
  gdt[4] = 0x00CFF2000000FFFFULL;  // ring-3 data   (DPL 3)
  gdt[5] = 0x00AFFA000000FFFFULL;  // ring-3 code64 (DPL 3, L=1)
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

  return vcpu_init(GK_VCPU_HOST | GK_VCPU_STACK);  // bring up the calling thread's vCPU
}

int gk_vcpu_count(void) { return atomic_load(&g_vcpus_created); }

const char *gk_last_error(void) { return g_err; }
unsigned long gk_fault_addr(void) { return g_fault_addr; }
void gk_set_syscall_filter(gk_syscall_filter f) { g_filter = f; }

// Test/diagnostic hook: hand out one supervisor page (the GDT) and one refused
// page (the page-table arena), so a test can verify that ring-3 code faults on
// gk's own control memory while its own user pages work.
void gk_debug_control_addrs(unsigned long *super, unsigned long *refuse) {
  if (super) *super = (unsigned long)g_gdt_va;
  if (refuse) *refuse = (unsigned long)g_pt_base;
}

#define GK_EPERM 1
#define GK_ENOMEM 12
#define GK_EINVAL 22

// Run one of the gk_asm.S stubs on this vCPU until it reports on `port`.
// Clobbers the vCPU's general registers: the caller reprograms them afterwards
// (gk_run and child_entry set the entry registers; a syscall exit sets the
// snapshot back and, because KVM only completes the pending outb when RIP is
// unchanged, points RIP at gk_syscall_tramp_resume). Sregs are untouched.
static int run_stub(void (*stub)(void), uint64_t rdi, uint64_t rsi, uint16_t port,
                    const char *what) {
  struct kvm_regs regs = {0};
  regs.rip = (uintptr_t)stub;
  regs.rdi = rdi;
  regs.rsi = rsi;
  regs.rflags = 0x2;
  ioctl(tls.fd, KVM_SET_REGS, &regs);
  for (;;) {
    if (ioctl(tls.fd, KVM_RUN, 0) < 0) {
      if (errno == EINTR) continue;
      if (g_dbg) fprintf(stderr, "[gk] vcpu %d: %s stub KVM_RUN: %s\n", tls.id, what, strerror(errno));
      return -1;
    }
    if (tls.run->exit_reason == KVM_EXIT_IO && tls.run->io.direction == KVM_EXIT_IO_OUT &&
        tls.run->io.port == port)
      return 0;
    if (g_dbg)
      fprintf(stderr, "[gk] vcpu %d: %s stub: unexpected exit reason=%u\n", tls.id, what,
              tls.run->exit_reason);
    return -1;
  }
}

// Flush this vCPU's TLB from a syscall exit, whose register snapshot is `r`:
// the guest reloads its CR3 (gk_flush_stub) and resumes after the hypercall.
// No ioctl does this from the host: KVM flushes the guest TLB only when
// KVM_SET_SREGS changes a control register, and re-setting the same values is
// a no-op.
static void flush_tlb(struct kvm_regs *r) {
  if (run_stub(gk_flush_stub, 0, 0, PORT_FLUSH, "flush") == 0)
    r->rip = (uintptr_t)&gk_syscall_tramp_resume;
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
  gk_arena *active_arena;
  void *host_stack;
  size_t host_stack_size;
  int parent_id;
  uint32_t pkru;          // the parent's guest PKRU, inherited like a real clone
} gk_child;

static long run_vcpu(void);
static int sync_cr3(void);
static void host_pkru_allow_all(void);
static uint32_t host_pkru(void);
static int guest_pkru_update(uint32_t keep, uint32_t set, uint32_t *out);

static long child_entry(void *arg) {
  gk_child c = *(gk_child *)arg;
  free(arg);
  tls.guest_thread = 1;
  tls.host_stack = c.host_stack;
  tls.host_stack_size = c.host_stack_size;
  gk_arena_enter(c.active_arena);  // inherits the parent's arena, and counts as a user of it
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
  // A clone's child starts with its parent's PKRU.
  if (guest_pkru_update(0, c.pkru, NULL) < 0) host_syscall(SYS_exit_group, 70, 0, 0, 0, 0, 0);
  tls.pkru_ready = 1;
  ioctl(tls.fd, KVM_SET_REGS, &c.regs);
  if (g_dbg)
    fprintf(stderr, "[gk] vcpu %d: guest thread tid %ld (from vcpu %d) rip=%#llx rsp=%#llx pkru=%#x\n",
            tls.id, host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0), c.parent_id,
            (unsigned long long)c.regs.rip, (unsigned long long)c.regs.rsp, c.pkru);
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

  // The child inherits the parent's PKRU, read from the parent vCPU now.
  uint32_t pkru = 0;
  if (guest_pkru_update(~0u, 0, &pkru) < 0) return -GK_EINVAL;
  r->rip = (uintptr_t)&gk_syscall_tramp_resume;  // the stub consumed the outb

  void *hs = mmap(NULL, GK_HOST_STACK, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (hs == MAP_FAILED) return -errno;
  // A guest thread's host-side gk-loop stack should be REFUSE too, but it is
  // unmapped when the thread exits (unlike the pooled side stack), so a bare
  // registration would outlive it and wrongly refuse whatever later reuses the
  // address. Registering it needs registry removal on thread exit, which is
  // deferred with ring-3 guest threads: in this slice guest threads run at ring
  // 0, so their host stack is not reachable by ring-3 code.
  gk_child *c = calloc(1, sizeof *c);
  if (!c) { munmap(hs, GK_HOST_STACK); return -12; }  // ENOMEM
  c->regs = *r;
  c->regs.rax = 0;          // the child's clone return value
  c->regs.rsp = child_sp;   // the stack the clone named
  c->regs.rip = r->rcx;     // straight to the guest's return address
  c->active_arena = tls.active_arena;
  c->host_stack = hs;
  c->host_stack_size = GK_HOST_STACK;
  c->parent_id = tls.id;
  c->pkru = pkru;
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
  // Protection keys (see the protection-keys section above). pkey_mprotect
  // records the key for the range and goes to the host as a plain mprotect,
  // so the key lives only in the guest PTEs; the PTE clearing below then makes
  // the range re-fault and pick the key up. pkey_alloc and pkey_free are
  // forwarded so the host hands out the key numbers, and the calling vCPU's
  // PKRU gets the new key's initial access rights, as the kernel would give the
  // calling thread.
  //
  // Protection/mapping changes are reflected by clearing the guest PTEs for the
  // affected range, in the base root and in every arena root the range
  // overlaps (see unmap_range_all), so the next access re-faults and
  // demand-maps with the new host permissions (this is what makes W^X, JIT code
  // and V8's commit/decommit of sandbox pages work), then flushing this vCPU's
  // TLB. The host call and the PTE clearing happen under g_lock so a demand
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
  uintptr_t rs = (uintptr_t)a1 & ~0xfffUL;
  uintptr_t re = ((uintptr_t)a1 + (uintptr_t)a2 + 0xfff) & ~0xfffUL;
  if (reflect) pthread_mutex_lock(&g_lock);
  if (nr == SYS_pkey_mprotect) {
    long pkey = a4;
    if (pkey != -1 && (pkey < 0 || pkey > 15 || !(g_pkeys_held & (1u << pkey))))
      ret = -GK_EINVAL;
    else if (pkey != -1 && !pkey_room())
      ret = -GK_ENOMEM;
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
      if ((nr == SYS_munmap || nr == SYS_mmap) && pkey_set_range(rs, re, 0) < 0 && g_dbg)
        fprintf(stderr, "[gk] pkey table full; [%#lx,%#lx) keeps a stale key\n",
                (unsigned long)rs, (unsigned long)re);
      unmap_range_all(rs, re);
    }
    pthread_mutex_unlock(&g_lock);
    flush_tlb(r);
  } else if (nr == SYS_mmap && a2 > 0 && (unsigned long)ret < (unsigned long)-4096) {
    // A fresh mapping, placed by the kernel: whatever key was last recorded
    // for those addresses belonged to a mapping that is gone.
    pthread_mutex_lock(&g_lock);
    if (pkey_set_range((uintptr_t)ret & ~0xfffUL,
                       ((uintptr_t)ret + (uintptr_t)a2 + 0xfff) & ~0xfffUL, 0) < 0 && g_dbg)
      fprintf(stderr, "[gk] pkey table full; mapping at %#lx keeps a stale key\n",
              (unsigned long)ret);
    pthread_mutex_unlock(&g_lock);
  }
  if (nr == SYS_pkey_alloc && ret >= 0 && ret <= 15) {
    // The kernel gave the calling *host* thread the key's initial rights; the
    // guest thread is this vCPU, so give them to its PKRU (2 bits per key:
    // bit 0 = access disable, bit 1 = write disable).
    g_pkeys_held |= 1u << ret;
    uint32_t shift = 2u * (uint32_t)ret, pkru = 0;
    if (guest_pkru_update(~(3u << shift), ((uint32_t)a2 & 3u) << shift, &pkru) == 0)
      r->rip = (uintptr_t)&gk_syscall_tramp_resume;  // the stub consumed the outb
    if (g_dbg)
      fprintf(stderr, "[gk] vcpu %d pkey_alloc -> key %ld, rights %#lx; guest PKRU now %#x\n",
              tls.id, ret, (unsigned long)a2, pkru);
  }
  if (nr == SYS_pkey_free && ret == 0 && a1 >= 1 && a1 <= 15)
    g_pkeys_held &= ~(1u << a1);
  if (g_dbg) {
    fprintf(stderr, "[gk] vcpu %d syscall %ld(%#lx, %#lx, %#lx, %#lx) -> %ld\n",
            tls.id, nr, (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
            (unsigned long)a4, ret);
    if (nr == SYS_pkey_mprotect && ret == 0)
      fprintf(stderr, "[gk] vcpu %d pkey_mprotect [%#lx,%#lx) key %ld: host mprotect only, "
              "guest PTEs will carry key %d\n", tls.id, (unsigned long)rs, (unsigned long)re,
              a4, a4 == -1 ? pkey_lookup(rs) : (int)a4);
  }
  return ret;
}

// The host thread's PKRU is what KVM's page backing (get_user_pages) checks
// against a host VMA's key. No host VMA carries a key (pkey_mprotect reaches
// the host as mprotect), so the host PKRU cannot deny a backing; it is cleared
// anyway so that also holds for memory keyed outside gk's knowledge. The
// guest's protection is the guest PKRU, which KVM keeps separate from this.
static void host_pkru_allow_all(void) {
  __asm__ __volatile__("xor %%ecx,%%ecx\n\t"
                       "xor %%edx,%%edx\n\t"
                       "xor %%eax,%%eax\n\t"
                       "wrpkru"
                       ::: "eax", "ecx", "edx");
}

static uint32_t host_pkru(void) {
  uint32_t v;
  __asm__ __volatile__("rdpkru" : "=a"(v) : "c"(0) : "edx");
  return v;
}

// Set this vCPU's guest PKRU to (pkru & keep) | set and return the new value in
// *out (if non-NULL), by running gk_pkru_stub on the vCPU (see run_stub for the
// register contract). KVM's view of the guest PKRU (KVM_GET_XSAVE) is
// unreliable when the host PKRU is zero, as it is here: the XSAVE header bit
// that says PKRU is present is written from the host value, so the guest's
// value can be reported as zero. Running the stub reads and writes the real
// register.
static int guest_pkru_update(uint32_t keep, uint32_t set, uint32_t *out) {
  if (run_stub(gk_pkru_stub, set, keep, PORT_PKRU, "pkru") < 0) return -1;
  struct kvm_regs regs;
  ioctl(tls.fd, KVM_GET_REGS, &regs);
  if (out) *out = (uint32_t)regs.rax;
  return 0;
}

// Refresh an arena root's shared entries from the base root so it sees every
// mapping added since it was last synced, and put the arena's private PDPT
// entries back in its own slots. Entry by entry, with 8-byte stores, never
// touching the arena's slots: another vCPU may be running under this root at
// the same time (two threads in one arena), and its page walker must never
// see a torn entry or a transiently absent slot. Caller holds g_lock.
static void arena_sync_root(gk_arena *a) {
  for (int i = 0; i < 512; i++) {
    if (i >= a->slot0 && i < a->slot0 + a->nslots) continue;
    if (a->pml4[i] != g_pml4[i]) __atomic_store_n(&a->pml4[i], g_pml4[i], __ATOMIC_RELAXED);
  }
  for (int i = 0; i < a->nslots; i++)
    if (a->pml4[a->slot0 + i] != a->slot_entry[i])
      __atomic_store_n(&a->pml4[a->slot0 + i], a->slot_entry[i], __ATOMIC_RELAXED);
}

// Point this vCPU's CR3 at the thread's active root, after bringing an active
// arena's root up to date with the base root (see arena_sync_root). A changed
// CR3 makes KVM flush the guest TLB; an unchanged one does not, so when
// page-table pages were freed since this vCPU last flushed (g_root_gen moved),
// the guest reloads CR3 itself (gk_flush_stub): the same root page may by now
// be a different arena's, or its subtrees may have been rebuilt from recycled
// tables, and the vCPU's cached translations would be stale.
static int sync_cr3(void) {
  if (tls.active_arena) {
    pthread_mutex_lock(&g_lock);
    arena_sync_root(tls.active_arena);
    pthread_mutex_unlock(&g_lock);
  }
  unsigned gen = __atomic_load_n(&g_root_gen, __ATOMIC_ACQUIRE);
  uint64_t want_cr3 = (uint64_t)(uintptr_t)tls.active_pml4;
  if (want_cr3 != tls.loaded_cr3) {
    struct kvm_sregs s;
    ioctl(tls.fd, KVM_GET_SREGS, &s);
    s.cr3 = want_cr3;
    if (ioctl(tls.fd, KVM_SET_SREGS, &s) < 0) return -1;
    tls.loaded_cr3 = want_cr3;
  } else if (tls.root_gen != gen) {
    if (run_stub(gk_flush_stub, 0, 0, PORT_FLUSH, "flush") < 0) return -1;
  }
  tls.root_gen = gen;
  return 0;
}

// Ready this thread's vCPU for a guest invocation: its root, its PKRU on first
// entry, and the per-invocation fault-repeat tracking.
static int prepare_entry(void) {
  if (sync_cr3() < 0) return -1;
  uint32_t hp = host_pkru();
  host_pkru_allow_all();
  if (!tls.pkru_ready) {
    // The thread enters the guest with the PKRU the kernel gave it (the
    // process default, or its creator's), as its guest value.
    if (guest_pkru_update(0, hp, NULL) < 0) return -1;
    tls.pkru_ready = 1;
    if (g_dbg) fprintf(stderr, "[gk] vcpu %d: initial guest PKRU %#x\n", tls.id, hp);
  }
  tls.last_fault = 0;  // fault-repeat tracking is per guest invocation
  tls.fault_repeat = 0;
  return 0;
}

// Enter the guest at fn(arg) with a stack whose top (16-byte aligned) is
// `stack_top`, and run it to completion. `user` selects the privilege level:
//
//  ring 0 (user == 0): RIP is set straight to fn, which runs privileged, and
//    returns into gk_exit_tramp -> PORT_EXIT.
//  ring 3 (user == 1): RIP is set to gk_user_launch, which SYSRETs down to fn
//    at ring 3 (RCX = fn, R11 = flags). fn returns into gk_user_exit_tramp,
//    which leaves the guest through the sentinel syscall. Untrusted code runs
//    this way: it cannot execute privileged instructions, reload CR3 or reach
//    the supervisor/refused pages, so the arena walls hold against it.
//
// Either way the return value comes back through run_vcpu.
static long enter_guest(long (*fn)(void *), void *arg, uint64_t stack_top, int user) {
  uint64_t sp = stack_top - 8;
  *(uint64_t *)sp = user ? (uintptr_t)&gk_user_exit_tramp : (uintptr_t)&gk_exit_tramp;

  struct kvm_regs regs = {0};
  regs.rsp = sp;
  regs.rdi = (uintptr_t)arg;
  regs.rflags = 0x2;
  if (user) {
    regs.rip = (uintptr_t)&gk_user_launch;
    regs.rcx = (uintptr_t)fn;   // SYSRET target RIP
    regs.r11 = 0x2;             // SYSRET target RFLAGS
  } else {
    regs.rip = (uintptr_t)fn;
  }
  ioctl(tls.fd, KVM_SET_REGS, &regs);
  tls.user_mode = user;
  return run_vcpu();
}

long gk_run(long (*fn)(void *), void *arg) {
  if (vcpu_init(GK_VCPU_HOST | GK_VCPU_STACK) < 0) return -1;
  if (prepare_entry() < 0) return -1;
  return enter_guest(fn, arg, tls.stack_top, 0);
}

// Like gk_run, but fn runs at guest ring 3 (see enter_guest). Same private
// guest stack, same fault/return reporting.
long gk_run_user(long (*fn)(void *), void *arg) {
  if (vcpu_init(GK_VCPU_HOST | GK_VCPU_STACK) < 0) return -1;
  if (prepare_entry() < 0) return -1;
  return enter_guest(fn, arg, tls.stack_top, 1);
}

// ---- gk_run_here: the guest on the caller's stack ---------------------------
// The guest starts with rsp just below the point where gk_host_call_on_stack
// switched this thread away from its stack, so it grows down into the thread's
// stack exactly as a native call from gk_run_here would. Meanwhile the host
// side (run_vcpu, forward_syscall, demand_map) runs on the vCPU's side stack,
// so its frames can never land on top of the guest's. The two stacks are host
// memory either way: the guest demand-pages the thread stack as it descends,
// and demand_map grows the main thread's stack when the kernel would have.
//
// GK_HERE_SLACK separates the guest's first frame from the switch point. No
// host frame is ever built below caller_sp while the guest runs (a signal
// arriving then is handled on the side stack, where the thread is), so the gap
// only guards against a host red-zone use around the switch itself.
#define GK_HERE_SLACK 128
typedef struct { long (*fn)(void *); void *arg; int user; } gk_here_ctx;

static long run_here(void *ctx, unsigned long caller_sp) {
  gk_here_ctx *c = ctx;
  uint64_t top = (caller_sp - GK_HERE_SLACK) & ~0xfULL;
  return enter_guest(c->fn, c->arg, top, c->user);
}

static long run_here_common(long (*fn)(void *), void *arg, int user) {
  if (vcpu_init(GK_VCPU_HOST) < 0) return -1;
  if (!tls.side_stack) {
    void *ss = mmap(NULL, GK_SIDE_STACK, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (ss == MAP_FAILED) { g_err = "mmap side stack"; return -1; }
    tls.side_stack = ss;
    // The gk-loop side stack is host-only: refuse it so ring-3 code cannot
    // reach the host frames running underneath it.
    pthread_mutex_lock(&g_lock);
    prot_add((uintptr_t)ss, (uintptr_t)ss + GK_SIDE_STACK, GK_PROT_REFUSE);
    pthread_mutex_unlock(&g_lock);
  }
  if (prepare_entry() < 0) return -1;
  gk_here_ctx c = {fn, arg, user};  // lives above caller_sp, out of the guest's way
  return gk_host_call_on_stack((char *)tls.side_stack + GK_SIDE_STACK, run_here, &c);
}

long gk_run_here(long (*fn)(void *), void *arg) {
  return run_here_common(fn, arg, 0);
}

// Like gk_run_here, but fn runs at guest ring 3 (see enter_guest).
long gk_run_here_user(long (*fn)(void *), void *arg) {
  return run_here_common(fn, arg, 1);
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
          // A ring-3 guest leaves through the sentinel exit syscall (ring 3
          // cannot use PORT_EXIT's OUT): its result is in RDI.
          if (r.rax == GK_EXIT_SYSCALL) return (long)r.rdi;
          r.rax = (uint64_t)forward_syscall(&r);
          // A ring-3 guest's syscall trampoline ran in ring 0; return it to
          // ring 3 with SYSRET rather than the ring-0 jmp. forward_syscall left
          // RCX/R11 (the SYSCALL-saved return RIP and flags) intact and may
          // have already pointed RIP at a resume label; override it to the
          // ring-3 one. Ring-0 guests keep RIP at the OUT so KVM completes it.
          if (tls.user_mode) r.rip = (uintptr_t)&gk_user_syscall_resume;
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
          // A protection-key violation is the guest CPU enforcing the page's
          // key against this vCPU's PKRU: a genuine fault, unless the entry
          // this vCPU used carried the page's previous key (another vCPU
          // changed it without a shootdown). The fault invalidated that entry,
          // so one retry against the current PTE tells the two apart.
          struct kvm_regs pr;
          ioctl(tls.fd, KVM_GET_REGS, &pr);
          uint64_t pf_err = *(uint64_t *)(uintptr_t)pr.rsp;  // top of the #PF frame
          int pk = (pf_err & PF_ERR_PK) != 0;
          if (pk && repeat) {
            if (g_dbg)
              fprintf(stderr, "[gk] vcpu %d: protection-key violation at %#llx (err=%#llx)\n",
                      tls.id, (unsigned long long)cr2, (unsigned long long)pf_err);
            g_fault_addr = cr2;
            return GK_EFAULT;
          }
          GK_HANDLER_ENTER();
          int dm = demand_map((uintptr_t)cr2);
          GK_HANDLER_LEAVE();
          if (dm < 0) {
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
          // The retry re-walks the page tables: a page fault invalidates the
          // TLB entry it was taken through, so no explicit flush is needed.
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
// The reservation must own whole PML4 slots: an arena root's private entry for
// a slot hides everything else in that 512GiB from the arena, so no other
// mapping may share it. The kernel would place a free-address mmap next to the
// libraries and stacks at the top of the address space, in a slot the whole
// process lives in; instead the span is asked for at a slot-aligned address
// with MAP_FIXED_NOREPLACE, which succeeds only if the entire span is free, and
// the next slots are tried when it is not. The reservation is PROT_NONE and
// MAP_NORESERVE: it costs address space only, and its pages stay unmapped in
// every root until the owner commits them and the guest touches them.
//
// Slots are taken from the lowest run of nslots that no live arena owns, so a
// destroyed arena's slots serve later arenas: under isolate churn the slot
// index would otherwise run off the end of the user address space.
gk_arena *gk_arena_create(size_t size) {
  size = (size + 0xfff) & ~0xfffUL;
  if (size == 0) return NULL;
  int nslots = (int)((size + GK_SLOT_BYTES - 1) >> GK_SLOT_BITS);
  if (nslots > GK_MAX_ARENA_SLOTS) return NULL;
  size_t span = (size_t)nslots << GK_SLOT_BITS;
  gk_arena *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  pthread_mutex_lock(&g_lock);
  void *mem = MAP_FAILED;
  int idx;
  for (idx = GK_FIRST_CAGE_SLOT; idx + nslots <= GK_USER_SLOTS; idx++) {
    int free = 1;
    for (int i = 0; i < nslots; i++)
      if (g_slot_used[idx + i]) { free = 0; break; }
    if (!free) continue;
    uintptr_t va = (uintptr_t)idx << GK_SLOT_BITS;
    mem = mmap((void *)va, span, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem != MAP_FAILED && (uintptr_t)mem == va) break;
    if (mem != MAP_FAILED) { munmap(mem, span); mem = MAP_FAILED; }  // old kernel: hint only
  }
  if (mem == MAP_FAILED || g_arena_n >= GK_MAX_ARENAS) goto fail;
  a->base = (uintptr_t)mem;
  a->size = size;
  a->end = a->base + span;
  a->slot0 = idx;
  a->nslots = nslots;
  a->pml4 = alloc_table();
  if (!a->pml4) goto fail;
  // One private PDPT per slot, allocated now so the entry never changes; the
  // PD and PT pages beneath it are allocated as pages demand-fault in.
  for (int i = 0; i < nslots; i++) {
    uint64_t *pdpt = alloc_table();
    if (!pdpt) goto fail;
    a->slot_entry[i] = ((uint64_t)(uintptr_t)pdpt) | PTE_P | PTE_W | PTE_U;
  }
  // The span was free host address space, so any base-root PTE in it (from
  // memory that once lived there) is stale; drop them so nothing maps into
  // the reservation from outside the arena.
  unmap_range_root(g_pml4, a->base, a->end);
  arena_sync_root(a);  // share the base root's other entries, own these slots
  for (int i = 0; i < nslots; i++) g_slot_used[idx + i] = 1;
  g_arenas[g_arena_n++] = a;
  pthread_mutex_unlock(&g_lock);
  return a;
fail:
  for (int i = 0; i < GK_MAX_ARENA_SLOTS; i++)
    if (a->slot_entry[i]) free_table((uint64_t *)(uintptr_t)(a->slot_entry[i] & ~0xfffULL));
  if (a->pml4) free_table(a->pml4);
  pthread_mutex_unlock(&g_lock);
  if (mem != MAP_FAILED) munmap(mem, span);
  free(a);
  return NULL;
}

void *gk_arena_base(const gk_arena *a) { return a ? (void *)a->base : NULL; }
size_t gk_arena_size(const gk_arena *a) { return a ? a->size : 0; }

gk_arena *gk_arena_enter(gk_arena *a) {
  gk_arena *prev = tls.active_arena;
  if (prev == a) return prev;
  if (a) __atomic_fetch_add(&a->active_threads, 1, __ATOMIC_SEQ_CST);
  tls.active_arena = a;
  tls.active_pml4 = a ? a->pml4 : g_pml4;
  if (prev) __atomic_fetch_sub(&prev->active_threads, 1, __ATOMIC_SEQ_CST);
  return prev;
}

// Teardown reconciles gk and KVM with the reservation going away, in this
// order, under g_lock:
//  1. The arena leaves the registry, so a fault in its span is no longer an
//     arena fault, and its span loses its protection keys.
//  2. Every root stops mapping the span: the arena's private subtrees are
//     freed whole (no other root reaches them), and any stale base-root PTEs
//     in the span are cleared (other arena roots share those tables).
//  3. Every memslot in the span is deleted. Nothing maps into the span at
//     this point, so no vCPU can reach a memslot as it goes.
//  4. Its slots are released for the next arena.
// Only then, outside the lock, is the host reservation unmapped, so KVM never
// holds a memslot over host memory that is gone while a guest PTE could still
// lead there. The page-table pages return to the allocator and the slots and
// memslot ids are reused, so arena churn is bounded in every resource.
//
// A thread that still has the arena active (a caller bug: the arena's memory
// is being unmapped under it) keeps a root that must stay intact and private,
// so in that case the tables, slots and struct are leaked instead of recycled;
// the memslots and reservation still go.
void gk_arena_destroy(gk_arena *a) {
  if (!a) return;
  if (tls.active_arena == a) gk_arena_enter(NULL);
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < g_arena_n; i++)
    if (g_arenas[i] == a) { g_arenas[i] = g_arenas[--g_arena_n]; break; }
  if (pkey_set_range(a->base, a->end, 0) < 0 && g_dbg)
    fprintf(stderr, "[gk] pkey table full; destroyed arena keeps stale keys\n");
  int in_use = __atomic_load_n(&a->active_threads, __ATOMIC_SEQ_CST);
  if (in_use > 0) {
    fprintf(stderr, "[gk] gk_arena_destroy: arena [%#lx,%#lx) is still active on %d thread(s); "
            "its page tables and slots are leaked\n", (unsigned long)a->base,
            (unsigned long)a->end, in_use);
  } else {
    for (int i = 0; i < a->nslots; i++) {
      free_subtree((uint64_t *)(uintptr_t)(a->slot_entry[i] & ~0xfffULL), 3);
      a->slot_entry[i] = 0;
      a->pml4[a->slot0 + i] = 0;
    }
    free_table(a->pml4);
    __atomic_fetch_add(&g_root_gen, 1, __ATOMIC_RELEASE);
  }
  unmap_range_root(g_pml4, a->base, a->end);
  int removed = region_remove_range(a->base, a->end);
  if (in_use == 0)
    for (int i = 0; i < a->nslots; i++) g_slot_used[a->slot0 + i] = 0;
  pthread_mutex_unlock(&g_lock);
  if (g_dbg)
    fprintf(stderr, "[gk] destroyed arena [%#lx,%#lx): %d memslots deleted, %ld tables in use\n",
            (unsigned long)a->base, (unsigned long)a->end, removed, g_pt_used);
  munmap((void *)a->base, a->end - a->base);
  if (in_use == 0) free(a);
}

void gk_get_stats(gk_stats *s) {
  pthread_mutex_lock(&g_lock);
  s->memslots = g_region_live;
  s->memslot_ids = g_next_slot;
  s->arenas = g_arena_n;
  s->pt_pages_used = g_pt_used;
  s->pt_pages_free = g_pt_free_n;
  s->pt_pages_total = (long)((g_pt_next - (uint8_t *)g_pt_base) >> 12);
  pthread_mutex_unlock(&g_lock);
}
