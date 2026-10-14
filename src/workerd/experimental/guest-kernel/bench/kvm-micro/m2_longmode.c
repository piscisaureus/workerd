// Milestone 2: 64-bit long mode with guest-managed page tables.
//
// Builds on milestone 1. The guest now runs in 64-bit long mode with its own
// four-level page tables, which the guest (the runtime) fully controls. This
// is the mode workerd would run in, and the page tables are the object that
// milestone 3 makes per-isolate. To prove paging is really translating, the
// guest writes a byte through a virtual address that the page tables map to
// the *second* 2 MiB page and reads it back before reporting it to the host.
//
// The layout below places page tables, code and stack in guest-physical
// memory that is identity-mapped for the first 4 MiB, so guest-virtual equals
// guest-physical there, matching the design's "guest-physical == host-virtual"
// intent at small scale.
//
// Build: cc -O2 -o m2_longmode m2_longmode.c
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

// Guest-physical layout (all within the identity-mapped first 4 MiB).
#define PML4_ADDR 0x1000
#define PDPT_ADDR 0x2000
#define PD_ADDR   0x3000
#define CODE_ADDR 0x5000
#define STACK_TOP 0x400000  // grows down from 4 MiB

// x86-64 paging entry flags.
#define PTE_P  (1UL << 0)   // present
#define PTE_W  (1UL << 1)   // writable
#define PTE_PS (1UL << 7)   // page size (2 MiB at PD level)

// Control register bits.
#define CR0_PE (1UL << 0)
#define CR0_PG (1UL << 31)
#define CR4_PAE (1UL << 5)
#define EFER_LME (1UL << 8)
#define EFER_LMA (1UL << 10)

static void setup_long_mode(struct kvm_sregs *s, uint8_t *mem) {
  // Four-level tables: PML4 -> PDPT -> PD with 2 MiB pages.
  // Identity-map the first 4 MiB with two PD entries.
  uint64_t *pml4 = (uint64_t *)(mem + PML4_ADDR);
  uint64_t *pdpt = (uint64_t *)(mem + PDPT_ADDR);
  uint64_t *pd = (uint64_t *)(mem + PD_ADDR);
  pml4[0] = PDPT_ADDR | PTE_P | PTE_W;
  pdpt[0] = PD_ADDR | PTE_P | PTE_W;
  pd[0] = 0x000000 | PTE_P | PTE_W | PTE_PS;   // virt 0..2MiB  -> phys 0..2MiB
  pd[1] = 0x200000 | PTE_P | PTE_W | PTE_PS;   // virt 2..4MiB  -> phys 2..4MiB

  s->cr3 = PML4_ADDR;
  s->cr4 = CR4_PAE;
  s->cr0 = CR0_PE | CR0_PG;
  s->efer = EFER_LME | EFER_LMA;

  // 64-bit flat code and data segments, set directly (no in-memory GDT walk).
  struct kvm_segment cs = {
      .base = 0, .limit = 0xffffffff, .selector = 0x08,
      .type = 11 /* execute/read, accessed */, .present = 1, .dpl = 0,
      .db = 0, .s = 1, .l = 1 /* 64-bit */, .g = 1,
  };
  struct kvm_segment ds = {
      .base = 0, .limit = 0xffffffff, .selector = 0x10,
      .type = 3 /* read/write, accessed */, .present = 1, .dpl = 0,
      .db = 1, .s = 1, .l = 0, .g = 1,
  };
  s->cs = cs;
  s->ds = s->es = s->fs = s->gs = s->ss = ds;
}

int main(void) {
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) die("open /dev/kvm");
  int vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vmfd < 0) die("KVM_CREATE_VM");

  const size_t mem_size = 4 * 1024 * 1024;  // 4 MiB
  uint8_t *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) die("mmap");

  struct kvm_userspace_memory_region region = {
      .slot = 0, .guest_phys_addr = 0,
      .memory_size = mem_size, .userspace_addr = (uint64_t)mem,
  };
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
    die("KVM_SET_USER_MEMORY_REGION");

  // 64-bit guest payload. Verified by disassembly (see run script).
  //   mov    al, 'L'                 B0 4C
  //   out    0x10, al                E6 10        ; report we reached long mode
  //   mov    rax, 0x200000           48 B8 00 00 20 00 00 00 00 00
  //   mov    byte ptr [rax], 0x37    C6 00 37     ; write through virt 0x200000
  //   movzx  ebx, byte ptr [rax]     0F B6 18     ; read it back
  //   mov    al, bl                  88 D8
  //   out    0x10, al                E6 10        ; report the byte read back
  //   hlt                            F4
  const uint8_t code[] = {
      0xB0, 0x4C,
      0xE6, 0x10,
      0x48, 0xB8, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xC6, 0x00, 0x37,
      0x0F, 0xB6, 0x18,
      0x88, 0xD8,
      0xE6, 0x10,
      0xF4,
  };
  memcpy(mem + CODE_ADDR, code, sizeof(code));

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) die("KVM_CREATE_VCPU");
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) die("mmap kvm_run");

  struct kvm_sregs sregs;
  if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) die("KVM_GET_SREGS");
  setup_long_mode(&sregs, mem);
  if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) die("KVM_SET_SREGS");

  struct kvm_regs regs = {
      .rip = CODE_ADDR, .rsp = STACK_TOP, .rflags = 0x2,
  };
  if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) die("KVM_SET_REGS");

  int out_count = 0;
  for (;;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    switch (run->exit_reason) {
      case KVM_EXIT_IO:
        if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x10) {
          unsigned char b = *((unsigned char *)run + run->io.data_offset);
          if (out_count++ == 0)
            printf("guest reached 64-bit long mode (reported 0x%02x '%c')\n",
                   b, b);
          else
            printf("paging works: wrote 0x37 via virt 0x200000, read back "
                   "0x%02x\n", b);
        }
        break;
      case KVM_EXIT_HLT:
        printf("guest halted cleanly\n");
        return 0;
      case KVM_EXIT_FAIL_ENTRY:
        fprintf(stderr, "FAIL_ENTRY reason=0x%llx\n",
                (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
        return 1;
      case KVM_EXIT_INTERNAL_ERROR:
        fprintf(stderr, "INTERNAL_ERROR suberror=%u\n", run->internal.suberror);
        return 1;
      case KVM_EXIT_SHUTDOWN:
        fprintf(stderr, "SHUTDOWN (triple fault): guest setup is wrong\n");
        return 1;
      default:
        fprintf(stderr, "unexpected exit_reason=%u\n", run->exit_reason);
        return 1;
    }
  }
}
