// gk: a guest-kernel isolation library.
//
// gk runs the calling thread inside a KVM virtual machine as guest ring 0 (the
// Dune model): the process's own memory is visible to the guest and its
// syscalls are forwarded to the host. On top of that it can give each isolate a
// private memory arena backed by a separate guest page-table root, so code
// running for one isolate cannot address another's arena, enforced by hardware.
//
// This is the runtime-side shape of security-idea-2.md. x86-64 Linux only;
// needs read/write access to /dev/kvm.
#ifndef GK_H
#define GK_H
// A C header: its types are spelled with typedef, which the C++ linter's
// modernize-use-using would otherwise flag at every C++ include.
// NOLINTBEGIN(modernize-use-using)

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the guest-kernel platform for this process. Creates the VM, maps
// the current address space, and sets up syscall forwarding and a fault
// handler. Returns 0 on success or a negative errno-style code. Call once
// before any other gk function.
int gk_init(void);

// A human-readable reason gk_init failed, or NULL. Valid after gk_init < 0.
const char *gk_last_error(void);

// Run fn(arg) inside guest ring 0 with syscalls forwarded to the host, and
// return its return value. If the guest takes an unhandled fault (for example
// an access to an inactive arena, or to a page whose memory protection key the
// guest's PKRU denies), gk_run stores the faulting address (see gk_fault_addr)
// and returns GK_EFAULT.
//
// Memory protection keys work in the guest as they do natively: pkey_alloc,
// pkey_mprotect and pkey_free are forwarded, the assigned keys are placed in
// the guest page tables, and the guest's own rdpkru/wrpkru (glibc's pkey_get/
// pkey_set) act on the calling thread's vCPU PKRU, which the CPU enforces. A
// thread enters the guest with the PKRU it has on the host; a thread created
// inside the guest inherits its creator's. A pkey_mprotect from the guest may
// use any key, including one pkey_alloc'ed on the host outside the guest; a
// key gk cannot reflect fails with ENOMEM, never EINVAL.
long gk_run(long (*fn)(void *), void *arg);

// Like gk_run, but fn runs on the calling thread's own stack rather than on a
// private guest stack: the guest's rsp starts just below gk_run_here's frame
// and grows down into the thread's stack, as a native call of fn would. This
// keeps stack-derived state valid across the host/guest boundary: a
// conservative GC that scans from an address captured on the host (workerd's
// __builtin_frame_address(0)) up through fn's frames, and a stack limit V8
// derives from pthread_getattr_np, both describe the stack fn actually uses.
// The thread's stack is demand-paged into the guest as fn descends, including
// the main thread's kernel-grown stack. gk's own host-side loop runs on a
// separate per-vCPU side stack meanwhile, so it never overwrites fn's frames.
// Return values and faults are reported exactly as by gk_run.
long gk_run_here(long (*fn)(void *), void *arg);

// Like gk_run / gk_run_here, but fn runs at guest ring 3 (user mode) instead of
// ring 0. Untrusted code should run this way: at ring 3 it cannot execute
// privileged instructions (CR3 reloads, wrmsr, in/out, ...), and it cannot read
// or write gk's supervisor pages (handler text, GDT/IDT, exception stacks, and
// gk's own control data: the arena registry, page-table allocator, memslot
// tree, per-thread records and the rest) or gk's refused pages (the page
// tables themselves, kvm_run, the host-side gk stacks). Any such attempt
// faults and the call returns GK_EFAULT (see gk_fault_addr); the process
// survives. Legitimate syscalls are forwarded and the function returns to
// ring 3 as usual.
//
// What this guarantees, and what it does not. The ring-3 wall protects three
// things against arbitrary code execution inside an isolate: every other
// isolate's arena (ring-3 code can neither reload CR3 nor rewrite the page
// tables or the bookkeeping that selects them, so the per-isolate walls hold),
// gk's own control state listed above, and -- for gk_run_here_user -- the host
// frames on the calling thread's stack. fn runs on that stack directly below
// the frames of gk_run_here_user and its callers, whose return addresses and
// saved registers the host reloads when the turn ends; for the length of the
// turn the caller's stack above fn's entry frame (up to the thread's stack top,
// or its TLS block on a pthread) is mapped read-only to the guest, so a store
// there faults and the call returns GK_EFAULT with gk_fault_addr the store's
// address. fn and everything it calls must therefore write only below its
// entry: results go back through memory fn's caller owns elsewhere (arg may
// point to the heap, never to a caller-frame local that fn will assign).
// Reads of the caller's frames still work, so a conservative GC that scans
// the whole thread stack is unaffected.
//
// It does not protect the shared, non-arena memory of the runtime: the C++/KJ
// heap, glibc, and every guest thread's stack -- including the parts of the
// calling thread's stack that fn itself uses, and other threads' stacks, whose
// host frames the wall of their own turn does not hold against this thread --
// are ordinary user-mapped pages that ring-3 code can read and write. The
// exception is memory from gk_host_alloc (below), which no guest root maps:
// runtime state placed there is out of ring 3's reach, and the remaining work
// is moving the rest of the runtime's own memory there, so that a sandbox
// escape that gains arbitrary code execution can no longer corrupt shared
// runtime state.
//
// Threads created inside the guest run at their creator's privilege: a thread
// a ring-3 fn creates (clone/clone3) starts at ring 3 too, and so do its own
// children, while a thread a ring-0 fn (gk_run) creates starts at ring 0. A
// ring-3 thread's syscalls are forwarded and faults are reported as above,
// except that a fault has no gk_run call to return GK_EFAULT to: it ends that
// thread alone (its vCPU is recycled and joiners are woken as for any thread
// exit; a message goes to stderr and gk_fault_addr records the address) and
// the process survives. Arena/CR3 switching stays host-driven in ring 0.
long gk_run_user(long (*fn)(void *), void *arg);
long gk_run_here_user(long (*fn)(void *), void *arg);

