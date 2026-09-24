// gk implementation. See gk.h.
#define _GNU_SOURCE
#include "gk.h"
#include <dlfcn.h>
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
#define PTE_G (1UL << 8)         // global: the translation survives a CR3 load (see the global-pages section)
#define PTE_WALL (1UL << 9)      // CPU-ignored software bit: W withheld by the host-frame wall
#define PTE_NX (1UL << 63)
#define PTE_PKEY_SHIFT 59        // bits 62:59 hold the page's protection key
#define PTE_PKEY_MASK (0xfUL << PTE_PKEY_SHIFT)
#define GK_MAP_WALLED 8          // map4k_root flag: host-writable page, W withheld (PTE_WALL)
#define GK_MAP_GLOBAL 16         // map4k_root flag: the page may be global (see the global-pages section)
#define PF_ERR_WR (1UL << 1)     // #PF error code: the access was a write
#define PF_ERR_PK (1UL << 5)     // #PF error code: protection-key violation
#define CR0_PE (1UL << 0)
#define CR0_WP (1UL << 16)
#define CR0_PG (1UL << 31)
#define CR4_PAE (1UL << 5)
#define CR4_PGE (1UL << 7)
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

// ---- gk control data ---------------------------------------------------------
// Everything gk trusts to enforce isolation -- the page-table allocator's
// metadata, the arena registry and the arena structs, the memslot tree and its
// node pool, the supervisor/refuse registry, the vCPU pool, the per-thread
// records and the rest of the platform state -- lives in one control block,
// g_ctl. The block is page-aligned and a whole number of pages, so it shares
// no page with anything else in .bss, and it is registered SUPERVISOR before
// anything is mapped: every guest root maps its pages with U cleared, so ring-0
// code (gk's handlers, trusted runtime code run through gk_run/gk_run_here) can
// read and write it while ring-3 code faults on any access. As ordinary .bss or
// heap it would demand-page in as user memory, and a ring-3 escape with an
// arbitrary write could then rewrite the structures that decide which root a
// thread runs under. The host side accesses the block outside KVM_RUN through
// the host's own page tables, which the registry does not affect.
//
// G names the block. It is the address of a static object, resolved at link
// (or load) time and embedded in the code, so there is no pointer in writable
// memory for a guest to redirect.
//
// Per-thread state is in the block too, one record per thread (gk_thread). A
// thread finds its record through tls_rec, a thread-local index; thread-local
// storage is user memory, so the index is never trusted on its own: the record
// it names must carry the calling thread's kernel tid (thread_cur), which no
// guest can forge. Between KVM exits the host side passes the validated record
// along rather than re-reading the index.

// vCPU pool. KVM never destroys a vCPU before the VM (closing its fd only
// drops a reference) and rejects a vcpu id that was already created, so a vCPU
// per thread ever created would exhaust KVM_CAP_MAX_VCPUS under thread churn.
// Instead a vCPU whose thread ends is parked here and handed to the next thread
// that needs one; a vCPU may be driven by a different host thread on each
// KVM_RUN. Live vCPUs are therefore bounded by peak concurrent guest threads.
// The pool holds the eager-mapped guest stack and the gk_run_here side stack
// too, when the vCPU had them, so stacks are recycled rather than leaked.
// Guarded by G->lock.
#define GK_MAX_VCPUS 4096
// The kvm_run register sets gk exchanges with KVM_CAP_SYNC_REGS (see regs_get).
// The constants are KVM ABI; spelled out for headers that predate them.
#ifndef KVM_CAP_SYNC_REGS
#define KVM_CAP_SYNC_REGS 74
#endif
#ifndef KVM_SYNC_X86_REGS
#define KVM_SYNC_X86_REGS (1UL << 0)
#define KVM_SYNC_X86_SREGS (1UL << 1)
#endif
#define GK_SYNC_REGS (KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS)
#define GK_IST_BYTES (64UL << 10)   // per-vCPU exception stack, TSS in its last page
#define GK_SIDE_STACK (512UL << 10) // host-side stack for gk_run_here's gk loop
typedef struct {
  int fd, id;
  struct kvm_run *run;
  uint64_t stack_top;   // 0 if the vCPU has no private guest stack
  uint64_t ist;         // its exception stack + TSS region (see vcpu_init)
  void *side_stack;     // NULL if the vCPU has no gk_run_here side stack
} gk_parked_vcpu;

// Per-thread guest state: each host thread that enters the guest is a vCPU. A
// thread that only selects an arena (gk_arena_enter) has a record too, without
// a vCPU until its first run. Records are allocated from G->threads and
// recycled when the thread ends (see thread_get, thread_release).
typedef struct gk_thread {
  long tid;                // kernel tid of the owning thread (0: free record)
  int inited;              // the thread has a vCPU
  int id;
  int fd;
  struct kvm_run *run;
  uint64_t stack_top;
  uint64_t ist;            // exception stack + TSS region of this vCPU
  void *side_stack;        // host stack for gk_run_here's gk loop (see there)
  unsigned root_gen;       // G->root_gen as of this vCPU's last TLB flush (0: never)
  uint64_t last_fault;
  int fault_repeat;
  int backing_retries;     // host-backing faults recovered this invocation (see run_vcpu)
  int pkru_ready;          // guest PKRU has been given its initial value
  int user_mode;           // the current invocation runs fn at ring 3 (gk_run_user)
  // The host-frame wall of a ring-3 gk_run_here_user turn (see run_here):
  // [ro_lo, ro_hi), page-aligned, is the caller's stack above the guest's
  // entry, which this vCPU maps read-only and whose write faults are genuine.
  // Empty (0, 0) outside such a turn. Written only by the owning thread.
  uintptr_t ro_lo, ro_hi;
  // wall_raise's flush bookkeeping (see there). tlb_touched: this vCPU's TLB
  // may have changed since wall_lower last ran. Set before every KVM_RUN (the
  // guest loads translations as it runs) and by a host-side CR3 write, cleared
  // only by wall_lower; while clear, the TLB holds exactly what it held when
  // the previous wall came down. prev_ro_lo/prev_ro_hi: the window of the
  // previous raised wall, (0, 0) when there is none to reason from (a fresh or
  // reused vCPU, a turn with no wall, a failed flush). Owning thread only.
  int tlb_touched;
  uintptr_t prev_ro_lo, prev_ro_hi;
  // stack_wall_bounds' cache: the host mapping [wall_map_lo, wall_map_hi)
  // that held the previous turn's switch point, and the wall's end within it.
  uintptr_t wall_map_lo, wall_map_hi, wall_end;
  uint64_t *active_pml4;   // root this thread runs under
  gk_arena *active_arena;
  // Set for a thread the guest created via clone (see clone_thread): it runs
  // its gk loop on this private host stack and leaves only through the guest's
  // exit syscall.
  int guest_thread;
  void *host_stack;
  size_t host_stack_size;
  // A guest-created thread's initial state, filled by its parent (clone_thread)
  // and consumed by the child (child_entry) before it enters the guest.
  struct {
    struct kvm_regs regs;
    uint32_t pkru;         // the parent's guest PKRU, inherited like a real clone
    int user;              // the parent ran at ring 3, so the child starts there too
    int parent_id;
  } start;
  struct gk_thread *next_free;
} gk_thread;
// 1 + the index of this thread's record in G->threads; 0 while it has none.
// User memory: validated by thread_cur before every use.
static __thread int tls_rec;

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
#define GK_MAX_ARENAS 4096
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
  gk_arena *next_free;        // free-list link while the struct is unused
};

// The supervisor/refuse registry (see prot_class below). An unregistered page
// is user-accessible, so the registry must never be too small to hold every
// range gk wants to protect: prot_add is fatal when it is full. The capacity
// covers the worst case, which is 4 ranges per vCPU (kvm_run and the IST/TSS
// region, which stay registered while the vCPU is pooled, plus a gk_run_here
// side stack and a guest thread's host stack, which are different classes and
// so cannot be coalesced), the fixed init ranges (control block, page-table
// area, GDT/IDT, handler text), and headroom.
#define GK_PROT_KEEP 0
#define GK_PROT_SUPER 1
#define GK_PROT_REFUSE 2
#define GK_MAX_PROT_RANGES (4 * GK_MAX_VCPUS + 1024)
typedef struct { uintptr_t start, end; int kind; } gk_prot_range;
_Static_assert(GK_MAX_PROT_RANGES >= 2 * GK_MAX_VCPUS + 512,
               "registry must hold every vCPU's permanent ranges with headroom");

// A memslot-backed window (see the MMU layer below).
#define GK_BACK_WIN (2UL << 20)   // memslot backing granularity (2MB, aligned)
// The neighborhood a demand fault maps around its page (see demand_map_neighbors).
#define GK_NEIGHBOR_WIN (64UL << 10)  // 16 pages, aligned; within one GK_BACK_WIN
#define GK_MAX_REGIONS 65536
typedef struct gk_region {
  uintptr_t start, end;   // [start, end), GK_BACK_WIN-aligned
  int slot;               // KVM memslot id backing this region
  unsigned prio;          // treap heap priority
  struct gk_region *l, *r;
} gk_region;

// A range holding a nonzero protection key (see the protection-keys section).
#define GK_MAX_PKEY_RANGES 4096
typedef struct { uintptr_t start, end; int pkey; } gk_pkey_range;

#define GK_CPUID_ENTRIES 128

struct gk_ctl {
  // Platform state, set once by gk_init.
  int kvm, vmfd;
  uint64_t *pml4;                    // base page-table root
  uint8_t *pt_next, *pt_end;         // bump allocator over the page-table area
  uintptr_t pt_base;
  size_t pt_bytes;
  uint64_t *pt_free;                 // freed page-table pages, linked through their first word
  long pt_used, pt_free_n;           // pages handed out and not returned; pages on the free list
  uint64_t gdt_va, idt_va;
  int next_slot;                     // KVM memslot ids handed out, ever (see the region pool)
  int run_size;
  int sync_regs;                     // registers go through kvm_run, not ioctls (see regs_get)
  // Host CPUID, applied to every vCPU: a struct kvm_cpuid2 (a flexible-array
  // struct, hence the raw buffer) with room for GK_CPUID_ENTRIES entries.
  _Alignas(struct kvm_cpuid2) unsigned char cpuid_buf[sizeof(struct kvm_cpuid2) +
                                                      GK_CPUID_ENTRIES *
                                                          sizeof(struct kvm_cpuid_entry2)];
  const char *err;
  unsigned long fault_addr;
  int dbg;
  gk_syscall_filter filter;
  // glibc's _dl_get_tls_static_info (GLIBC_PRIVATE, resolved at gk_init; NULL
  // if absent): the size of a thread's static TLS block, which on a pthread
  // sits at the top of the stack mapping and must stay outside the host-frame
  // wall (see stack_wall_bounds).
  void (*tls_static_info)(size_t *, size_t *);
  pthread_mutex_t lock;
  long demand_ok;
  long wall_flushed, wall_skipped;  // wall_raise: TLB flushes done / provably unneeded (see there)
  long global_pages;                // PTEs installed with PTE_G (see the global-pages section)
  long neighbor_pages;              // PTEs installed ahead of a fault (see demand_map_neighbors)
  atomic_long root_switches;        // guest entries that loaded another root (enter_guest)
  atomic_long tlb_flushes;          // full flushes run on a vCPU, on any path (see flush_run)
  void (*flush_stub)(void);         // the flush stub for this CPU (see flush_stub_select)
  // Bumped whenever page-table pages are returned to the allocator (an arena
  // was destroyed). A vCPU whose root is unchanged since it last entered the
  // guest may still hold TLB and paging-structure-cache entries derived from
  // tables that have since been freed and reused, so it flushes at its next
  // entry when it sees a new generation (see sync_cr3).
  unsigned root_gen;

  // The vCPU pool. Guarded by G->lock.
  gk_parked_vcpu parked[GK_MAX_VCPUS];
  int parked_n;
  atomic_int vcpus_created;          // vcpu ids handed out, ever
  pthread_key_t thread_key;          // releases a host thread's record at exit

  // Thread records. Guarded by G->lock.
  gk_thread threads[GK_MAX_VCPUS];
  int thread_n;                      // records ever taken
  gk_thread *thread_free;            // records of ended threads

  // Registry of live arenas, so demand paging can tell a fault inside the
  // active arena (backed, into that arena's root) from one inside any other
  // arena (a cross-arena access, refused), and so protection changes can be
  // reflected into every arena root they touch. The structs come from
  // arena_pool. Guarded by G->lock.
  gk_arena *arenas[GK_MAX_ARENAS];
  int arena_n;
  gk_arena arena_pool[GK_MAX_ARENAS];
  int arena_pool_n;                  // pool structs ever taken
  gk_arena *arena_free;              // structs of destroyed arenas
  // Which PML4 slots live arenas own. A destroyed arena's slots are reused by
  // later arenas, so slot churn does not run through the user address space.
  unsigned char slot_used[GK_USER_SLOTS];
  // Which PML4 slots have ever held a global PTE. No arena is ever placed over
  // one (see the global-pages section).
  unsigned char slot_pinned[GK_USER_SLOTS];

  // The supervisor/refuse registry: disjoint ranges sorted by start. Guarded
  // by G->lock.
  gk_prot_range prot[GK_MAX_PROT_RANGES];
  int prot_n;

  // The memslot interval tree and its node pool (see the MMU layer). Guarded
  // by G->lock.
  gk_region *regions;                // treap root
  gk_region region_pool[GK_MAX_REGIONS];
  int region_n;                      // pool nodes ever taken
  gk_region *region_free;            // nodes of deleted regions, linked through `l`
  int region_live;                   // regions currently in the treap (= live memslots)
  unsigned rand_state;               // xorshift32 state for treap priorities

  // Protection-key ranges (see the protection-keys section). Guarded by G->lock.
  gk_pkey_range pkeys[GK_MAX_PKEY_RANGES];
  int pkey_n;

  // A word for tests to read and write through the guest (gk_debug_ctl_addr).
  unsigned long debug_scratch;
};
// The union pads the block to whole pages; with the alignment, no other object
// can share a page with it.
static union {
  struct gk_ctl c;
  char pad[(sizeof(struct gk_ctl) + 0xfff) & ~(size_t)0xfff];
} g_ctl __attribute__((aligned(4096)));
#define G (&g_ctl.c)
#define GK_CTL_START ((uintptr_t)&g_ctl)
#define GK_CTL_END (GK_CTL_START + sizeof g_ctl)
#define GK_CPUID ((struct kvm_cpuid2 *)G->cpuid_buf)

static gk_arena *arena_containing(uintptr_t a) {
  for (int i = 0; i < G->arena_n; i++)
    if (a >= G->arenas[i]->base && a < G->arenas[i]->end) return G->arenas[i];
  return NULL;
}

