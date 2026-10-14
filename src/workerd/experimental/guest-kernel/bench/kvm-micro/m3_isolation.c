// Milestone 3: per-isolate page tables. The core of the design.
//
// The guest runs one body of "runtime" code in ring 0. Two page-table roots
// stand for two isolates. Both roots map the runtime identically (shared
// subtree at PML4 entry 0), so the same code and stack execute under either.
// They differ only at PML4 entry 1, the "cage": one guest-virtual address,
// CAGE_VA, resolves to physical page PA under root A and to a different
// physical page PB under root B.
//
// The runtime reads CAGE_VA, switches CR3 to the other root, reads the same
// address again, and switches back. The byte it reads changes with CR3 even
// though the pointer never does. That is the hardware-enforced per-isolate
// memory view: with root A loaded, isolate B's cage page is not mapped by any
// virtual address the CPU can form, so it cannot be read or written. The CR3
// write is the switch primitive; on this CPU it does not trap to the host.
//
// Build: cc -O2 -o m3_isolation m3_isolation.c
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

// Guest-physical layout.
#define PML4_A      0x1000
#define PDPT_SHARED 0x2000   // runtime, shared by both roots (PML4 entry 0)
#define PD_SHARED   0x3000
#define CODE_ADDR   0x5000
#define PDPT_CAGE_A 0x7000   // isolate A's cage subtree (PML4 entry 1)
#define PD_CAGE_A   0x8000
#define PML4_B      0x9000
#define PDPT_CAGE_B 0xA000   // isolate B's cage subtree
#define PD_CAGE_B   0xB000
#define STACK_TOP   0x200000
#define CAGE_PA     0x400000 // isolate A's cage data, 2 MiB page
#define CAGE_PB     0x600000 // isolate B's cage data, 2 MiB page

// Cage virtual address: 512 GiB, i.e. PML4 entry 1, offset 0. Choosing a whole
// PML4 entry keeps the cage's page-table subtree entirely private per isolate
// while entry 0 (the runtime) stays shared, exactly as the plan describes.
#define CAGE_VA 0x8000000000ULL

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
  // Shared runtime subtree: identity-map the first 4 MiB with 2 MiB pages.
  ENT(mem, PDPT_SHARED, 0) = PD_SHARED | PTE_P | PTE_W;
  ENT(mem, PD_SHARED, 0) = 0x000000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PD_SHARED, 1) = 0x200000 | PTE_P | PTE_W | PTE_PS;

  // Cage index within its PML4 entry: CAGE_VA's PDPT/PD indices are 0.
  // Isolate A's cage subtree maps CAGE_VA -> CAGE_PA.
  ENT(mem, PDPT_CAGE_A, 0) = PD_CAGE_A | PTE_P | PTE_W;
  ENT(mem, PD_CAGE_A, 0) = CAGE_PA | PTE_P | PTE_W | PTE_PS;
  // Isolate B's cage subtree maps the same CAGE_VA -> CAGE_PB.
  ENT(mem, PDPT_CAGE_B, 0) = PD_CAGE_B | PTE_P | PTE_W;
  ENT(mem, PD_CAGE_B, 0) = CAGE_PB | PTE_P | PTE_W | PTE_PS;

  // Root A: entry 0 shared runtime, entry 1 cage A.
  ENT(mem, PML4_A, 0) = PDPT_SHARED | PTE_P | PTE_W;
  ENT(mem, PML4_A, 1) = PDPT_CAGE_A | PTE_P | PTE_W;
  // Root B: entry 0 the *same* shared runtime, entry 1 cage B.
  ENT(mem, PML4_B, 0) = PDPT_SHARED | PTE_P | PTE_W;
  ENT(mem, PML4_B, 1) = PDPT_CAGE_B | PTE_P | PTE_W;
}

static void setup_sregs(struct kvm_sregs *s) {
  s->cr3 = PML4_A;  // start in isolate A
  s->cr4 = CR4_PAE;
  s->cr0 = CR0_PE | CR0_PG;
  s->efer = EFER_LME | EFER_LMA;
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

  // Fill the two cage pages with distinct data from the host side. In the real
  // design these host virtual addresses are the isolate cages themselves.
  memset(mem + CAGE_PA, 'A', 0x1000);
  memset(mem + CAGE_PB, 'B', 0x1000);

  // Runtime payload (64-bit ring 0). Verified by disassembly in the run script.
  //   mov   rsi, CAGE_VA            48 BE 00 00 00 00 80 00 00 00
  //   movzx eax, byte [rsi]         0F B6 06
  //   out   0x10, al                E6 10            ; read cage under root A
  //   mov   rax, PML4_B             48 B8 00 90 00 00 00 00 00 00
  //   mov   cr3, rax                0F 22 D8         ; switch to isolate B
  //   movzx eax, byte [rsi]         0F B6 06
  //   out   0x10, al                E6 10            ; same pointer, root B
  //   mov   rax, PML4_A             48 B8 00 10 00 00 00 00 00 00
  //   mov   cr3, rax                0F 22 D8         ; switch back to isolate A
  //   movzx eax, byte [rsi]         0F B6 06
  //   out   0x10, al                E6 10            ; same pointer, root A
  //   hlt                           F4
  const uint8_t code[] = {
      0x48, 0xBE, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
      0x0F, 0xB6, 0x06, 0xE6, 0x10,
      0x48, 0xB8, 0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0F, 0x22, 0xD8, 0x0F, 0xB6, 0x06, 0xE6, 0x10,
      0x48, 0xB8, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0F, 0x22, 0xD8, 0x0F, 0xB6, 0x06, 0xE6, 0x10,
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
  setup_sregs(&sregs);
  if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) die("KVM_SET_SREGS");
  struct kvm_regs regs = {.rip = CODE_ADDR, .rsp = STACK_TOP, .rflags = 0x2};
  if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) die("KVM_SET_REGS");

  const char *label[] = {"root A (isolate A)", "root B (isolate B)",
                         "root A again"};
  int n = 0, ok = 1;
  char seen[3] = {0};
  for (;;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    switch (run->exit_reason) {
      case KVM_EXIT_IO:
        if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x10) {
          unsigned char b = *((unsigned char *)run + run->io.data_offset);
          if (n < 3) {
            printf("CAGE_VA (0x%llx) under %-18s -> '%c'\n",
                   (unsigned long long)CAGE_VA, label[n], b);
            seen[n] = b;
          }
          n++;
        }
        break;
      case KVM_EXIT_HLT:
        ok = (seen[0] == 'A' && seen[1] == 'B' && seen[2] == 'A' && n == 3);
        printf("\n%s: one pointer, three reads, memory selected by CR3 "
               "(A,B,A) = (%c,%c,%c)\n",
               ok ? "PASS" : "FAIL", seen[0], seen[1], seen[2]);
        return ok ? 0 : 1;
      case KVM_EXIT_SHUTDOWN:
        fprintf(stderr, "SHUTDOWN (triple fault)\n");
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