// Test/diagnostic: fills *super with the address of a gk supervisor page and
// *refuse with the address of a gk refused page, so a test can confirm ring-3
// code cannot reach either. Either pointer may be NULL.
void gk_debug_control_addrs(unsigned long *super, unsigned long *refuse);

// Test/diagnostic: the address of one of gk's own control structures. They all
// live in supervisor memory, so ring-3 code faults on them while ring-0 code
// and the host can use them. GK_CTL_SCRATCH is a word reserved for tests to
// write; the others are live structures and must only be probed, never written.
enum {
  GK_CTL_SCRATCH,        // a scratch word in the control block
  GK_CTL_ARENAS,         // the arena registry
  GK_CTL_ARENA_POOL,     // the arena structs (page-table roots, slot entries)
  GK_CTL_PROT,           // the supervisor/refuse registry
  GK_CTL_REGIONS,        // the memslot interval tree's root
  GK_CTL_REGION_POOL,    // its node pool
  GK_CTL_PT_ALLOC,       // the page-table page allocator's free list
  GK_CTL_THREADS,        // the per-thread records (active root, arena nesting, vCPU, ring)
  GK_CTL_VCPU_POOL,      // the parked-vCPU pool
  GK_CTL_PKEYS,          // the protection-key ranges
  GK_CTL_ROOT,           // the base page-table root pointer
  GK_CTL_FILTER,         // the syscall filter
  GK_CTL_HOST_SLOTS,     // the host slot table
  GK_CTL_HOSTCALLS,      // the hostcall handler table
  GK_CTL_SIGNAL_FIELDS,  // the signal field table
  GK_CTL_RESOLVE,        // the ring-0 resolve table
  GK_CTL_THREAD_WORDS,   // the calling thread's thread words (0 if it has no record)
  GK_CTL_COUNT
};
unsigned long gk_debug_ctl_addr(int which);

#define GK_EFAULT (-1001)     // guest faulted; see gk_fault_addr()
#define GK_ESHUTDOWN (-1002)  // guest triple-faulted or shut down

// The guest-virtual address of the last fault reported by gk_run.
//
// A page the host side decommits or unmaps without the change passing through
// a forwarded syscall (another host thread's mmap, say) faults on the guest's
// next access too. Inside an arena that fault is reported at the page like any
// other; outside one, where gk cannot recover the access's address, it is the
// address of the faulting instruction instead. Either way the calling thread's
// next gk_run runs normally.
unsigned long gk_fault_addr(void);

// The number of KVM vCPUs created so far. vCPUs of ended threads are pooled
// and reused, so this is bounded by the peak number of concurrently running
// guest threads, not by the number of threads ever created.
int gk_vcpu_count(void);

// Per-isolate memory arena: a large PROT_NONE reservation with its own
// page-table root. The arena's owner commits and decommits sub-ranges with
// mprotect (or fixed mmap/munmap), as V8 does for its sandbox; a committed
// page is demand-paged into the arena's root the first time the guest touches
// it while the arena is active, with the protection the host mapping has at
// that moment. While an arena is active, every other arena's memory is
// unmapped and unaddressable, and from the base root (no arena active) all
// arena memory is. This is the isolate-cage primitive; in workerd one arena
// holds one isolate's V8 sandbox reservation.
typedef struct gk_arena gk_arena;

// Create an arena of at least `size` bytes. The reservation is placed on a
// 512GiB boundary and may span several of those units (V8's sandbox
// reservation can exceed one); nothing in it is accessible until committed.
// Returns NULL on failure.
gk_arena *gk_arena_create(size_t size);

// The base address of an arena's reservation (the same address on the host
// and, when the arena is active, in the guest) and its usable size.
void *gk_arena_base(const gk_arena *a);
size_t gk_arena_size(const gk_arena *a);

// Make `a` the active arena for subsequent guest execution: the thread's guest
// entries run under `a`'s page-table root from the next gk_run on (and, when
// called from within forwarded code, immediately). The arena that was active
// before is recorded in the thread's own record, in gk's supervisor memory, and
// gk_arena_leave returns to it; the caller never names the arena to return to,
// so no value that guest code could have rewritten ever selects a root. Enters
// nest up to GK_ARENA_NEST deep per thread.
//
// `a` must be a live arena, between its gk_arena_create and gk_arena_destroy.
// Any other pointer is refused before anything is read through it (it is
// checked against gk's own arena pool, not by inspecting what it points at),
// with -EINVAL, a message on stderr, and the thread's root left as it was.
// Nesting past the bound is refused the same way with -ENOMEM. NULL is the
// outermost leave: it selects the base root and empties the thread's nesting
// history. Returns 0 on success.
#define GK_ARENA_NEST 16
int gk_arena_enter(gk_arena *a);

// Return to the arena that was active before the innermost gk_arena_enter on
// this thread, as gk recorded it; an arena destroyed since then counts as the
// base root. A leave with nothing entered on this thread selects the base root,
// prints a message on stderr and returns -EINVAL. Returns 0 on success.
int gk_arena_leave(void);

// Tear an arena down: its reservation is unmapped, every mapping and KVM
// memslot inside it is removed, and its page tables and address-space slots
// are recycled for later arenas, so creating and destroying arenas
// indefinitely is bounded in every resource. The calling thread leaves the
// arena if it is active there. No other thread may still have it active (or
// be a guest thread that inherited it): such an arena's tables are leaked
// rather than recycled, with a message on stderr.
void gk_arena_destroy(gk_arena *a);