// Trampolines and exception handlers (gk_asm.S), in this binary's mapped text.
extern void gk_syscall_tramp(void);
extern void gk_syscall_tramp_resume(void);
extern void gk_user_syscall_resume(void);
extern void gk_user_launch(void);
extern void gk_launch_cr3(void);
extern void gk_user_launch_cr3(void);
extern void gk_user_child_launch(void);
extern void gk_exit_tramp(void);
extern void gk_user_exit_tramp(void);
extern void gk_exc_de(void), gk_exc_ud(void), gk_exc_df(void), gk_exc_gp(void),
    gk_exc_pf(void);
extern void gk_pkru_stub(void);
extern void gk_flush_stub(void);
extern void gk_flush_stub_invpcid(void);
extern const unsigned char gk_invpcid_desc[16];
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

// Raw stderr output for the fault path and for fatal errors: a plain write(2),
// no stdio, no allocation, no lock the guest thread could hold (see the
// signal-safety note above region_add). Debug lines are composed in a
// stack buffer (gk_dbuf) and written in one call.
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

typedef struct { char b[512]; size_t n; } gk_dbuf;
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

// Page-table page allocator over a fixed arena: a bump allocator with a free
// list in front of it, so the tables of a destroyed arena serve the next one.
// Freed pages are linked through their first word; the page is zeroed again
// when handed out. Caller holds G->lock (except in single-threaded gk_init).
static uint64_t *alloc_table(void) {
  uint64_t *p;
  if (G->pt_free) {
    p = G->pt_free;
    G->pt_free = *(uint64_t **)p;
    G->pt_free_n--;
  } else {
    if (G->pt_next + 0x1000 > G->pt_end) return NULL;
    p = (uint64_t *)G->pt_next;
    G->pt_next += 0x1000;
  }
  memset(p, 0, 0x1000);
  G->pt_used++;
  return p;
}

// Return a page-table page. The caller guarantees no root reaches it any more.
static void free_table(uint64_t *p) {
  *(uint64_t **)p = G->pt_free;
  G->pt_free = p;
  G->pt_free_n++;
  G->pt_used--;
}

// Free a table and every table beneath it. `level` is the table's paging
// level: 3 for a PDPT, 2 for a PD, 1 for a PT (whose entries are pages, not
// tables). gk maps only 4KiB pages, so every present entry above level 1
// points at a table. Caller holds G->lock.
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
// a binary-search lookup and no allocation. Guarded by G->lock.

// The class of the page holding `addr` (GK_PROT_KEEP if unregistered). Caller
// holds G->lock.
static int prot_class(uintptr_t addr) {
  int lo = 0, hi = G->prot_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (addr < G->prot[mid].start) hi = mid;
    else if (addr >= G->prot[mid].end) lo = mid + 1;
    else return G->prot[mid].kind;
  }
  return GK_PROT_KEEP;
}

static void unmap_range_all(uintptr_t s, uintptr_t e);

// Register [s, e) (page-aligned) as SUPER or REFUSE, keeping the array sorted by
// start. Ranges never overlap: they are gk's own allocations. Any PTE a root
// already holds for the range is dropped, so a page that some vCPU touched
// between its allocation and this call is re-faulted under its new class
// rather than staying a user page. Caller holds G->lock (or is single-threaded
// gk_init).
//
// A full registry is fatal, never a silent no-op: an unregistered range is
// KEEP, i.e. user-accessible, so leaving a supervisor or refused range out
// would hand ring 3 the very memory (a TSS/IST, kvm_run, a host stack) the
// registry exists to keep from it. GK_MAX_PROT_RANGES is sized so that this
// cannot happen below the vCPU limit.
static void prot_add(uintptr_t s, uintptr_t e, int kind) {
  if (G->prot_n >= GK_MAX_PROT_RANGES)
    gk_fatal("supervisor/refuse registry full; cannot leave a protected range user-mapped");
  int i = 0;
  while (i < G->prot_n && G->prot[i].start < s) i++;
  memmove(&G->prot[i + 1], &G->prot[i], (size_t)(G->prot_n - i) * sizeof G->prot[0]);
  G->prot[i] = (gk_prot_range){s, e, kind};
  G->prot_n++;
  if (G->pml4) unmap_range_all(s, e);
}

// Unregister the range [s, e) registered by prot_add, when the memory behind
// it is being given back (a guest thread's host stack at its exit), so that
// whatever the address is later reused for is classified afresh. Caller holds
// G->lock.
static void prot_remove(uintptr_t s, uintptr_t e) {
  for (int i = 0; i < G->prot_n; i++) {
    if (G->prot[i].start != s || G->prot[i].end != e) continue;
    memmove(&G->prot[i], &G->prot[i + 1], (size_t)(G->prot_n - i - 1) * sizeof G->prot[0]);
    G->prot_n--;
    return;
  }
}

// ---- global pages ----------------------------------------------------------
// The guest runs with CR4.PGE, so a PTE carrying the G bit yields a TLB entry
// that a CR3 load keeps. A turn on a thread whose previous turn ran another
// isolate loads that isolate's root at entry (see enter_guest); without global
// pages that load drops every translation the vCPU had, and the turn starts
// cold on the shared runtime's text, data and heap. With them, only the
// previous arena's translations go.
//
// A global entry is correct only if the translation it caches is the same
// under every root the vCPU can ever load, and stays so until a flush that
// includes global entries. Both hold, by construction, for the pages that get
// PTE_G, and map4k_root sets it for no others:
//
//  * The PTE lives in a table every root shares. PTE_G goes only into the base
//    root, for a page outside every arena's reservation. Such a PTE sits under
//    a base-root PML4 entry, which every arena root copies (arena_sync_root)
//    and which is never replaced once set (the shared subtree is never freed),
//    so every root reaches the identical PTE or, until its next sync, none. An
//    arena's private subtree is never global: it differs per root by design,
//    and a surviving entry would let one isolate's turn reach another's memory
//    after the switch.
//  * The page's address can never later become arena memory. A slot that has
//    ever held a global PTE is pinned (slot_pinned) and gk_arena_create never
//    places an arena over it; otherwise a stale global translation of a runtime
//    mapping that was later unmapped could outlive the slot's reuse as an
//    arena, and a turn for another arena could read that arena's memory through
//    it. Pinning changes nothing in practice: an arena needs its whole 512GiB
//    span free, and the slots holding the runtime's mappings never are.
//  * The page is an ordinary user page (KEEP) -- supervisor pages stay
//    non-global as a matter of caution, not need -- and not part of a thread
//    stack: not in the calling vCPU's host-frame wall window, not in the stack
//    mapping its ring-3 turns run on, and not in the mapping holding the
//    interrupted code's stack pointer (see demand_map; the eager-mapped guest
//    and child stacks never get the flag either). The wall withholds W on a
//    window page per turn; with the stack non-global, a root switch between
//    turns drops the vCPU's stack translations regardless, so wall_raise's
//    flush-skip argument only ever has to carry turns with no switch between.
//
// Every TLB flush gk does itself includes global entries: the flush stub
// (see flush_run) runs INVPCID type 2 or toggles CR4.PGE, either of which the
// CPU defines to drop every entry, global or not.
// So a reflected mprotect/munmap, a wall raise and the root_gen flush all drop
// them, and a changed shared PTE never leaves a stale global entry on the vCPU
// that changed it; other vCPUs are not shot down, with or without global pages
// (see the README's limitations). The per-turn root load is the guest's own
// MOV CR3 (gk_launch_cr3, gk_user_launch_cr3), which keeps global entries; a
// root load through KVM (cr3_load_host, host_backing_retry) makes KVM flush the
// whole guest TLB, global entries included, which is harmless.

// Four-level map of one page; guest-virtual == guest-physical == host-virtual.
// `pkey` (0..15) is the page's protection key, placed in PTE bits 62:59; the
// guest CPU checks it against the guest PKRU on every data access (with
// CR4.PKE and CR0.WP set, see vcpu_init), which is what makes the guest's own
// protection keys real. Key 0 is the default, unrestricted key.
//
// The leaf's U bit follows the supervisor/refuse registry: a SUPER page is
// mapped with U cleared (ring 3 cannot reach it), a REFUSE page is never mapped
// at all, and everything else is user-accessible. GK_MAP_GLOBAL asks for the G
// bit; it is granted only under the conditions of the global-pages section,
// which are checked here again rather than trusted from the caller. Caller
// holds G->lock.
static int map4k_root(uint64_t *root, uint64_t va, uint64_t flags, int pkey) {
  int cls = prot_class(va & ~0xfffULL);
  if (cls == GK_PROT_REFUSE) return -1;  // a control page: never reachable
  int slot = (int)((va >> 39) & 0x1ff);
  int global = (flags & GK_MAP_GLOBAL) && root == G->pml4 && cls == GK_PROT_KEEP &&
               !(flags & GK_MAP_WALLED) && slot < GK_USER_SLOTS && !arena_containing(va);
  uint64_t *pdpt = next_table(root, (va >> 39) & 0x1ff);
  if (!pdpt) return -1;
  uint64_t *pd = next_table(pdpt, (va >> 30) & 0x1ff);
  if (!pd) return -1;
  uint64_t *pt = next_table(pd, (va >> 21) & 0x1ff);
  if (!pt) return -1;
  // `flags` is a protection bitmask: 1=read, 2=write, 4=execute. Honor it so
  // W^X holds: code is mapped executable but not writable, data writable but
  // not executable. KVM also enforces the host VMA's real protection.
  // GK_MAP_WALLED marks a page the host allows writes to whose W this vCPU's
  // host-frame wall withholds: it is mapped without W and tagged PTE_WALL so
  // wall_lower can give W back in place.
  uint64_t pte = (va & ~0xfffULL) | PTE_P;
  if (cls != GK_PROT_SUPER) pte |= PTE_U;  // user pages only; supervisor clears U
  if (flags & 2) pte |= PTE_W;
  if (flags & GK_MAP_WALLED) pte |= PTE_WALL;
  if (!(flags & 4)) pte |= PTE_NX;
  pte |= ((uint64_t)pkey << PTE_PKEY_SHIFT) & PTE_PKEY_MASK;
  if (global) {
    pte |= PTE_G;
    G->slot_pinned[slot] = 1;  // no arena may ever take this slot (see the global-pages section)
    G->global_pages++;
  }
  pt[(va >> 12) & 0x1ff] = pte;
  return 0;
}

// What a page-table walk over a range does to each leaf PTE it reaches.
enum {
  PTE_OP_UNMAP,       // clear the entry: reflect a munmap, fixed mmap or PROT_NONE mprotect
  PTE_OP_WALL_RAISE,  // present and writable: withhold W, tag PTE_WALL (see wall_raise)
  PTE_OP_WALL_LOWER,  // present and PTE_WALL: give W back, untag; counts the present and
                      // writable entries that carry no tag (see wall_lower)
  PTE_OP_REPROTECT,   // present: rewrite W and NX from a new host protection (see reprotect_range_all)
};

// The arguments of PTE_OP_REPROTECT: the protection a successful mprotect
// just established over the range (bit0=r, bit1=w, bit2=x, the mprotect's own
// prot argument) and the calling vCPU's host-frame wall window, inside which
// W is withheld and tagged PTE_WALL exactly as demand_map would map the page.
typedef struct { int prot; uintptr_t wall_lo, wall_hi; } gk_reprot;

// Apply `op` to the leaf PTEs of [s, e) (page-aligned) in a root. Walks the
// hierarchy and skips a whole 512GiB, 1GiB or 2MiB range at once where no
// table exists beneath it, so a walk over a large, sparsely committed
// reservation costs in proportion to what is mapped, not to the range. Leaves
// intermediate tables in place and never allocates. `rp` is PTE_OP_REPROTECT's
// argument and NULL for the other ops. Returns PTE_OP_WALL_LOWER's count of
// untagged writable entries, 0 for the other ops. Caller holds G->lock.
#define GK_NEXT_BOUNDARY(v, bits) ((((v) >> (bits)) + 1) << (bits))
static long pte_range_root(uint64_t *root, uintptr_t s, uintptr_t e, int op,
                           const gk_reprot *rp) {
  long n = 0;
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
    uint64_t *pte = &pt[(v >> 12) & 0x1ff];
    switch (op) {
      case PTE_OP_UNMAP:
        *pte = 0;
        break;
      case PTE_OP_WALL_RAISE:
        if ((*pte & (PTE_P | PTE_W)) == (PTE_P | PTE_W)) *pte = (*pte & ~PTE_W) | PTE_WALL;
        break;
      case PTE_OP_WALL_LOWER:
        if ((*pte & (PTE_P | PTE_WALL)) == (PTE_P | PTE_WALL)) *pte = (*pte & ~PTE_WALL) | PTE_W;
        else if ((*pte & (PTE_P | PTE_W)) == (PTE_P | PTE_W)) n++;
        break;
      case PTE_OP_REPROTECT:
        // Only W, NX and the wall tag change; the frame, P, U (the
        // supervisor/refuse class), the protection key and the accessed/dirty
        // bits are the page's own and stay.
        if (*pte & PTE_P) {
          uint64_t n = *pte & ~(PTE_W | PTE_NX | PTE_WALL);
          if (!(rp->prot & 4)) n |= PTE_NX;
          if (rp->prot & 2) n |= (v >= rp->wall_lo && v < rp->wall_hi) ? PTE_WALL : PTE_W;
          *pte = n;
        }
        break;
    }
    v += 0x1000;
  }
  return n;
}

// The leaf PTE a root holds for the page of `va`, or 0 if no table on the way
// down exists. Never allocates. Caller holds G->lock.
static uint64_t pte_lookup(const uint64_t *root, uintptr_t va) {
  uint64_t pml4e = root[(va >> 39) & 0x1ff];
  if (!(pml4e & PTE_P)) return 0;
  const uint64_t *pdpt = (const uint64_t *)(uintptr_t)(pml4e & ~0xfffULL);
  uint64_t pdpte = pdpt[(va >> 30) & 0x1ff];
  if (!(pdpte & PTE_P)) return 0;
  const uint64_t *pd = (const uint64_t *)(uintptr_t)(pdpte & ~0xfffULL);
  uint64_t pde = pd[(va >> 21) & 0x1ff];
  if (!(pde & PTE_P)) return 0;
  const uint64_t *pt = (const uint64_t *)(uintptr_t)(pde & ~0xfffULL);
  return pt[(va >> 12) & 0x1ff];
}

// Apply `op` over [s, e) in every root that may hold PTEs for it: the base
// root (whose subtrees every arena root shares for addresses outside arenas)
// and each arena whose reservation the range overlaps (their private subtrees
// are reachable from no other root). Returns the sum of the walks' counts
// (see pte_range_root). Caller holds G->lock.
static long pte_range_all(uintptr_t s, uintptr_t e, int op, const gk_reprot *rp) {
  long n = pte_range_root(G->pml4, s, e, op, rp);
  for (int i = 0; i < G->arena_n; i++) {
    gk_arena *a = G->arenas[i];
    uintptr_t lo = s > a->base ? s : a->base, hi = e < a->end ? e : a->end;
    if (lo < hi) n += pte_range_root(a->pml4, lo, hi, op, rp);
  }
  return n;
}

