// Milestone 4: the isolation boundary, enforced by hardware and handled by the
// runtime. Extends milestone 3.
//
// Isolate B has a private "secret" page mapped at SECRET_VA, present only in
// root B. Root A does not map SECRET_VA at all (its PML4 entry is absent). The
// guest sets up its own interrupt descriptor table with a page-fault handler,
// which stands for the runtime's fault path. The runtime reads SECRET_VA under
// root B (allowed), switches CR3 to isolate A, and reads SECRET_VA again. That
// second read has no valid translation, so the CPU raises #PF and vectors to
// the guest's own handler. The handler is the runtime catching an isolate's
// out-of-bounds access; in workerd it would raise SIGSEGV to the isolate or an
// EFAULT to a forwarded syscall, matching what the kernel does for MPK today.
//
// This is the property the whole design is for: with root A loaded, isolate B's
// memory is not merely permission-denied, it is unaddressable.
//
// Build: cc -O2 -o m4_fault m4_fault.c
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void die(const char *m) { perror(m); exit(1); }

#define PML4_A       0x1000
#define PDPT_SHARED  0x2000
#define PD_SHARED    0x3000
#define IDT_ADDR     0x4000
#define CODE_ADDR    0x5000
#define GDT_ADDR     0x6000
#define HANDLER_ADDR 0x5100
#define PML4_B       0x9000
#define PDPT_SEC_B   0xC000
#define PD_SEC_B     0xD000
#define STACK_TOP    0x200000
#define SECRET_PB    0x800000  // isolate B's secret data, 2 MiB page

// SECRET_VA = 1 TiB = PML4 entry 2, lower indices 0.
#define SECRET_VA 0x10000000000ULL

#define PTE_P  (1UL << 0)
#define PTE_W  (1UL << 1)
#define PTE_PS (1UL << 7)
#define CR0_PE (1UL << 0)
#define CR0_PG (1UL << 31)
#define CR4_PAE (1UL << 5)
#define EFER_LME (1UL << 8)
#define EFER_LMA (1UL << 10)
#define ENT(mem, base, idx) (((uint64_t *)((mem) + (base)))[(idx)])

static void build_tables(uint8_t *mem) {
  // Shared runtime subtree: identity-map first 4 MiB (covers code, IDT, tables,
  // stack) so the runtime and its fault handler are mapped under either root.
  ENT(mem, PDPT_SHARED, 0) = PD_SHARED | PTE_P | PTE_W;
  ENT(mem, PD_SHARED, 0) = 0x000000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PD_SHARED, 1) = 0x200000 | PTE_P | PTE_W | PTE_PS;

  // Isolate B's private secret subtree: SECRET_VA -> SECRET_PB.
  ENT(mem, PDPT_SEC_B, 0) = PD_SEC_B | PTE_P | PTE_W;
  ENT(mem, PD_SEC_B, 0) = SECRET_PB | PTE_P | PTE_W | PTE_PS;

  ENT(mem, PML4_A, 0) = PDPT_SHARED | PTE_P | PTE_W;  // runtime
  // PML4_A entry 2 deliberately left absent: root A cannot form SECRET_VA.
  ENT(mem, PML4_B, 0) = PDPT_SHARED | PTE_P | PTE_W;  // same runtime
  ENT(mem, PML4_B, 2) = PDPT_SEC_B | PTE_P | PTE_W;   // isolate B's secret
}

static void build_gdt(uint8_t *mem) {
  // Minimal GDT: interrupt delivery reloads CS from selector 0x08, so the
  // descriptor must exist in memory even though initial execution uses the
  // cached segment registers set through kvm_sregs.
  uint64_t *g = (uint64_t *)(mem + GDT_ADDR);
  g[0] = 0;                        // null
  g[1] = 0x00AF9A000000FFFFULL;    // 0x08: 64-bit code, present, DPL0
  g[2] = 0x00CF92000000FFFFULL;    // 0x10: data, present, DPL0
}

static void build_idt(uint8_t *mem, uint64_t handler_va) {
  // 64-bit interrupt gate for vector 14 (#PF): present, DPL0, selector 0x08.
  uint8_t *e = mem + IDT_ADDR + 14 * 16;
  memset(mem + IDT_ADDR, 0, 256 * 16);
  *(uint16_t *)(e + 0) = handler_va & 0xffff;
  *(uint16_t *)(e + 2) = 0x08;
  e[4] = 0;
  e[5] = 0x8e;  // present, interrupt gate
  *(uint16_t *)(e + 6) = (handler_va >> 16) & 0xffff;
  *(uint32_t *)(e + 8) = (handler_va >> 32) & 0xffffffff;
}