// Resource counters, for tests and diagnostics.
typedef struct {
  int memslots;         // live KVM memslots
  int memslot_ids;      // memslot ids ever handed out (bounded by peak live memslots)
  int arenas;           // live arenas
  long pt_pages_used;   // page-table pages in use, across all roots
  long pt_pages_free;   // page-table pages on the free list
  long pt_pages_total;  // page-table pages ever taken from the fixed PT area
  int prot_ranges;      // ranges in the supervisor/refuse registry
  long demand_faults;   // guest page faults resolved by mapping the page from the host
  long wall_flushed;    // host-frame walls raised with a TLB flush
  long wall_skipped;    // host-frame walls raised without one, the flush being provably unneeded
  long
      global_pages;  // PTEs installed as global (shared runtime pages whose TLB entries survive a root switch)
  long
      neighbor_pages;  // PTEs installed ahead of a fault, for pages adjacent to a demand-faulted one
  long root_switches;  // guest entries that loaded a different page-table root (an arena switch)
  long tlb_flushes;    // full TLB flushes run on a vCPU, on every path: a wall raise that could not
                       // skip, a reflected mprotect/munmap, freed page tables (wall_flushed is the
                       // wall's share)
  int flush_invpcid;  // the flush stub in use: 1 = INVPCID (one VM exit), 0 = CR4.PGE toggle (three)
  int host_regions;   // host-memory regions mapped (each is one entry in the registry)
  long host_blocks;   // host-memory blocks allocated and not yet freed
  int host_slots;     // host slots allocated and not yet freed (see gk_host_slot_alloc)
  int hostcall_handlers;  // hostcall indices with a handler registered (see gk_hostcall_register)
  long hostcalls;         // hostcalls that reached a handler, from either side
  int signal_fields;  // signal fields registered and not yet unregistered (see gk_signal_field_register)
  int resolve_entries;  // ring-0 resolve entries registered and not yet unregistered (see gk_resolve_register)
  long
      path_refusals;  // forwarded path syscalls the turn-directory policy refused (see gk_confine_paths)
} gk_stats;
void gk_get_stats(gk_stats *s);

// Host memory: memory the host side of the runtime owns and that no guest
// page-table root maps. gk_host_alloc returns a zero-filled block of at least
// `size` bytes that the host reads and writes as ordinary memory, while any
// guest access to it, at ring 3 or at ring 0, inside an arena or on the base
// root, takes a fault at the accessed address: a guest read does not see zeroes
// or stale bytes, it does not complete. The block comes from a region
// registered GK_PROT_REFUSE in the supervisor/refuse registry (so a forwarded
// mprotect/munmap/mremap over it is refused too, see gk_mapping_change_ok),
// and is never mapped into any root. This is the memory class memory-classes.md
// calls Host (H): runtime state that only host code touches, such as secrets
// and anything a pointer the host dereferences is loaded from.
//
// Regions are mapped a few MiB at a time and registered once each, so the
// registry grows by one entry per region, never per block, and the number of
// regions is bounded (a bounded total of host memory); blocks are suballocated
// by size class and recycled through free lists kept inside the refused pages
// themselves. gk_host_free scrubs the block before recycling it. `size` must
// be the size the block was allocated with. The block is aligned to the
// smaller of its size rounded up to a power of two and 4 KiB.
//
// Host-side only: both functions take gk's lock, may map memory, and write the
// block, so a call from inside the guest faults or returns an error rather
// than working, and neither runs on gk's fault or syscall-forward path.
// gk_host_alloc returns NULL for a zero size, a size above GK_HOST_MAX_ALLOC,
// when the region bound or the host's memory is exhausted, and before gk_init
// has succeeded (see gk_initialized), so an embedder that must never fall back
// to ordinary memory checks the result. Freeing a pointer that gk_host_alloc
// did not return, or with a size of another class, is a fatal integrity
// error: a free is a write through the pointer, and the pointer may have been
// loaded from memory the guest can rewrite.
#define GK_HOST_MAX_ALLOC (1UL << 20)
void *gk_host_alloc(size_t size);
void gk_host_free(void *ptr, size_t size);

// Whether `ptr` lies in host memory: in a block gk_host_alloc has handed out
// or could hand out. For tests and for an embedder's own assertions.
int gk_host_owns(const void *ptr);

