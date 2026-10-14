// Milestone 7: syscall forwarding. The gateway to running real code in guest.
//
// Builds on the long-mode setup. The guest enables the SYSCALL instruction and
// points LSTAR at an in-guest trampoline. When guest code executes `syscall`,
// the CPU jumps to that trampoline, which issues a hypercall (a port-I/O exit).
// The host reads the guest register file, performs the real syscall on the host
// side, writes the result back into the guest's rax, and resumes. The
// trampoline returns to the caller. To the guest code, `syscall` behaves like a
// normal Linux syscall; this is how unmodified runtime and libc calls would be
// serviced.
//
// Two syscalls are forwarded: write(1, msg, len), whose buffer lives in guest
// memory and is reached by the host through guest-physical == host-virtual
// (here the low region is identity-mapped, so guest-virtual == guest-physical),
// and getpid(), which takes no memory argument. The write actually appears on
// this process's stdout, forwarded from inside the VM.
//
// Build: cc -O2 -o m7_syscall m7_syscall.c
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

#define PML4     0x1000
#define PDPT     0x2000
#define PD       0x3000
#define CODE     0x5000
#define HANDLER  0x5200
#define MSG      0x8000
#define RES_WR   0x9000
#define RES_PID  0x9008
#define STACK_TOP 0x200000

#define PTE_P (1UL<<0)
#define PTE_W (1UL<<1)
#define PTE_PS (1UL<<7)
#define ENT(mem,base,idx) (((uint64_t*)((mem)+(base)))[(idx)])

#define MSR_EFER         0xc0000080
#define MSR_STAR         0xc0000081
#define MSR_LSTAR        0xc0000082
#define MSR_SYSCALL_MASK 0xc0000084

#define HYPERCALL_PORT 0x11

static void set_msr(int vcpu, uint32_t idx, uint64_t val) {
  struct { struct kvm_msrs h; struct kvm_msr_entry e[1]; } m = {0};
  m.h.nmsrs = 1;
  m.e[0].index = idx;
  m.e[0].data = val;
  if (ioctl(vcpu, KVM_SET_MSRS, &m) < 1) die("KVM_SET_MSRS");
}

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

  // Identity-map the first 4 MiB.
  ENT(mem, PDPT, 0) = PD | PTE_P | PTE_W;
  ENT(mem, PD, 0) = 0x000000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PD, 1) = 0x200000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PML4, 0) = PDPT | PTE_P | PTE_W;

  const char msg[] = "hello from guest via forwarded write\n";
  uint32_t len = (uint32_t)strlen(msg);
  memcpy(mem + MSG, msg, len);

  // Guest main. Verified by disassembly in the run script.
  //   mov rax, 1            48 C7 C0 01 00 00 00   SYS_write
  //   mov rdi, 1            48 C7 C7 01 00 00 00   fd = stdout
  //   mov rsi, MSG          48 C7 C6 00 80 00 00   buffer (guest addr)
  //   mov rdx, len          48 C7 C2 <len32>       length  (imm at offset 24)
  //   syscall               0F 05
  //   mov rdi, RES_WR       48 C7 C7 00 90 00 00
  //   mov [rdi], rax        48 89 07               store bytes written
  //   mov rax, 39           48 C7 C0 27 00 00 00   SYS_getpid
  //   syscall               0F 05
  //   mov rdi, RES_PID      48 C7 C7 08 90 00 00
  //   mov [rdi], rax        48 89 07               store pid
  //   hlt                   F4
  uint8_t code[] = {
      0x48,0xC7,0xC0,0x01,0x00,0x00,0x00,
      0x48,0xC7,0xC7,0x01,0x00,0x00,0x00,
      0x48,0xC7,0xC6,0x00,0x80,0x00,0x00,
      0x48,0xC7,0xC2,0x00,0x00,0x00,0x00,
      0x0F,0x05,
      0x48,0xC7,0xC7,0x00,0x90,0x00,0x00,
      0x48,0x89,0x07,
      0x48,0xC7,0xC0,0x27,0x00,0x00,0x00,
      0x0F,0x05,
      0x48,0xC7,0xC7,0x08,0x90,0x00,0x00,
      0x48,0x89,0x07,
      0xF4,
  };
  memcpy(&code[24], &len, 4);  // patch rdx immediate
  memcpy(mem + CODE, code, sizeof(code));

  // LSTAR trampoline: hypercall to the host, then return to the caller.
  //   out HYPERCALL_PORT, al   E6 11
  //   jmp rcx                  FF E1   (rcx = return rip saved by syscall)
  const uint8_t handler[] = {0xE6, HYPERCALL_PORT, 0xFF, 0xE1};
  memcpy(mem + HANDLER, handler, sizeof(handler));

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);

  struct kvm_sregs sregs;
  ioctl(vcpu, KVM_GET_SREGS, &sregs);
  sregs.cr3 = PML4;
  sregs.cr4 = (1UL << 5);
  sregs.cr0 = (1UL << 0) | (1UL << 31);
  sregs.efer = (1UL << 8) | (1UL << 10) | (1UL << 0);  // LME | LMA | SCE
  struct kvm_segment cs = {.base=0,.limit=0xffffffff,.selector=0x08,
                           .type=11,.present=1,.s=1,.l=1,.g=1};
  struct kvm_segment ds = {.base=0,.limit=0xffffffff,.selector=0x10,
                           .type=3,.present=1,.s=1,.db=1,.g=1};
  sregs.cs = cs; sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = ds;
  ioctl(vcpu, KVM_SET_SREGS, &sregs);

  // SYSCALL MSRs: STAR sets the CS/SS selectors (0x08/0x10), LSTAR the handler.
  set_msr(vcpu, MSR_STAR, (uint64_t)0x08 << 32);
  set_msr(vcpu, MSR_LSTAR, HANDLER);
  set_msr(vcpu, MSR_SYSCALL_MASK, 0);

  struct kvm_regs regs = {.rip = CODE, .rsp = STACK_TOP, .rflags = 0x2};
  ioctl(vcpu, KVM_SET_REGS, &regs);

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
        case SYS_write: {
          // Translate the guest buffer pointer: identity-mapped low region, so
          // guest-virtual == guest-physical, and host address is mem + that.
          void *buf = mem + r.rsi;
          ret = write((int)r.rdi, buf, (size_t)r.rdx);
          break;
        }
        case SYS_getpid:
          ret = getpid();
          break;
        default:
          ret = -ENOSYS;
      }
      r.rax = (uint64_t)ret;  // all other regs preserved, incl. rcx and r11
      ioctl(vcpu, KVM_SET_REGS, &r);
      forwarded++;
      continue;
    }
    if (run->exit_reason == KVM_EXIT_HLT) break;
    if (run->exit_reason == KVM_EXIT_SHUTDOWN) {
      fprintf(stderr, "SHUTDOWN (triple fault)\n"); return 1; }
    fprintf(stderr, "unexpected exit %u\n", run->exit_reason); return 1;
  }

  uint64_t wr = *(uint64_t *)(mem + RES_WR);
  uint64_t pid = *(uint64_t *)(mem + RES_PID);
  int ok = (forwarded == 2) && (wr == len) && (pid == (uint64_t)getpid());
  printf("forwarded %d syscalls from inside the guest\n", forwarded);
  printf("guest write() returned %llu (message length %u)\n",
         (unsigned long long)wr, len);
  printf("guest getpid() returned %llu (host getpid() = %d)\n",
         (unsigned long long)pid, getpid());
  printf("\n%s: syscalls forwarded transparently through a hypercall\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
