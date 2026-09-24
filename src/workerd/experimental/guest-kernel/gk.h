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

#include <stddef.h>

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
// inside the guest inherits its creator's.
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
// are ordinary user-mapped pages that ring-3 code can read and write. A
// sandbox escape that gains arbitrary code execution can therefore still
// corrupt shared runtime state; containing that requires keeping the runtime's
// own memory out of ring 3's reach, which is remaining work.
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
  GK_CTL_SCRATCH,      // a scratch word in the control block
  GK_CTL_ARENAS,       // the arena registry
  GK_CTL_ARENA_POOL,   // the arena structs (page-table roots, slot entries)
  GK_CTL_PROT,         // the supervisor/refuse registry
  GK_CTL_REGIONS,      // the memslot interval tree's root
  GK_CTL_REGION_POOL,  // its node pool
  GK_CTL_PT_ALLOC,     // the page-table page allocator's free list
  GK_CTL_THREADS,      // the per-thread records (active root, vCPU, ring)
  GK_CTL_VCPU_POOL,    // the parked-vCPU pool
  GK_CTL_PKEYS,        // the protection-key ranges
  GK_CTL_ROOT,         // the base page-table root pointer
  GK_CTL_FILTER,       // the syscall filter
  GK_CTL_COUNT
};
unsigned long gk_debug_ctl_addr(int which);

#define GK_EFAULT (-1001)  // guest faulted; see gk_fault_addr()
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

// Make `a` the active arena for subsequent guest execution, returning the
// previously active arena (or NULL for the base root with no private arena).
// Passing NULL selects the base root. Takes effect on the next gk_run, and,
// when called from within forwarded code, immediately.
gk_arena *gk_arena_enter(gk_arena *a);

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
} gk_stats;
void gk_get_stats(gk_stats *s);

// Syscall policy. By default gk forwards every syscall to the host (blind
// re-issue), which is fine for a spike but not for production. Install a filter
// to bound what forwarded code may do: it is called with the syscall number and
// its six arguments and returns nonzero to allow the syscall or zero to deny
// it. A denied syscall is not forwarded; the guest sees it return -EPERM, as
// the kernel would under seccomp. Pass NULL to clear the filter.
typedef int (*gk_syscall_filter)(long nr, long a1, long a2, long a3, long a4,
                                 long a5, long a6);
void gk_set_syscall_filter(gk_syscall_filter f);

#ifdef __cplusplus
}
#endif

#endif  // GK_H
