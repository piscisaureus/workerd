// gk internals: the state and the interface the parts of gk share. gk is one
// translation unit (gk.c includes every part), so all of this is static.
#ifndef GK_INTERNAL_H
#define GK_INTERNAL_H
#define _GNU_SOURCE
#include "gk.h"
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <sys/uio.h>
#include <unistd.h>

// The hypervisor: KVM on Linux, Hypervisor.framework (HVF) on macOS, which is
// arm64 only. gk_hvf.c says what HVF does differently.
#if defined(__APPLE__)
#define GK_HVF 1
#include <Hypervisor/Hypervisor.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0  // placement by hint only (see gk_arena_create)
#endif
#ifndef MAP_STACK
#define MAP_STACK 0
#endif
#else
#define GK_HVF 0
#include <link.h>
#include <linux/futex.h>
#include <linux/kvm.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#if defined(__aarch64__)
#include <elf.h>
#include <sys/auxv.h>
#endif
#endif

#if defined(__x86_64__)
#include "gk_x86_64.h"
#elif defined(__aarch64__)
#include "gk_arm64.h"
#else
#error "gk supports x86-64 and arm64 only"
#endif

#define GK_MAP_WALLED 8          // map4k_root flag: host-writable page, W withheld (PTE_WALL)
#define GK_MAP_GLOBAL 16         // map4k_root flag: the page may be global (see the global-pages section)

// Reserved syscall number a ring-3 guest issues to leave the guest (see
// gk_user_exit_tramp in gk_asm.S). Ring-3 code cannot execute OUT, so it cannot
// use the PORT_EXIT hypercall the ring-0 path uses; it returns its result
// through this sentinel syscall instead. Well above any real Linux syscall
// number, so it never collides with a forwarded call. Keep in sync with the
// literal in gk_asm.S.
#define GK_EXIT_SYSCALL 0xF4240
// Reserved syscall number for a hostcall (see gk_hostcall in gk.h): the
// handler index is in RDI. Recognized at the syscall exit next to the exit
// sentinel, before the filter and before any forwarding.
#define GK_HOSTCALL_SYSCALL 0xF4241
// Reserved syscall number for a ring-0 resolve (see gk_resolve in gk.h): the
// handle is in RDI. Answered by the syscall trampoline itself, inside the
// guest (gk_resolve_tramp in gk_asm.S), so it never reaches this file's
// syscall exit. Keep in sync with the literal in gk_asm.S.
#define GK_RESOLVE_SYSCALL 0xF4242
// The ring-3 code selector SYSRET loads (STAR[63:48] + 16, see vcpu_init and
// the GDT layout in gk_init): the only CS a guest runs with at ring 3, and so
// the sign that a caller is at guest ring 3 (see gk_resolve).
#define GK_USER_CS 0x2b

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
#define GK_EPERM 1
#define GK_ESRCH 3
#define GK_E2BIG 7
#define GK_ENOMEM 12
#define GK_EADDR 14   // EFAULT: GK_EFAULT is gk_run's guest-fault status
#define GK_EBUSY 16
#define GK_EINVAL 22
#define GK_ENOSYS 38
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
#if defined(__x86_64__)
#define GK_IST_BYTES (64UL << 10)   // per-vCPU exception stack, TSS in its last page
#else
#define GK_IST_BYTES (4UL << 10)    // per-vCPU vcb (struct gk_vcb)
#endif
#define GK_SIDE_STACK (512UL << 10) // host-side stack for gk_run_here's gk loop
typedef struct {
#if GK_HVF
  int id;               // the vCPU itself is destroyed with its thread (see vcpu_park)
#else
  int fd, id;
  struct kvm_run *run;
#endif
  uint64_t stack_top;   // 0 if the vCPU has no private guest stack
  uint64_t ist;         // its exception stack + TSS region (see vcpu_init)
  void *side_stack;     // NULL if the vCPU has no gk_run_here side stack
  struct gk_runner *runner;  // the thread that runs its KVM_RUN, if any (see the runner section)
  struct gk_park *park; // its park frame, if it is resident (see the runner section)
  int efd;              // the eventfd its park loop and runner wake the owner through, or -1
  int stats_fd;         // KVM_GET_STATS_FD, taken at creation (see gk_debug_vcpu_stat), or -1
} gk_parked_vcpu;