// Host slots: host-memory blocks named by a handle instead of a pointer.
//
// A block from gk_host_alloc is out of the guest's reach, but the pointer to
// it is not: it lives wherever its owner lives, and an owner in the ordinary
// heap is memory a ring-3 escape with a write primitive can rewrite, after
// which the host reads and writes through whatever address it was given. The
// provenance rule in memory-classes.md is that a pointer the host dereferences
// comes from host memory; an index into a host-owned table may come from
// anywhere, because the host validates it. Host slots are that table for the
// runtime's host-memory tenants. A slot owns one block, and the only thing the
// embedder keeps outside host memory is the handle.
//
// The table lives in gk's supervisor control block, so ring 3 can neither read
// nor write an entry, and it is fixed-size: GK_HOST_SLOTS entries, never more.
// gk_host_slot_alloc allocates a block of `size` bytes exactly as gk_host_alloc
// would (same limits, same alignment, zero-filled), places it in a free slot
// and returns the slot's handle, or 0 when the table or host memory is
// exhausted, for a zero or oversized `size`, or before gk_init has succeeded;
// exhaustion is reported this way, never fatal. 0 is never a valid handle.
//
// gk_host_slot_get resolves a handle to its block, or to NULL. It validates
// the handle completely before returning anything: the index is bounds-checked
// against the table, the slot must be live, and the handle's generation must
// be the one the slot was allocated with, so a handle to a freed slot resolves
// to nothing even after the slot has been reused -- every allocation takes a
// fresh generation from a counter that never repeats within 2^48 allocations.
// Resolution reads only the table, never anything the caller supplied: a
// handle that gk did not hand out, whatever its value (a pointer, a stale
// handle, random bits), resolves to nothing and nothing is read through it.
// Such a handle is reported on stderr as well, as gk_arena_enter reports a
// pointer that is not a live arena: a handle that fails validation is never a
// normal outcome (an owner holds its slot for its lifetime), so it is either an
// embedder use-after-free or a value the guest rewrote, and both deserve a
// trace; the only silent case is 0, which names nothing by definition. The
// returned pointer is for the host's immediate use and is not to be stored
// outside host memory: resolve the handle again for each use.
//
// What no check here can remove: a handle rewritten to the live handle of
// another tenant names that tenant's block, and the table cannot tell that use
// from a legitimate one. This is the same ownership bound gk_arena_enter and
// gk_arena_destroy carry for a pointer rewritten to another live arena; the
// table turns "any address" into "one of the live tenants", and the rest is a
// host-side ownership question (see the README's limitations).
//
// gk_host_slot_free validates the handle the same way, then scrubs and frees
// the block (as gk_host_free does), retires the slot's generation and returns
// the slot to the free list; a handle that fails validation frees nothing and
// is reported. Both functions are host-side only, like gk_host_alloc: they take
// gk's lock and are never on the fault or syscall-forward path.
#define GK_HOST_SLOTS 4096
typedef uint64_t gk_host_handle;
gk_host_handle gk_host_slot_alloc(size_t size);
void *gk_host_slot_get(gk_host_handle handle);
void gk_host_slot_free(gk_host_handle handle);

// Hostcalls: a synchronous call from the guest to a handler the host has
// registered, with a result back before the guest's next instruction. It is
// the crossing for a runtime operation that is synchronous to JS -- one whose
// caller cannot be handed a promise -- and that must run host-side because it
// touches host memory: minting an actor id with a key in a host slot, say.
//
// The guest issues it as a syscall with the reserved number
// GK_HOSTCALL_SYSCALL and the handler's index in RDI, which gk's syscall exit
// recognizes, like the exit sentinel, before any forwarding or filtering: a
// hostcall never reaches the kernel and never passes the syscall filter. It
// costs one VM exit and the handler's own work. gk_hostcall issues it; the
// same function called on the host side, outside the guest, runs the handler
// directly through the same table and the same frame, so an embedder calls
// one function from either side of the boundary. Which side it is on comes
// from a per-thread flag gk sets around every guest entry; the flag is
// thread-local storage, which the guest can rewrite, and a lie costs only the
// liar: a hostcall issued from the host reaches the kernel and comes back
// -ENOSYS, and the direct path taken at ring 3 faults on the supervisor record
// it reads.
//
// Selection. The guest names the handler by a small integer index, nothing
// else: never an address, never a pointer the host would call through. The
// index is checked against the handler table, which lives in gk's supervisor
// control block (ring 3 can neither read nor write it) and has GK_HOSTCALLS
// entries, fixed; an index at or past the table, an index whose entry is
// empty, and any index while nothing is registered all return -ENOSYS to the
// guest with nothing called and nothing read. Registration is host-side:
// gk_hostcall_register binds an index to a handler and a context pointer, or
// returns -EINVAL for an index past the table or a NULL handler and -EBUSY for
// an index already bound (unregister it first); the capacity is GK_HOSTCALLS
// indices and a table that is full is a registration refused, never a fault.
// The context pointer is handed back to the handler from the table, never from
// the guest, so by the provenance rule it may be dereferenced -- and what it
// points at should itself be in host memory, so that the guest cannot change
// what the handler finds there.
//
// Arguments and results. The handler is never handed a pointer the guest
// chose. Everything crosses through a per-thread exchange region whose
// location and bounds gk owns: one gk_hostcall_frame per thread record, in a
// reservation gk made at init and commits one region at a time, which the
// mapping policy (gk_mapping_change_ok) refuses to let the guest unmap,
// decommit or reprotect. The region is ordinary user memory -- the guest
// writes its arguments there and reads its result back -- and so it is
// untrusted: before the handler runs, gk reads in_len from the region once,
// refuses the call with -E2BIG if it exceeds GK_HOSTCALL_BYTES, and copies the
// args words and the first in_len bytes of buf into a second frame in host
// memory, which the handler alone sees. Nothing in the region is read twice,
// and nothing read after the copy can change what the handler was given, so a
// second guest thread rewriting the region under the call changes nothing the
// handler sees. On return gk copies the handler's out_len bytes of buf back
// to the region (a handler that sets out_len past the frame has made a host
// error: nothing is copied and the guest gets -EINVAL), scrubs the exchanged
// bytes from the host frame, and the handler's return value becomes the
// guest's. The by-value words in args are for small arguments -- an operation
// code, a host slot handle -- and buf for bulk bytes. By-value registers alone
// were not enough: the operations this exists for carry keys' worth of bytes
// each way, and a name of any length in. A pointer argument into guest memory
// would have been simpler and is exactly the thing this design exists to
// refuse. The region is reached by the guest through gk_hostcall_region,
// whose answer comes from thread-local storage: a guest that rewrites it only
// misdirects its own writes, since the host locates the region through its
// record of the thread in supervisor memory, never through that value.
//
// The host frame also carries `state`: GK_HOSTCALL_STATE bytes of host
// memory per thread that gk never copies to or from the guest and zeroes when
// the thread record is taken, for a handler whose operation spans several
// calls (a hash over more bytes than buf holds). It is shared by every
// handler on that thread, and the guest decides what calls come between, so a
// handler keeps a tag in it and checks the tag on every continuation.
//
// What a handler is. It runs on the calling thread, on the host side of the
// exit, with gk's lock not held: on the vCPU's refused side stack for a
// gk_run_here/gk_run_here_user turn or a guest-created thread, on the thread's
// own stack for gk_run/gk_run_user. It must return rather than unwind (no
// exception, no longjmp may cross gk's frames), must not enter the guest or
// make a hostcall itself (the nested call reaches the kernel and fails), and
// should not take the process heap: the guest issued the call at a point of
// its choosing, possibly in the middle of the allocator, so a handler that
// locks the heap can be deadlocked by the guest that called it. gk's own part
// of the path -- the exit, the table check, the copies, the resume --
// allocates nothing, which the GK_GUARD_HANDLER_ALLOC build checks. A handler
// returning one of gk's own refusal values (-ENOSYS, -E2BIG, -EINVAL) is
// indistinguishable to the guest from the refusal.
//
// What this does not give. A hostcall is a host-side call at ring 3's
// request: the guest can make it as often as it likes, with any index it
// likes (only registered ones answer) and any arguments it likes, and the
// handler runs with host privilege on the guest's behalf. Every handler is
// therefore an attack surface: it validates its own arguments -- a handle it
// receives resolves through gk_host_slot_get, a length is bounds-checked
// against in_len, an operation code is matched against a closed set -- and it
// gives the guest only what the guest is entitled to compute. A handle
// rewritten to another live tenant's names that tenant (the host-slot
// ownership residual), and a handler cannot tell. Nor does the crossing
// protect the guest's side of it: the arguments and the result sit in the
// region, which any guest thread of the process can read and write.
#define GK_HOSTCALLS 64
#define GK_HOSTCALL_BYTES 4096
#define GK_HOSTCALL_STATE 1024
typedef struct {
  uint64_t args[4];  // guest to host: by-value words
  uint64_t in_len;   // guest to host: bytes of buf that carry arguments (<= GK_HOSTCALL_BYTES)
  uint64_t out_len;  // host to guest: bytes of buf that carry the result
  unsigned char buf[GK_HOSTCALL_BYTES];
  unsigned char state[GK_HOSTCALL_STATE];  // host frame only: handler state across calls
} gk_hostcall_frame;
typedef long (*gk_hostcall_fn)(void *ctx, gk_hostcall_frame *frame);
int gk_hostcall_register(unsigned idx, gk_hostcall_fn fn, void *ctx);
int gk_hostcall_unregister(unsigned idx);
// The calling thread's exchange region: fill args, in_len and buf, call,
// read out_len and buf. Callable at any ring inside the guest; on the host
// side it gives the thread a record if it has none. NULL only if that fails.
gk_hostcall_frame *gk_hostcall_region(void);
// Calls handler `idx` on the frame in the calling thread's region and returns
// its result, or gk's refusal (-ENOSYS, -E2BIG, -EINVAL). One VM exit from
// inside the guest; a direct call on the host side.
long gk_hostcall(unsigned idx);

