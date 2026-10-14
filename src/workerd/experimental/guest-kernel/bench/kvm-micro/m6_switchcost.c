// Milestone 6: CR3 switch cost. The other half of Gate 1.
//
// The isolate switch primitive in this design is a guest CR3 write, which does
// not trap to the host. This measures it from inside the guest with RDTSC:
// M iterations, each writing CR3 to root A then root B, both of which map the
// runtime identically so execution continues across the switch. The two roots
// differ, so each write flushes non-global TLB entries and the next fetch
// re-walks; that TLB refill is part of the cost and is what marking runtime
// pages global would remove. Contrast the result with the VM-exit cost from
// milestone 5.
//
// Build: cc -O2 -o m6_switchcost m6_switchcost.c
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
#define PML4_A 0x1000
#define PDPT_S 0x2000
#define PD_S   0x3000
#define CODE   0x5000
#define SCRATCH 0x7000
#define PML4_B 0x9000
#define STACK_TOP 0x200000
#define GHZ 2.85
#define PTE_P (1UL<<0)
#define PTE_W (1UL<<1)
#define PTE_PS (1UL<<7)
#define ENT(mem,base,idx) (((uint64_t*)((mem)+(base)))[(idx)])

int main(void) {
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) die("open /dev/kvm");
  int vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
  const size_t mem_size = 4 * 1024 * 1024;
  uint8_t *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) die("mmap");
  struct kvm_userspace_memory_region region = {
      .slot = 0, .guest_phys_addr = 0,
      .memory_size = mem_size, .userspace_addr = (uint64_t)mem};
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) die("memregion");

  // Two roots, both identity-mapping the first 4 MiB via a shared subtree.
  ENT(mem, PDPT_S, 0) = PD_S | PTE_P | PTE_W;
  ENT(mem, PD_S, 0) = 0x000000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PD_S, 1) = 0x200000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PML4_A, 0) = PDPT_S | PTE_P | PTE_W;
  ENT(mem, PML4_B, 0) = PDPT_S | PTE_P | PTE_W;

  const uint32_t M = 50000;
  //   mov  rdi, SCRATCH        48 BF 00 70 00 00 00 00 00 00
  //   rdtsc                    0F 31
  //   mov  [rdi], eax          89 07
  //   mov  [rdi+4], edx        89 57 04
  //   mov  ecx, M              B9 <M32>
  // L:mov  rax, PML4_A         48 B8 00 10 00 00 00 00 00 00
  //   mov  cr3, rax            0F 22 D8
  //   mov  rax, PML4_B         48 B8 00 90 00 00 00 00 00 00
  //   mov  cr3, rax            0F 22 D8
  //   dec  ecx                 FF C9
  //   jnz  L                   75 E2
  //   rdtsc                    0F 31
  //   mov  [rdi+8], eax        89 47 08
  //   mov  [rdi+12], edx       89 57 0C
  //   hlt                      F4
  uint8_t code[] = {
      0x48,0xBF,0x00,0x70,0x00,0x00,0x00,0x00,0x00,0x00,
      0x0F,0x31, 0x89,0x07, 0x89,0x57,0x04,
      0xB9,0,0,0,0,
      0x48,0xB8,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
      0x0F,0x22,0xD8,
      0x48,0xB8,0x00,0x90,0x00,0x00,0x00,0x00,0x00,0x00,
      0x0F,0x22,0xD8,
      0xFF,0xC9, 0x75,0xE2,
      0x0F,0x31, 0x89,0x47,0x08, 0x89,0x57,0x0C, 0xF4};
  memcpy(&code[18], &M, 4);  // B9 at [17], imm32 at [18..21]
  memcpy(mem + CODE, code, sizeof(code));

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);
  struct kvm_sregs sregs;
  ioctl(vcpu, KVM_GET_SREGS, &sregs);
  sregs.cr3 = PML4_A;
  sregs.cr4 = (1UL << 5);            // PAE
  sregs.cr0 = (1UL << 0) | (1UL << 31);  // PE | PG
  sregs.efer = (1UL << 8) | (1UL << 10); // LME | LMA
  struct kvm_segment cs = {.base=0,.limit=0xffffffff,.selector=0x08,
                           .type=11,.present=1,.s=1,.l=1,.g=1};
  struct kvm_segment ds = {.base=0,.limit=0xffffffff,.selector=0x10,
                           .type=3,.present=1,.s=1,.db=1,.g=1};
  sregs.cs = cs; sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = ds;
  ioctl(vcpu, KVM_SET_SREGS, &sregs);
  struct kvm_regs regs = {.rip = CODE, .rsp = STACK_TOP, .rflags = 0x2};
  ioctl(vcpu, KVM_SET_REGS, &regs);

  uint64_t runs = 0;
  for (;;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    runs++;
    if (run->exit_reason == KVM_EXIT_HLT) break;
    if (run->exit_reason == KVM_EXIT_SHUTDOWN) { fprintf(stderr,"triple fault\n"); return 1; }
    fprintf(stderr, "unexpected exit %u\n", run->exit_reason); return 1;
  }
  printf("KVM_RUN returns to userspace: %llu (1 = CR3 writes stayed in guest)\n",
         (unsigned long long)runs);
  uint32_t *s = (uint32_t *)(mem + SCRATCH);
  uint64_t start = ((uint64_t)s[1] << 32) | s[0];
  uint64_t end = ((uint64_t)s[3] << 32) | s[2];
  uint64_t cycles = end - start;
  double per = (double)cycles / (2.0 * M);
  printf("%u iterations, 2 CR3 writes each = %u switches\n", M, 2 * M);
  printf("total %llu TSC cycles\n", (unsigned long long)cycles);
  printf("per CR3 switch (incl. TLB refill of the code path): %.1f cycles "
         "(~%.1f ns at %.2f GHz)\n", per, per / GHZ, GHZ);
  return 0;
}