// Per-thread guest state: each host thread that enters the guest is a vCPU. A
// thread that only selects an arena (gk_arena_enter) has a record too, without
// a vCPU until its first run. Records are allocated from G->threads and
// recycled when the thread ends (see thread_get, thread_release).
typedef struct gk_thread {
  long tid;                // kernel tid of the owning thread (0: free record)
  int inited;              // the thread has a vCPU
  int id;
#if GK_HVF
  hv_vcpu_t vcpu;          // this thread's HVF vCPU
  hv_vcpu_exit_t *exit;    // its exit record, written by hv_vcpu_run
#else
  int fd;
  struct kvm_run *run;
#endif
  uint64_t stack_top;
  uint64_t ist;            // exception stack + TSS region of this vCPU
  void *side_stack;        // host stack for gk_run_here's gk loop (see there)
  struct gk_runner *runner;  // the thread that runs this vCPU's KVM_RUN, or NULL (see the runner section)
  // A resident vCPU (see the runner section): its park frame, whether it is
  // inside KVM_RUN in the park loop right now, the sequence word of the turn
  // the owner last handed it and of the KVM_RUN it last handed the runner,
  // the root the host knows it holds while parked (sync_cr3), the eventfd
  // the owner sleeps on, and the vCPU's KVM stats descriptor.
  struct gk_park *park;
  int parked;
  uint64_t turn_seq;
  uint32_t run_seq;
  uint64_t cr3_shadow;
  int efd;
  int stats_fd;
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
  // The arenas that gk_arena_enter displaced on this thread, innermost last:
  // gk_arena_leave returns to the last of them. The history lives here, in
  // supervisor memory, so that the arena a thread returns to is never taken
  // from a value the caller holds (which guest code may have rewritten). Every
  // entry was a live arena when it was pushed, and gk_arena_destroy clears a
  // destroyed arena from every thread's history. Guarded by G->lock.
  gk_arena *arena_stack[GK_ARENA_NEST];
  int arena_depth;
  // Set for a thread the guest created via clone (see clone_thread): it runs
  // its gk loop on this private host stack and leaves only through the guest's
  // exit syscall.
  int guest_thread;
  void *host_stack;
  size_t host_stack_size;
  // A guest-created thread's initial state, filled by its parent (clone_thread)
  // and consumed by the child (child_entry) before it enters the guest.
  struct {
    gk_regs regs;
    uint32_t pkru;         // the parent's guest PKRU, inherited like a real clone
    int user;              // the parent ran at ring 3, so the child starts there too
    int parent_id;
  } start;
  // This record's host-side hostcall frame (see the hostcalls section): a
  // block of host memory allocated when the record is first taken from the
  // pool and kept across its recycling, so the pool's records own at most
  // GK_MAX_VCPUS frames between them.
  gk_hostcall_frame *hostcall_frame;
  // Thread words (see gk_thread_words_set in gk.h): the embedder's per-thread
  // state that a signal handler or host code must read from memory the guest
  // cannot write. Zeroed when the record is taken from the pool.
  uint64_t words[GK_THREAD_WORDS];
  // The turn directory (see gk_confine_paths in gk.h): the descriptor a
  // forwarded path syscall from this thread may resolve beneath, with the
  // identity of the directory it named when it was set, so a descriptor the
  // guest has since closed and reused names nothing. turn_dir_set is cleared
  // at every guest entry, so a turn starts with no directory.
  // Whether the embedder's hostcall handler is running on this thread (see
  // gk_in_handler in gk.h). Here, in supervisor memory, and not in a
  // thread-local: a turn that could clear it would put an allocation back
  // inside a handler, which is the thing the flag exists to keep out.
  int in_hostcall;
  int turn_dir;
  int turn_dir_set;
  uint64_t turn_dir_dev, turn_dir_ino;
  struct gk_thread *next_free;
} gk_thread;
// 1 + the index of this thread's record in G->threads; 0 while it has none.
// User memory: validated by thread_cur before every use.
static __thread int tls_rec;
// Whether the thread is inside the guest, set by the host side around every
// guest entry, and the thread's hostcall exchange region. Both are the guest's
// own copies, in user memory, read by gk_hostcall and gk_hostcall_region and
// trusted by nothing on the host (see the hostcalls section).
static __thread int tls_in_guest;
static __thread gk_hostcall_frame *tls_hostcall_region;

#if defined(__aarch64__)
// Whether the thread is inside the hypervisor's run call, raised and lowered
// around that call alone (run_vcpu), so that host code running on the thread
// between two runs -- a hostcall handler, a forwarded syscall -- sees it
// clear. The guest's own copy, in user memory, like tls_in_guest.
static __thread int tls_in_run;
#endif

// Whether the caller runs in the guest at ring 3. On x86-64 that is the CS
// selector, which nothing in memory decides: at guest ring 3 it is the one
// SYSRET loads (GK_USER_CS), at guest ring 0 it is 0x08 and on the host it is
// the kernel's user selector. arm64 has no unprivileged read of the exception
// level (CurrentEL is undefined at EL0 and on the host), so there the guest is
// told from the host by tls_in_run, and a ring-0 caller takes the ring-3 path
// too: an svc the host answers at the hypercall exit, one VM exit more than
// the direct read x86-64 ring 0 makes. A rewrite of tls_in_run by ring 3 costs
// the rewriter's own thread the direct read's fault, or a host thread an svc
// the kernel refuses with ENOSYS, which the callers treat as a refusal: the
// same exposure gk_hostcall has through tls_in_guest on both architectures.
static inline int at_guest_ring3(void) {
#if defined(__x86_64__)
  unsigned short cs;
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
  return cs == GK_USER_CS;
#else
  return tls_in_run;
#endif
}

// A spin-wait hint for the CPU.
static inline void cpu_relax(void) {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#else
  __asm__ __volatile__("yield");
#endif
}

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

// One report site's budget (see gk_log_budget in gk.h). Twelve bytes, because
// every arena carries GK_LOG_SITES of them: the window is a coarse monotonic
// second, which fits 32 bits for 136 years of uptime.
typedef struct {
  _Atomic uint32_t window;      // the coarse monotonic second it is counting in
  _Atomic uint32_t allowed;     // reports let through in that second
  _Atomic uint32_t suppressed;  // reports held back since the last one let through
} gk_log_bucket;

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
  // Set while the arena is in G->arenas, from gk_arena_create to
  // gk_arena_destroy; gk_arena_enter accepts no arena without it (see
  // arena_live_locked). Guarded by G->lock.
  int registered;
  // G->pml4_gen as of the last arena_sync_root of this root (see sync_cr3).
  // Written under G->lock, read without it.
  unsigned synced_gen;
  // The arena's signal fields (see the signal-fields section): the index + 1
  // of the first entry in G->signal_fields, 0 for none; the entries link on
  // from there. Written under G->lock by the thread that has the arena
  // active, read without it by that thread's signal handler.
  int field_head;
  // The arena's ring-0 resolve entries (see the resolve section): the index + 1
  // of the first entry of gk_resolve_table that belongs to this arena, 0 for
  // none, with the rest linked both ways from there through the entries' chain
  // words, and how many there are. The head and the count are here, in the
  // control block, which is GK_PROT_SUPER, as the table they describe is: a
  // ring-3 access to either faults. Only the links are in the table. gk_arena_destroy retires whatever is left
  // before the arena's root returns to the page-table allocator, since the next
  // arena is handed that same root (see gk_resolve in gk.h). Guarded by G->lock.
  int resolve_head;
  int resolve_live;
  // What each report site may still say about this arena (see gk_log_budget
  // in gk.h). Per arena and not per process because the budget is reachable
  // from a turn -- which can ask for a verdict it has no report to make --
  // and a shared bucket is one tenant's to spend on every other tenant's
  // behalf. Zeroed with the rest of the struct when it is taken from the
  // pool, so a recycled arena starts with a budget of its own.
  gk_log_bucket log_budget[GK_LOG_SITES];
  gk_arena *next_free;        // free-list link while the struct is unused
};