// Ring-0 resolve: a handle-to-pointer lookup that ring-3 guest code makes
// against a table in supervisor memory, served at ring 0 inside the guest,
// without a VM exit. It is the crossing for an operation a turn makes often --
// resolving the handle an isolate-side object holds for a host object, on
// every dereference -- where the hostcall's exit (a few microseconds) is out
// of the question and the answer cannot come from memory the guest can
// write, since a table ring 3 can read is a table ring 3 can rewrite.
//
// The mechanism. The guest issues a syscall with the reserved number
// GK_RESOLVE_SYSCALL and the handle in RDI. SYSCALL lands in gk's trampoline
// at ring 0, which recognizes the number before the hypercall that would
// leave the VM, looks the handle up in the table, and SYSRETs straight back
// to ring 3 with the pointer, or 0, in RAX. The whole path is a few dozen
// instructions of gk's own handler text, touching no stack and no memory but
// the table, and runs at guest ring 0 with the VM never exited: the round
// trip is about 19 ns on the test machine, against 6.4 us for a hostcall
// (measured by resolve_check in gk_test.c, which also proves the no-exit
// claim from KVM's own per-vCPU exit counter). The operation is fixed, not a
// handler the embedder registers: embedder code run at guest ring 0 would
// need its data mapped supervisor, where any ring-0 code the guest reaches
// could read it, so the only thing that runs at ring 0 is gk's lookup, and
// the set of operations is this one. gk_resolve is the call: at guest ring 3
// it is the trap; at guest ring 0 and on the host side it reads the table
// directly, with the same validation. Which path is taken is decided from
// the CS register -- the ring-3 code selector gk's GDT hands out is the only
// one that traps -- not from any value in memory, so there is nothing for
// the guest to rewrite and no privilege the trap could confer on a caller
// that already has ring 0 (a ring-0 caller issuing the syscall directly,
// bypassing gk_resolve, would be returned to ring 3, which is a trusted
// caller's own doing).
//
// The table. GK_RESOLVE_SLOTS entries of {pointer, generation}, fixed; the
// table is gk's own, registered GK_PROT_SUPER: mapped with U clear in every
// root, so the trampoline reaches it at ring 0 and a ring-3 access faults.
// SUPER is a weaker class than the GK_PROT_REFUSE that host memory
// (gk_host_alloc) and the page tables use -- a REFUSE page is in no guest
// root at all, while a SUPER page is readable by any ring-0 code the guest
// can get to run -- and it is the right class here because the lookup has to
// run inside the guest, at ring 0, which REFUSE memory cannot serve by
// definition; it is the first SUPER tenant outside gk's own control block,
// and what it holds is pointers, which name host objects but are not the
// objects. The host-memory facilities stay on REFUSE. The handle is the
// slot's index in its low 16 bits and the generation the slot was registered
// with above them, and it is validated like a host slot handle: the index is
// bounds-checked against the table, the entry must be live, and its
// generation must be the handle's, so a handle to an unregistered entry
// resolves to nothing even after the slot has been reused -- every
// registration takes a fresh generation from a counter that does not repeat
// within 2^48 registrations. Nothing is read through the handle itself, at
// either ring: a value gk did not issue (a pointer, a stale handle, random
// bits) resolves to nothing. The lookup takes no lock and writes nothing;
// against a registration or unregistration in progress on another thread it
// reads the generation on both sides of the pointer and answers only if both
// agree, so the pointer it returns was the entry's under that generation.
//
// gk_resolve_register places `ptr` in a free slot and returns its handle, or
// 0 when the table is full (exhaustion is a refusal, never fatal), for a NULL
// pointer, or before gk_init has succeeded; 0 is never a valid handle.
// gk_resolve_unregister retires the slot: the handle resolves to nothing from
// then on, and the slot is free for a later registration under a new
// generation. A handle that fails validation unregisters nothing, is
// reported on stderr (as a host slot handle is) and returns -EINVAL. Both are
// host-side: they take gk's lock, and a call at ring 3 faults on the table.
// gk_resolve resolves a handle to its pointer, or to NULL, from any ring and
// from the host, and allocates nothing on any path; the GK_GUARD_HANDLER_ALLOC
// build checks the host-side path, and the ring-0 path is asm with no call in
// it. The pointer it returns names a host object the caller is about to use;
// it is for immediate use, resolved again for each use, and never stored
// where the guest can write it.
//
// What this does not give. Ring 3 can resolve as often as it likes, with any
// handle it likes, and the only thing it learns is the pointer behind a live
// handle it holds -- a pointer it could not dereference anyway if the object
// is in host memory, and which the owner must therefore still protect as an
// address: the lookup makes a handle safe to keep in guest memory, it does not
// make the object safe to reach from there. The operation must be safe for
// arbitrary input, and is: it reads the table and nothing else. And a handle
// rewritten to another live entry's names that entry, which the table cannot
// tell from a legitimate use -- the ownership residual that gk_arena_enter and
// the host slot table carry too (see the README's limitations). Nor does the
// table know what it points at: an object unregistered and freed while a
// resolve of its handle is in flight on another thread is the owner's race to
// avoid, as it is for every lock-free table; in the runtime the entries of an
// isolate's objects change only on the thread that holds its lock.
#define GK_RESOLVE_SLOTS 16384
typedef uint64_t gk_resolve_handle;
gk_resolve_handle gk_resolve_register(void *ptr);
int gk_resolve_unregister(gk_resolve_handle handle);
void *gk_resolve(gk_resolve_handle handle);

