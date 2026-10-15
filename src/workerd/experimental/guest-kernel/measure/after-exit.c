// How much slower host code runs right after a VM exit.
//
// A fixed, fully predictable workload the size of one request -- a thousand
// distinct small functions, each a handful of branches, called through a table
// in a random but fixed order -- is run many times and timed with the TSC.
// Between runs the program does nothing, makes one trivial syscall, issues one
// IBPB (through the msr driver, root only: `modprobe msr`), or makes one
// KVM_RUN round trip into a guest whose first instruction exits. The run after
// each kind of break is the measurement: what a VM exit does to the code that
// follows it, as against a syscall, which does nothing to it.
//
// Build: cc -O1 -o after-exit after-exit.c
// Run:   after-exit <none|syscall|ibpb|kvm> [ops] [runs]   (pin it: taskset -c N)
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

// A thousand functions from one template, each with its own constants: the
// footprint of real code, not one hot loop.
#define H(n, k) ((((n) + 1) * 2654435761u + (k) * 40503u) ^ ((((n) + 7) * 97u + (k)) >> 3))
#define BR(n, k) \
  if (x & (1ull << (H(n, k) % 40))) { s[H(n, k + 1) % 8] ^= x >> (H(n, k + 2) % 30 + 1); x += H(n, k + 3) % 9999; } \
  else { s[H(n, k + 4) % 8] += x * (H(n, k + 5) % 255 + 1); }
#define F(id, n) \
  __attribute__((noinline)) static void f##id(uint64_t *s) { \
    uint64_t x = s[0] * (H(n, 0) % 255 + 1) + H(n, 1) % 255; \
    BR(n, 10) BR(n, 20) BR(n, 30) BR(n, 40) BR(n, 50) \
    s[H(n, 60) % 8] = x; \
  }
#define T(id, n) f##id,
#define D(M, p, q) M(p##0, (q) * 10 + 0) M(p##1, (q) * 10 + 1) M(p##2, (q) * 10 + 2) M(p##3, (q) * 10 + 3) \
  M(p##4, (q) * 10 + 4) M(p##5, (q) * 10 + 5) M(p##6, (q) * 10 + 6) M(p##7, (q) * 10 + 7) M(p##8, (q) * 10 + 8) M(p##9, (q) * 10 + 9)
#define DD(M, p, q) D(M, p##0, (q) * 10 + 0) D(M, p##1, (q) * 10 + 1) D(M, p##2, (q) * 10 + 2) D(M, p##3, (q) * 10 + 3) \
  D(M, p##4, (q) * 10 + 4) D(M, p##5, (q) * 10 + 5) D(M, p##6, (q) * 10 + 6) D(M, p##7, (q) * 10 + 7) D(M, p##8, (q) * 10 + 8) D(M, p##9, (q) * 10 + 9)
#define DDD(M) DD(M, x0, 0) DD(M, x1, 1) DD(M, x2, 2) DD(M, x3, 3) DD(M, x4, 4) DD(M, x5, 5) DD(M, x6, 6) DD(M, x7, 7) DD(M, x8, 8) DD(M, x9, 9)
DDD(F)
static void (*const table[])(uint64_t *) = { DDD(T) };
#define TABLE_N ((int)(sizeof table / sizeof table[0]))

static int vcpu_fd, msr_fd = -1;
static struct kvm_run *run;

static void kvm_setup(void) {
  int kvm = open("/dev/kvm", O_RDWR);
  if (kvm < 0) { perror("/dev/kvm"); exit(1); }
  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  void *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memcpy(mem, "\xe6\x10\xeb\xfc", 4);  // out %al,$0x10; jmp .-4  (real mode)
  struct kvm_userspace_memory_region r = {0, 0, 0x1000, 0x1000, (uint64_t)(uintptr_t)mem};
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) < 0) { perror("KVM_SET_USER_MEMORY_REGION"); exit(1); }
  vcpu_fd = ioctl(vm, KVM_CREATE_VCPU, 0);
  run = mmap(NULL, ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0), PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
  struct kvm_sregs s;
  ioctl(vcpu_fd, KVM_GET_SREGS, &s);
  s.cs.base = 0; s.cs.selector = 0;
  ioctl(vcpu_fd, KVM_SET_SREGS, &s);
  struct kvm_regs g = {0};
  g.rip = 0x1000; g.rflags = 2;
  ioctl(vcpu_fd, KVM_SET_REGS, &g);
}

static void kvm_roundtrip(void) {
  if (ioctl(vcpu_fd, KVM_RUN, 0) < 0 || run->exit_reason != KVM_EXIT_IO) { perror("KVM_RUN"); exit(1); }
}

static void ibpb(void) {  // the kernel writes PRED_CMD on this CPU and returns to us
  uint64_t v = 1;
  if (pwrite(msr_fd, &v, 8, 0x49) != 8) { perror("wrmsr PRED_CMD"); exit(1); }
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "none";
  int nops = argc > 2 ? atoi(argv[2]) : 8000, runs = argc > 3 ? atoi(argv[3]) : 3000;
  int kind = !strcmp(mode, "syscall") ? 1 : !strcmp(mode, "ibpb") ? 2 : !strcmp(mode, "kvm") ? 3 : 0;
  uint16_t *prog = malloc(nops * sizeof *prog);
  uint32_t seed = 12345;
  for (int i = 0; i < nops; i++) { seed = seed * 1103515245u + 12345u; prog[i] = (seed >> 8) % TABLE_N; }
  if (kind == 3) kvm_setup();
  if (kind == 2) {
    int cpu = sched_getcpu();
    char path[64];
    snprintf(path, sizeof path, "/dev/cpu/%d/msr", cpu);
    msr_fd = open(path, O_WRONLY);
    if (msr_fd < 0) { perror(path); exit(1); }
  }
  uint64_t s[8];
  double sum = 0, best = 1e30;
  int n = 0;
  for (int r = 0; r < runs; r++) {
    if (kind == 1) syscall(SYS_getppid);
    else if (kind == 2) ibpb();
    else if (kind == 3) kvm_roundtrip();
    for (int i = 0; i < 8; i++) s[i] = i + 1;  // identical every run, as a steady-state request is
    uint64_t t0 = __rdtsc();
    for (int i = 0; i < nops; i++) table[prog[i]](s);
    uint64_t t1 = __rdtsc();
    if (r >= runs / 10) { double c = (double)(t1 - t0); sum += c; n++; if (c < best) best = c; }
  }
  printf("%-8s %d ops/run: mean %8.0f TSC ticks per run, best %8.0f  [%lx]\n", mode, nops, sum / n, best, (unsigned long)s[0]);
  return 0;
}