// Host memory (see gk_host_alloc in gk.h): regions mapped by the host,
// registered GK_PROT_REFUSE once each, and suballocated by power-of-two size
// class. A region is never unmapped or unregistered, so the registry holds at
// most GK_MAX_HOST_REGIONS entries for them, whatever the allocation pattern.
#define GK_HOST_REGION_BYTES (4UL << 20)
#define GK_MAX_HOST_REGIONS 64
#define GK_HOST_MIN_SHIFT 4   // the smallest block, 16 bytes: room for a free-list link
#define GK_HOST_MAX_SHIFT 20  // the largest block, GK_HOST_MAX_ALLOC
#define GK_HOST_CLASSES (GK_HOST_MAX_SHIFT + 1)
typedef struct { uintptr_t start, end, bump; } gk_host_region;
_Static_assert((1UL << GK_HOST_MAX_SHIFT) == GK_HOST_MAX_ALLOC, "gk.h and gk.c disagree");
_Static_assert(GK_HOST_MAX_ALLOC <= GK_HOST_REGION_BYTES, "a block must fit a region");

// A host slot (see gk_host_slot_alloc in gk.h): one host-memory block, named
// by a handle. A handle is the slot's index in its low 16 bits and the
// generation the slot was allocated with above them, so a handle is validated
// against the table alone: the index bounds-checks against GK_HOST_SLOTS, and
// the generation must match the slot's. Generations come from one counter
// that every allocation advances, so a reused slot never repeats a handle.
#define GK_HOST_SLOT_INDEX_BITS 16
#define GK_HOST_SLOT_GEN_BITS (64 - GK_HOST_SLOT_INDEX_BITS)
typedef struct {
  void *ptr;        // the block, NULL while the slot is free
  size_t size;      // the size it was allocated with (selects the class on free)
  uint64_t gen;     // the generation in the slot's current handle; 0 never issued
  int next_free;    // free-list link, as index + 1; 0 ends the list
  int live;
} gk_host_slot;
_Static_assert(GK_HOST_SLOTS <= (1 << GK_HOST_SLOT_INDEX_BITS), "a slot index must fit a handle");

// A hostcall handler table entry (see gk_hostcall_register in gk.h). Empty
// while fn is NULL. The exchange regions are carved from one reservation of
// GK_MAX_VCPUS regions, indexed by thread record, so a record's region is a
// fixed address the host computes from the record and the guest can neither
// move (the mapping policy refuses the whole reservation) nor choose.
typedef struct { gk_hostcall_fn fn; void *ctx; } gk_hostcall_entry;
#define GK_HOSTCALL_REGION_BYTES (8UL << 10)
#define GK_HOSTCALL_AREA_BYTES ((size_t)GK_MAX_VCPUS * GK_HOSTCALL_REGION_BYTES)
_Static_assert(sizeof(gk_hostcall_frame) <= GK_HOSTCALL_REGION_BYTES, "a frame must fit its region");
_Static_assert(sizeof(gk_hostcall_frame) <= GK_HOST_MAX_ALLOC, "a frame must fit a host block");

// A signal field (see gk_signal_field_register in gk.h): one 4-byte word in
// an arena that the host writes from a signal handler. Its handle has the
// same shape as a host slot's (index in the low 16 bits, generation above).
// `live` is the reader's gate: stored last with release when the entry is
// published, cleared first when it is retired, so a lock-free walk that sees
// it set sees the rest of the entry. `next` links the arena's list and is
// left in place when the entry is retired, so a walk standing on a retired
// entry still finds the rest of the list; `next_free` links the free list.
typedef struct {
  uintptr_t addr;    // absolute address of the field
  gk_arena *arena;   // the arena it lies in
  uint64_t gen;      // the generation in the entry's current handle
  uint32_t kind;
  int live;
  int next;          // per-arena list link, as index + 1; 0 ends the list
  int next_free;     // free-list link, as index + 1; 0 ends the list
} gk_signal_field;
_Static_assert(GK_SIGNAL_FIELDS <= (1 << GK_HOST_SLOT_INDEX_BITS), "a field index must fit a handle");

// The ring-0 resolve table (see gk_resolve in gk.h): GK_RESOLVE_SLOTS entries
// of {pointer, generation, owner}, read at guest ring 0 by gk_resolve_tramp
// (gk_asm.S) and on the host by gk_resolve, written only by the host under
// G->lock. It is a separate page-aligned object rather than a member of the
// control block so that the trampoline can address it RIP-relative by symbol;
// gk_init registers it GK_PROT_SUPER and maps it into the base root, which
// every arena root shares, before the first guest entry, so the lookup never
// takes a demand fault. An entry is live while its generation is nonzero; a
// handle's generation is never 0. The order of the stores is what the
// lock-free reader relies on (see gk_resolve_tramp): publish stores the owner
// and the pointer, then the generation; a commit into a reserved entry stores
// the owner and then the pointer, which is the word the reader gates on;
// retire stores a zero generation, then a NULL pointer, and leaves the owner
// for the next registration to overwrite. A free entry's pointer word holds
// the free-list link (the next free index + 1, 0 ending the list) rather than
// a pointer: the reader never returns the word of an entry whose generation
// is 0, so the list costs no space outside the table, which matters at this
// capacity. The counters are in the control block.
//
// The owner is the page-table root of the arena the entry was registered or
// committed under -- the base root for a registration made with no arena
// active -- and the trampoline compares it against CR3, the root the calling
// turn is running under, so that one isolate's handle resolves to nothing at
// another isolate's ring 3 (see gk_resolve in gk.h). It is an address of gk's
// own, never one the caller supplies, and a reserved entry's is 0, which is no
// root at all; the host side does not compare it, since host code resolves on
// behalf of whichever context it is serving and is the party the tag protects.
//
// The fourth word is the chain word, which the trampoline never reads: the
// arena an entry belongs to and the entry's neighbours in that arena's list,
// so that gk_arena_destroy can retire an arena's entries without walking the
// table (see gk_resolve in gk.h and the arena's resolve_head). All three are
// 0 for an entry the base root owns, which is on no list because the base
// root is never destroyed, and for a reserved entry, which joins its owner's
// list when it is committed.
//
// The fifth is the type word, which the caller gives at registration and the
// lookup hands back with the pointer (see the type word in gk.h). gk does not
// interpret it; 0 is "none" by convention between registrant and resolver.
// It has a word of its own rather than bits borrowed from another so that
// the trampoline can return it with one load and no masking, and so that the
// chain, which the host rewrites while ring 3 may be reading, shares no word
// with something a reader compares.
//
// An entry is therefore 64 bytes, up from 32: the five words need 40, the
// trampoline indexes by a shift, and an entry inside one cache line is what
// makes the lookup one line. The cost is the capacity's reservation, 64 MiB
// rather than 32, untouched until an entry is written, and twice the page
// tables at init (16384 PTEs rather than 8192, in the base root every arena
// shares). An entry in use costs 64 resident bytes rather than 32, so the
// measured working set of 200000 live entries is 12.8 MiB rather than 6.4.
// The three words after the type are unused.
//
// A handle is the entry's index in its low GK_RESOLVE_INDEX_BITS bits and the
// generation above them. The capacity is exactly 2^GK_RESOLVE_INDEX_BITS, so
// masking the index bounds it and every 64-bit value decomposes into one
// (generation, index) pair; the trampoline relies on both.
typedef struct {
  void *ptr;
  uint64_t gen;
  uint64_t owner;
  uint64_t aidx : 13, prev : 21, next : 21;  // the chain word, as index + 1 each
  uint64_t type;
  uint64_t unused[3];
} gk_resolve_entry;
_Static_assert(sizeof(gk_resolve_entry) == 64, "gk_resolve_tramp indexes 64-byte entries");
_Static_assert(offsetof(gk_resolve_entry, owner) == 16, "gk_resolve_tramp reads the owner at 16");
_Static_assert(offsetof(gk_resolve_entry, type) == 32, "gk_resolve_tramp reads the type at 32");
_Static_assert(GK_RESOLVE_INDEX_BITS == 20, "gk_resolve_tramp splits a handle at bit 20");
_Static_assert(GK_RESOLVE_SLOTS == (1 << GK_RESOLVE_INDEX_BITS), "gk_resolve_tramp bounds the index by masking");
_Static_assert(GK_MAX_ARENAS < (1 << 13), "an arena's pool index must fit the chain word");
_Static_assert(GK_RESOLVE_SLOTS < (1 << 21), "an entry index must fit the chain word");
#define GK_RESOLVE_GEN_BITS (64 - GK_RESOLVE_INDEX_BITS)
__attribute__((visibility("hidden"), aligned(4096))) union {
  gk_resolve_entry e[GK_RESOLVE_SLOTS];
  char pad[(sizeof(gk_resolve_entry) * GK_RESOLVE_SLOTS + 0xfff) & ~(size_t)0xfff];
} gk_resolve_table;
#define GK_RESOLVE_START ((uintptr_t)&gk_resolve_table)
#define GK_RESOLVE_END (GK_RESOLVE_START + sizeof gk_resolve_table)

