// gk implementation. See gk.h.
#define _GNU_SOURCE
#include "gk.h"

#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// ---- x86-64 paging and control-register bits -------------------------------
#define PTE_P (1UL << 0)
#define PTE_W (1UL << 1)
#define PTE_U (1UL << 2)
#define CR0_PE (1UL << 0)
#define CR0_PG (1UL << 31)
#define CR4_PAE (1UL << 5)
#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)
#define CR4_OSXSAVE (1UL << 18)
#define EFER_LME (1UL << 8)
#define EFER_LMA (1UL << 10)
#define EFER_SCE (1UL << 0)

#define MSR_EFER 0xc0000080
#define MSR_STAR 0xc0000081
#define MSR_LSTAR 0xc0000082
#define MSR_SYSCALL_MASK 0xc0000084

// Hypercall ports.
#define PORT_SYSCALL 0x11
#define PORT_EXIT 0xFF
#define PORT_FAULT 0xFE
#define PORT_UDRIP 0xFD

// ---- global platform state (set once by gk_init) ---------------------------
static int g_kvm = -1, g_vmfd = -1;
static uint64_t *g_pml4;             // base page-table root
static uint8_t *g_pt_next, *g_pt_end;
static uintptr_t g_pt_base;
static uint64_t g_gdt_va, g_idt_va;
static int g_next_slot;
static int g_run_size;
static struct kvm_cpuid2 *g_cpuid;   // host CPUID, applied to every vCPU
static const char *g_err;
static unsigned long g_fault_addr;
static uint64_t g_brk;
static int g_dbg;
static gk_syscall_filter g_filter;
static int g_next_cage_idx = 64;
static atomic_int g_next_vcpu_id;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Per-thread guest state: each host thread that enters the guest is a vCPU.
typedef struct {
  int inited;
  int fd;
  struct kvm_run *run;
  uint64_t stack_top;
  uint64_t loaded_cr3;
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

// The following table helpers must be called with g_lock held (they mutate the
// shared page-table arena and slot counter), except during single-threaded
// gk_init.
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

static int map4k_root(uint64_t *root, uint64_t va, uint64_t flags) {
  uint64_t *pdpt = next_table(root, (va >> 39) & 0x1ff);
  if (!pdpt) return -1;
  uint64_t *pd = next_table(pdpt, (va >> 30) & 0x1ff);
  if (!pd) return -1;
  uint64_t *pt = next_table(pd, (va >> 21) & 0x1ff);
  if (!pt) return -1;
  pt[(va >> 12) & 0x1ff] = (va & ~0xfffULL) | PTE_P | PTE_U | flags;
  return 0;
}

static int map4k(uint64_t va, uint64_t flags) { return map4k_root(g_pml4, va, flags); }

static int map_range(uintptr_t s, uintptr_t e, uint64_t flags) {
  for (uintptr_t v = s & ~0xfffUL; v < e; v += 0x1000)
    if (map4k(v, flags) < 0) return -1;
  return 0;
}

static int add_memslot(uintptr_t addr, size_t len) {
  struct kvm_userspace_memory_region r = {.slot = g_next_slot++,
                                          .guest_phys_addr = addr,
                                          .memory_size = len,
                                          .userspace_addr = addr};
  return ioctl(g_vmfd, KVM_SET_USER_MEMORY_REGION, &r);
}

// Add a runtime-discovered region into the base root. Takes the lock.
static int add_region_locked(uintptr_t addr, size_t len, uint64_t flags) {
  if (add_memslot(addr, len) < 0) return -1;
  return map_range(addr, addr + len, flags);
}
static int add_region(uintptr_t addr, size_t len, uint64_t flags) {
  pthread_mutex_lock(&g_lock);
  int r = add_region_locked(addr, len, flags);
  pthread_mutex_unlock(&g_lock);
  return r;
}

// ---- init ------------------------------------------------------------------
static int map_self_maps(void) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) { g_err = "open /proc/self/maps"; return -1; }
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    uintptr_t s, e;
    char perms[8] = {0}, path[512] = {0};
    int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &s, &e, perms, path);
    if (n < 3) continue;
    if (perms[0] != 'r') continue;
    if (strstr(path, "[vsyscall]")) continue;
    uint64_t flags = (perms[1] == 'w') ? PTE_W : 0;
    if (add_memslot(s, e - s) < 0) { fclose(f); g_err = "memslot for maps"; return -1; }
    if (s == g_pt_base) continue;  // walker reaches PT pages by physical addr
    if (map_range(s, e, flags) < 0) { fclose(f); g_err = "page tables for maps"; return -1; }
  }
  fclose(f);
  return 0;
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

  // A private guest stack for this vCPU.
  const size_t stksz = 2 * 1024 * 1024;
  uint8_t *stk = mmap(NULL, stksz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (stk == MAP_FAILED) { g_err = "mmap guest stack"; return -1; }
  if (add_region((uintptr_t)stk, stksz, PTE_W) < 0) { g_err = "map guest stack"; return -1; }

  struct kvm_sregs s;
  ioctl(fd, KVM_GET_SREGS, &s);
  s.cr3 = (uint64_t)(uintptr_t)g_pml4;
  s.cr4 = CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE;
  s.cr0 = CR0_PE | CR0_PG;
  s.efer = EFER_LME | EFER_LMA | EFER_SCE;
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
  m.e[0].index = MSR_EFER; m.e[0].data = EFER_LME | EFER_LMA | EFER_SCE;
  m.e[1].index = MSR_STAR; m.e[1].data = (uint64_t)0x08 << 32;
  m.e[2].index = MSR_LSTAR; m.e[2].data = (uintptr_t)&gk_syscall_tramp;
  m.e[3].index = MSR_SYSCALL_MASK; m.e[3].data = 0x3f7fd5;
  if (ioctl(fd, KVM_SET_MSRS, &m) < 4) { g_err = "KVM_SET_MSRS"; return -1; }

  tls.fd = fd;
  tls.run = run;
  tls.stack_top = (uintptr_t)stk + stksz;
  tls.loaded_cr3 = (uint64_t)(uintptr_t)g_pml4;
  // Respect an arena already selected (gk_arena_enter may run before the
  // thread's first gk_run creates this vCPU).
  if (!tls.active_pml4) tls.active_pml4 = g_pml4;
  tls.inited = 1;
  return 0;
}

