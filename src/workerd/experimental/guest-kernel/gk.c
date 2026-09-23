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
static atomic_int g_next_vcpu_id;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_demand_ok;

// Per-thread guest state: each host thread that enters the guest is a vCPU.
typedef struct {
  int inited;
  int fd;
  struct kvm_run *run;
  uint64_t stack_top;
  uint64_t loaded_cr3;
  uint64_t last_fault;
  int fault_repeat;
  uint64_t *active_pml4;   // root this thread runs under
  gk_arena *active_arena;
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

// ---- the MMU layer: chunk-based memslot manager ----------------------------
// guest-physical == host-virtual everywhere. KVM memslots map guest-physical to
// host-virtual; we create one memslot per fixed, aligned CHUNK of the address
// space, on demand. Because chunks are aligned and disjoint, memslots never
// overlap (KVM rejects overlapping memslots), and every mapped PTE is guaranteed
// to have a backing memslot -- without one KVM takes the MMIO/emulation path and
// KVM_RUN returns EFAULT.
//
// TODO: this fixed-chunk scheme is deliberately simple and was chosen to get V8
// running. A production MMU should track the guest's VMAs in an interval tree
// (cf. FreeBSD sys/vm vm_map, 2-clause BSD) and create/split/merge memslots that
// follow mmap/mprotect/munmap exactly: reflecting protection changes, freeing
// memslots and PTEs on unmap, and issuing cross-vCPU TLB shootdowns. It should
// also cache /proc/self/maps lookups rather than reparsing per fault. Revisit
// the algorithm and pick something more optimal then.
#define GK_CHUNK (32UL << 20)
#define GK_CHUNK_MASK (GK_CHUNK - 1)
#define GK_CHUNK_SLOTS (1 << 16)  // open-addressing hash set (power of two)
static uintptr_t g_chunk[GK_CHUNK_SLOTS];  // chunk base, or 0 for empty

static int chunk_has(uintptr_t base) {
  size_t i = (base / GK_CHUNK) & (GK_CHUNK_SLOTS - 1);
  for (size_t n = 0; n < GK_CHUNK_SLOTS; n++, i = (i + 1) & (GK_CHUNK_SLOTS - 1)) {
    if (g_chunk[i] == 0) return 0;
    if (g_chunk[i] == base) return 1;
  }
  return 0;
}
static void chunk_put(uintptr_t base) {
  size_t i = (base / GK_CHUNK) & (GK_CHUNK_SLOTS - 1);
  for (size_t n = 0; n < GK_CHUNK_SLOTS; n++, i = (i + 1) & (GK_CHUNK_SLOTS - 1)) {
    if (g_chunk[i] == 0) { g_chunk[i] = base; return; }
    if (g_chunk[i] == base) return;
  }
}

// Ensure a KVM memslot backs the chunk containing `va`. Caller holds g_lock.
static int mmu_ensure_chunk(uintptr_t va) {
  uintptr_t base = va & ~GK_CHUNK_MASK;
  if (base == 0) return 0;  // the [0, GK_CHUNK) chunk is never used
  if (chunk_has(base)) return 0;
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)g_next_slot,
                                          .guest_phys_addr = base,
                                          .memory_size = GK_CHUNK,
                                          .userspace_addr = base};
  if (ioctl(g_vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) return -1;
  g_next_slot++;
  chunk_put(base);
  return 0;
}

// Map one guest page into a root, creating its chunk memslot first. Caller
// holds g_lock.
static int mmu_map_into(uint64_t *root, uintptr_t va, uint64_t flags) {
  if (mmu_ensure_chunk(va) < 0) return -1;
  return map4k_root(root, va, flags);
}
static int mmu_map(uintptr_t va, uint64_t flags) {
  return mmu_map_into(g_pml4, va, flags);
}
static int mmu_map_range(uintptr_t s, uintptr_t e, uint64_t flags) {
  for (uintptr_t v = s & ~0xfffUL; v < e; v += 0x1000)
    if (mmu_map(v, flags) < 0) return -1;
  return 0;
}

// Is the host page at `page` currently accessible (committed)? V8 reserves huge
// PROT_NONE regions and commits sub-ranges; we must map only committed pages.
// TODO: reparsing /proc/self/maps per fault is O(regions); cache it.
// Returns 0 if the page is not host-accessible, else bit0=readable, bit1=writable.
static int host_page_perms(uintptr_t page) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return 0;
  char line[512]; uintptr_t s, e; char perms[8] = {0}; int p = 0;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) >= 3 && page >= s && page < e) {
      if (perms[0] == 'r') p |= 1;
      if (perms[1] == 'w') p |= 2;
      if (perms[2] == 'x') p |= 4;
      break;
    }
  }
  fclose(f);
  return p;
}