// The supervisor/refuse registry (see prot_class in gk_pt.c). An unregistered page
// is user-accessible, so the registry must never be too small to hold every
// range gk wants to protect: prot_add is fatal when it is full. The capacity
// covers the worst case, which is 4 ranges per vCPU (kvm_run and the IST/TSS
// region, which stay registered while the vCPU is pooled, plus a gk_run_here
// side stack and a guest thread's host stack, which are different classes and
// so cannot be coalesced), one range per host-memory region (see the
// host-memory section), the fixed init ranges (control block, page-table
// area, GDT/IDT, handler text), and headroom.
#define GK_PROT_KEEP 0
#define GK_PROT_SUPER 1
#define GK_PROT_REFUSE 2
#define GK_MAX_PROT_RANGES (4 * GK_MAX_VCPUS + GK_MAX_HOST_REGIONS + 1024)
typedef struct { uintptr_t start, end; int kind; } gk_prot_range;
_Static_assert(GK_MAX_PROT_RANGES >= 2 * GK_MAX_VCPUS + GK_MAX_HOST_REGIONS + 512,
               "registry must hold every vCPU's permanent ranges with headroom");

// A memslot-backed window (see the MMU layer in gk_region.c).
#define GK_BACK_WIN (2UL << 20)   // memslot backing granularity (2MB, aligned)
// The neighborhood a demand fault maps around its page (see demand_map_neighbors).
#define GK_NEIGHBOR_WIN (256UL << 10)  // 64 pages, aligned; within one GK_BACK_WIN

// ---- page sizes -------------------------------------------------------------
// The guest translates with a 4KiB granule on every host. GK_GUEST_PAGE is the
// unit of every PTE and so of everything decided per PTE: the supervisor/refuse
// registry, the protection-key ranges, the host-frame wall, demand paging; the
// 0xfff and >> 12 arithmetic of the page-table code is this granule. The host
// kernel's page size is the unit of everything the kernel does: mappings and
// their protections, memslots, the bounds /proc/self/maps reports, so the
// reflection of a guest's mprotect or munmap and the rounding of an arena's
// size follow it. On x86-64 the two coincide (always 4KiB) and GK_HOST_PAGE is
// that constant; an arm64 kernel may use 16KiB or 64KiB pages, so there it is
// read at gk_init. The 4KiB granule needs the CPU's support
// (ID_AA64MMFR0_EL1.TGran4), checked when the first vCPU is created.
#define GK_GUEST_PAGE 4096UL
#if defined(__x86_64__)
#define GK_HOST_PAGE 4096UL
#else
#define GK_HOST_PAGE ((uintptr_t)G->host_page)
#endif
#define GUEST_FLOOR(a) ((a) & ~(GK_GUEST_PAGE - 1))
#define GUEST_CEIL(a) (((a) + GK_GUEST_PAGE - 1) & ~(GK_GUEST_PAGE - 1))
#define HOST_FLOOR(a) ((a) & ~(GK_HOST_PAGE - 1))
#define HOST_CEIL(a) (((a) + GK_HOST_PAGE - 1) & ~(GK_HOST_PAGE - 1))
#define GK_MAX_REGIONS 65536
typedef struct gk_region {
  uintptr_t start, end;   // [start, end), GK_BACK_WIN-aligned
  int slot;               // KVM memslot id backing this region
#if defined(__aarch64__)
  uint64_t gpa;           // guest-physical address of `start` (see the guest-physical allocator)
#endif
#if GK_HVF
  long s2_n;              // how many of its host pages are in stage 2 (see the HVF section)
#endif
  unsigned prio;          // treap heap priority
  struct gk_region *l, *r;
} gk_region;

// A range holding a nonzero protection key (see the protection-keys section).
#define GK_MAX_PKEY_RANGES 4096
typedef struct { uintptr_t start, end; int pkey; } gk_pkey_range;

