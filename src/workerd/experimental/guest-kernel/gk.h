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

#define GK_EFAULT (-1001)  // guest faulted; see gk_fault_addr()
#define GK_ESHUTDOWN (-1002)  // guest triple-faulted or shut down

// The guest-virtual address of the last fault reported by gk_run.
unsigned long gk_fault_addr(void);

// The number of KVM vCPUs created so far. vCPUs of ended threads are pooled
// and reused, so this is bounded by the peak number of concurrently running
// guest threads, not by the number of threads ever created.
int gk_vcpu_count(void);

// Per-isolate memory arena: a region with its own page-table root. While an
// arena is active, other arenas' memory is unmapped and unaddressable. This is
// the isolate-cage primitive; in workerd one arena would hold one isolate's V8
// cage.
typedef struct gk_arena gk_arena;

// Create an arena of at least `size` bytes. Returns NULL on failure.
gk_arena *gk_arena_create(size_t size);

// The base address of an arena's memory (valid on host and, when the arena is
// active, in the guest at the same address).
void *gk_arena_base(const gk_arena *a);
size_t gk_arena_size(const gk_arena *a);

// Make `a` the active arena for subsequent guest execution, returning the
// previously active arena (or NULL for the base root with no private arena).
// Passing NULL selects the base root. Takes effect on the next gk_run, and,
// when called from within forwarded code, immediately.
gk_arena *gk_arena_enter(gk_arena *a);

void gk_arena_destroy(gk_arena *a);

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