// Signal fields: 4-byte words inside an arena that the host writes from a
// signal handler, found through a table in gk's supervisor memory rather than
// through anything the guest can write.
//
// The case this serves: a runtime that signals an isolate whose turn is using
// up its CPU budget by writing a constant into the isolate's own memory (a
// Wasm module's linear memory, say), from the handler of a per-thread CPU
// timer, while the turn is running. The handler cannot take a lock and
// cannot allocate, and it must not trust a descriptor kept in the ordinary
// heap: a pointer and a size the isolate can rewrite turn the handler into
// an arbitrary write of a known constant, triggered by burning CPU. So the
// descriptor is validated once, host-side, and kept here.
//
// Registration is host-side (a hostcall handler, or any host code) and is
// for the calling thread's active arena, taken from gk's record of the
// thread, never from the caller: a turn can register fields only in the
// arena it is running in. gk_signal_field_register names a buffer by its
// offset within that arena and its length, and the field by its offset
// within the buffer, and refuses -- with nothing recorded -- a buffer that
// does not lie wholly within the arena's reservation, a field that does not
// lie wholly within the buffer (including by overflow of any sum), a kind at
// or past GK_SIGNAL_KINDS, a calling thread with no active arena (-ESRCH),
// and a full table (-ENOMEM, never fatal). What the table keeps is the
// field's absolute address, the arena and the kind. The handle it returns is
// validated like a host slot handle (index, liveness, generation; see
// gk_host_slot_get), and gk_signal_field_unregister accepts it only from a
// thread whose active arena is the field's, so a turn cannot unregister
// another arena's fields with a handle it guessed; a handle that fails
// validation is -EINVAL with nothing changed. Destroying an arena
// unregisters whatever fields it still has.
//
// gk_signal_field_write writes `value` to every registered field of kind
// `kind` in the calling thread's active arena, and returns how many it
// wrote, or a negative value with nothing written: -ESRCH when the thread has
// no record or no active arena, -EINVAL for a bad kind. It is for use from a
// signal handler on the thread whose turn is running: it takes no lock,
// allocates nothing, makes at most one syscall (gettid, to validate the
// thread's record), and reads only the control block -- the thread record is
// found by validating the thread-local hint against the record's tid and,
// failing that, by scanning the records for the tid, so a rewritten hint
// changes nothing; the arena is the record's; the fields are the arena's
// list, walked without a lock. It is correct against a signal that arrives
// in the middle of a registration or unregistration: the only mutations of
// an arena's list come from the threads that have that arena active (a
// registration is for the caller's own active arena, an unregistration is
// refused for any other), which in the runtime is the one thread holding the
// isolate's lock -- the thread the signal interrupts. The interrupted
// mutation holds gk's lock, which the walk never takes, and the walk sees the
// mutation's stores in program order: a field is published by storing its
// `live` flag last, with release semantics, after its address, arena and
// kind, and retired by clearing `live` first, with its address left in place
// until the entry is recycled, so a walk that observes `live` observes a
// complete descriptor, and the link it reads from a retired entry still
// leads to the rest of the list. Each address is checked against the
// arena's bounds again before the write, so a torn or recycled entry (which
// the ordering above rules out, but which costs nothing to check) cannot
// aim the write outside the arena. Under the GK_GUARD_HANDLER_ALLOC build
// the walk runs under the allocation guard.
//
// What this does not do: the write lands wherever the field is, at the
// address the buffer had when it was registered. A buffer the isolate has
// since freed or moved (a Wasm memory that grew by relocation) gets the
// write at its old address, which is still inside that isolate's own arena,
// so the isolate can at worst mislay its own signal; an owner that moves a
// buffer unregisters and re-registers the field. A field in a page the
// isolate has decommitted since faults the host's write: a crash the
// isolate could cause by other means, not a write anywhere else.
#define GK_SIGNAL_FIELDS 8192
#define GK_SIGNAL_KINDS 4
#define GK_SIGNAL_FIELD_BYTES 4
typedef uint64_t gk_signal_handle;
int gk_signal_field_register(uintptr_t buffer_offset,
    size_t buffer_len,
    uint32_t field_offset,
    unsigned kind,
    gk_signal_handle *handle);