static void setup_sregs(struct kvm_sregs *s) {
  s->cr3 = PML4_B;  // start in isolate B, which can see its secret
  s->cr4 = CR4_PAE;
  s->cr0 = CR0_PE | CR0_PG;
  s->efer = EFER_LME | EFER_LMA;
  s->idt.base = IDT_ADDR;
  s->idt.limit = 256 * 16 - 1;
  s->gdt.base = GDT_ADDR;
  s->gdt.limit = 3 * 8 - 1;
  struct kvm_segment cs = {.base = 0, .limit = 0xffffffff, .selector = 0x08,
                           .type = 11, .present = 1, .s = 1, .l = 1, .g = 1};
  struct kvm_segment ds = {.base = 0, .limit = 0xffffffff, .selector = 0x10,
                           .type = 3, .present = 1, .s = 1, .db = 1, .g = 1};
  s->cs = cs;
  s->ds = s->es = s->fs = s->gs = s->ss = ds;
}

int main(void) {
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) die("open /dev/kvm");
  int vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vmfd < 0) die("KVM_CREATE_VM");

  const size_t mem_size = 16 * 1024 * 1024;
  uint8_t *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) die("mmap");
  struct kvm_userspace_memory_region region = {
      .slot = 0, .guest_phys_addr = 0,
      .memory_size = mem_size, .userspace_addr = (uint64_t)mem};
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
    die("KVM_SET_USER_MEMORY_REGION");

  build_tables(mem);
  build_gdt(mem);
  build_idt(mem, HANDLER_ADDR);
  memset(mem + SECRET_PB, 'S', 0x1000);

  // Runtime main payload (starts under root B):
  //   mov   rsi, SECRET_VA          48 BE 00 00 00 00 00 01 00 00
  //   movzx eax, byte [rsi]         0F B6 06
  //   out   0x10, al                E6 10        ; secret readable under B
  //   mov   rax, PML4_A             48 B8 00 10 00 00 00 00 00 00
  //   mov   cr3, rax                0F 22 D8     ; switch to isolate A
  //   movzx eax, byte [rsi]         0F B6 06     ; #PF: unmapped under A
  //   out   0x10, al                E6 10        ; (never reached)
  //   hlt                           F4
  const uint8_t code[] = {
      0x48, 0xBE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
      0x0F, 0xB6, 0x06, 0xE6, 0x10,
      0x48, 0xB8, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0F, 0x22, 0xD8,
      0x0F, 0xB6, 0x06, 0xE6, 0x10,
      0xF4,
  };
  memcpy(mem + CODE_ADDR, code, sizeof(code));

  // Fault handler (the runtime's #PF path): report 'F' and halt without return.
  //   mov al, 'F'    B0 46
  //   out 0x10, al   E6 10
  //   hlt            F4
  const uint8_t handler[] = {0xB0, 0x46, 0xE6, 0x10, 0xF4};
  memcpy(mem + HANDLER_ADDR, handler, sizeof(handler));

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) die("KVM_CREATE_VCPU");
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) die("mmap kvm_run");

  struct kvm_sregs sregs;
  if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) die("KVM_GET_SREGS");
  setup_sregs(&sregs);
  if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) die("KVM_SET_SREGS");
  struct kvm_regs regs = {.rip = CODE_ADDR, .rsp = STACK_TOP, .rflags = 0x2};
  if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) die("KVM_SET_REGS");

  int secret_read = 0, fault_caught = 0;
  for (;;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    switch (run->exit_reason) {
      case KVM_EXIT_IO:
        if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x10) {
          unsigned char b = *((unsigned char *)run + run->io.data_offset);
          if (b == 'S') {
            secret_read = 1;
            printf("under root B: read isolate B's secret at 0x%llx -> '%c'\n",
                   (unsigned long long)SECRET_VA, b);
          } else if (b == 'F') {
            fault_caught = 1;
            printf("under root A: same address 0x%llx raised #PF; runtime "
                   "handler caught it (isolate B's page is unaddressable)\n",
                   (unsigned long long)SECRET_VA);
          }
        }
        break;
      case KVM_EXIT_HLT: {
        int ok = secret_read && fault_caught;
        printf("\n%s: isolate B's secret is readable under root B and faults "
               "under root A\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
      }
      case KVM_EXIT_SHUTDOWN:
        fprintf(stderr, "SHUTDOWN (triple fault): the fault was not handled\n");
        return 1;
      case KVM_EXIT_INTERNAL_ERROR:
        fprintf(stderr, "INTERNAL_ERROR suberror=%u\n", run->internal.suberror);
        return 1;
      default:
        fprintf(stderr, "unexpected exit_reason=%u\n", run->exit_reason);
        return 1;
    }
  }
}
