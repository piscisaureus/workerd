// Milestone 5: VM-exit cost. Gate 1's key number.
//
// Every host syscall the guest makes becomes a VM exit in this design. Phase 0
// assumed 1.5 us per exit-plus-host-work; this measures the exit round trip
// itself on this CPU. The guest issues N port-I/O instructions in a tight
// loop, each causing a lightweight exit that the host handles by doing nothing
// and resuming. The host times the whole run and divides by N.
//
// Build: cc -O2 -o m5_exitcost m5_exitcost.c
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void die(const char *m) { perror(m); exit(1); }
#define CODE_ADDR 0x1000
#define GHZ 2.85  // EPYC 7443P base clock, for a cycles estimate

int main(void) {
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) die("open /dev/kvm");
  int vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vmfd < 0) die("KVM_CREATE_VM");
  const size_t mem_size = 0x2000;
  uint8_t *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) die("mmap");
  struct kvm_userspace_memory_region region = {
      .slot = 0, .guest_phys_addr = 0,
      .memory_size = mem_size, .userspace_addr = (uint64_t)mem};
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) die("memregion");

  const uint32_t N = 5000000;
  // Real-mode guest loop:
  //   mov ecx, N       66 B9 <N32>
  //   L: out 0x10, al  E6 10
  //   dec ecx          66 FF C9
  //   jnz L            75 F9
  //   hlt              F4
  uint8_t code[] = {0x66, 0xB9, 0, 0, 0, 0, 0xE6, 0x10,
                    0x66, 0xFF, 0xC9, 0x75, 0xF9, 0xF4};
  memcpy(&code[2], &N, 4);
  memcpy(mem + CODE_ADDR, code, sizeof(code));

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);
  struct kvm_sregs sregs;
  ioctl(vcpu, KVM_GET_SREGS, &sregs);
  sregs.cs.base = 0; sregs.cs.selector = 0;
  ioctl(vcpu, KVM_SET_SREGS, &sregs);
  struct kvm_regs regs = {.rip = CODE_ADDR, .rflags = 0x2};
  ioctl(vcpu, KVM_SET_REGS, &regs);

  struct timespec t0, t1;
  uint64_t exits = 0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    if (run->exit_reason == KVM_EXIT_IO) { exits++; continue; }
    if (run->exit_reason == KVM_EXIT_HLT) break;
    fprintf(stderr, "unexpected exit %u\n", run->exit_reason);
    return 1;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
  printf("%llu VM exits (port I/O) in %.1f ms\n",
         (unsigned long long)exits, ns / 1e6);
  printf("per exit round trip: %.0f ns  (~%.0f cycles at %.2f GHz)\n",
         ns / exits, ns / exits * GHZ, GHZ);
  return 0;
}
