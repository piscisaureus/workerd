// Milestone 8: Dune-style guest enter. Run a real compiled function in guest.
//
// The previous milestones ran hand-encoded payloads at made-up addresses. This
// one runs an ordinary compiled C function, guest_entry(), inside the VM at its
// real address, with the process's own memory visible to the guest. That is the
// leap that makes running the real runtime conceivable.
//
// The mechanism is the Dune mapping: guest-physical address == host-virtual
// address. Each KVM memslot maps a range of guest-physical to the same
// host-virtual range, and the guest page tables map guest-virtual to the equal
// guest-physical, so a guest pointer and a host pointer are the same number and
// name the same bytes. The host maps its own text (so the function's code is
// visible) and a scratch region (page tables, stack, buffers), builds page
// tables covering the ranges the function touches, points LSTAR at a
// trampoline, and enters with RIP at guest_entry.
//
// guest_entry does write(1, msg, len) and getpid() with raw syscall
// instructions; both are forwarded to the host. Because the mapping is a true
// identity of addresses, the host services the write using the guest's pointer
// value directly, with no offset arithmetic.
//
// Build: cc -O2 -fno-stack-protector -fcf-protection=none -o m8_guest_enter m8_guest_enter.c
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static void die(const char *m) { perror(m); exit(1); }

#define PTE_P (1UL << 0)
#define PTE_W (1UL << 1)
#define HYPERCALL_PORT 0x11
#define MSR_STAR 0xc0000081
#define MSR_LSTAR 0xc0000082
#define MSR_SYSCALL_MASK 0xc0000084

// The function that runs inside the guest. Ordinary compiled C; it uses raw
// syscall instructions so it needs no libc, TLS or GOT to be mapped.
__attribute__((noinline)) static void guest_entry(volatile uint64_t *res,
                                                  const char *msg,
                                                  unsigned long len) {
  long w;
  __asm__ __volatile__("syscall"
                       : "=a"(w)
                       : "a"(1L), "D"(1L), "S"(msg), "d"(len)
                       : "rcx", "r11", "memory");
  res[0] = (uint64_t)w;
  long pid;
  __asm__ __volatile__("syscall"
                       : "=a"(pid)
                       : "a"(39L)
                       : "rcx", "r11", "memory");
  res[1] = (uint64_t)pid;
  __asm__ __volatile__("hlt");
  for (;;) {
  }
}

// LSTAR trampoline: reached by the `syscall` instruction. rcx holds the return
// address. Issue the hypercall, then jump back to the caller.
__attribute__((naked, noinline)) static void trampoline(void) {
  __asm__ __volatile__("outb %al, $0x11\n\t"
                       "jmpq *%rcx\n\t");
}

// Page-table bump allocator over the scratch region, and a 4 KiB mapper that
// sets guest-physical == guest-virtual (both == host-virtual).
static uint64_t *g_pml4;
static uint8_t *g_pt_next;
static uint8_t *g_pt_end;

static uint64_t *alloc_table(void) {
  if (g_pt_next + 0x1000 > g_pt_end) { fprintf(stderr, "PT arena full\n"); exit(1); }
  uint64_t *p = (uint64_t *)g_pt_next;
  memset(p, 0, 0x1000);
  g_pt_next += 0x1000;
  return p;
}

static uint64_t *next_table(uint64_t *tbl, int idx) {
  if (!(tbl[idx] & PTE_P)) {
    uint64_t *n = alloc_table();
    tbl[idx] = ((uint64_t)(uintptr_t)n) | PTE_P | PTE_W;
  }
  return (uint64_t *)(uintptr_t)(tbl[idx] & ~0xfffULL);
}

static void map4k(uint64_t va) {
  uint64_t *pdpt = next_table(g_pml4, (va >> 39) & 0x1ff);
  uint64_t *pd = next_table(pdpt, (va >> 30) & 0x1ff);
  uint64_t *pt = next_table(pd, (va >> 21) & 0x1ff);
  pt[(va >> 12) & 0x1ff] = (va & ~0xfffULL) | PTE_P | PTE_W;  // phys == va
}

static void map_range(uintptr_t s, uintptr_t e) {
  for (uintptr_t v = s & ~0xfffUL; v < e; v += 0x1000) map4k(v);
}