int gk_init(void) {
  g_dbg = getenv("GK_DEBUG") != NULL;
  g_kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (g_kvm < 0) { g_err = "open /dev/kvm"; return -1; }
  if (ioctl(g_kvm, KVM_GET_API_VERSION, 0) != 12) { g_err = "KVM API != 12"; return -1; }
  g_vmfd = ioctl(g_kvm, KVM_CREATE_VM, 0);
  if (g_vmfd < 0) { g_err = "KVM_CREATE_VM"; return -1; }

  const size_t pt_bytes = 64 * 1024 * 1024;
  uint8_t *pt = mmap(NULL, pt_bytes, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (pt == MAP_FAILED) { g_err = "mmap PT arena"; return -1; }
  g_pt_next = pt; g_pt_end = pt + pt_bytes; g_pt_base = (uintptr_t)pt;
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

  if (map_self_maps() < 0) return -1;

  size_t nent = 128;
  g_cpuid = calloc(1, sizeof(*g_cpuid) + nent * sizeof(struct kvm_cpuid_entry2));
  g_cpuid->nent = nent;
  if (ioctl(g_kvm, KVM_GET_SUPPORTED_CPUID, g_cpuid) < 0) { g_err = "GET_SUPPORTED_CPUID"; return -1; }
  g_run_size = ioctl(g_kvm, KVM_GET_VCPU_MMAP_SIZE, 0);

  g_brk = (uint64_t)host_syscall(SYS_brk, 0, 0, 0, 0, 0, 0);

  return vcpu_init();  // bring up the calling thread's vCPU
}

const char *gk_last_error(void) { return g_err; }
unsigned long gk_fault_addr(void) { return g_fault_addr; }
void gk_set_syscall_filter(gk_syscall_filter f) { g_filter = f; }

#define GK_EPERM 1

// ---- syscall forwarding ----------------------------------------------------
static long forward_syscall(struct kvm_regs *r) {
  long nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx, a4 = r->r10,
       a5 = r->r8, a6 = r->r9;
  long ret;
  if (g_filter && !g_filter(nr, a1, a2, a3, a4, a5, a6)) {
    if (g_dbg) fprintf(stderr, "[gk] syscall %ld DENIED\n", nr);
    return -GK_EPERM;
  }
  switch (nr) {
    case SYS_mmap:
      ret = host_syscall(nr, a1, a2, a3, a4, a5, a6);
      if (ret >= 0 || ret < -4095) {
        size_t len = ((size_t)a2 + 0xfff) & ~0xfffUL;
        uint64_t f = (a3 & PROT_WRITE) ? PTE_W : 0;
        add_region((uintptr_t)ret, len, f);
      }
      break;
    case SYS_brk:
      pthread_mutex_lock(&g_lock);
      ret = host_syscall(nr, a1, 0, 0, 0, 0, 0);
      if ((uint64_t)ret > g_brk) {
        add_region_locked(g_brk, (size_t)((uint64_t)ret - g_brk), PTE_W);
        g_brk = (uint64_t)ret;
      }
      pthread_mutex_unlock(&g_lock);
      break;
    default:
      ret = host_syscall(nr, a1, a2, a3, a4, a5, a6);
  }
  if (g_dbg)
    fprintf(stderr, "[gk] syscall %ld -> %ld\n", nr, ret);
  return ret;
}

long gk_run(long (*fn)(void *), void *arg) {
  if (vcpu_init() < 0) return -1;
  // Refresh the active arena's shared (non-cage) entries from the base root so
  // it sees every mapping added since the arena was created (thread stacks,
  // runtime mmaps), while keeping its private cage entry.
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
  uint64_t sp = tls.stack_top - 8;
  *(uint64_t *)sp = (uintptr_t)&gk_exit_tramp;

  struct kvm_regs regs = {0};
  regs.rip = (uintptr_t)fn;
  regs.rsp = sp;
  regs.rdi = (uintptr_t)arg;
  regs.rflags = 0x2;
  ioctl(tls.fd, KVM_SET_REGS, &regs);

  long result = 0;
  for (;;) {
    if (ioctl(tls.fd, KVM_RUN, 0) < 0) return -1;
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
        } else if (port == PORT_FAULT) {
          struct kvm_sregs s;
          ioctl(tls.fd, KVM_GET_SREGS, &s);
          g_fault_addr = s.cr2;
          return GK_EFAULT;
        }
        break;
      }
      case KVM_EXIT_HLT:
        return result;
      case KVM_EXIT_SHUTDOWN:
        return GK_ESHUTDOWN;
      default:
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
  if (add_memslot(va, size) < 0) { munmap(mem, size); pthread_mutex_unlock(&g_lock); return NULL; }
  gk_arena *a = calloc(1, sizeof *a);
  if (!a) { munmap(mem, size); pthread_mutex_unlock(&g_lock); return NULL; }
  a->base = mem;
  a->size = size;
  a->pml4 = alloc_table();
  a->cage_idx = (int)((va >> 39) & 0x1ff);
  memcpy(a->pml4, g_pml4, 0x1000);  // share the base root's top-level entries
  for (uint64_t v = va; v < va + size; v += 0x1000)
    if (map4k_root(a->pml4, v, PTE_W) < 0) { free(a); pthread_mutex_unlock(&g_lock); return NULL; }
  a->cage_entry = a->pml4[a->cage_idx];  // remember the private cage subtree
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