// Reflect a host protection or mapping change over [s, e) by dropping every
// PTE any root holds for it (see pte_range_all), so the guest's next access
// demand-faults and re-derives the mapping from the host. A stale PTE over a
// page the host has decommitted would otherwise make that access an
// unrecoverable KVM_RUN EFAULT rather than a demand fault. Caller holds
// G->lock.
static void unmap_range_all(uintptr_t s, uintptr_t e) {
  pte_range_all(s, e, PTE_OP_UNMAP, NULL);
}

// Reflect a successful plain mprotect over [s, e) that leaves the range
// readable by rewriting, in place, every PTE any root holds for it (see
// pte_range_all) to `prot`, the syscall's own argument: with the host call
// just made under G->lock, that is exactly the host protection of every page
// in the range, so nothing is cached and nothing needs re-deriving. Pages the
// guest has no PTE for yet are untouched and fault in later against the host
// as always. Dropping the PTEs instead (unmap_range_all) would have each one
// re-fault, and every demand fault reads /proc/self/maps, which is long in
// this process. `t` is the calling vCPU, whose host-frame wall the rewrite
// honors (see gk_reprot). Caller holds G->lock.
static void reprotect_range_all(gk_thread *t, uintptr_t s, uintptr_t e, int prot) {
  gk_reprot rp = {prot, t->ro_lo, t->ro_hi};
  pte_range_all(s, e, PTE_OP_REPROTECT, &rp);
}

// The same for one root only: an arena root being built or torn down, whose
// private subtree no other root reaches. Caller holds G->lock.
static void unmap_range_root(uint64_t *root, uintptr_t s, uintptr_t e) {
  pte_range_root(root, s, e, PTE_OP_UNMAP, NULL);
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

// xorshift32 PRNG for treap priorities. Caller holds G->lock.
static unsigned gk_rand(void) {
  unsigned s = G->rand_state;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  G->rand_state = s;
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

// The region containing `addr`, or NULL. Caller holds G->lock.
static gk_region *region_find(uintptr_t addr) {
  gk_region *n = G->regions;
  while (n) {
    if (addr < n->start) n = n->l;
    else if (addr >= n->end) n = n->r;
    else return n;
  }
  return NULL;
}

// The region with the smallest start strictly greater than `s`, or NULL (the
// in-order successor of `s` in the start-ordered tree). Caller holds G->lock.
static gk_region *region_succ(uintptr_t s) {
  gk_region *n = G->regions, *best = NULL;
  while (n) {
    if (n->start > s) { best = n; n = n->l; }
    else n = n->r;
  }
  return best;
}

// Unlink `node` (which is in the tree) by rotating it down to a leaf, keeping
// the heap order among the others. Caller holds G->lock.
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

// Create one memslot-backed region for [s, e). Caller holds G->lock and has
// ensured [s, e) does not overlap any existing region. A pool node freed by a
// region deletion is reused first, with the KVM memslot id it was created
// with, so both are bounded by the peak number of live regions.
static int region_add(uintptr_t s, uintptr_t e) {
  gk_region *n;
  int slot, recycled = G->region_free != NULL;
  if (recycled) {
    n = G->region_free;
    slot = n->slot;
  } else {
    if (G->region_n >= GK_MAX_REGIONS) return -1;
    n = &G->region_pool[G->region_n];
    slot = G->next_slot;
  }
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)slot,
                                          .guest_phys_addr = s,
                                          .memory_size = e - s,
                                          .userspace_addr = s};
  if (ioctl(G->vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (G->dbg) {  // fault path: raw output only (no strerror, it may allocate)
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] memslot "); db_dec(&d, slot);
      db_str(&d, " ["); db_hex(&d, s); db_str(&d, ","); db_hex(&d, e);
      db_str(&d, ") failed: errno "); db_dec(&d, errno);
      db_flush(&d);
    }
    return -1;  // the node stays where it was (free list or untouched pool tail)
  }
  if (recycled) {
    G->region_free = n->l;
  } else {
    G->region_n++;
    G->next_slot++;
  }
  memset(n, 0, sizeof *n);
  n->start = s;
  n->end = e;
  n->slot = slot;
  n->prio = gk_rand();
  G->regions = treap_insert(G->regions, n);
  G->region_live++;
  return 0;
}

// Delete a region's memslot and drop it from the tree; its node (and memslot
// id) go to the free list. Caller holds G->lock and has made sure no root maps
// a page in the region any more, so the guest cannot reach it. Returns the
// ioctl's result: on failure the region stays.
static int region_remove(gk_region *n) {
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)n->slot,
                                          .guest_phys_addr = n->start,
                                          .memory_size = 0,
                                          .userspace_addr = n->start};
  if (ioctl(G->vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (G->dbg)
      fprintf(stderr, "[gk] memslot %d [%#lx,%#lx) delete failed: %s\n", n->slot,
              (unsigned long)n->start, (unsigned long)n->end, strerror(errno));
    return -1;
  }
  G->regions = treap_remove(G->regions, n);
  G->region_live--;
  n->l = G->region_free;
  G->region_free = n;
  return 0;
}

// The region with the smallest start at or beyond `s`, or NULL. Caller holds
// G->lock.
static gk_region *region_lower_bound(uintptr_t s) {
  gk_region *n = G->regions, *best = NULL;
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
// how many were deleted. Caller holds G->lock.
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
// memslots. Caller holds G->lock.
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
// binary search: no allocation there. Guarded by G->lock.
//
// Which keys a pkey_mprotect may use is not checked against the set of keys
// the process has allocated: that set is the kernel's, and gk sees only the
// pkey_alloc calls forwarded from inside the guest, not the ones the host side
// makes outside any guest entry (V8 allocates its sandbox, JIT and pointer-
// table keys at initialization, and keys pages with them from within a turn
// whenever it commits or decommits sandbox, code or table pages). A key the
// kernel would reject as unallocated is thus virtualized like any other; it
// costs nothing in isolation, which comes from the page-table root, and the
// guest can only ever key pages it could mprotect anyway. What gk cannot do
// -- reflect a key outside the PTE field, or record one more range -- fails
// with ENOMEM, the one errno a pkey_mprotect caller has to be prepared for
// (V8 aborts on any other).
//
// Master switch for protection-key virtualization. gk isolates isolates by
// page-table root, so V8's host-side keys are not needed for security in this
// model; set this to 0 and the guest PTEs carry no key, so nothing is enforced
// and the keys become no-ops -- turning the whole scheme off in one line while
// leaving the rest (W^X, CR0.WP, the real TLB flush) intact.
#ifndef GK_VIRTUALIZE_PKEYS
#define GK_VIRTUALIZE_PKEYS 1
#endif


// The key of the page holding `addr`, or 0. Caller holds G->lock.
static int pkey_lookup(uintptr_t addr) {
  if (!GK_VIRTUALIZE_PKEYS) return 0;  // keys not reflected into PTEs -> no-ops
  int lo = 0, hi = G->pkey_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (addr < G->pkeys[mid].start) hi = mid;
    else if (addr >= G->pkeys[mid].end) lo = mid + 1;
    else return G->pkeys[mid].pkey;
  }
  return 0;
}

// Index of the first range whose end is beyond `addr`. Caller holds G->lock.
static int pkey_lower_bound(uintptr_t addr) {
  int lo = 0, hi = G->pkey_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (G->pkeys[mid].end <= addr) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Set the key of [s, e) (page-aligned) to `pkey`, replacing whatever keys the
// range held. Existing ranges overlapping [s, e) are trimmed, split or removed;
// a nonzero key is then inserted and merged with equal-key neighbors. Fails
// only when the table is full (pkey_room() guarantees it is not); the table is
// then left unchanged. Caller holds G->lock.
static int pkey_room(void) { return G->pkey_n + 2 <= GK_MAX_PKEY_RANGES; }
static int pkey_set_range(uintptr_t s, uintptr_t e, int pkey) {
  if (!pkey_room()) return -1;
  int i = pkey_lower_bound(s);
  while (i < G->pkey_n && G->pkeys[i].start < e) {
    gk_pkey_range *r = &G->pkeys[i];
    if (r->start < s && r->end > e) {  // [s, e) is strictly inside r: split it
      memmove(&G->pkeys[i + 2], &G->pkeys[i + 1], (size_t)(G->pkey_n - i - 1) * sizeof *r);
      G->pkey_n++;
      G->pkeys[i + 1] = (gk_pkey_range){e, r->end, r->pkey};
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
      memmove(r, r + 1, (size_t)(G->pkey_n - i - 1) * sizeof *r);
      G->pkey_n--;
    }
  }
  if (pkey == 0) return 0;
  // Now every range before index i ends at or before s, and every range from i
  // on starts at or after e. Insert [s, e), absorbing adjacent equal keys.
  if (i > 0 && G->pkeys[i - 1].end == s && G->pkeys[i - 1].pkey == pkey) {
    G->pkeys[i - 1].end = e;
    if (i < G->pkey_n && G->pkeys[i].start == e && G->pkeys[i].pkey == pkey) {
      G->pkeys[i - 1].end = G->pkeys[i].end;
      memmove(&G->pkeys[i], &G->pkeys[i + 1], (size_t)(G->pkey_n - i - 1) * sizeof G->pkeys[0]);
      G->pkey_n--;
    }
    return 0;
  }
  if (i < G->pkey_n && G->pkeys[i].start == e && G->pkeys[i].pkey == pkey) {
    G->pkeys[i].start = s;
    return 0;
  }
  memmove(&G->pkeys[i + 1], &G->pkeys[i], (size_t)(G->pkey_n - i) * sizeof G->pkeys[0]);
  G->pkeys[i] = (gk_pkey_range){s, e, pkey};
  G->pkey_n++;
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
// an existing memslot. Refused pages within the range are left unmapped, as a
// demand fault would leave them: a host mapping that abuts one of gk's refused
// allocations (the kernel merges neighboring anonymous mappings) can be
// eager-mapped without the refused part failing the whole call. So are pages
// inside an arena's reservation (a guest-created thread whose clone named a
// stack there, say): they belong to the arena's private subtree and are left
// to demand paging under the arena's root. Installed here they would land in
// the base root's subtree for the arena's slot, which every other root shares,
// and be reachable from every other arena. Nothing mapped here is global: the
// ranges are gk's control pages and guest stacks. Caller holds G->lock.
static int mmu_map_range(uintptr_t s, uintptr_t e, int perms) {
  if (region_ensure(s, e) < 0) return -1;
  for (uintptr_t v = s & ~0xfffUL; v < e; v += 0x1000) {
    if (prot_class(v) == GK_PROT_REFUSE || arena_containing(v)) continue;
    if (map4k_root(G->pml4, v, (uint64_t)perms, pkey_lookup(v)) < 0) return -1;
  }
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

// Map the pages around a just-mapped user page ahead of their own faults, from
// the same /proc/self/maps read that mapped it. That read is the bulk of a
// demand fault's cost (the kernel renders every mapping of a V8-sized process
// up to the one asked for), and it describes the whole host mapping [rs, re)
// with `perms`, so one read can settle a neighborhood of pages rather than
// one. The neighborhood is the GK_NEIGHBOR_WIN-aligned window holding `page`,
// clipped to the host mapping: it is aligned, so it never leaves the 2MB
// backing window of `page` or crosses a 512GiB slot, and everything in it that
// gets a PTE is host-backed at exactly `perms` under the same G->lock hold
// that read them. Nothing here is cached beyond the read: a later host change
// is reflected or recovered for these PTEs exactly as for a page the guest
// faulted itself (unmap_range_all, reprotect_range_all, host_backing_retry).
//
// This is an optimization, never a widening: a page is mapped only when it
// would have been mapped the same way by its own fault under demand_map's
// rules, and skipped otherwise, to fault on its own later.
//  * Its class is KEEP: SUPER and REFUSE pages are left to their own faults
//    (a REFUSE page is refused there; a SUPER page is mapped with U cleared).
//  * It lies in the same arena as `page`, or like `page` in none, so it goes
//    into the same root: an arena page never lands in the base root, and no
//    other arena's page is mapped at all. (Aligned windows cannot straddle an
//    arena boundary anyway; the check is per page regardless.)
//  * Its backing window has a memslot; none is created here.
//  * It has no PTE in the root yet: an existing entry may be carrying wall or
//    protection state that a reflection maintains in place, and is left alone.
//  * It is in no other vCPU's host-frame wall window. Such a page is that
//    vCPU's caller stack, which only that vCPU's own faults should map, and
//    they map it without W. A page in this vCPU's own window is mapped the
//    way its own read fault would be: without W, tagged PTE_WALL.
//  * Its protection key is looked up per page, like the global-pages
//    decision, which map4k_root re-checks itself.
// Faults counted in demand_faults are the guest's own; pages mapped here are
// counted in neighbor_pages. Caller holds G->lock; `root`, `in`, `rs`, `re`,
// `perms` and `sp` are demand_map's for the page it just mapped.
static void demand_map_neighbors(gk_thread *t, uintptr_t page, uint64_t *root, gk_arena *in,
                                 uintptr_t rs, uintptr_t re, int perms, uintptr_t sp) {
  uintptr_t lo = page & ~(GK_NEIGHBOR_WIN - 1), hi = lo + GK_NEIGHBOR_WIN;
  if (lo < rs) lo = rs;
  if (hi > re) hi = re;
  // The mapping is the interrupted code's stack, or this vCPU's ring-3 stack:
  // the same for every page of it (see demand_map).
  int stack_map = (rs < t->wall_map_hi && re > t->wall_map_lo) || (sp >= rs && sp < re);
  for (uintptr_t v = lo; v < hi; v += 0x1000) {
    if (v == page) continue;
    if (prot_class(v) != GK_PROT_KEEP) continue;
    if (arena_containing(v) != in) continue;
    if (!region_find(v)) continue;
    if (pte_lookup(root, v) & PTE_P) continue;
    int foreign_wall = 0;
    for (int i = 0; i < G->thread_n && !foreign_wall; i++) {
      gk_thread *o = &G->threads[i];
      if (o != t && v >= o->ro_lo && v < o->ro_hi) foreign_wall = 1;
    }
    if (foreign_wall) continue;
    int walled = v >= t->ro_lo && v < t->ro_hi;
    uint64_t flags = (uint64_t)perms;
    if (walled && (perms & 2)) flags = (flags & ~2ULL) | GK_MAP_WALLED;
    if (!in && !walled && !stack_map) flags |= GK_MAP_GLOBAL;
    if (map4k_root(root, v, flags, pkey_lookup(v)) < 0) continue;
    G->neighbor_pages++;
  }
}

// Handle a guest page fault: if the faulting page is host-accessible, back it
// with a memslot and PTE so the guest can retry. Caller must not hold G->lock.
//
// The PTE always carries the page's *current* host protection, re-read from
// /proc/self/maps on every fault, so mprotect/mmap protection changes are
// honored even if a syscall reflection missed them; a stale read-only PTE on a
// page the host has made writable would silently drop guest writes. The host
// protection is read under G->lock, the same lock a protection-changing syscall
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
// subtrees all arena roots share, and is global unless the page is part of a
// stack (see the global-pages section); `sp` is the interrupted code's stack
// pointer, which names the mapping its stack lies in.
static int demand_map(gk_thread *t, uintptr_t addr, uintptr_t sp) {
  uintptr_t page = addr & ~0xfffUL;
  uintptr_t rs, re; int perms;
  pthread_mutex_lock(&G->lock);
  uint64_t *root = G->pml4;
  gk_arena *in = arena_containing(page);
  if (in) {
    if (in != t->active_arena) {
      pthread_mutex_unlock(&G->lock);
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
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": REFUSE fault "); db_hex(&d, page);
      db_str(&d, " (gk control region)");
      db_flush(&d);
    }
    pthread_mutex_unlock(&G->lock);
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
    pthread_mutex_unlock(&G->lock);
    return -1;  // not host-readable: a genuine fault
  }
  if (!region_find(page) && region_ensure(page, page + 1) < 0) {
    pthread_mutex_unlock(&G->lock);
    return -1;
  }
  int pkey = pkey_lookup(page);
  // Inside this vCPU's host-frame wall the page is mapped without write
  // permission whatever the host has: readable (a conservative GC scan reads
  // the caller's frames) but not writable by the turn. A write fault there
  // never reaches this point (see run_vcpu), so only reads are mapped. A page
  // the host does allow writes to is tagged PTE_WALL, so that wall_lower can
  // give W back in place instead of the page re-faulting after the turn.
  uint64_t flags = (uint64_t)perms;
  if (page >= t->ro_lo && page < t->ro_hi && (perms & 2)) flags = (flags & ~2ULL) | GK_MAP_WALLED;
  // A base-root page may be global unless it is part of a stack: in this
  // vCPU's wall window, in the stack mapping its ring-3 turns run on (cached
  // by stack_wall_bounds; the mapping may have grown since, hence the overlap
  // test), or in the mapping the interrupted code's stack pointer lies in.
  // map4k_root checks the rest of the global-pages conditions itself.
  int stack = (page >= t->ro_lo && page < t->ro_hi) ||
              (rs < t->wall_map_hi && re > t->wall_map_lo) || (sp >= rs && sp < re);
  if (!in && !stack) flags |= GK_MAP_GLOBAL;
  int r = map4k_root(root, page, flags, pkey);  // honor R/W/X for W^X
  if (r >= 0) {
    G->demand_ok++;
    // Settle the page's neighborhood from the same read (an aligned window,
    // so it shares the page's top-level entry, which the carry-over below
    // covers).
    if (cls == GK_PROT_KEEP) demand_map_neighbors(t, page, root, in, rs, re, perms, sp);
    // A base-root mapping that populated a fresh top-level entry is not yet in
    // the active arena's root (which copied the base root's entries at entry,
    // see sync_cr3); carry it over now so the retry does not fault again. The
    // index cannot be one of the arena's own slots: the page is outside every
    // arena.
    gk_arena *a = t->active_arena;
    if (root == G->pml4 && a) {
      int idx = (int)((page >> 39) & 0x1ff);
      if (a->pml4[idx] != G->pml4[idx]) a->pml4[idx] = G->pml4[idx];
    }
  }
  pthread_mutex_unlock(&G->lock);
  if (G->dbg && (cls == GK_PROT_SUPER || pkey != 0)) {
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, cls == GK_PROT_SUPER ? ": SUPER demand-map " : ": demand-map ");
    db_hex(&d, page);
    db_str(&d, " perms="); db_dec(&d, perms);
    if (cls == GK_PROT_SUPER) db_str(&d, " (U cleared)");
    if (pkey != 0) { db_str(&d, " pkey="); db_dec(&d, pkey); db_str(&d, " (PTE bits 62:59)"); }
    db_flush(&d);
  }
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

// ---- thread records ----------------------------------------------------------
static long gettid_raw(void) { return host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0); }

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
  else if (G->thread_n < GK_MAX_VCPUS) t = &G->threads[G->thread_n++];
  else return NULL;
  memset(t, 0, sizeof *t);
  t->tid = tid;
  t->active_pml4 = G->pml4;
  return t;
}