// A page-aligned [start, end) span of the runtime image (see the
// mapping-change policy section). The table holds the PT_LOAD segments of
// every object the dynamic loader has mapped when gk_init runs, plus the main
// thread's stack, after image_compact has merged the spans that touch.
//
// The bound is on spans rather than on segments: image_add compacts when the
// table fills and fails only if that frees nothing, so what has to fit is the
// set of ranges the address space actually holds, which does not depend on
// how the binary was linked. The shipping workerd maps 6 objects with 21
// PT_LOAD segments, which compact to 6 ranges; a Bazel cc_test of the runtime,
// which links each library as its own shared object, maps 158 objects with 629
// segments, which compact to 41 -- the loader places an object's segments next
// to each other, and whole objects next to each other, so nearly all of them
// merge. 1024 spans (16 KiB of the control block) is twenty times the larger
// of those.
#ifdef GK_NEG_IMAGE_LIMIT
// GK_NEG_IMAGE_LIMIT (a negative-test build, never shipped) shrinks the table
// below what any real binary needs, so that the overflow path runs.
#define GK_MAX_IMAGE_RANGES 4
#else
#define GK_MAX_IMAGE_RANGES 1024
#endif
typedef struct { uintptr_t start, end; } gk_span;

#define GK_CPUID_ENTRIES 128
#if defined(__aarch64__)
typedef struct { uint64_t start, size; } gk_gpa_extent;
#endif
#if GK_HVF
// A stage-2 shadow (see the HVF section): host page `hva` is backed in the
// guest by the copy at `copy`.
typedef struct { uintptr_t hva; void *copy; } gk_shadow;
#define GK_MAX_SHADOWS 8192   // the shadow pool: this many host pages
#define GK_COMMPAGE_N 2
#endif

struct gk_ctl {
  // Platform state, set once by gk_init.
#if !GK_HVF
  int kvm, vmfd;
#endif
  uint64_t *pml4;                    // base page-table root
  uint8_t *pt_next, *pt_end;         // bump allocator over the page-table area
  uintptr_t pt_base;
  size_t pt_bytes;
  uint64_t *pt_free;                 // freed page-table pages, linked through their first word
  long pt_used, pt_free_n;           // pages handed out and not returned; pages on the free list
#if defined(__x86_64__)
  uint64_t gdt_va, idt_va;
#else
  uint64_t vbar;                     // the vector table: the supervisor text's first page
  uint64_t pt_gpa;                   // guest-physical address of pt_base
  int ips;                           // TCR_EL1.IPS for the VM's guest-physical size
#if !GK_HVF
  struct kvm_vcpu_init vcpu_target;  // KVM_ARM_PREFERRED_TARGET, for every vCPU
#endif
  // The guest-physical allocator (see there). Guarded by G->lock.
  uint64_t gpa_next, gpa_limit;
  gk_gpa_extent gpa_free[GK_MAX_REGIONS];
  int gpa_free_n;
  size_t pthread_size;               // glibc's struct pthread (see stack_wall_bounds), 0 if unknown
  // The vDSO and the kernel's data pages behind it (see the vDSO section).
  uintptr_t vdso_lo, vdso_hi;
  uintptr_t vdso_fn[64];             // the vDSO's function entry points
  int vdso_n;
  gk_prot_range holes[8];            // host ranges no memslot may cover ([vvar])
  int hole_n;
  size_t host_page;                  // the kernel's page size (see the page-sizes section)
#if GK_HVF
  // The shadow pool and the commpage shadows in it (see the HVF section).
  uintptr_t shadow_pool;
  int shadow_used;
  gk_shadow commpage[GK_COMMPAGE_N];
  int commpage_n;
  uint64_t *s2_map;                  // one bit per guest-physical host page: in stage 2
  long s2_pages, s2_shadowed;        // host pages in stage 2: all, and those that are shadows
#endif
#endif
  int next_slot;                     // KVM memslot ids handed out, ever (see the region pool)
  int run_size;
#if defined(__x86_64__)
  int sync_regs;                     // registers go through kvm_run, not ioctls (see regs_get)
  uint64_t xcr0;                     // the XCR0 every vCPU starts with (see xcr0_select)
  // Host CPUID, applied to every vCPU: a struct kvm_cpuid2 (a flexible-array
  // struct, hence the raw buffer) with room for GK_CPUID_ENTRIES entries.
  _Alignas(struct kvm_cpuid2) unsigned char cpuid_buf[sizeof(struct kvm_cpuid2) +
                                                      GK_CPUID_ENTRIES *
                                                          sizeof(struct kvm_cpuid_entry2)];
#endif
  const char *err;
  unsigned long fault_addr;
  int dbg;
  int thp;                 // arena host backing may use transparent huge pages (see arena_thp_advise)
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
#if defined(__x86_64__)
  void (*flush_stub)(void);         // the flush stub for this CPU (see flush_stub_select)
#endif
  // vCPU runners (see the runner section): whether new vCPUs get one, how
  // long a waiting side spins before it sleeps, where the runner is pinned,
  // the pool of runner records and the count of KVM_RUNs they have executed.
  int runner_mode;
  long runner_spin;
  int runner_pin;                   // GK_RUNNER_PIN_NONE, _SIBLING or a CPU number + GK_RUNNER_PIN_CPU
  struct gk_runner *runner_pool;    // GK_MAX_VCPUS records, in supervisor memory (see gk_init)
  int runner_n;
  atomic_long runner_runs;
  // Resident vCPUs (see the runner section): whether new vCPUs are resident,
  // the flush kind their park loop runs (1: INVPCID, 2: the CR4.PGE toggle,
  // as flush_stub_select chose), how many vCPUs are resident, how many turns
  // ended in a park loop and how many halted vCPUs were kicked.
  int resident_mode;
  int resident_n;
  long park_flush_kind;
  atomic_long park_turns, park_kicks;
  // Bumped whenever page-table pages are returned to the allocator (an arena
  // was destroyed). A vCPU whose root is unchanged since it last entered the
  // guest may still hold TLB and paging-structure-cache entries derived from
  // tables that have since been freed and reused, so it flushes at its next
  // entry when it sees a new generation (see sync_cr3).
  unsigned root_gen;
  // Bumped whenever the base root gains a top-level (PML4) entry (next_table),
  // the one change to the base root an arena root has to pick up: its other
  // shared entries are the base root's own tables, and a leaf change inside
  // them is seen by every root at once. An arena root whose synced_gen equals
  // this needs no arena_sync_root at entry (see sync_cr3). Base PML4 entries
  // are never removed, so the generation only ever moves forward.
  unsigned pml4_gen;

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
  // Which unowned slots gk still holds a PROT_NONE reservation over: a
  // destroyed arena's, kept so that no other mapping can take them (see
  // gk_arena_create).
  unsigned char slot_held[GK_USER_SLOTS];
  // Which PML4 slots have ever held a global PTE. No arena is ever placed over
  // one (see the global-pages section).
  unsigned char slot_pinned[GK_USER_SLOTS];