// Handle a guest page fault: if the faulting page is host-accessible, back it
// with a memslot and PTE so the guest can retry. Caller must not hold g_lock.
static int demand_map(uintptr_t addr) {
  uintptr_t page = addr & ~0xfffUL;
  // A fault at an arena address is a cross-arena access (each arena's own cage
  // is fully mapped in its root); refuse it to preserve isolation.
  if (addr_in_any_arena(page)) return -1;
  int p = host_page_perms(page);
  if (!(p & 1)) return -1;  // not host-readable: a genuine fault
  pthread_mutex_lock(&g_lock);
  int r = mmu_map(page, (uint64_t)p);  // honor R/W/X for W^X
  if (r >= 0) g_demand_ok++;
  pthread_mutex_unlock(&g_lock);
  return r;
}

static void set_idt_gate(uint8_t *idt, int vec, uint64_t h) {
  uint8_t *e = idt + vec * 16;
  *(uint16_t *)(e + 0) = h & 0xffff;
  *(uint16_t *)(e + 2) = 0x08;
  e[4] = 0;
  e[5] = 0x8e;
  *(uint16_t *)(e + 6) = (h >> 16) & 0xffff;
  *(uint32_t *)(e + 8) = (h >> 32);
}

// Bring the calling thread's vCPU up. Each thread has its own vCPU, stack and
// TLS base, but they share the VM, memslots and page tables.
static int vcpu_init(void) {
  if (tls.inited) return 0;
  int id = atomic_fetch_add(&g_next_vcpu_id, 1);
  int fd = ioctl(g_vmfd, KVM_CREATE_VCPU, id);
  if (fd < 0) { g_err = "KVM_CREATE_VCPU"; return -1; }
  if (ioctl(fd, KVM_SET_CPUID2, g_cpuid) < 0) { g_err = "KVM_SET_CPUID2"; return -1; }
  struct kvm_run *run = mmap(NULL, g_run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
  if (run == MAP_FAILED) { g_err = "mmap kvm_run"; return -1; }

  // A private guest stack for this vCPU, eager-mapped so interrupt delivery
  // (which pushes a frame) never itself faults.
  const size_t stksz = 2 * 1024 * 1024;
  uint8_t *stk = mmap(NULL, stksz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (stk == MAP_FAILED) { g_err = "mmap guest stack"; return -1; }
  pthread_mutex_lock(&g_lock);
  int mrc = mmu_map_range((uintptr_t)stk, (uintptr_t)stk + stksz, 1 | 2);  // rw, NX
  pthread_mutex_unlock(&g_lock);
  if (mrc < 0) { g_err = "map guest stack"; return -1; }

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
  s.gdt.base = g_gdt_va; s.gdt.limit = 3 * 8 - 1;
  s.idt.base = g_idt_va; s.idt.limit = 256 * 16 - 1;
  if (ioctl(fd, KVM_SET_SREGS, &s) < 0) { g_err = "KVM_SET_SREGS"; return -1; }

  struct kvm_xcrs xcrs = {0};
  xcrs.nr_xcrs = 1; xcrs.xcrs[0].xcr = 0; xcrs.xcrs[0].value = 0x7;
  if (ioctl(fd, KVM_SET_XCRS, &xcrs) < 0) { g_err = "KVM_SET_XCRS"; return -1; }

  struct { struct kvm_msrs h; struct kvm_msr_entry e[4]; } m = {0};
  m.h.nmsrs = 4;
  m.e[0].index = MSR_EFER; m.e[0].data = EFER_LME | EFER_LMA | EFER_SCE | EFER_NXE;
  m.e[1].index = MSR_STAR; m.e[1].data = (uint64_t)0x08 << 32;
  m.e[2].index = MSR_LSTAR; m.e[2].data = (uintptr_t)&gk_syscall_tramp;
  m.e[3].index = MSR_SYSCALL_MASK; m.e[3].data = 0x3f7fd5;
  if (ioctl(fd, KVM_SET_MSRS, &m) < 4) { g_err = "KVM_SET_MSRS"; return -1; }

  tls.fd = fd;
  tls.run = run;
  tls.stack_top = (uintptr_t)stk + stksz;
  tls.loaded_cr3 = (uint64_t)(uintptr_t)g_pml4;
  if (!tls.active_pml4) tls.active_pml4 = g_pml4;
  tls.inited = 1;
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
  for (uintptr_t v = g_pt_base; v < g_pt_base + g_pt_bytes; v += GK_CHUNK)
    if (mmu_ensure_chunk(v) < 0) { g_err = "memslot for PT arena"; return -1; }
  // GDT/IDT and the handler text must be present before the first fault.
  if (mmu_map_range(g_gdt_va, g_gdt_va + 0x2000, 1 | 2) < 0) { g_err = "map gdt/idt"; return -1; }
  if (map_handler_text() < 0) { g_err = "map handler text"; return -1; }

  size_t nent = 128;
  g_cpuid = calloc(1, sizeof(*g_cpuid) + nent * sizeof(struct kvm_cpuid_entry2));
  g_cpuid->nent = nent;
  if (ioctl(g_kvm, KVM_GET_SUPPORTED_CPUID, g_cpuid) < 0) { g_err = "GET_SUPPORTED_CPUID"; return -1; }
  g_run_size = ioctl(g_kvm, KVM_GET_VCPU_MMAP_SIZE, 0);

  return vcpu_init();  // bring up the calling thread's vCPU
}

const char *gk_last_error(void) { return g_err; }
unsigned long gk_fault_addr(void) { return g_fault_addr; }
void gk_set_syscall_filter(gk_syscall_filter f) { g_filter = f; }

#define GK_EPERM 1

// ---- syscall forwarding ----------------------------------------------------
// Every syscall is re-issued on the host; guest and host share memory, so
// pointer arguments work directly. New mappings (mmap/brk/mprotect) are picked
// up lazily by demand paging on first access, so nothing special is needed here.
static long forward_syscall(struct kvm_regs *r) {
  long nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx, a4 = r->r10,
       a5 = r->r8, a6 = r->r9;
  if (g_filter && !g_filter(nr, a1, a2, a3, a4, a5, a6)) {
    if (g_dbg) fprintf(stderr, "[gk] syscall %ld DENIED\n", nr);
    return -GK_EPERM;
  }
  // pkey_mprotect strips to a plain mprotect: gk isolates via page tables and
  // arenas, so V8's host-side protection keys are not needed, and stripping them
  // lets KVM back the guest's writes (V8's default MPK code protection otherwise
  // faults, because KVM backs the write using the host PKRU, not the guest's).
  long ret;
  if (nr == SYS_pkey_mprotect)
    ret = host_syscall(SYS_mprotect, a1, a2, a3, 0, 0, 0);
  else
    ret = host_syscall(nr, a1, a2, a3, a4, a5, a6);
  // Reflect protection/mapping changes: drop the guest PTEs for the affected
  // range so the next access re-faults and demand-maps with the new host
  // permissions (this is what makes W^X and JIT code work), then flush this
  // vCPU's TLB. TODO: cross-vCPU shootdown for the multi-threaded case.
  if (ret == 0 && (nr == SYS_mprotect || nr == SYS_munmap || nr == SYS_pkey_mprotect) && a2 > 0) {
    pthread_mutex_lock(&g_lock);
    for (uintptr_t v = (uintptr_t)a1 & ~0xfffUL; v < (uintptr_t)a1 + (uintptr_t)a2; v += 0x1000)
      unmap4k_root(g_pml4, v);
    pthread_mutex_unlock(&g_lock);
    struct kvm_sregs s;
    ioctl(tls.fd, KVM_GET_SREGS, &s);
    ioctl(tls.fd, KVM_SET_SREGS, &s);  // reload CR3 -> flush non-global TLB
  }
  if (g_dbg) fprintf(stderr, "[gk] syscall %ld -> %ld\n", nr, ret);
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

long gk_run(long (*fn)(void *), void *arg) {
  if (vcpu_init() < 0) return -1;
  // Refresh the active arena's shared (non-cage) entries from the base root so
  // it sees every mapping added since the arena was created, keeping its cage.
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
  host_pkru_allow_all();
  uint64_t sp = tls.stack_top - 8;
  *(uint64_t *)sp = (uintptr_t)&gk_exit_tramp;

  struct kvm_regs regs = {0};
  regs.rip = (uintptr_t)fn;
  regs.rsp = sp;
  regs.rdi = (uintptr_t)arg;
  regs.rflags = 0x2;
  ioctl(tls.fd, KVM_SET_REGS, &regs);

  for (;;) {
    if (ioctl(tls.fd, KVM_RUN, 0) < 0) {
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
          if (cr2 == tls.last_fault) {
            if (++tls.fault_repeat > 2) { g_fault_addr = cr2; return GK_EFAULT; }
          } else { tls.last_fault = cr2; tls.fault_repeat = 0; }
          if (demand_map((uintptr_t)cr2) < 0) { g_fault_addr = cr2; return GK_EFAULT; }
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
  for (uint64_t v = va; v < va + size; v += 0x1000)
    if (mmu_map_into(a->pml4, v, 1 | 2) < 0) { free(a); pthread_mutex_unlock(&g_lock); return NULL; }
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