// Bind `t` (taken from the pool) to the calling thread. `host` says the thread
// is one that ends through pthread exit (and so gets the key destructor), as
// opposed to a guest-created thread, which ends through its exit syscall.
static void thread_bind(gk_thread *t, int host) {
  tls_rec = (int)(t - G->threads) + 1;
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
  return t;
}

// Select `a` (NULL: the base root) as the active arena of the thread `t`
// belongs to, returning the previous one.
static gk_arena *arena_enter(gk_thread *t, gk_arena *a) {
  gk_arena *prev = t->active_arena;
  if (prev == a) return prev;
  if (a) __atomic_fetch_add(&a->active_threads, 1, __ATOMIC_SEQ_CST);
  t->active_arena = a;
  t->active_pml4 = a ? a->pml4 : G->pml4;
  if (prev) __atomic_fetch_sub(&prev->active_threads, 1, __ATOMIC_SEQ_CST);
  return prev;
}

// Return the thread's vCPU (and guest stack, if any) to the pool.
static void vcpu_park(gk_thread *t) {
  if (!t->inited) return;
  pthread_mutex_lock(&G->lock);
  if (G->parked_n < GK_MAX_VCPUS) {
    gk_parked_vcpu *p = &G->parked[G->parked_n++];
    p->fd = t->fd;
    p->id = t->id;
    p->run = t->run;
    p->stack_top = t->stack_top;
    p->ist = t->ist;
    p->side_stack = t->side_stack;
  } else {  // cannot happen with KVM_CAP_MAX_VCPUS <= GK_MAX_VCPUS; be safe
    munmap(t->run, G->run_size);
    close(t->fd);
  }
  pthread_mutex_unlock(&G->lock);
  if (G->dbg) fprintf(stderr, "[gk] vcpu %d parked\n", t->id);
  t->inited = 0;
}

// The thread `t` belongs to is ending: park its vCPU, drop it from its active
// arena's user count, and return the record to the pool.
static void thread_release(gk_thread *t) {
  vcpu_park(t);
  arena_enter(t, NULL);
  tls_rec = 0;
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
  (void)p;
  gk_thread *t = thread_cur();
  if (t) thread_release(t);
}

// Bring the calling thread's vCPU up. Each thread has its own vCPU, stack and
// TLS base, but they share the VM, memslots and page tables. A parked vCPU is
// reused when one is available (see the vCPU pool); otherwise a new one is
// created. Either way the segment bases, CR3 and FPU are programmed for the
// calling thread, so a reused vCPU carries nothing over from its last thread
// except its id. `t` is the calling thread's record. `flags`: GK_VCPU_STACK
// when the thread needs the private guest stack (gk_run). A thread the guest
// created itself already has the stack its clone named (see clone_thread), so
// it passes nothing.
#define GK_VCPU_STACK 2

// Load the flat ring-0 code and data descriptors (GDT indices 1 and 2) into
// s's CS and SS/DS/ES: the privilege every guest entry starts at, and the one
// the stubs and handlers need.
static void sregs_ring0(struct kvm_sregs *s) {
  struct kvm_segment cs = {.base = 0, .limit = 0xffffffff, .selector = 0x08,
                           .type = 11, .present = 1, .s = 1, .l = 1, .g = 1};
  struct kvm_segment ds = {.base = 0, .limit = 0xffffffff, .selector = 0x10,
                           .type = 3, .present = 1, .s = 1, .db = 1, .g = 1};
  s->cs = cs;
  s->ds = s->es = s->ss = ds;
}

static int vcpu_init(gk_thread *t, int flags) {
  if (t->inited) return 0;
  int id, fd, reused = 0;
  struct kvm_run *run = NULL;
  uint64_t stack_top = 0, ist = 0;
  void *side_stack = NULL;
  pthread_mutex_lock(&G->lock);
  if (G->parked_n > 0) {
    gk_parked_vcpu *p = &G->parked[--G->parked_n];
    fd = p->fd; id = p->id; run = p->run; stack_top = p->stack_top; ist = p->ist;
    side_stack = p->side_stack;
    reused = 1;
  }
  pthread_mutex_unlock(&G->lock);
  if (!reused) {
    id = atomic_fetch_add(&G->vcpus_created, 1);
    fd = ioctl(G->vmfd, KVM_CREATE_VCPU, id);
    if (fd < 0) { G->err = "KVM_CREATE_VCPU"; return -1; }
    if (ioctl(fd, KVM_SET_CPUID2, GK_CPUID) < 0) { G->err = "KVM_SET_CPUID2"; return -1; }
    run = mmap(NULL, G->run_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (run == MAP_FAILED) { G->err = "mmap kvm_run"; return -1; }

    // Exception stack. The guest runs ordinary user-mode code, which keeps
    // locals in the 128-byte red zone below rsp; the ABI promises that zone
    // survives asynchronous events. A ring-0 exception without a stack switch
    // would push its frame right there, so every gate switches to this vCPU's
    // IST1 stack, selected through a per-vCPU TSS that lives in the last page
    // of the region. Eager-mapped: the switch itself must never fault.
    uint8_t *ir = mmap(NULL, GK_IST_BYTES, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ir == MAP_FAILED) { G->err = "mmap exception stack"; return -1; }
    pthread_mutex_lock(&G->lock);
    // kvm_run is host/KVM shared state; the IST region holds the exception
    // stack, TSS and RSP0 stack. Ring 3 must reach neither: refuse the former,
    // map the latter supervisor (registered before mapping so U is cleared).
    prot_add((uintptr_t)run, (uintptr_t)run + G->run_size, GK_PROT_REFUSE);
    prot_add((uintptr_t)ir, (uintptr_t)ir + GK_IST_BYTES, GK_PROT_SUPER);
    int irc = mmu_map_range((uintptr_t)ir, (uintptr_t)ir + GK_IST_BYTES, 1 | 2);
    pthread_mutex_unlock(&G->lock);
    if (irc < 0) { G->err = "map exception stack"; return -1; }
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
    if (stk == MAP_FAILED) { G->err = "mmap guest stack"; return -1; }
    pthread_mutex_lock(&G->lock);
    int mrc = mmu_map_range((uintptr_t)stk, (uintptr_t)stk + stksz, 1 | 2);  // rw, NX
    pthread_mutex_unlock(&G->lock);
    if (mrc < 0) { G->err = "map guest stack"; return -1; }
    stack_top = (uintptr_t)stk + stksz;
  }

  struct kvm_sregs s;
  ioctl(fd, KVM_GET_SREGS, &s);
  s.cr3 = (uint64_t)(uintptr_t)G->pml4;
  // PGE makes the G bit of a PTE take effect (see the global-pages section).
  s.cr4 = CR4_PAE | CR4_PGE | CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE | CR4_PKE;
  // The guest runs in ring 0, where the CPU ignores a PTE's write-protect bit
  // and a protection key's write-disable bit unless CR0.WP is set. It is, so
  // read-only pages and write-disabled keys hold for the guest as they would
  // for a user-mode process.
  s.cr0 = CR0_PE | CR0_WP | CR0_PG;
  s.efer = EFER_LME | EFER_LMA | EFER_SCE | EFER_NXE;
  sregs_ring0(&s);
  uint64_t fsbase = 0, gsbase = 0;
  host_syscall(SYS_arch_prctl, 0x1003, (long)&fsbase, 0, 0, 0, 0);  // ARCH_GET_FS
  host_syscall(SYS_arch_prctl, 0x1004, (long)&gsbase, 0, 0, 0, 0);  // ARCH_GET_GS
  s.fs = s.ds; s.fs.base = fsbase;
  s.gs = s.ds; s.gs.base = gsbase;
  // TR comes from the vCPU's cached descriptor, set here; the GDT has no TSS
  // entry because the guest never executes ltr (iretq reloads only CS and SS).
  struct kvm_segment tr = {.base = ist + GK_IST_BYTES - 0x1000, .limit = 0x67,
                           .selector = 0x18, .type = 11, .present = 1};
  s.tr = tr;
  s.gdt.base = G->gdt_va; s.gdt.limit = 6 * 8 - 1;  // through the ring-3 descriptors (0x28)
  s.idt.base = G->idt_va; s.idt.limit = 256 * 16 - 1;
  if (ioctl(fd, KVM_SET_SREGS, &s) < 0) { G->err = "KVM_SET_SREGS"; return -1; }
  if (G->sync_regs) {
    // From here the vCPU's registers go through kvm_run (see regs_get). Its
    // sregs copy is made current now, as sync_cr3 may read it before this vCPU
    // first runs; and a dirty flag left by a previous thread's write that never
    // ran must not load that thread's registers over what was just set.
    ioctl(fd, KVM_GET_SREGS, &run->s.regs.sregs);
    run->kvm_valid_regs = GK_SYNC_REGS;
    run->kvm_dirty_regs = 0;
  }

  if (reused) {
    // Fresh x87/SSE control state, as a new thread would start with; the
    // previous thread's register contents are otherwise irrelevant because
    // the guest entry point sets every register it relies on.
    struct kvm_fpu f = {0};
    f.fcw = 0x37f;
    f.mxcsr = 0x1f80;
    if (ioctl(fd, KVM_SET_FPU, &f) < 0) { G->err = "KVM_SET_FPU"; return -1; }
    goto ready;
  }

  struct kvm_xcrs xcrs = {0};
  xcrs.nr_xcrs = 1; xcrs.xcrs[0].xcr = 0; xcrs.xcrs[0].value = 0x7;
  if (ioctl(fd, KVM_SET_XCRS, &xcrs) < 0) { G->err = "KVM_SET_XCRS"; return -1; }

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
    if (ioctl(fd, KVM_SET_DEVICE_ATTR, &ta) < 0 && G->dbg)
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
  if (ioctl(fd, KVM_SET_MSRS, &m) < 4) { G->err = "KVM_SET_MSRS"; return -1; }

ready:
  t->id = id;
  t->fd = fd;
  t->run = run;
  t->stack_top = stack_top;
  t->ist = ist;
  t->side_stack = side_stack;
  t->root_gen = 0;    // a fresh or reused vCPU flushes its TLB at first entry
  t->last_fault = 0;
  t->fault_repeat = 0;
  t->backing_retries = 0;
  t->pkru_ready = 0;  // a fresh or reused vCPU gets its PKRU at first entry
  t->user_mode = 0;   // ring 0 until enter_guest or child_entry selects ring 3
  t->tlb_touched = 1; // no known TLB state and no previous wall: the first
  t->prev_ro_lo = t->prev_ro_hi = 0;  // wall_raise on this vCPU flushes
  if (!t->active_pml4) t->active_pml4 = G->pml4;
  t->inited = 1;
  if (G->dbg)
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
      (uintptr_t)&gk_flush_stub_invpcid, (uintptr_t)gk_invpcid_desc,
      (uintptr_t)&gk_user_syscall_resume, (uintptr_t)&gk_user_launch,
      (uintptr_t)&gk_launch_cr3, (uintptr_t)&gk_user_launch_cr3,
      (uintptr_t)&gk_user_child_launch};
  // gk_user_exit_tramp is deliberately absent: it runs in ring 3, so it must
  // stay a user page (its own page-aligned section, demand-paged as user r-x).
  uintptr_t lo = hs[0], hi = hs[0];
  for (size_t i = 1; i < sizeof hs / sizeof hs[0]; i++) {
    if (hs[i] < lo) lo = hs[i];
    if (hs[i] > hi) hi = hs[i];
  }
  lo &= ~0xfffUL;
  hi = (hi + 64 + 0xfff) & ~0xfffUL;
  // The addresses are resolved by the linker, so the layout assumption above
  // is checked here rather than at compile time: if the ring-3 trampoline's
  // page ever fell inside the supervisor span, every ring-3 exit would fault.
  uintptr_t tramp = (uintptr_t)&gk_user_exit_tramp & ~0xfffUL;
  if (tramp < hi && tramp + 0x1000 > lo)
    gk_fatal("gk_user_exit_tramp shares a page with the supervisor handler text");
  // The handlers run in ring 0 and must be unreadable to ring 3, so map them
  // supervisor r-x. Register before mapping so map4k_root clears U.
  prot_add(lo, hi, GK_PROT_SUPER);
  return mmu_map_range(lo, hi, 1 | 4);  // r-x
}