  // The supervisor/refuse registry: disjoint ranges sorted by start. Guarded
  // by G->lock.
  gk_prot_range prot[GK_MAX_PROT_RANGES];
  int prot_n;

  // Host memory (see the host-memory section): the regions, the bump pointer
  // of the newest one, and a free list per size class, linked through the
  // first word of each free block. Guarded by G->lock.
  gk_host_region host_regions[GK_MAX_HOST_REGIONS];
  int host_region_n;
  void *host_free[GK_HOST_CLASSES];
  long host_blocks;                  // blocks handed out and not yet freed

  // Host slots (see the host-slots section). The table is here, in supervisor
  // memory, so no ring-3 access reaches an entry. Guarded by G->lock.
  gk_host_slot host_slots[GK_HOST_SLOTS];
  int host_slot_n;                   // slots ever taken from the table
  int host_slot_free;                // free-list head, as index + 1; 0 when empty
  int host_slot_live;                // slots allocated and not yet freed
  uint64_t host_slot_gen;            // the generation of the last allocation

  // Hostcalls (see the hostcalls section): the handler table, in supervisor
  // memory so no ring-3 access reaches an entry, and the exchange-region
  // reservation. Guarded by G->lock; hostcall_calls is a counter.
  gk_hostcall_entry hostcalls[GK_HOSTCALLS];
  int hostcall_n;                    // entries with a handler
  atomic_long hostcall_calls;        // calls that reached a handler
  uintptr_t hostcall_area;           // GK_HOSTCALL_AREA_BYTES, PROT_NONE until a region is committed

  // Signal fields (see the signal-fields section). The table is here, in
  // supervisor memory, so no ring-3 access reaches an entry. Mutated under
  // G->lock; walked without it by gk_signal_field_write (see there).
  gk_signal_field signal_fields[GK_SIGNAL_FIELDS];
  int signal_field_n;                // entries ever taken from the table
  int signal_field_free;             // free-list head, as index + 1; 0 when empty
  int signal_field_live;             // fields registered and not yet unregistered
  uint64_t signal_field_gen;         // the generation of the last registration

  // The ring-0 resolve table's bookkeeping (see the resolve section): the
  // table itself is gk_resolve_table, outside the block, and the free list is
  // threaded through its free entries. Guarded by G->lock.
  int resolve_n;                     // entries ever taken from the table
  int resolve_free;                  // free-list head, as index + 1; 0 when empty
  int resolve_live;                  // entries registered and not yet unregistered
  uint64_t resolve_gen;              // the generation of the last registration
  long resolve_orphans;              // entries retired by gk_arena_destroy, their arena having died first

  // The turn-directory policy (see gk_confine_paths in gk.h): when set,
  // forward_syscall confines every thread's path syscalls to its turn
  // directory. path_refusals counts the syscalls the policy refused.
  int confine_paths;
  atomic_long path_refusals;

  // Set at the end of a successful gk_init (see gk_initialized).
  int initialized;

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

  // The runtime image: the loaded segments of the executable and of every
  // shared object present at gk_init, and the main thread's stack (see the
  // mapping-change policy section). Written once by gk_init, read-only
  // afterwards.
  gk_span image[GK_MAX_IMAGE_RANGES];
  int image_n;
  // Set when image_add found the table full, so that gk_init can tell an
  // overflow from the other reasons recording the image fails.
  int image_full;

