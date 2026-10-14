// Milestone 9: asynchronous signal delivery into the guest.
//
// Until now the guest only left via its own hypercalls or faults. A real
// runtime also needs to receive asynchronous events: timers, preemption, and
// host signals. This shows the mechanism. The guest runs an endless loop with
// interrupts enabled. The host arms a timer; when SIGALRM fires it interrupts
// KVM_RUN, and the host injects a maskable interrupt (vector 0x20) into the
// guest through the interrupt window. The guest's IDT vectors it to an in-guest
// handler, which sets a flag and returns with iretq; the main loop sees the
// flag and halts. This is how the runtime, as guest ring 0, would take timer
// ticks and translated host signals.
//
// Build: cc -O2 -o m9_signal m9_signal.c
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>

static void die(const char *m) { perror(m); exit(1); }

static volatile sig_atomic_t g_alarm;
static void on_kick(int s) { (void)s; g_alarm = 1; }
static pthread_t g_vcpu_thread;
static void *kicker(void *arg) {
  (void)arg;
  usleep(100000);  // 100 ms of real time, then interrupt the running guest
  pthread_kill(g_vcpu_thread, SIGUSR1);
  return NULL;
}

#define PML4 0x1000
#define PDPT 0x2000
#define PD   0x3000
#define GDT  0x4000
#define IDT  0x5000
#define CODE 0x6000
#define HANDLER 0x6100
#define COUNTER 0x7000
#define FLAG 0x7008
#define STACK_TOP 0x200000
#define IRQ_VECTOR 0x20

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
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) die("memslot");

  // Identity-map the first 4 MiB.
  ENT(mem, PDPT, 0) = PD | PTE_P | PTE_W;
  ENT(mem, PD, 0) = 0x000000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PD, 1) = 0x200000 | PTE_P | PTE_W | PTE_PS;
  ENT(mem, PML4, 0) = PDPT | PTE_P | PTE_W;

  // GDT: null, 64-bit code (0x08), data (0x10). Needed for interrupt delivery.
  ENT(mem, GDT, 0) = 0;
  ENT(mem, GDT, 1) = 0x00AF9A000000FFFFULL;
  ENT(mem, GDT, 2) = 0x00CF92000000FFFFULL;

  // IDT gate for vector 0x20 -> HANDLER.
  uint8_t *ent = mem + IDT + IRQ_VECTOR * 16;
  memset(mem + IDT, 0, 256 * 16);
  uint64_t h = HANDLER;
  *(uint16_t *)(ent + 0) = h & 0xffff;
  *(uint16_t *)(ent + 2) = 0x08;
  ent[5] = 0x8e;
  *(uint16_t *)(ent + 6) = (h >> 16) & 0xffff;
  *(uint32_t *)(ent + 8) = (h >> 32);

  // Guest main loop (rbx=COUNTER, rdi=FLAG):
  //   loop: inc qword [rbx]        48 FF 03
  //         cmp qword [rdi], 0      48 83 3F 00
  //         je loop                 74 F7
  //         hlt                     F4
  const uint8_t code[] = {0x48,0xFF,0x03, 0x48,0x83,0x3F,0x00, 0x74,0xF7, 0xF4};
  memcpy(mem + CODE, code, sizeof(code));
  //   handler: inc qword [rdi]      48 FF 07
  //            iretq                 48 CF
  const uint8_t handler[] = {0x48,0xFF,0x07, 0x48,0xCF};
  memcpy(mem + HANDLER, handler, sizeof(handler));

  int vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vcpu, 0);

  struct kvm_sregs s;
  ioctl(vcpu, KVM_GET_SREGS, &s);
  s.cr3 = PML4; s.cr4 = (1UL<<5); s.cr0 = (1UL<<0)|(1UL<<31);
  s.efer = (1UL<<8)|(1UL<<10);
  struct kvm_segment cs = {.base=0,.limit=0xffffffff,.selector=0x08,.type=11,.present=1,.s=1,.l=1,.g=1};
  struct kvm_segment ds = {.base=0,.limit=0xffffffff,.selector=0x10,.type=3,.present=1,.s=1,.db=1,.g=1};
  s.cs = cs; s.ds = s.es = s.fs = s.gs = s.ss = ds;
  s.gdt.base = GDT; s.gdt.limit = 3*8-1;
  s.idt.base = IDT; s.idt.limit = 256*16-1;
  ioctl(vcpu, KVM_SET_SREGS, &s);

  struct kvm_regs regs = {0};
  regs.rip = CODE; regs.rsp = STACK_TOP;
  regs.rflags = 0x2 | 0x200;  // IF=1: accept maskable interrupts
  regs.rbx = COUNTER; regs.rdi = FLAG;
  ioctl(vcpu, KVM_SET_REGS, &regs);



  // A helper thread interrupts the running guest with SIGUSR1 after a delay.
  // The handler has no SA_RESTART so KVM_RUN returns EINTR instead of resuming.
  struct sigaction sa = {0};
  sa.sa_handler = on_kick;
  sigaction(SIGUSR1, &sa, NULL);
  g_vcpu_thread = pthread_self();
  pthread_t kt;
  pthread_create(&kt, NULL, kicker, NULL);

  int injected = 0;
  for (;;) {
    run->request_interrupt_window = (g_alarm && !injected) ? 1 : 0;
    if (ioctl(vcpu, KVM_RUN, 0) < 0) {
      if (errno == EINTR) continue;  // the kick landed; loop to open a window
      die("KVM_RUN");
    }
    if (run->exit_reason == 10 /*KVM_EXIT_INTR*/) continue;
    switch (run->exit_reason) {
      case KVM_EXIT_IRQ_WINDOW_OPEN:
        if (g_alarm && !injected && run->ready_for_interrupt_injection) {
          struct kvm_interrupt in = {.irq = IRQ_VECTOR};
          if (ioctl(vcpu, KVM_INTERRUPT, &in) < 0) die("KVM_INTERRUPT");
          injected = 1;
        }
        break;
      case KVM_EXIT_HLT: {
        uint64_t counter = *(uint64_t *)(mem + COUNTER);
        uint64_t flag = *(uint64_t *)(mem + FLAG);
        int ok = injected && flag == 1 && counter > 0;
        printf("guest looped %llu times before the timer signal fired\n",
               (unsigned long long)counter);
        printf("host SIGALRM/kick injected as guest interrupt 0x%x; in-guest "
               "handler ran (flag=%llu) and the guest resumed to halt\n",
               IRQ_VECTOR, (unsigned long long)flag);
        printf("\n%s: async host signal delivered into the guest and handled\n",
               ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
      }
      case KVM_EXIT_SHUTDOWN:
        fprintf(stderr, "SHUTDOWN (triple fault)\n"); return 1;
      default:
        fprintf(stderr, "unexpected exit %u\n", run->exit_reason); return 1;
    }
  }
}