int gk_signal_field_unregister(gk_signal_handle handle);
long gk_signal_field_write(unsigned kind, uint32_t value);

// Thread words: GK_THREAD_WORDS 64-bit words per thread, in the thread's
// record in gk's supervisor control block, for per-thread state of the
// embedder's that host code or a signal handler must read from memory no
// ring-3 code can write. Thread-local storage is the wrong place for such
// state: it is user memory, so a turn can rewrite it, and a pointer the host
// later loads from it names whatever the turn put there. The words are
// addressed by a small key, validated against GK_THREAD_WORDS before anything
// is read: a range that starts at or past the table, or runs past it, is
// -EINVAL with nothing read or written. The capacity is fixed; a key is an
// assignment the embedder makes once, not something allocated, so there is
// nothing to exhaust and nothing to free.
//
// gk_thread_words_set stores n words from `values` at keys first..first+n-1
// for the calling thread, gk_thread_words_get copies them out. Both find the
// thread's record lock-free -- the thread-local index hint checked against
// the record's kernel tid, with a scan of the records when the hint is wrong
// -- and so cost a gettid each (about 50 ns on the test machine); a range is
// one lookup however many words it spans, so state that is read together is
// kept adjacent and read in one call. Both are usable from a signal handler
// on the thread, except the first set on a thread with no record, which takes
// one (and gk's lock). A thread without a record reads -ESRCH; a record is
// zeroed when it is taken, so a thread that has one and has set nothing reads
// zeroes, and a thread the guest creates starts with zeroes too, never its
// creator's words. Before gk_init both are -ESRCH.
//
// Host-side only: the record is supervisor memory, so a call from ring 3
// faults. A word is only as good as what it holds: a pointer stored here may
// be dereferenced by the host because the guest cannot have changed it, but
// the memory it points at is wherever the embedder put it.
#define GK_THREAD_WORDS 8
int gk_thread_words_set(unsigned first, unsigned n, const uint64_t *values);
int gk_thread_words_get(unsigned first, unsigned n, uint64_t *values);

// Whether the calling thread is inside the guest, as gk's own per-thread
// flag says. The flag is thread-local storage: gk sets it from a constant
// around every guest entry, so a rewrite by the guest does not outlast the
// entry, but inside one the guest can say what it likes. For host code that
// must choose between a path that reads supervisor memory and one that does
// not when it may be running at ring 3 under a turn: a lie costs only the
// liar, since the supervisor path faults at ring 3 and ends the turn. Never
// a basis for anything the host trusts.
int gk_in_guest_hint(void);

// The turn directory: a bound on where the path syscalls a turn forwards may
// land. With the policy on (gk_confine_paths(1), process-wide, host-side),
// every thread starts each guest entry with no directory, and a forwarded
// syscall that names a file by path is refused with -EPERM unless the thread
// has one, the syscall is one of the eight below, and its dirfd is that
// directory; the lookup then runs beneath it. The directory is set host-side
// by gk_turn_dir_set -- from a hostcall handler, in the runtime, so that the
// value comes from a table the host validates (a host slot) and never from
// the turn's own argument -- and the descriptor's device and inode are
// recorded then and compared on every use, so a descriptor the guest has
// closed and reused for another file names nothing. GK_TURN_DIR_NONE sets no
// directory; a descriptor that is not a directory is -EINVAL. The guest's own
// copy of the descriptor, wherever it keeps it, is untrusted bookkeeping: a
// rewritten copy makes its syscalls fail, nothing else.
//
// The lookup is the kernel's. openat is forwarded as openat2 with
// RESOLVE_BENEATH and RESOLVE_NO_MAGICLINKS, so the kernel refuses, atomically
// with the lookup, an absolute path (EXDEV), a ".." that would leave the
// directory, a symbolic link that would, and a /proc magic link; the mode is
// passed only with O_CREAT or O_TMPFILE, as openat2 requires. newfstatat and
// statx become an O_PATH open under the same rules (O_NOFOLLOW for
// AT_SYMLINK_NOFOLLOW) followed by fstat or statx on the descriptor, which is
// then closed; with AT_EMPTY_PATH they stat the directory itself. unlinkat,
// mkdirat, readlinkat, faccessat and faccessat2 act on a name rather than
// opening it and have no beneath-only form, so each is split: the path is
// read into host memory once (a guest thread could rewrite it under the
// call), its parent is opened beneath the directory under the rules above,
// and the syscall is issued against that descriptor with the final component
// alone, which cannot traverse and is never followed -- the two access
// forms become faccessat2 with AT_SYMLINK_NOFOLLOW, so a symbolic link is
// examined as itself. A final component of ".." is refused. Every other
// syscall gk knows to name a path -- open, access, stat, readlink, rename,
// unlink, mkdir, the rest of the *at family, openat2 itself, execve, mount
// and so on -- is refused, and so is fchdir, which would move the process's
// working directory. A thread the guest creates is confined like any other
// and starts with no directory. The policy is applied after the embedder's
// filter, which still decides what is forwarded at all; refusals are counted
// in gk_stats.path_refusals. It costs one fstat per path syscall, and an
// extra open and close per stat and per name-acting syscall.
//
// What it does not bound: the descriptors the turn already holds or can
// guess. read, pwrite, fcntl, mmap and close take a descriptor by number and
// are not path syscalls, so a turn can still act on any file the process has
// open, as the process; confining that needs a per-turn descriptor table,
// which this is not. Nor does it hide the directory's contents from the turn:
// what the turn may open beneath its directory, it may read, write, create
// and unlink.
#define GK_TURN_DIR_NONE (-1)
void gk_confine_paths(int on);
int gk_turn_dir_set(int dirfd);