  // What each report site may still say outside any arena -- on the event
  // loop, in a teardown, before the first isolate exists (see gk_log_budget
  // in gk.h). A thread with an arena active counts in the arena's buckets
  // instead. No ring-3 code can reach this one at all: reaching the budget
  // takes a thread with an arena, which is what a turn is.
  gk_log_bucket log_budget[GK_LOG_SITES];

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
#if defined(__x86_64__)
#define GK_CPUID ((struct kvm_cpuid2 *)G->cpuid_buf)
#endif

#if !GK_HVF
static void arena_thp_advise(uintptr_t s, uintptr_t e);  // see the arenas section
#endif
#if defined(__aarch64__)
static struct gk_region *region_find(uintptr_t addr);  // see the MMU layer
#endif

// Trampolines and exception handlers (gk_asm.S), in this binary's mapped text.
#if defined(__x86_64__)
extern void gk_syscall_tramp(void);
extern void gk_syscall_tramp_resume(void);
extern void gk_resolve_tramp(void);
extern void gk_user_syscall_resume(void);
extern void gk_user_launch(void);
extern void gk_launch_cr3(void);
extern void gk_user_launch_cr3(void);
extern void gk_user_child_launch(void);
extern void gk_exit_tramp(void);
extern void gk_user_exit_tramp(void);
extern void gk_park_entry(void), gk_park_exit_tramp(void), gk_park_syscall_tramp(void),
    gk_park_exit(void), gk_park_loop(void), gk_exc_nmi(void);
extern void gk_exc_de(void), gk_exc_ud(void), gk_exc_df(void), gk_exc_gp(void),
    gk_exc_pf(void);
extern void gk_pkru_stub(void);
extern void gk_flush_stub(void);
extern void gk_flush_stub_invpcid(void);
extern const unsigned char gk_invpcid_desc[16];
#else
extern void gk_user_exit_tramp(void);
extern void gk_dispatch(void);
extern const unsigned char gk_super_start[], gk_super_end[], gk_vectors[];
#endif
#if !GK_HVF
// Host-side helpers for guest thread creation (gk_asm.S); never run in the guest.
extern long gk_host_clone_raw(long nr, long a1, long a2, long a3, long a4,
                              long a5);
extern void gk_host_unmapself_exit(void *addr, size_t len, long code)
    __attribute__((noreturn));
#endif
extern long gk_host_call_on_stack(void *stack_top,
                                  long (*f)(void *ctx, unsigned long caller_sp),
                                  void *ctx);
typedef struct { char b[512]; size_t n; } gk_dbuf;
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

// Allocation guard for the paths a handler runs on (off by default). Neither the
// fault handler nor a registered hostcall handler may call malloc (see the
// region-pool note and "What a handler is"): the guest thread may be mid-malloc
// with the allocator's lock held, and that lock is not reentrant. Building with
// -DGK_GUARD_HANDLER_ALLOC and -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc
// makes any allocation between GK_HANDLER_ENTER/LEAVE abort loudly rather than
// silently corrupt glibc's arena, so a stray malloc/fopen on these paths is
// caught deterministically at any backing granularity. The brackets are around
// the fault path, the signal-time field write and hostcall_dispatch's call to
// the registered handler, so the check reaches the embedder's handler and
// everything it calls, not only gk's own code.
//
// For the runtime, whose handlers are the ones worth checking, the define and
// the wrap come from one flag on this library (see BUILD.bazel), so that the
// wrap reaches the binaries that link gk and no others:
//
//   bazel build -c opt --platform_suffix=_guard \
//     --//src/workerd/experimental/guest-kernel:handler_alloc_guard=True \
//     --//src/workerd/server:use_tcmalloc=False \
//     //src/workerd/server:workerd
//
// tcmalloc has to be off for that: it defines operator new without going through
// malloc, so --wrap would see none of the C++ allocations, which are the ones a
// handler is liable to make. What the wrap still misses is an allocation that
// reaches the kernel without libc (a direct mmap), one made on another thread
// that the handler then waits on, and a lock other than the allocator's.
//
// Shipping builds compile the macros to nothing and never wrap malloc.
//
// The brackets nest. A bracketed entry point can be reached from inside another
// -- gk_resolve_typed from a hostcall handler is the common one -- and a
// signal-time write can interrupt either. So ENTER yields what the flag held
// and LEAVE puts that back, rather than clearing it: a LEAVE that cleared left
// everything the outer handler did after its first inner call unguarded, which
// is most of what a hostcall handler does.
#ifdef GK_GUARD_HANDLER_ALLOC
static __thread int g_in_handler;
static inline int gk_handler_enter(void) {
  int was = g_in_handler;
  g_in_handler = 1;
  return was;
}
#define GK_HANDLER_ENTER() gk_handler_enter()
#define GK_HANDLER_LEAVE(saved) (g_in_handler = (saved))
#else
#define GK_HANDLER_ENTER() 0
#define GK_HANDLER_LEAVE(saved) ((void)(saved))
#endif

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

#define GK_BACKING_RETRIES 3

// ---- the parts' functions ----------------------------------------------------
// gk_util.c
static void write_stderr(const char *s);
static void gk_fatal(const char *what) __attribute__((noreturn));
static void db_str(gk_dbuf *d, const char *s);
static void db_hex(gk_dbuf *d, uint64_t v);
static void db_dec(gk_dbuf *d, long v);
static void db_flush(gk_dbuf *d);

#if GK_HVF
// gk_hvf.c
static int host_stack_grow(uintptr_t page);
static uintptr_t thread_pointer(void);
static uintptr_t thread_data_floor(uintptr_t tp);
static void turn_prepare(void);
static void wall_probe(void);
static int vm_create(void);
static int memslot_add(int slot, uint64_t gpa, uintptr_t s, uintptr_t e);
static int memslot_remove(gk_region *n, int *keep_gpa);
static void vcpu_detach(gk_thread *t);
static void vcpu_park_hw(gk_thread *t, gk_parked_vcpu *p);
static gk_thread *thread_ending(void *p);
static long host_syscall(long nr, long a1, long a2, long a3, long a4, long a5,
                         long a6);
static int copy_in(void *dst, uintptr_t src, size_t n);
static int s2_ensure(gk_region *rg, uintptr_t hva);
static int s2_release(gk_region *rg);
static void commpage_refresh(void);
static int host_region(uintptr_t page, uintptr_t *rs, uintptr_t *re, int *perms);
static long gettid_raw(void);
static void stack_refresh(void);
static int vcpu_set_sys(hv_vcpu_t v, hv_sys_reg_t id, uint64_t val);
static int vcpu_set_reg(hv_vcpu_t v, hv_reg_t id, uint64_t val);
static long forward_syscall(gk_thread *t, gk_regs *r);
static long run_vcpu(gk_thread *t);
#endif

#if !GK_HVF
// gk_kvm.c
static void turn_prepare(void);
static int kvm_open(void);
static int memslot_add(int slot, uint64_t gpa, uintptr_t s, uintptr_t e);
static int memslot_remove(gk_region *n, int *keep_gpa);
static void vcpu_detach(gk_thread *t);
static void vcpu_park_hw(gk_thread *t, gk_parked_vcpu *p);
static void stack_refresh(void);
#endif

#if !GK_HVF
// gk_linux.c
static int host_stack_grow(uintptr_t page);
static void arena_thp_advise(uintptr_t s, uintptr_t e);
static gk_thread *thread_ending(void *p);
static long host_syscall(long nr, long a1, long a2, long a3, long a4, long a5,
                         long a6);
static int copy_in(void *dst, uintptr_t src, size_t n);
static int host_region(uintptr_t page, uintptr_t *rs, uintptr_t *re, int *perms);
static long gettid_raw(void);
static long child_entry(void *arg);
static long forward_syscall(gk_thread *t, gk_regs *r);
#endif

// gk_pt.c
static int prot_class(uintptr_t addr);
static gk_hostcall_frame *hostcall_region_of(const gk_thread *t);
static uint64_t *alloc_table(void);
static void free_table(uint64_t *p);
static void free_subtree(uint64_t *tbl, int level);
static void prot_add(uintptr_t s, uintptr_t e, int kind);
static void prot_remove(uintptr_t s, uintptr_t e);
static int map4k_root(uint64_t *root, uint64_t va, uint64_t flags, int pkey);
static uint64_t pte_lookup(const uint64_t *root, uintptr_t va);
static long pte_range_all(uintptr_t s, uintptr_t e, int op, const gk_reprot *rp);
static void unmap_range_all(uintptr_t s, uintptr_t e);
static void reprotect_range_all(gk_thread *t, uintptr_t s, uintptr_t e, int prot);
static void unmap_range_root(uint64_t *root, uintptr_t s, uintptr_t e);
static int mmu_map_range(uintptr_t s, uintptr_t e, int perms);
// Why demand_map refused a fault, for the report the fault path writes (see
// run_vcpu). `reason` is a literal; the bounds are the arena the page lies in
// and the arena the faulting turn runs under, both zero unless the reason
// names them.
typedef struct {
  const char *reason;
  uintptr_t page_base, page_end, turn_base, turn_end;
} gk_dm_refusal;
static int demand_map(gk_thread *t, uintptr_t addr, uintptr_t sp, gk_dm_refusal *why);

// gk_region.c
static struct gk_region *region_find(uintptr_t addr);
static gk_region *region_succ(uintptr_t s);
static int region_add(uintptr_t s, uintptr_t e);
static int region_remove(gk_region *n, int keep_gpa);
static int region_remove_range(uintptr_t s, uintptr_t e, int keep_gpa);
static int region_ensure(uintptr_t s, uintptr_t e);

// gk_pkey.c
static int pkey_lookup(uintptr_t addr);
static int pkey_room(void);
static int pkey_set_range(uintptr_t s, uintptr_t e, int pkey);

// gk_thread.c
static gk_thread *thread_cur(void);
static void park_leave(gk_thread *t);
static void runner_stop(struct gk_runner *r);
static int arena_live_locked(const gk_arena *a);
static gk_thread *thread_alloc_locked(long tid);
static void thread_bind(gk_thread *t, int host);
static gk_thread *thread_get(void);
static void vcpu_park(gk_thread *t);
static void thread_release(gk_thread *t);
static void thread_key_dtor(void *p);

#if defined(__x86_64__)
// gk_x86_64.c
static uintptr_t thread_pointer(void);
static uintptr_t thread_data_floor(uintptr_t tp);
static int flush_uses_invpcid(void);
static uintptr_t super_page(void);
static void wall_probe(void);
static int page_size_init(void);
static int vm_create(void);
static int pt_arena_init(void);
static int guest_tables_init(void);
static int gpa_assign(uintptr_t s, uintptr_t e, uint64_t *gpa);
static void gpa_unassign(uintptr_t s, uintptr_t e, uint64_t gpa);
static void gpa_retire(gk_region *n);
static void region_set_gpa(gk_region *n, uint64_t gpa);
static inline uint64_t region_gpa(const gk_region *n);
static int region_clip_holes(uintptr_t *s, uintptr_t *gap_end);
static int pte_leaf(uint64_t va, uint64_t flags, int pkey, int super, uint64_t *out);
static uint64_t pte_reprotect(uint64_t e, const gk_reprot *rp, uintptr_t v);
static void set_idt_gate(uint8_t *idt, int vec, uint64_t h);
static int vcpu_init(gk_thread *t, int flags);
static int map_handler_text(void);
static void flush_stub_select(void);
static void regs_get(gk_thread *t, struct kvm_regs *r);
static void regs_set(gk_thread *t, const struct kvm_regs *r);
static int flush_run(gk_thread *t);
static void flush_tlb(gk_thread *t, struct kvm_regs *r);
static void host_pkru_allow_all(void);
static uint32_t host_pkru(void);
static int guest_pkru_update(gk_thread *t, uint32_t keep, uint32_t set, uint32_t *out);
static int sync_cr3(gk_thread *t, uint64_t *load);
static int cr3_load_host(gk_thread *t, uint64_t cr3);
static long enter_guest(gk_thread *t, long (*fn)(void *), void *arg, uint64_t stack_top,
                        int user, uint64_t cr3);
static int host_backing_retry(gk_thread *t);
static long host_backing_fail(gk_thread *t, int err);
static long run_vcpu(gk_thread *t);
#endif

#if defined(__aarch64__)
// gk_arm64.c
static int flush_uses_invpcid(void);
static uintptr_t super_page(void);
static int page_size_init(void);
static int pt_arena_init(void);
static int guest_tables_init(void);
static int gpa_assign(uintptr_t s, uintptr_t e, uint64_t *gpa);
static void gpa_unassign(uintptr_t s, uintptr_t e, uint64_t gpa);
static void gpa_retire(gk_region *n);
static void region_set_gpa(gk_region *n, uint64_t gpa);
static inline uint64_t region_gpa(const gk_region *n);
static int region_clip_holes(uintptr_t *s, uintptr_t *gap_end);
static int pte_leaf(uint64_t va, uint64_t flags, int pkey, int super, uint64_t *out);
static uint64_t pte_reprotect(uint64_t e, const gk_reprot *rp, uintptr_t v);
static int gpa_alloc(uint64_t size, uint64_t *out);
static void gpa_release(uint64_t start, uint64_t size);
static int vcpu_reset_pc(gk_thread *t);
static int vcpu_init(gk_thread *t, int flags);
static int map_handler_text(void);
static int flush_run(gk_thread *t);
static void flush_tlb(gk_thread *t, gk_regs *r);
static void host_pkru_allow_all(void);
static uint32_t host_pkru(void);
static int guest_pkru_update(gk_thread *t, uint32_t keep, uint32_t set, uint32_t *out);
static int ttbr0_preload(gk_thread *t, uint64_t root);
static int sync_cr3(gk_thread *t, uint64_t *load);
static long enter_guest(gk_thread *t, long (*fn)(void *), void *arg, uint64_t stack_top,
                        int user, uint64_t cr3);
static gk_region *region_by_gpa(gk_region *n, uint64_t gpa);
static long vcb_bad(gk_thread *t);
static int vcb_exception(gk_thread *t, long *ret);
#endif

#if defined(__aarch64__) && !GK_HVF
// gk_kvm_arm64.c
static uintptr_t thread_pointer(void);
static uintptr_t thread_data_floor(uintptr_t tp);
static void wall_probe(void);
static int vm_create(void);
static int s2_ensure(gk_region *rg, uintptr_t hva);
static void vdso_setup(void);
static int vdso_call(gk_frame *f, uint64_t pc);
static int vcpu_set(int fd, uint64_t id, uint64_t v);
static int vcpu_get(int fd, uint64_t id, uint64_t *v);
static int cr3_load_host(gk_thread *t, uint64_t cr3);
static int host_backing_retry(gk_thread *t);
static long host_backing_fail(gk_thread *t, int err);
static long run_vcpu(gk_thread *t);
#endif

// gk_core.c
static int arena_sync_root(gk_arena *a);
static void arena_sync_if_stale(gk_arena *a);
static int stack_wall_bounds(gk_thread *t, uintptr_t caller_sp, uintptr_t top,
                             uintptr_t *lo, uintptr_t *hi);
static int wall_raise(gk_thread *t, uintptr_t lo, uintptr_t hi);
static void wall_lower(gk_thread *t);
static long run_here(void *ctx, unsigned long caller_sp);

// gk_arena.c
static gk_arena *arena_containing(uintptr_t a);

#endif  // GK_INTERNAL_H
