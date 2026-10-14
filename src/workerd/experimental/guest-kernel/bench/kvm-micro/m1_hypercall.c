// Milestone 1: the host/guest boundary.
//
// A process creates a KVM virtual machine over a slice of its own memory,
// runs a few bytes of guest code in that VM, and receives control back when
// the guest issues a port-I/O instruction. This is the primitive the whole
// design rests on: the guest (the workerd runtime, later) calls "down" into
// the host (the real kernel plus the host half of the same process), and the
// host services the call and resumes the guest. Here the call is a single
// OUT instruction and the host just prints its argument.
//
// Build:  cc -O2 -o m1_hypercall m1_hypercall.c
// Run:    ./m1_hypercall        (needs read/write on /dev/kvm)
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void die(const char *msg) { perror(msg); exit(1); }

int main(void) {
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) die("open /dev/kvm");

  int api = ioctl(kvm, KVM_GET_API_VERSION, 0);
  if (api != 12) { fprintf(stderr, "unexpected KVM API version %d\n", api); return 1; }

  int vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vmfd < 0) die("KVM_CREATE_VM");

  // One page of guest memory, mapped as guest-physical 0.
  const size_t mem_size = 0x1000;
  void *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) die("mmap guest memory");

  struct kvm_userspace_memory_region region = {
      .slot = 0,
      .guest_phys_addr = 0,
      .memory_size = mem_size,
      .userspace_addr = (uint64_t)mem,
  };
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
    die("KVM_SET_USER_MEMORY_REGION");

  // Real-mode guest: write 'K' to port 0x10, then halt.
  //   mov al, 'K'      B0 4B
  //   out 0x10, al     E6 10   (8-bit immediate port, 0..255)
  //   hlt              F4
  //   jmp $            EB FE   (in case hlt is resumed)
  const uint8_t code[] = {0xB0, 'K', 0xE6, 0x10, 0xF4, 0xEB, 0xFE};
  memcpy(mem, code, sizeof(code));

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) die("KVM_CREATE_VCPU");

  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  if (run_size < 0) die("KVM_GET_VCPU_MMAP_SIZE");
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) die("mmap kvm_run");

  // Start executing at guest-physical 0 in 16-bit real mode.
  struct kvm_sregs sregs;
  if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) die("KVM_GET_SREGS");
  sregs.cs.base = 0;
  sregs.cs.selector = 0;
  if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) die("KVM_SET_SREGS");

  struct kvm_regs regs = {
      .rip = 0,
      .rflags = 0x2,  // reserved bit, required
  };
  if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) die("KVM_SET_REGS");

  for (;;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    switch (run->exit_reason) {
      case KVM_EXIT_IO:
        if (run->io.direction == KVM_EXIT_IO_OUT &&
            run->io.port == 0x10) {
          char *p = (char *)run + run->io.data_offset;
          printf("guest -> host hypercall: byte 0x%02x ('%c')\n",
                 (unsigned char)*p, *p);
        }
        break;
      case KVM_EXIT_HLT:
        printf("guest halted; host regains control cleanly\n");
        return 0;
      case KVM_EXIT_FAIL_ENTRY:
        fprintf(stderr, "KVM_EXIT_FAIL_ENTRY reason=0x%llx\n",
                (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
        return 1;
      case KVM_EXIT_INTERNAL_ERROR:
        fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR suberror=%u\n",
                run->internal.suberror);
        return 1;
      default:
        fprintf(stderr, "unexpected exit_reason=%u\n", run->exit_reason);
        return 1;
    }
  }
}