// Pick the TLB flush stub (see gk_asm.S): INVPCID when the CPUID every vCPU
// gets (the host's, as KVM supports it) carries it in leaf 7 EBX bit 10 --
// KVM then lets the guest execute it natively under nested paging, one VM
// exit per flush -- and the CR4.PGE toggle otherwise, three. GK_NO_INVPCID in
// the environment forces the toggle, so the fallback stays testable on a CPU
// that has both. Both stubs drop every entry, global ones included (see the
// global-pages section).
static void flush_stub_select(void) {
  G->flush_stub = gk_flush_stub;
  for (unsigned i = 0; i < GK_CPUID->nent && !getenv("GK_NO_INVPCID"); i++) {
    const struct kvm_cpuid_entry2 *e = &GK_CPUID->entries[i];
    if (e->function == 7 && e->index == 0 && (e->ebx & (1u << 10))) {
      G->flush_stub = gk_flush_stub_invpcid;
      break;
    }
  }
  if (G->dbg)
    fprintf(stderr, "[gk] TLB flush stub: %s\n",
            G->flush_stub == gk_flush_stub_invpcid ? "invpcid" : "cr4.pge toggle");
}

int gk_init(void) {
  // The control block first: it is registered before anything is mapped, so
  // no root ever holds a user PTE for it (see the control-data note). Its
  // non-zero initial values are set here rather than statically, which would
  // move the whole multi-megabyte block from .bss into the file.
  pthread_mutex_init(&G->lock, NULL);
  G->root_gen = 1;
  G->rand_state = 0x9e3779b9u;
  prot_add(GK_CTL_START, GK_CTL_END, GK_PROT_SUPER);
  G->dbg = getenv("GK_DEBUG") != NULL;
  *(void **)&G->tls_static_info = dlsym(RTLD_DEFAULT, "_dl_get_tls_static_info");
  if (!G->tls_static_info)
    fprintf(stderr, "[gk] _dl_get_tls_static_info not found: gk_run_here_user builds no "
                    "host-frame wall on pthread stacks\n");
  G->kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (G->kvm < 0) { G->err = "open /dev/kvm"; return -1; }
  if (ioctl(G->kvm, KVM_GET_API_VERSION, 0) != 12) { G->err = "KVM API != 12"; return -1; }
  G->vmfd = ioctl(G->kvm, KVM_CREATE_VM, 0);
  if (G->vmfd < 0) { G->err = "KVM_CREATE_VM"; return -1; }

  G->pt_bytes = 64 * 1024 * 1024;
  uint8_t *pt = mmap(NULL, G->pt_bytes, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (pt == MAP_FAILED) { G->err = "mmap PT arena"; return -1; }
  G->pt_next = pt; G->pt_end = pt + G->pt_bytes; G->pt_base = (uintptr_t)pt;
  // The page-table arena is walked by the CPU through guest-physical addresses
  // (it has memslots), never through guest-virtual PTEs. Refuse it so ring-3
  // code cannot fault it in and rewrite the tables that enforce the arenas.
  prot_add(G->pt_base, G->pt_base + G->pt_bytes, GK_PROT_REFUSE);
  G->pml4 = alloc_table();

  uint8_t *tables = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (tables == MAP_FAILED) { G->err = "mmap gdt/idt"; return -1; }
  G->gdt_va = (uintptr_t)tables;
  G->idt_va = (uintptr_t)tables + 0x1000;
  // The GDT and IDT are read by the CPU (a supervisor access) during exception
  // delivery and ring transitions; ring-3 code has no business touching them.
  prot_add(G->gdt_va, G->gdt_va + 0x2000, GK_PROT_SUPER);
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
  uint64_t *gdt = (uint64_t *)(uintptr_t)G->gdt_va;
  gdt[0] = 0;
  gdt[1] = 0x00AF9A000000FFFFULL;  // ring-0 code64 (DPL 0, L=1)
  gdt[2] = 0x00CF92000000FFFFULL;  // ring-0 data   (DPL 0)
  gdt[3] = 0;
  gdt[4] = 0x00CFF2000000FFFFULL;  // ring-3 data   (DPL 3)
  gdt[5] = 0x00AFFA000000FFFFULL;  // ring-3 code64 (DPL 3, L=1)
  uint8_t *idt = (uint8_t *)(uintptr_t)G->idt_va;
  memset(idt, 0, 0x1000);
  set_idt_gate(idt, 0, (uintptr_t)&gk_exc_de);
  set_idt_gate(idt, 6, (uintptr_t)&gk_exc_ud);
  set_idt_gate(idt, 8, (uintptr_t)&gk_exc_df);
  set_idt_gate(idt, 13, (uintptr_t)&gk_exc_gp);
  set_idt_gate(idt, 14, (uintptr_t)&gk_exc_pf);

  // The page-table arena is read by the CPU page walker via guest-physical
  // addresses, so it needs memslots (but no PTEs of its own).
  if (region_ensure(G->pt_base, G->pt_base + G->pt_bytes) < 0) { G->err = "memslot for PT arena"; return -1; }
  // GDT/IDT and the handler text must be present before the first fault.
  if (mmu_map_range(G->gdt_va, G->gdt_va + 0x2000, 1 | 2) < 0) { G->err = "map gdt/idt"; return -1; }
  if (map_handler_text() < 0) { G->err = "map handler text"; return -1; }

  GK_CPUID->nent = GK_CPUID_ENTRIES;
  if (ioctl(G->kvm, KVM_GET_SUPPORTED_CPUID, GK_CPUID) < 0) { G->err = "GET_SUPPORTED_CPUID"; return -1; }
  flush_stub_select();
  G->run_size = ioctl(G->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  // Register exchange through kvm_run (see regs_get) when KVM offers it for
  // both register sets; GK_NO_SYNC_REGS in the environment forces the ioctls.
  if (!getenv("GK_NO_SYNC_REGS")) {
    long caps = ioctl(G->kvm, KVM_CHECK_EXTENSION, KVM_CAP_SYNC_REGS);
    G->sync_regs = caps > 0 && (caps & GK_SYNC_REGS) == GK_SYNC_REGS;
  }
  if (G->dbg) fprintf(stderr, "[gk] registers via %s\n", G->sync_regs ? "kvm_run" : "ioctl");
  if (pthread_key_create(&G->thread_key, thread_key_dtor) != 0) { G->err = "pthread_key_create"; return -1; }

  // Bring up the calling thread's record and vCPU.
  gk_thread *t = thread_get();
  if (!t) { G->err = "thread record"; return -1; }
  return vcpu_init(t, GK_VCPU_STACK);
}

int gk_vcpu_count(void) { return atomic_load(&G->vcpus_created); }

const char *gk_last_error(void) { return G->err; }
unsigned long gk_fault_addr(void) { return G->fault_addr; }
void gk_set_syscall_filter(gk_syscall_filter f) { G->filter = f; }

// Test/diagnostic hook: hand out one supervisor page (the GDT) and one refused
// page (the page-table arena), so a test can verify that ring-3 code faults on
// gk's own control memory while its own user pages work.
void gk_debug_control_addrs(unsigned long *super, unsigned long *refuse) {
  if (super) *super = (unsigned long)G->gdt_va;
  if (refuse) *refuse = (unsigned long)G->pt_base;
}

// Test/diagnostic hook: the address of one of gk's control structures (see
// gk.h for the indices), all of which live in the supervisor control block. A
// test writes there from ring 3 and expects a fault; from ring 0 the scratch
// word (index 0) can be written freely.
unsigned long gk_debug_ctl_addr(int which) {
  switch (which) {
    case GK_CTL_SCRATCH: return (unsigned long)&G->debug_scratch;
    case GK_CTL_ARENAS: return (unsigned long)&G->arena_n;
    case GK_CTL_ARENA_POOL: return (unsigned long)&G->arena_pool[0];
    case GK_CTL_PROT: return (unsigned long)&G->prot[0];
    case GK_CTL_REGIONS: return (unsigned long)&G->regions;
    case GK_CTL_REGION_POOL: return (unsigned long)&G->region_pool[0];
    case GK_CTL_PT_ALLOC: return (unsigned long)&G->pt_free;
    case GK_CTL_THREADS: return (unsigned long)&G->threads[0];
    case GK_CTL_VCPU_POOL: return (unsigned long)&G->parked[0];
    case GK_CTL_PKEYS: return (unsigned long)&G->pkeys[0];
    case GK_CTL_ROOT: return (unsigned long)&G->pml4;
    case GK_CTL_FILTER: return (unsigned long)&G->filter;
    default: return 0;
  }
}

#define GK_EPERM 1
#define GK_ENOMEM 12
#define GK_EINVAL 22

// ---- vCPU registers ---------------------------------------------------------
// With KVM_CAP_SYNC_REGS (G->sync_regs) a vCPU's general and special registers
// are exchanged through its mmap'd kvm_run instead of by ioctl: KVM stores both
// sets into run->s.regs at every KVM_RUN return, including a failed one
// (kvm_valid_regs, set once by vcpu_init), and at the next KVM_RUN loads the
// sets flagged in kvm_dirty_regs from there, before it completes a pending
// port write or runs the guest: exactly where an ioctl issued in between would
// have taken effect. A write is thus deferred to the next KVM_RUN on this
// vCPU, which every writer follows it with; sregs_set_now is for writes that
// must take effect back to back (host_backing_retry). kvm_run is refused to the
// guest, so it can no more forge a register here than an ioctl argument.
// Without the capability (or with GK_NO_SYNC_REGS set), the ioctls.
static void regs_get(gk_thread *t, struct kvm_regs *r) {
  if (G->sync_regs) *r = t->run->s.regs.regs;
  else ioctl(t->fd, KVM_GET_REGS, r);
}

static void regs_set(gk_thread *t, const struct kvm_regs *r) {
  if (G->sync_regs) {
    t->run->s.regs.regs = *r;
    t->run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
  } else {
    ioctl(t->fd, KVM_SET_REGS, r);
  }
}

static void sregs_get(gk_thread *t, struct kvm_sregs *s) {
  if (G->sync_regs) *s = t->run->s.regs.sregs;
  else ioctl(t->fd, KVM_GET_SREGS, s);
}

static int sregs_set(gk_thread *t, const struct kvm_sregs *s) {
  if (G->sync_regs) {
    t->run->s.regs.sregs = *s;
    t->run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS;
    return 0;
  }
  return ioctl(t->fd, KVM_SET_SREGS, s) < 0 ? -1 : 0;
}

// Loads the sregs into the vCPU right away, by ioctl, keeping the kvm_run copy
// current so the next sregs_get reads what was set.
static int sregs_set_now(gk_thread *t, const struct kvm_sregs *s) {
  if (ioctl(t->fd, KVM_SET_SREGS, s) < 0) return -1;
  if (G->sync_regs) {
    t->run->s.regs.sregs = *s;
    t->run->kvm_dirty_regs &= ~(uint64_t)KVM_SYNC_X86_SREGS;
  }
  return 0;
}

// Run one of the gk_asm.S stubs on this vCPU until it reports on `port`.
// Clobbers the vCPU's general registers: the caller reprograms them afterwards
// (gk_run and child_entry set the entry registers; a syscall exit sets the
// snapshot back and, because KVM only completes the pending outb when RIP is
// unchanged, points RIP at gk_syscall_tramp_resume). Sregs are untouched.
static int run_stub(gk_thread *t, void (*stub)(void), uint64_t rdi, uint64_t rsi,
                    uint16_t port, const char *what) {
  struct kvm_regs regs = {0};
  regs.rip = (uintptr_t)stub;
  regs.rdi = rdi;
  regs.rsi = rsi;
  regs.rflags = 0x2;
  regs_set(t, &regs);
  for (;;) {
    t->tlb_touched = 1;  // the vCPU runs: its TLB is no longer known (see wall_raise)
    if (ioctl(t->fd, KVM_RUN, 0) < 0) {
      if (errno == EINTR) continue;
      if (G->dbg) fprintf(stderr, "[gk] vcpu %d: %s stub KVM_RUN: %s\n", t->id, what, strerror(errno));
      return -1;
    }
    if (t->run->exit_reason == KVM_EXIT_IO && t->run->io.direction == KVM_EXIT_IO_OUT &&
        t->run->io.port == port)
      return 0;
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d: %s stub: unexpected exit reason=%u\n", t->id, what,
              t->run->exit_reason);
    return -1;
  }
}

// Flush this vCPU's whole TLB, global entries included, by running the flush
// stub gk_init selected (see flush_stub_select). Every full flush gk does
// goes through here, so gk_stats.tlb_flushes counts them all. No ioctl does
// this from the host: KVM flushes the guest TLB only when KVM_SET_SREGS
// changes a control register, and re-setting the same values is a no-op.
static int flush_run(gk_thread *t) {
  atomic_fetch_add_explicit(&G->tlb_flushes, 1, memory_order_relaxed);
  return run_stub(t, G->flush_stub, 0, 0, PORT_FLUSH, "flush");
}

// Flush this vCPU's TLB from a syscall exit, whose register snapshot is `r`:
// the guest runs the flush stub and resumes after the hypercall.
static void flush_tlb(gk_thread *t, struct kvm_regs *r) {
  if (flush_run(t) == 0) r->rip = (uintptr_t)&gk_syscall_tramp_resume;
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
static long run_vcpu(gk_thread *t);
static int sync_cr3(gk_thread *t, uint64_t *load);
static int cr3_load_host(gk_thread *t, uint64_t cr3);
static void host_pkru_allow_all(void);
static uint32_t host_pkru(void);
static int guest_pkru_update(gk_thread *t, uint32_t keep, uint32_t set, uint32_t *out);

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
// start routine. The child runs at the privilege its creator had: a ring-0
// creator's child starts at its RIP in ring 0, a ring-3 creator's child is
// launched through gk_user_child_launch, which SYSRETs down to that RIP in ring
// 3. A thread spawned by untrusted code thus never gains ring 0, and from ring 3
// its syscalls, faults and privileged instructions take the same paths as the
// top-level ring-3 function's (see enter_guest).
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
  int ok = host_region(t->start.regs.rsp - 1, &rs, &re, &perms) && (perms & 2) &&
           mmu_map_range(rs, re, perms) == 0;
  pthread_mutex_unlock(&G->lock);
  if (!ok) {
    fprintf(stderr, "[gk] guest thread: cannot map child stack at %#llx\n",
            (unsigned long long)t->start.regs.rsp);
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
  struct kvm_regs regs = t->start.regs;
  if (t->start.user) {
    // Enter ring 3 through the SYSRET stub. The snapshot's RCX and R11 already
    // hold the return RIP and RFLAGS SYSCALL saved in the parent; the stub's
    // contract wants RCX = the start RIP, which is that same return address.
    regs.rip = (uintptr_t)&gk_user_child_launch;
    regs.rcx = t->start.regs.rip;
  }
  regs_set(t, &regs);
  if (G->dbg)
    fprintf(stderr, "[gk] vcpu %d: guest thread tid %ld (from vcpu %d) rip=%#llx rsp=%#llx pkru=%#x ring %d\n",
            t->id, t->tid, t->start.parent_id, (unsigned long long)t->start.regs.rip,
            (unsigned long long)t->start.regs.rsp, t->start.pkru, t->start.user ? 3 : 0);
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
// parent's record and `r` its register snapshot at the syscall trampoline (rip
// at the outb, rcx = the guest's return address). Returns what the guest sees
// as the clone result.
static long clone_thread(gk_thread *t, struct kvm_regs *r, long nr) {
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
  if (guest_pkru_update(t, ~0u, 0, &pkru) < 0) return -GK_EINVAL;
  r->rip = (uintptr_t)&gk_syscall_tramp_resume;  // the stub consumed the outb

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
  c->start.regs.rax = 0;          // the child's clone return value
  c->start.regs.rsp = child_sp;   // the stack the clone named
  c->start.regs.rip = r->rcx;     // straight to the guest's return address
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

// ---- syscall forwarding ----------------------------------------------------
// Every syscall is re-issued on the host; guest and host share memory, so
// pointer arguments work directly. New mappings (mmap/brk/mprotect) are picked
// up lazily by demand paging on first access, so nothing special is needed here.
static long forward_syscall(gk_thread *t, struct kvm_regs *r) {
  long nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx, a4 = r->r10,
       a5 = r->r8, a6 = r->r9;
  if (G->filter && !G->filter(nr, a1, a2, a3, a4, a5, a6)) {
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d syscall %ld DENIED\n", t->id, nr);
    return -GK_EPERM;
  }
  // Thread creation: the child must enter the guest too. Only true threads
  // (CLONE_VM|CLONE_THREAD) are intercepted; fork/vfork-style clones forward.
  if (nr == SYS_clone || nr == GK_SYS_clone3) {
    uint64_t flags = 0;
    if (nr == SYS_clone) flags = (uint64_t)a1;
    else if (a1 && (uint64_t)a2 >= 64) flags = ((struct gk_clone_args *)a1)->flags;
    if ((flags & (GK_CLONE_VM | GK_CLONE_THREAD)) == (GK_CLONE_VM | GK_CLONE_THREAD)) {
      long tid = clone_thread(t, r, nr);
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
  // Protection keys (see the protection-keys section above). pkey_mprotect
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
  uintptr_t rs = (uintptr_t)a1 & ~0xfffUL;
  uintptr_t re = ((uintptr_t)a1 + (uintptr_t)a2 + 0xfff) & ~0xfffUL;
  if (reflect) pthread_mutex_lock(&G->lock);
  if (nr == SYS_pkey_mprotect) {
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
    if (pkey_set_range((uintptr_t)ret & ~0xfffUL,
                       ((uintptr_t)ret + (uintptr_t)a2 + 0xfff) & ~0xfffUL, 0) < 0 && G->dbg)
      fprintf(stderr, "[gk] pkey table full; mapping at %#lx keeps a stale key\n",
              (unsigned long)ret);
    pthread_mutex_unlock(&G->lock);
  }
  if (nr == SYS_pkey_alloc && ret >= 0 && ret <= 15) {
    // The kernel gave the calling *host* thread the key's initial rights; the
    // guest thread is this vCPU, so give them to its PKRU (2 bits per key:
    // bit 0 = access disable, bit 1 = write disable).
    uint32_t shift = 2u * (uint32_t)ret, pkru = 0;
    if (guest_pkru_update(t, ~(3u << shift), ((uint32_t)a2 & 3u) << shift, &pkru) == 0)
      r->rip = (uintptr_t)&gk_syscall_tramp_resume;  // the stub consumed the outb
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d pkey_alloc -> key %ld, rights %#lx; guest PKRU now %#x\n",
              t->id, ret, (unsigned long)a2, pkru);
  }
  if (G->dbg) {
    fprintf(stderr, "[gk] vcpu %d syscall %ld(%#lx, %#lx, %#lx, %#lx) -> %ld\n",
            t->id, nr, (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
            (unsigned long)a4, ret);
    if (nr == SYS_pkey_mprotect && ret == 0)
      fprintf(stderr, "[gk] vcpu %d pkey_mprotect [%#lx,%#lx) key %ld: host mprotect only, "
              "guest PTEs will carry key %d\n", t->id, (unsigned long)rs, (unsigned long)re,
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
static int guest_pkru_update(gk_thread *t, uint32_t keep, uint32_t set, uint32_t *out) {
  if (run_stub(t, gk_pkru_stub, set, keep, PORT_PKRU, "pkru") < 0) return -1;
  struct kvm_regs regs;
  regs_get(t, &regs);
  if (out) *out = (uint32_t)regs.rax;
  return 0;
}

// Refresh an arena root's shared entries from the base root so it sees every
// mapping added since it was last synced, and put the arena's private PDPT
// entries back in its own slots. Entry by entry, with 8-byte stores, never
// touching the arena's slots: another vCPU may be running under this root at
// the same time (two threads in one arena), and its page walker must never
// see a torn entry or a transiently absent slot. Caller holds G->lock.
static void arena_sync_root(gk_arena *a) {
  for (int i = 0; i < 512; i++) {
    if (i >= a->slot0 && i < a->slot0 + a->nslots) continue;
    if (a->pml4[i] != G->pml4[i]) __atomic_store_n(&a->pml4[i], G->pml4[i], __ATOMIC_RELAXED);
  }
  for (int i = 0; i < a->nslots; i++)
    if (a->pml4[a->slot0 + i] != a->slot_entry[i])
      __atomic_store_n(&a->pml4[a->slot0 + i], a->slot_entry[i], __ATOMIC_RELAXED);
}

// Ready this vCPU's root for an entry: bring an active arena's root up to date
// with the base root (see arena_sync_root) and find out whether the vCPU's CR3
// already names the thread's active root. If not, *load is set to the root
// the entry must load and 0 otherwise. The load itself is the guest's own MOV
// CR3 at the start of the turn (see enter_guest and the global-pages
// section), or cr3_load_host for an entry that does not go through
// enter_guest. The vCPU's CR3 is read back from KVM rather than remembered:
// a load from inside the guest, one from the host (host_backing_retry) and one
// that failed all leave it wherever the vCPU actually is, and an entry that
// assumed a root it does not have would run one isolate's turn under
// another's tables.
//
// A CR3 load drops every non-global translation. An unchanged CR3 drops
// nothing, so when page-table pages were freed since this vCPU last flushed
// (G->root_gen moved), the guest flushes explicitly (flush_run): the same
// root page may by now be a different arena's, or its subtrees may have been
// rebuilt from recycled tables, and the vCPU's cached translations would be
// stale. The freed tables were an arena's private ones, whose translations are
// never global, so on a changed CR3 the load alone covers the generation
// change too.
static int sync_cr3(gk_thread *t, uint64_t *load) {
  if (t->active_arena) {
    pthread_mutex_lock(&G->lock);
    arena_sync_root(t->active_arena);
    pthread_mutex_unlock(&G->lock);
  }
  unsigned gen = __atomic_load_n(&G->root_gen, __ATOMIC_ACQUIRE);
  uint64_t want_cr3 = (uint64_t)(uintptr_t)t->active_pml4;
  struct kvm_sregs s;
  sregs_get(t, &s);
  *load = 0;
  if (s.cr3 != want_cr3) {
    *load = want_cr3;
  } else if (t->root_gen != gen) {
    if (flush_run(t) < 0) return -1;
  }
  t->root_gen = gen;
  return 0;
}

// Load `cr3` into this vCPU from the host, for an entry that does not go
// through enter_guest (a guest-created thread's first, see child_entry). KVM
// then flushes the whole guest TLB, global entries included, at the next
// KVM_RUN.
static int cr3_load_host(gk_thread *t, uint64_t cr3) {
  struct kvm_sregs s;
  sregs_get(t, &s);
  s.cr3 = cr3;
  t->tlb_touched = 1;  // the TLB changes under the host's hand (see wall_raise)
  return sregs_set(t, &s);
}

// Ready this thread's vCPU for a guest invocation: its root (*load, see
// sync_cr3), its PKRU on first entry, and the per-invocation fault-repeat
// tracking.
static int prepare_entry(gk_thread *t, uint64_t *load) {
  if (sync_cr3(t, load) < 0) return -1;
  uint32_t hp = host_pkru();
  host_pkru_allow_all();
  if (!t->pkru_ready) {
    // The thread enters the guest with the PKRU the kernel gave it (the
    // process default, or its creator's), as its guest value.
    if (guest_pkru_update(t, 0, hp, NULL) < 0) return -1;
    t->pkru_ready = 1;
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d: initial guest PKRU %#x\n", t->id, hp);
  }
  t->last_fault = 0;  // fault-repeat tracking is per guest invocation
  t->fault_repeat = 0;
  t->backing_retries = 0;
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
// `cr3`, when nonzero, is the root the turn must run under (see sync_cr3):
// RIP then goes to the variant of the entry stub that loads it into CR3 first
// (RSI = the root; gk_launch_cr3 also takes RAX = fn), inside the turn's own
// KVM_RUN. The guest's MOV CR3 is not intercepted under nested paging and
// keeps the TLB's global entries, the shared runtime's translations, whereas a
// CR3 set through KVM makes KVM flush the whole guest TLB (see the
// global-pages section). Either way the return value comes back through
// run_vcpu.
static long enter_guest(gk_thread *t, long (*fn)(void *), void *arg, uint64_t stack_top,
                        int user, uint64_t cr3) {
  uint64_t sp = stack_top - 8;
  *(uint64_t *)sp = user ? (uintptr_t)&gk_user_exit_tramp : (uintptr_t)&gk_exit_tramp;

  struct kvm_regs regs = {0};
  regs.rsp = sp;
  regs.rdi = (uintptr_t)arg;
  regs.rsi = cr3;
  regs.rflags = 0x2;
  if (user) {
    regs.rip = cr3 ? (uintptr_t)&gk_user_launch_cr3 : (uintptr_t)&gk_user_launch;
    regs.rcx = (uintptr_t)fn;   // SYSRET target RIP
    regs.r11 = 0x2;             // SYSRET target RFLAGS
  } else if (cr3) {
    regs.rip = (uintptr_t)&gk_launch_cr3;
    regs.rax = (uintptr_t)fn;
  } else {
    regs.rip = (uintptr_t)fn;
  }
  if (cr3) {
    atomic_fetch_add_explicit(&G->root_switches, 1, memory_order_relaxed);
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d: root switch to %#llx (%s)\n", t->id,
              (unsigned long long)cr3, t->active_arena ? "arena" : "base");
  }
  regs_set(t, &regs);
  t->user_mode = user;
  return run_vcpu(t);
}

long gk_run(long (*fn)(void *), void *arg) {
  gk_thread *t = thread_get();
  uint64_t cr3;
  if (!t || vcpu_init(t, GK_VCPU_STACK) < 0) return -1;
  if (prepare_entry(t, &cr3) < 0) return -1;
  return enter_guest(t, fn, arg, t->stack_top, 0, cr3);
}

// Like gk_run, but fn runs at guest ring 3 (see enter_guest). Same private
// guest stack, same fault/return reporting.
long gk_run_user(long (*fn)(void *), void *arg) {
  gk_thread *t = thread_get();
  uint64_t cr3;
  if (!t || vcpu_init(t, GK_VCPU_STACK) < 0) return -1;
  if (prepare_entry(t, &cr3) < 0) return -1;
  return enter_guest(t, fn, arg, t->stack_top, 1, cr3);
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
// arriving then is handled on the side stack, where the thread is), so a
// red-zone's worth of gap guards the switch itself. The rest of the slack is
// a whole page, so that the host frames above caller_sp and the guest's own
// stack never share a page: the host-frame wall below is page-granular.
#define GK_HERE_SLACK (4096 + 128)
typedef struct { gk_thread *t; long (*fn)(void *); void *arg; int user; uint64_t cr3; } gk_here_ctx;

// ---- the host-frame wall --------------------------------------------------
// A ring-3 fn on the caller's stack sits directly below the host frames it
// will eventually return into: gk_host_call_on_stack's saved rbp and return
// address at caller_sp, and above them run_here_common, gk_run_here_user and
// every caller up to the thread's start. Those frames are ordinary user pages,
// so arbitrary ring-3 code could overwrite a return address there and, once
// the turn ends cleanly and the host frames unwind, run at host privilege.
//
// For the length of a ring-3 turn, the caller's stack from the first page
// boundary above the guest's entry rsp up to the top of the stack (ro_hi) is
// therefore read-only to the guest: demand_map maps pages in the window
// without write permission, and a write fault inside it is genuine (see
// run_vcpu). Reads keep working, which a conservative GC scan that walks the
// whole thread stack needs. Legitimate turn code writes only below its entry
// frame; a store above it is a wall violation and ends the turn with
// GK_EFAULT.
//
// The wall is read-only, not no-read: the host frames stay readable, so a
// ring-3 escape can still read the return addresses there (an ASLR infoleak,
// though no longer a write/control primitive). Making them unreadable too
// (mapping the window supervisor) would fault the conservative GC scan, which
// walks the whole thread stack, unless that scan were first bounded to the
// guest's entry frame. V8 has the hook for that -- a per-turn
// v8::StackStartMarker (jsg.h, guarded by V8_HAS_STACK_START_MARKER) that sets
// the isolate's stack-scan start -- but it depends on an internal Cloudflare
// V8 patch that is not in the OSS tree. The low-level setter it needs
// (base::Stack::SetStackStart) does exist here, so the patch itself is small;
// the real work is confirming nothing else (V8's main-heap scan, the unwinder,
// stack-trace capture, the profiler, glibc) reads above the bound during a
// live turn. We could upgrade to a no-read wall once that patch is available;
// until then the wall stays read-only.
//
// The window is per vCPU: it walls the turn's own writes. Another thread's
// vCPU maps the same shared base subtree with its own (empty) window, so its
// writes to this thread's frames are not walled; that is the cross-thread
// residual of the shared-runtime model, not this wall's job.

// The thread pointer (the fs base): a pthread's TCB, which glibc places at the
// top of the thread's stack mapping, with its static TLS block just below.
static uintptr_t thread_pointer(void) {
  uintptr_t tp;
  __asm__ __volatile__("mov %%fs:0, %0" : "=r"(tp));
  return tp;
}

// Compute the wall for a turn entered at rsp `top` from a host switch point
// at caller_sp: [*lo, *hi) is page-aligned and empty if nothing can be walled.
// *hi is the end of the host mapping holding the stack, clamped below the
// TCB and static TLS when they sit on that mapping (a pthread; the main
// thread's live in ld.so's own mapping): the turn writes TLS and glibc writes
// the TCB, so those must stay writable. Runs on the host before the turn.
//
// The mapping lookup reads /proc/self/maps, which is long in a process with
// many mappings and lists the stack last, so its result is cached on the
// thread record and reused while the switch point stays inside the same
// mapping (a thread's stack top and TLS never move; a switch point elsewhere,
// say on a fiber stack, is looked up afresh).
static int stack_wall_bounds(gk_thread *t, uintptr_t caller_sp, uintptr_t top,
                             uintptr_t *lo, uintptr_t *hi) {
  *lo = *hi = 0;
  if (caller_sp < t->wall_map_lo || caller_sp >= t->wall_map_hi) {
    uintptr_t rs, re; int perms;
    if (!host_region(caller_sp & ~0xfffUL, &rs, &re, &perms)) return -1;
    uintptr_t end = re;
    uintptr_t tp = thread_pointer();
    if (tp > caller_sp && tp < re) {
      if (!G->tls_static_info) {
        end = 0;  // cannot place the TLS: no wall (warned at init)
      } else {
        size_t size = 0, align = 0;
        G->tls_static_info(&size, &align);
        uintptr_t tls_lo = (tp - size) & ~0xfffUL;
        if (tls_lo < end) end = tls_lo;
      }
    }
    t->wall_map_lo = rs;
    t->wall_map_hi = re;
    t->wall_end = end;
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d: stack mapping [%#lx,%#lx) tp=%#lx caller_sp=%#lx -> wall end %#lx\n",
              t->id, (unsigned long)rs, (unsigned long)re, (unsigned long)tp,
              (unsigned long)caller_sp, (unsigned long)end);
  }
  uintptr_t first = (top + 0xfffUL) & ~0xfffUL;
  if (first >= t->wall_end) return 0;
  *lo = first;
  *hi = t->wall_end;
  return 0;
}

// Raise the wall for this thread's turn: record the window and withhold write
// permission on every writable PTE any root holds for it, tagging each one
// PTE_WALL (pte_range_all: the base root and any arena root sharing the range,
// the same set a host decommit is reflected into). The PTEs stay present, so
// the few caller-stack pages a turn touches keep their translations from turn
// to turn; dropping them instead would make each one re-fault every turn, and
// every demand fault reads /proc/self/maps, which is long in this process.
// Pages in the window that are not mapped yet fault in without W through
// demand_map. This vCPU's TLB may still hold writable translations of the
// window from an earlier turn, so it is flushed too, unless that is provably
// unnecessary (below); no other vCPU's translations are touched, as the wall
// is this vCPU's alone (see above).
//
// The flush (a KVM_RUN of the flush stub, and the cold-TLB page walks the next
// turn then starts with) is a large share of a turn's fixed cost, and on the
// steady-state path it flushes nothing that matters: the turns of an event
// loop enter at the same call depth, one after another, with nothing else
// running the guest in between. Only a writable translation of a page in the
// new window needs flushing, and the TLB can hold one only if the vCPU ran
// while the page's PTE carried W. So the flush is skipped when both hold:
//
//  1. the vCPU has not run since the previous wall came down (tlb_touched is
//     clear: no KVM_RUN and no host-side CR3 write since wall_lower). The
//     window's PTEs have carried W again since then, but nothing could have
//     loaded a translation through them: the TLB holds exactly what the
//     previous turn left, and that turn ran under its own wall, under which
//     every page of [prev_ro_lo, prev_ro_hi) was read-only or absent in the
//     TLB (its raise flushed, or was itself skipped on the same argument);
//  2. the new window lies within the previous one: prev_ro_hi == hi (the same
//     stack end) and lo >= prev_ro_lo. Every page of [lo, hi) was then a
//     read-only-or-absent page of the previous turn, and a translation the
//     next run loads for it comes from the PTE, which withholds W again.
//
// Either condition failing means a writable translation of a window page may
// exist, and the flush is done as before: the vCPU ran (a ring-0 turn on this
// stack, a syscall retry, anything that entered the guest) and could have
// written through the lowered PTEs; or the new window starts lower than the
// previous one and takes in pages that were the previous turn's writable
// working stack. A vCPU with no previous wall to reason from (fresh or
// reused, see vcpu_init; a turn that had no wall; a failed flush) flushes.
// The counts are in gk_stats (wall_flushed, wall_skipped).
static int wall_raise(gk_thread *t, uintptr_t lo, uintptr_t hi) {
  if (lo >= hi) {
    // No wall this turn: the guest may write through the whole caller stack,
    // so the next wall cannot treat this turn's window as its predecessor.
    t->prev_ro_lo = t->prev_ro_hi = 0;
    return 0;
  }
  int skip = !t->tlb_touched && t->prev_ro_hi != 0 && hi == t->prev_ro_hi && lo >= t->prev_ro_lo;
  pthread_mutex_lock(&G->lock);
  t->ro_lo = lo;
  t->ro_hi = hi;
  pte_range_all(lo, hi, PTE_OP_WALL_RAISE, NULL);
  if (skip) G->wall_skipped++; else G->wall_flushed++;
  pthread_mutex_unlock(&G->lock);
  if (G->dbg)
    fprintf(stderr, "[gk] vcpu %d: wall [%#lx,%#lx) raised; previous [%#lx,%#lx), TLB %s since "
            "lowered -> %s\n", t->id, (unsigned long)lo, (unsigned long)hi,
            (unsigned long)t->prev_ro_lo, (unsigned long)t->prev_ro_hi,
            t->tlb_touched ? "touched" : "untouched", skip ? "flush skipped" : "flush");
  t->prev_ro_lo = lo;
  t->prev_ro_hi = hi;
  if (skip) return 0;
  if (flush_run(t) < 0) {
    t->prev_ro_lo = t->prev_ro_hi = 0;  // TLB state unknown: the next wall flushes
    return -1;
  }
  return 0;
}

// Lower the wall after the turn: give write permission back to every PTE the
// wall withheld it from (those tagged PTE_WALL, by wall_raise or by demand_map
// during the turn) and clear the window. A page the host allows no write on
// was never tagged and stays as it is, so the window's PTEs again mirror the
// host protection. This vCPU's TLB is not flushed: its read-only translations
// of the window are what the next ring-3 turn wants anyway (wall_raise
// flushes before the window can differ, or proves that it cannot), and a
// ring-0 run in between that writes through one takes a spurious #PF, which
// the fault path resolves by re-mapping against the now-writable PTE. From
// here until the vCPU next runs, its TLB is known (tlb_touched clear), unless
// the walk finds a window page writable without the wall's tag: the wall
// withheld W from every writable page when it went up, so such a page was
// mapped or reprotected writable during the turn by another vCPU (the
// cross-thread residual, see run_here) and this vCPU may hold a writable
// translation of it. The TLB then counts as touched, so the next wall_raise
// flushes rather than reasons from this turn.
static void wall_lower(gk_thread *t) {
  if (t->ro_lo >= t->ro_hi) return;
  pthread_mutex_lock(&G->lock);
  long foreign = pte_range_all(t->ro_lo, t->ro_hi, PTE_OP_WALL_LOWER, NULL);
  t->ro_lo = t->ro_hi = 0;
  pthread_mutex_unlock(&G->lock);
  if (foreign == 0) {
    t->tlb_touched = 0;
  } else if (G->dbg) {
    fprintf(stderr, "[gk] vcpu %d: wall lowered over %ld page(s) another vCPU mapped writable; "
            "the next wall flushes\n", t->id, foreign);
  }
}

static long run_here(void *ctx, unsigned long caller_sp) {
  gk_here_ctx *c = ctx;
  uint64_t top = (caller_sp - GK_HERE_SLACK) & ~0xfULL;
  if (!c->user) return enter_guest(c->t, c->fn, c->arg, top, 0, c->cr3);
  uintptr_t lo, hi;
  if (stack_wall_bounds(c->t, caller_sp, top, &lo, &hi) < 0) {
    G->err = "gk_run_here_user: caller stack not in /proc/self/maps";
    return -1;
  }
  if (wall_raise(c->t, lo, hi) < 0) {
    wall_lower(c->t);
    return -1;
  }
  long r = enter_guest(c->t, c->fn, c->arg, top, 1, c->cr3);
  wall_lower(c->t);
  return r;
}

static long run_here_common(long (*fn)(void *), void *arg, int user) {
  gk_thread *t = thread_get();
  if (!t || vcpu_init(t, 0) < 0) return -1;
  if (!t->side_stack) {
    // The gk-loop side stack is host-only: refuse it so ring-3 code cannot
    // reach the host frames running underneath it. It is mapped PROT_NONE
    // until registered, so no vCPU can fault it in as user memory in between.
    void *ss = mmap(NULL, GK_SIDE_STACK, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (ss == MAP_FAILED) { G->err = "mmap side stack"; return -1; }
    pthread_mutex_lock(&G->lock);
    prot_add((uintptr_t)ss, (uintptr_t)ss + GK_SIDE_STACK, GK_PROT_REFUSE);
    pthread_mutex_unlock(&G->lock);
    if (mprotect(ss, GK_SIDE_STACK, PROT_READ | PROT_WRITE) != 0) {
      G->err = "mprotect side stack";
      return -1;
    }
    t->side_stack = ss;
  }
  uint64_t cr3;
  if (prepare_entry(t, &cr3) < 0) return -1;
  gk_here_ctx c = {t, fn, arg, user, cr3};  // lives above caller_sp, out of the guest's way
  return gk_host_call_on_stack((char *)t->side_stack + GK_SIDE_STACK, run_here, &c);
}

long gk_run_here(long (*fn)(void *), void *arg) {
  return run_here_common(fn, arg, 0);
}

// Like gk_run_here, but fn runs at guest ring 3 (see enter_guest).
long gk_run_here_user(long (*fn)(void *), void *arg) {
  return run_here_common(fn, arg, 1);
}

// ---- host-backing faults ----------------------------------------------------
// KVM_RUN fails with EFAULT when the guest accesses a page whose PTE is in
// place but whose host backing is gone: the host side replaced or decommitted
// the mapping (a fixed mmap or mprotect to PROT_NONE, an munmap) without the
// change passing through a forwarded syscall, so the PTE was never dropped
// (see unmap_range_all). KVM cannot back the guest-physical page, and the run
// stops on the faulting instruction with the vCPU at whatever privilege the
// guest had, not in a handler. The access was an EPT violation, not a guest
// #PF, so guest CR2 still holds the previous demand fault's address, and this
// kernel's kvm_run carries no address for it either.
//
// Inside an arena the fault is turned into the guest #PF it would have been
// had the change been reflected: every PTE of the arena is dropped from the
// arena root (a late version of the one-page drop a reflected decommit does),
// the vCPU's TLB is flushed from the host, and the instruction is restarted.
// Its access now demand-faults with CR2 exact, and demand_map applies the
// host's current protection: a decommitted page is refused and the turn ends
// with GK_EFAULT at that address through the path every genuine fault takes; a
// page the host merely downgraded is remapped with what it has, and a write to
// it then fails through the fault-repeat limit; a page the host has meanwhile
// recommitted is mapped and the turn simply goes on. Another vCPU under the
// same arena root demand-faults its pages back. The retries are bounded per
// invocation in case the host keeps pulling pages from under the guest.
// Returns 1 to retry KVM_RUN, 0 to give the fault up (host_backing_fail).
#define GK_BACKING_RETRIES 3
static int host_backing_retry(gk_thread *t) {
  gk_arena *a = t->active_arena;
  if (!a || t->backing_retries >= GK_BACKING_RETRIES) return 0;
  t->backing_retries++;
  pthread_mutex_lock(&G->lock);
  unmap_range_root(a->pml4, a->base, a->end);
  pthread_mutex_unlock(&G->lock);
  // A CR3 change through KVM makes it flush the whole guest TLB (see the
  // global-pages section), and the stopped vCPU may be at ring 3, where it
  // cannot run the flush stub: swing CR3 through the base root and back to the
  // arena's, two loads that must both take effect (so not through kvm_run,
  // which would keep only the last). Should the second fail, the vCPU is left
  // on the base root, which the next entry's sync_cr3 reads back and corrects.
  struct kvm_sregs s;
  sregs_get(t, &s);
  s.cr3 = (uint64_t)(uintptr_t)G->pml4;
  if (sregs_set_now(t, &s) < 0) return 0;
  s.cr3 = (uint64_t)(uintptr_t)a->pml4;
  if (sregs_set_now(t, &s) < 0) return 0;
  if (G->dbg) {  // fault path: raw output only
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": host backing gone under an arena PTE; retrying as a guest fault (");
    db_dec(&d, t->backing_retries); db_str(&d, ")");
    db_flush(&d);
  }
  return 1;
}

// A KVM_RUN failure that is not retried (no arena is active, so there is no
// bounded set of PTEs to drop; the retries are used up; or errno is not
// EFAULT). Reports EFAULT as GK_EFAULT like any other fault; the address is
// the faulting instruction's, the closest thing to the access's address KVM
// leaves behind (a kernel that fills kvm_run.memory_fault supplies the page
// itself). Other errnos stay -1: they are not guest faults.
//
// Either way the vCPU is then put back into the state a turn's end leaves it
// in, so the next entry on this thread runs a normal turn: ring 0, since the
// stubs and handlers run privileged (run_stub's code page is supervisor, so a
// vCPU left at ring 3 faults on its first instruction there), and no event
// pending, since a fault taken during exception delivery would be requeued
// and delivered at the next entry. The registers are set afresh by every
// entry.
static long host_backing_fail(gk_thread *t, int err) {
  struct kvm_regs r; struct kvm_sregs s;
  regs_get(t, &r);
  sregs_get(t, &s);
  uint64_t addr = r.rip;
  int exact = 0;
#ifdef KVM_EXIT_MEMORY_FAULT
  if (t->run->exit_reason == KVM_EXIT_MEMORY_FAULT) {
    addr = t->run->memory_fault.gpa;  // guest-physical == host-virtual
    exact = 1;
  }
#endif
  if (G->dbg) {  // fault path: raw output only
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": KVM_RUN failed, errno "); db_dec(&d, err);
    if (err == EFAULT)
      db_str(&d, exact ? ": host backing gone at " : ": host backing gone under an access at rip ");
    else
      db_str(&d, ": rip ");
    db_hex(&d, addr);
    db_str(&d, " (cr2 "); db_hex(&d, s.cr2); db_str(&d, " is the previous demand fault)");
    db_flush(&d);
  }
  sregs_ring0(&s);
  sregs_set_now(t, &s);
  struct kvm_vcpu_events ev = {0};
  ioctl(t->fd, KVM_SET_VCPU_EVENTS, &ev);
  if (err != EFAULT) return -1;
  G->fault_addr = addr;
  return GK_EFAULT;
}

// Run this thread's vCPU from its current register state until the guest exits
// (via gk_exit_tramp) or faults.
static long run_vcpu(gk_thread *t) {
  for (;;) {
    t->tlb_touched = 1;  // the vCPU runs: its TLB is no longer known (see wall_raise)
    if (ioctl(t->fd, KVM_RUN, 0) < 0) {
      int err = errno;
      if (err == EINTR) continue;
      if (err == EFAULT && host_backing_retry(t)) continue;
      return host_backing_fail(t, err);
    }
    switch (t->run->exit_reason) {
      case KVM_EXIT_IO: {
        if (t->run->io.direction != KVM_EXIT_IO_OUT) break;
        uint16_t port = t->run->io.port;
        if (port == PORT_SYSCALL) {
          struct kvm_regs r;
          regs_get(t, &r);
          // A ring-3 guest leaves through the sentinel exit syscall (ring 3
          // cannot use PORT_EXIT's OUT): its result is in RDI.
          if (r.rax == GK_EXIT_SYSCALL) return (long)r.rdi;
          r.rax = (uint64_t)forward_syscall(t, &r);
          // A ring-3 guest's syscall trampoline ran in ring 0; return it to
          // ring 3 with SYSRET rather than the ring-0 jmp. forward_syscall left
          // RCX/R11 (the SYSCALL-saved return RIP and flags) intact and may
          // have already pointed RIP at a resume label; override it to the
          // ring-3 one. Ring-0 guests keep RIP at the OUT so KVM completes it.
          if (t->user_mode) r.rip = (uintptr_t)&gk_user_syscall_resume;
          regs_set(t, &r);
        } else if (port == PORT_EXIT) {
          struct kvm_regs r;
          regs_get(t, &r);
          return (long)r.rax;
        } else if (port == PORT_UDRIP) {
          struct kvm_regs r;
          regs_get(t, &r);
          G->fault_addr = r.rax;
          if (G->dbg) fprintf(stderr, "[gk] #UD at rip=%#llx\n", (unsigned long long)r.rax);
          return GK_EFAULT;
        } else if (port == PORT_DEMAND) {
          struct kvm_sregs s;
          sregs_get(t, &s);
          uint64_t cr2 = s.cr2;
          int repeat = (cr2 == t->last_fault);
          if (repeat) {
            if (++t->fault_repeat > 2) { G->fault_addr = cr2; return GK_EFAULT; }
          } else { t->last_fault = cr2; t->fault_repeat = 0; }
          // A protection-key violation is the guest CPU enforcing the page's
          // key against this vCPU's PKRU: a genuine fault, unless the entry
          // this vCPU used carried the page's previous key (another vCPU
          // changed it without a shootdown). The fault invalidated that entry,
          // so one retry against the current PTE tells the two apart.
          struct kvm_regs pr;
          regs_get(t, &pr);
          // The #PF frame on the IST stack: [err, rip, cs, rflags, rsp].
          const uint64_t *pf_frame = (const uint64_t *)(uintptr_t)pr.rsp;
          uint64_t pf_err = pf_frame[0], pf_sp = pf_frame[4];
          int pk = (pf_err & PF_ERR_PK) != 0;
          if (pk && repeat) {
            if (G->dbg) {
              gk_dbuf d = {.n = 0};
              db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
              db_str(&d, ": protection-key violation at "); db_hex(&d, cr2);
              db_str(&d, " (err="); db_hex(&d, pf_err); db_str(&d, ")");
              db_flush(&d);
            }
            G->fault_addr = cr2;
            return GK_EFAULT;
          }
          // A write into this turn's host-frame wall (see run_here) is the wall
          // holding: genuine, whatever the host protection of the page. Reads
          // there fall through and are mapped without write permission.
          if ((pf_err & PF_ERR_WR) && cr2 >= t->ro_lo && cr2 < t->ro_hi) {
            if (G->dbg) {
              gk_dbuf d = {.n = 0};
              db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
              db_str(&d, ": write into the host-frame wall at "); db_hex(&d, cr2);
              db_str(&d, " (wall ["); db_hex(&d, t->ro_lo); db_str(&d, ", ");
              db_hex(&d, t->ro_hi); db_str(&d, "), err="); db_hex(&d, pf_err);
              db_str(&d, ", rip="); db_hex(&d, pf_frame[1]);
              db_str(&d, ", rsp="); db_hex(&d, pf_sp); db_str(&d, ")");
              db_flush(&d);
              // Best-effort frame-pointer walk of the faulting code's stack.
              uint64_t bp = pr.rbp, lo = pf_sp, hi = pf_sp + (8UL << 20);
              for (int i = 0; i < 24 && bp >= lo && bp + 16 <= hi; i++) {
                uint64_t *f = (uint64_t *)(uintptr_t)bp;
                db_str(&d, "[gk]   frame "); db_dec(&d, i);
                db_str(&d, ": bp="); db_hex(&d, bp);
                db_str(&d, " ret="); db_hex(&d, f[1]);
                db_flush(&d);
                if (f[0] <= bp) break;
                bp = f[0];
              }
            }
            G->fault_addr = cr2;
            return GK_EFAULT;
          }
          GK_HANDLER_ENTER();
          int dm = demand_map(t, (uintptr_t)cr2, (uintptr_t)pf_sp);
          GK_HANDLER_LEAVE();
          if (dm < 0) {
            if (G->dbg) {  // still the fault path: raw output only
              struct kvm_regs rr;
              regs_get(t, &rr);
              // The #PF frame on the guest stack: [err, rip, cs, rflags, rsp].
              uint64_t *frame = (uint64_t *)(uintptr_t)rr.rsp;
              gk_dbuf d = {.n = 0};
              db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
              db_str(&d, ": unmappable fault cr2="); db_hex(&d, cr2);
              db_str(&d, " rip="); db_hex(&d, frame[1]);
              db_str(&d, " err="); db_hex(&d, frame[0]);
              db_str(&d, " rsp="); db_hex(&d, frame[4]);
              db_str(&d, " rax="); db_hex(&d, rr.rax);
              db_flush(&d);
              // Best-effort frame-pointer walk of the guest stack (host memory,
              // identity mapped); stops when the chain leaves the stack.
              uint64_t bp = rr.rbp, lo = frame[4], hi = frame[4] + (8UL << 20);
              for (int i = 0; i < 16 && bp >= lo && bp + 16 <= hi; i++) {
                uint64_t *f = (uint64_t *)(uintptr_t)bp;
                db_str(&d, "[gk]   frame "); db_dec(&d, i);
                db_str(&d, ": ret="); db_hex(&d, f[1]);
                db_flush(&d);
                if (f[0] <= bp) break;
                bp = f[0];
              }
            }
            G->fault_addr = cr2;
            return GK_EFAULT;
          }
          // The retry re-walks the page tables: a page fault invalidates the
          // TLB entry it was taken through, so no explicit flush is needed.
        } else if (port == PORT_FAULT) {
          struct kvm_regs rr; struct kvm_sregs sr;
          regs_get(t, &rr);
          sregs_get(t, &sr);
          if (G->dbg) {
            gk_dbuf d = {.n = 0};
            db_str(&d, "[gk] fatal exception, handler_rax="); db_hex(&d, rr.rax);
            db_str(&d, " cr2="); db_hex(&d, sr.cr2);
            db_flush(&d);
          }
          G->fault_addr = rr.rax;
          return GK_EFAULT;
        }
        break;
      }
      case KVM_EXIT_HLT:
        return 0;
      case KVM_EXIT_SHUTDOWN:
        return GK_ESHUTDOWN;
      default:
        if (G->dbg) fprintf(stderr, "[gk] unexpected exit reason=%u\n", t->run->exit_reason);
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
// Slots are taken from the lowest run of nslots that no live arena owns and
// that has never held a global PTE (see the global-pages section), so a
// destroyed arena's slots serve later arenas: under isolate churn the slot
// index would otherwise run off the end of the user address space.
gk_arena *gk_arena_create(size_t size) {
  size = (size + 0xfff) & ~0xfffUL;
  if (size == 0) return NULL;
  int nslots = (int)((size + GK_SLOT_BYTES - 1) >> GK_SLOT_BITS);
  if (nslots > GK_MAX_ARENA_SLOTS) return NULL;
  size_t span = (size_t)nslots << GK_SLOT_BITS;
  pthread_mutex_lock(&G->lock);
  // The struct comes from the control block's pool: its root pointer and slot
  // entries are the arena's walls, so it must be as unreachable to ring 3 as
  // the page tables themselves.
  gk_arena *a = G->arena_free;
  if (a) G->arena_free = a->next_free;
  else if (G->arena_pool_n < GK_MAX_ARENAS) a = &G->arena_pool[G->arena_pool_n++];
  if (!a) { pthread_mutex_unlock(&G->lock); return NULL; }
  memset(a, 0, sizeof *a);
  void *mem = MAP_FAILED;
  int idx;
  for (idx = GK_FIRST_CAGE_SLOT; idx + nslots <= GK_USER_SLOTS; idx++) {
    int free = 1;
    for (int i = 0; i < nslots; i++)
      if (G->slot_used[idx + i] || G->slot_pinned[idx + i]) { free = 0; break; }
    if (!free) continue;
    uintptr_t va = (uintptr_t)idx << GK_SLOT_BITS;
    mem = mmap((void *)va, span, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem != MAP_FAILED && (uintptr_t)mem == va) break;
    if (mem != MAP_FAILED) { munmap(mem, span); mem = MAP_FAILED; }  // old kernel: hint only
  }
  if (mem == MAP_FAILED || G->arena_n >= GK_MAX_ARENAS) goto fail;
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
  unmap_range_root(G->pml4, a->base, a->end);
  arena_sync_root(a);  // share the base root's other entries, own these slots
  for (int i = 0; i < nslots; i++) G->slot_used[idx + i] = 1;
  G->arenas[G->arena_n++] = a;
  pthread_mutex_unlock(&G->lock);
  return a;
fail:
  for (int i = 0; i < GK_MAX_ARENA_SLOTS; i++)
    if (a->slot_entry[i]) free_table((uint64_t *)(uintptr_t)(a->slot_entry[i] & ~0xfffULL));
  if (a->pml4) free_table(a->pml4);
  a->next_free = G->arena_free;
  G->arena_free = a;
  pthread_mutex_unlock(&G->lock);
  if (mem != MAP_FAILED) munmap(mem, span);
  return NULL;
}

void *gk_arena_base(const gk_arena *a) { return a ? (void *)a->base : NULL; }
size_t gk_arena_size(const gk_arena *a) { return a ? a->size : 0; }

gk_arena *gk_arena_enter(gk_arena *a) {
  gk_thread *t = thread_get();
  if (!t) return NULL;  // record pool exhausted: the thread stays on the base root
  return arena_enter(t, a);
}

// Teardown reconciles gk and KVM with the reservation going away, in this
// order, under G->lock:
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
  gk_thread *t = thread_cur();
  if (t && t->active_arena == a) arena_enter(t, NULL);
  pthread_mutex_lock(&G->lock);
  for (int i = 0; i < G->arena_n; i++)
    if (G->arenas[i] == a) { G->arenas[i] = G->arenas[--G->arena_n]; break; }
  if (pkey_set_range(a->base, a->end, 0) < 0 && G->dbg)
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
    __atomic_fetch_add(&G->root_gen, 1, __ATOMIC_RELEASE);
  }
  unmap_range_root(G->pml4, a->base, a->end);
  int removed = region_remove_range(a->base, a->end);
  uintptr_t base = a->base, end = a->end;
  if (in_use == 0) {
    for (int i = 0; i < a->nslots; i++) G->slot_used[a->slot0 + i] = 0;
    a->next_free = G->arena_free;  // the struct is the pool's again
    G->arena_free = a;
  }
  pthread_mutex_unlock(&G->lock);
  if (G->dbg)
    fprintf(stderr, "[gk] destroyed arena [%#lx,%#lx): %d memslots deleted, %ld tables in use\n",
            (unsigned long)base, (unsigned long)end, removed, G->pt_used);
  munmap((void *)base, end - base);
}

void gk_get_stats(gk_stats *s) {
  pthread_mutex_lock(&G->lock);
  s->memslots = G->region_live;
  s->memslot_ids = G->next_slot;
  s->arenas = G->arena_n;
  s->pt_pages_used = G->pt_used;
  s->pt_pages_free = G->pt_free_n;
  s->pt_pages_total = (long)((G->pt_next - (uint8_t *)G->pt_base) >> 12);
  s->prot_ranges = G->prot_n;
  s->demand_faults = G->demand_ok;
  s->wall_flushed = G->wall_flushed;
  s->wall_skipped = G->wall_skipped;
  s->global_pages = G->global_pages;
  s->neighbor_pages = G->neighbor_pages;
  s->root_switches = atomic_load_explicit(&G->root_switches, memory_order_relaxed);
  s->tlb_flushes = atomic_load_explicit(&G->tlb_flushes, memory_order_relaxed);
  s->flush_invpcid = G->flush_stub == gk_flush_stub_invpcid;
  pthread_mutex_unlock(&G->lock);
}

// Test/diagnostic hook (see gk.h): drop the PTE of the page holding `addr`
// from every root, without a flush. Equivalent to the reflection of a munmap of
// that page minus the calling vCPU's flush, so any translation a vCPU holds
// keeps working until something flushes it.
void gk_debug_drop_pte(unsigned long addr) {
  uintptr_t page = (uintptr_t)addr & ~0xfffUL;
  pthread_mutex_lock(&G->lock);
  unmap_range_all(page, page + 0x1000);
  pthread_mutex_unlock(&G->lock);
}

// Test/diagnostic hook (see gk.h): register or unregister a page-aligned range
// of the caller's own memory in the supervisor/refuse registry, so a test can
// place a SUPER or REFUSE page wherever it wants one. Registering drops any
// PTE a root holds for the range, as gk's own registrations do.
void gk_debug_protect_range(unsigned long addr, size_t len, int kind) {
  uintptr_t s = (uintptr_t)addr & ~0xfffUL, e = ((uintptr_t)addr + len + 0xfffUL) & ~0xfffUL;
  if (s >= e) return;
  pthread_mutex_lock(&G->lock);
  if (kind == GK_DEBUG_PROT_SUPER) prot_add(s, e, GK_PROT_SUPER);
  else if (kind == GK_DEBUG_PROT_REFUSE) prot_add(s, e, GK_PROT_REFUSE);
  else prot_remove(s, e);
  pthread_mutex_unlock(&G->lock);
}