// Nonzero once gk_init has succeeded in this process.
int gk_initialized(void);

// Test/diagnostic: the value of the named per-vCPU statistic of the calling
// thread's vCPU, read from KVM's own statistics file (KVM_GET_STATS_FD), or
// -1 if the thread has no vCPU, the kernel does not export the statistic, or
// the file cannot be read. "exits" counts every VM exit the vCPU has taken,
// including the ones the kernel services without returning to gk (nested
// page faults, host interrupts), so a run whose "exits" did not move provably
// stayed inside the guest. Host-side; allocates nothing.
long gk_debug_vcpu_stat(const char *name);

// Test/diagnostic: drop the guest PTE for the page holding `addr` from every
// root, flushing no TLB. A vCPU that holds a translation of the page in its TLB
// keeps using it, with no demand fault, until that entry is flushed: by gk's
// own flushes, or, for a translation that is not global, by the root switch at
// an entry for another arena. A vCPU without one takes a demand fault, which
// re-installs the PTE. With gk_stats.demand_faults a test can thus tell which
// translations survive what. Not for production use.
void gk_debug_drop_pte(unsigned long addr);

// Test/diagnostic: register [addr, addr+len), rounded out to pages, as a
// supervisor (GK_DEBUG_PROT_SUPER) or refused (GK_DEBUG_PROT_REFUSE) range, or
// unregister exactly such a range again (GK_DEBUG_PROT_KEEP), so a test can
// put a page of either class where it wants one. The range must be the
// caller's own memory and must not overlap a range gk registered. Any PTE a
// root holds for it is dropped on registration. Not for production use.
enum { GK_DEBUG_PROT_KEEP, GK_DEBUG_PROT_SUPER, GK_DEBUG_PROT_REFUSE };
void gk_debug_protect_range(unsigned long addr, size_t len, int kind);

// Syscall policy. By default gk forwards every syscall to the host (blind
// re-issue), which is fine for a spike but not for production. Install a filter
// to bound what forwarded code may do: it is called with the syscall number and
// its six arguments and returns nonzero to allow the syscall or zero to deny
// it. A denied syscall is not forwarded; the guest sees it return -EPERM, as
// the kernel would under seccomp. Pass NULL to clear the filter.
typedef int (*gk_syscall_filter)(long nr, long a1, long a2, long a3, long a4, long a5, long a6);
void gk_set_syscall_filter(gk_syscall_filter f);

// Mapping-change policy, for a filter to apply to the memory syscalls it
// allows. A forwarded mmap/mprotect/munmap/mremap/madvise/pkey_mprotect
// reaches the host with the guest's own arguments, so a filter that admits
// those by number alone lets ring-3 code change or remove any mapping in the
// process: make the runtime's text writable, unmap it, or map a file over the
// host's structures. These functions are the range check that closes that.
//
// gk_mapping_change_ok answers, for the calling thread, whether a change to
// the mappings of [addr, addr+len) is the guest's to make: it is unless the
// range, rounded out to pages, overlaps gk's own supervisor or refused memory
// (the control block, page tables, GDT/IDT, handler text, kvm_run, exception
// and host-side stacks), an arena other than the one the calling thread is
// active in (every arena when none is active), or the runtime image: the
// loaded segments of the executable and of every shared object present at
// gk_init, and the main thread's stack, recorded then. A zero length names no
// page and is accepted; the kernel's own answer to it (EINVAL, or a no-op)
// stands. A range that wraps around the address space is refused. The thread's active arena is read from gk's own
// record of it, not from anything the guest can write. May take gk's lock;
// never allocates, so it may run on the syscall-forward path.
//
// gk_mapping_syscall_ok applies that to one syscall as the filter receives
// it: the range of an mprotect, pkey_mprotect, munmap or madvise; the
// range of an mmap with MAP_FIXED or MAP_FIXED_NOREPLACE; and for mremap the
// old range extended to the new size, plus the destination of an
// MREMAP_FIXED move. An mmap the kernel places itself, and any other syscall,
// is accepted without a range check: neither can touch an existing mapping.
// Returns nonzero to allow, zero to deny, like a filter.
int gk_mapping_change_ok(unsigned long addr, unsigned long len);
int gk_mapping_syscall_ok(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

#ifdef __cplusplus
}
#endif

// NOLINTEND(modernize-use-using)
#endif  // GK_H