int main(void) {
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) die("open /dev/kvm");
  int vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vmfd < 0) die("KVM_CREATE_VM");

  // Scratch region: page tables, stack, and the message/result buffers.
  const size_t span = 16 * 1024 * 1024;
  uint8_t *lo = mmap(NULL, span, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (lo == MAP_FAILED) die("mmap scratch");
  uintptr_t L = (uintptr_t)lo;

  g_pt_next = lo;                 // first 8 MiB: page-table arena
  g_pt_end = lo + 8 * 1024 * 1024;
  g_pml4 = alloc_table();
  uintptr_t MSG = L + 8 * 1024 * 1024;
  uintptr_t RES = MSG + 0x1000;
  uintptr_t STACK_TOP = L + span;

  const char text[] = "hello from a compiled function inside the guest\n";
  uint32_t len = (uint32_t)strlen(text);
  memcpy((void *)MSG, text, len);

  // Find this binary's executable region that holds guest_entry.
  uintptr_t ge = (uintptr_t)&guest_entry, tr = (uintptr_t)&trampoline;
  uintptr_t tstart = 0, tend = 0;
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) die("open maps");
  char line[512];
  while (fgets(line, sizeof line, f)) {
    uintptr_t s, e;
    if (sscanf(line, "%lx-%lx", &s, &e) == 2 && ge >= s && ge < e) {
      tstart = s; tend = e; break;
    }
  }
  fclose(f);
  if (!tstart) { fprintf(stderr, "text region not found\n"); return 1; }
  if (tr < tstart || tr >= tend) { fprintf(stderr, "trampoline not in text region\n"); return 1; }

  // Memslots: scratch and text, each guest-physical == host-virtual.
  struct kvm_userspace_memory_region s0 = {
      .slot = 0, .guest_phys_addr = L, .memory_size = span, .userspace_addr = L};
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &s0) < 0) die("memslot scratch");
  struct kvm_userspace_memory_region s1 = {
      .slot = 1, .guest_phys_addr = tstart, .memory_size = tend - tstart,
      .userspace_addr = tstart};
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &s1) < 0) die("memslot text");

  // Guest page tables cover the text, the stack, and the buffers.
  map_range(tstart, tend);
  map_range(MSG, STACK_TOP);

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) die("KVM_CREATE_VCPU");

  // Give the guest the host CPUID so its physical-address width matches the
  // host's. Without this the guest MAXPHYADDR is too small for the ~47-bit
  // host-virtual addresses this Dune mapping uses as guest-physical.
  {
    size_t n = 128;
    struct kvm_cpuid2 *cpuid =
        calloc(1, sizeof(*cpuid) + n * sizeof(struct kvm_cpuid_entry2));
    cpuid->nent = n;
    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid) < 0) die("GET_SUPPORTED_CPUID");
    if (ioctl(vcpu, KVM_SET_CPUID2, cpuid) < 0) die("KVM_SET_CPUID2");
    free(cpuid);
  }

  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);

  struct kvm_sregs sregs;
  ioctl(vcpu, KVM_GET_SREGS, &sregs);
  sregs.cr3 = (uint64_t)(uintptr_t)g_pml4;
  sregs.cr4 = (1UL << 5);                             // PAE
  sregs.cr0 = (1UL << 0) | (1UL << 31);               // PE | PG
  sregs.efer = (1UL << 8) | (1UL << 10) | (1UL << 0); // LME | LMA | SCE
  struct kvm_segment cs = {.base = 0, .limit = 0xffffffff, .selector = 0x08,
                           .type = 11, .present = 1, .s = 1, .l = 1, .g = 1};
  struct kvm_segment ds = {.base = 0, .limit = 0xffffffff, .selector = 0x10,
                           .type = 3, .present = 1, .s = 1, .db = 1, .g = 1};
  sregs.cs = cs; sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = ds;
  if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) die("KVM_SET_SREGS");

  struct { struct kvm_msrs h; struct kvm_msr_entry e[3]; } m = {0};
  m.h.nmsrs = 3;
  m.e[0].index = MSR_STAR; m.e[0].data = (uint64_t)0x08 << 32;
  m.e[1].index = MSR_LSTAR; m.e[1].data = tr;
  m.e[2].index = MSR_SYSCALL_MASK; m.e[2].data = 0;
  if (ioctl(vcpu, KVM_SET_MSRS, &m) < 3) die("KVM_SET_MSRS");

  // Enter guest_entry(res, msg, len). RSP mimics the post-call alignment.
  struct kvm_regs regs = {0};
  regs.rip = ge;
  regs.rsp = STACK_TOP - 8;
  regs.rflags = 0x2;
  regs.rdi = RES;
  regs.rsi = MSG;
  regs.rdx = len;
  if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) die("KVM_SET_REGS");

  int forwarded = 0;
  for (;;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    if (run->exit_reason == KVM_EXIT_IO &&
        run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == HYPERCALL_PORT) {
      struct kvm_regs r;
      ioctl(vcpu, KVM_GET_REGS, &r);
      long ret;
      switch (r.rax) {
        case SYS_write:
          // Dune identity: the guest pointer value is a valid host pointer.
          ret = write((int)r.rdi, (void *)(uintptr_t)r.rsi, (size_t)r.rdx);
          break;
        case SYS_getpid:
          ret = getpid();
          break;
        default:
          ret = -ENOSYS;
      }
      r.rax = (uint64_t)ret;
      ioctl(vcpu, KVM_SET_REGS, &r);
      forwarded++;
      continue;
    }
    if (run->exit_reason == KVM_EXIT_HLT) break;
    if (run->exit_reason == KVM_EXIT_SHUTDOWN) {
      fprintf(stderr, "SHUTDOWN (triple fault)\n"); return 1; }
    if (run->exit_reason == KVM_EXIT_FAIL_ENTRY) {
      fprintf(stderr, "FAIL_ENTRY 0x%llx\n",
              (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
      return 1; }
    fprintf(stderr, "unexpected exit %u\n", run->exit_reason); return 1;
  }

  uint64_t wr = *(uint64_t *)RES, pid = *(uint64_t *)(RES + 8);
  int ok = (forwarded == 2) && (wr == len) && (pid == (uint64_t)getpid());
  printf("compiled guest_entry() ran inside the VM at its real address 0x%lx\n", ge);
  printf("forwarded %d syscalls; write() -> %llu (len %u), getpid() -> %llu "
         "(host %d)\n", forwarded, (unsigned long long)wr, len,
         (unsigned long long)pid, getpid());
  printf("\n%s: a compiled function ran in guest ring 0 and its syscalls were "
         "serviced by the host\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
