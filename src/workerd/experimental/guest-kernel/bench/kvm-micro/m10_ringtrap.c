// Milestone 10: the ring 3 -> ring 0 trap inside the guest. Prices the
// alternative to a VM exit.
//
// The runtime's mutable state has to be out of reach of an escaped isolate,
// and two boundaries can do that. (A) The runtime lives on the host, outside
// the VM, so every crossing is a VM exit; milestone 5 prices that. (B) The
// runtime's state stays mapped in the guest but supervisor-only (U bit clear
// in its page-table entries), isolate code runs at guest ring 3, and reaches
// the runtime through a ring 3 -> ring 0 transition that never leaves the VM.
// This prices (B) two ways, so the choice rests on a measured number:
//
//   1. Explicit trap. Ring-3 code executes `syscall`; the LSTAR handler, in
//      the guest at ring 0, bumps a counter and returns with `sysretq`.
//   2. Fault-driven. Ring-3 code loads from a page that is present but has
//      the U bit clear. The CPU raises #PF through the guest's own IDT; the
//      ring-0 handler bumps a counter, records the error code and the faulting
//      CS, steps the saved RIP past the 2-byte load and returns with `iretq`.
//      The handler steps over the instruction rather than fixing the PTE, so
//      every iteration takes the identical path and the loop contains no PTE
//      write and no TLB maintenance: that is the trap itself, which is what a
//      trap-and-service runtime would pay per access. Fixing the PTE instead
//      would oblige the handler to clear it again (a second write and an
//      INVLPG) before the next iteration could fault, and that maintenance,
//      not the trap, would then dominate the number.
//
// Both handlers do the least work that still lets the host check they ran.
// A real runtime adds swapgs, a stack switch and a register save on top.
//
// A trap that silently turned into a VM exit would make (B) look like (A), so
// exits are counted from KVM's own per-vCPU statistics (KVM_GET_STATS_FD),
// which include exits the kernel services without returning to userspace, such
// as NPT/EPT faults and intercepted exceptions. Counting KVM_RUN returns, as
// milestone 6 does, would miss those. The loop runs as short sub-runs, each
// entered from ring 0 through a stub that `iretq`s to ring 3 and ended by a
// `syscall` with rax = 1, on which the LSTAR handler halts instead of
// returning; the host snapshots the counters around each sub-run and keeps it
// only if `exits` moved by exactly one, the HLT that ends it. Host timer
// interrupts (CONFIG_HZ ticks) also exit the vCPU; sub-runs they land in are
// discarded and reported, so every kept iteration provably ran with zero
// exits. Only KVM_SET_REGS is used between sub-runs; the segment state is set
// once, because KVM_SET_SREGS can reload the MMU and the following sub-run
// would then refault its NPT entries. The sub-run cannot end with `ud2`: KVM
// intercepts #UD on both SVM and VMX and runs its instruction emulator on it,
// which this program's own exit check exposed as insn_emulation = 1 per
// sub-run.
//
// The same VM then runs a ring-0 port-I/O loop, milestone 5's method, so the
// ratio against a VM exit comes from the same machine in the same run.
//
// Build: cc -O2 -o m10_ringtrap m10_ringtrap.c
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static void die(const char *m) { perror(m); exit(1); }
static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }

// Guest-physical layout, identity-mapped. The first 2 MiB uses 4 KiB pages so
// that one of them, SUPER_PAGE, can be supervisor-only.
#define PML4       0x1000
#define PDPT       0x2000
#define PD         0x3000
#define PT         0x4000
#define GDT        0x5000
#define IDT        0x6000
#define TSS        0x7000
#define ENTER_USER 0x8000  // ring 0: iretq into ring 3 at r14
#define USER_SYS   0x8100  // ring 3: the syscall loop
#define USER_PF    0x8200  // ring 3: the faulting-load loop
#define SYS_HANDLER 0x8300 // LSTAR target
#define PF_HANDLER 0x8400  // IDT vector 14
#define STOP_UD    0x8500  // IDT vector 6: record the vector, hlt (diagnostics)
#define STOP_DF    0x8540  // IDT vector 8
#define STOP_GP    0x8580  // IDT vector 13
#define EXIT_LOOP  0x8600  // ring 0: port-I/O exit loop (m5's method)
#define TSC_OUT    0x9000  // guest writes: TSC cycles over its loop
#define SYS_COUNT  0x9008  // guest writes: times the LSTAR handler ran
#define PF_COUNT   0x9010  // guest writes: times the #PF handler ran
#define PF_ERR     0x9018  // guest writes: #PF error code
#define PF_CS      0x9020  // guest writes: CS of the faulting code
#define STOP_VEC   0x9028  // guest writes: 1 = stopped via syscall, else the vector
#define SUPER_PAGE 0xA000  // present, writable, U clear: stands for the runtime
#define KSTACK_TOP 0x1E0000
#define USTACK_TOP 0x1F0000

#define KCODE 0x08
#define KDATA 0x10
#define UDATA 0x1B  // GDT entry 3, RPL 3
#define UCODE 0x23  // GDT entry 4, RPL 3
#define TSS_SEL 0x28

#define PTE_P  (1UL << 0)
#define PTE_W  (1UL << 1)
#define PTE_U  (1UL << 2)
#define PTE_PS (1UL << 7)
#define CR0_PE (1UL << 0)
#define CR0_PG (1UL << 31)
#define CR4_PAE (1UL << 5)
#define EFER_SCE (1UL << 0)
#define EFER_LME (1UL << 8)
#define EFER_LMA (1UL << 10)
#define ENT(mem, base, idx) (((uint64_t *)((mem) + (base)))[(idx)])

#define MSR_STAR 0xc0000081
#define MSR_LSTAR 0xc0000082
#define MSR_SYSCALL_MASK 0xc0000084
#define HYPERCALL_PORT 0x11

#define WARM_ITERS 20000
#define SUB_ITERS 2000     // per sub-run: under half a CONFIG_HZ=1000 tick
#define KEEP_RUNS 1000     // kept sub-runs per path: 2M measured round trips
#define MAX_ATTEMPTS 20000
#define OVERHEAD_RUNS 33
#define EXIT_ITERS 200000

static void set_msr(int vcpu, uint32_t idx, uint64_t val) {
  struct { struct kvm_msrs h; struct kvm_msr_entry e[1]; } m = {0};
  m.h.nmsrs = 1;
  m.e[0].index = idx;
  m.e[0].data = val;
  if (ioctl(vcpu, KVM_SET_MSRS, &m) < 1) die("KVM_SET_MSRS");
}

// ---- KVM per-vCPU statistics ------------------------------------------------

enum { ST_EXITS, ST_IRQ, ST_HALT, ST_IO, ST_PF_TAKEN, ST_PF_GUEST, ST_EMUL,
       NSTATS };
static const char *const stat_names[NSTATS] = {
    "exits", "irq_exits", "halt_exits", "io_exits",
    "pf_taken", "pf_guest", "insn_emulation"};

struct stats {
  int fd;
  uint32_t data_offset;
  uint32_t off[NSTATS];
};

static void stats_open(struct stats *st, int vcpu) {
  st->fd = ioctl(vcpu, KVM_GET_STATS_FD, 0);
  if (st->fd < 0) die("KVM_GET_STATS_FD (needs kernel 5.14+)");
  struct kvm_stats_header h;
  if (pread(st->fd, &h, sizeof h, 0) != sizeof h) die("read stats header");
  size_t dsz = sizeof(struct kvm_stats_desc) + h.name_size;
  char *descs = malloc(dsz * h.num_desc);
  if (pread(st->fd, descs, dsz * h.num_desc, h.desc_offset) !=
      (ssize_t)(dsz * h.num_desc))
    die("read stats descriptors");
  st->data_offset = h.data_offset;
  for (int i = 0; i < NSTATS; i++) {
    st->off[i] = UINT32_MAX;
    for (uint32_t d = 0; d < h.num_desc; d++) {
      struct kvm_stats_desc *desc = (struct kvm_stats_desc *)(descs + d * dsz);
      if (strcmp(desc->name, stat_names[i]) == 0) {
        st->off[i] = desc->offset;
        break;
      }
    }
    if (st->off[i] == UINT32_MAX) {
      fprintf(stderr, "vcpu stat %s not exported by this kernel\n",
              stat_names[i]);
      exit(1);
    }
  }
  free(descs);
}

static void stats_read(const struct stats *st, uint64_t v[NSTATS]) {
  for (int i = 0; i < NSTATS; i++)
    if (pread(st->fd, &v[i], 8, st->data_offset + st->off[i]) != 8)
      die("read stat");
}

// ---- guest construction -----------------------------------------------------

static void build_tables(uint8_t *mem) {
  ENT(mem, PML4, 0) = PDPT | PTE_P | PTE_W | PTE_U;
  ENT(mem, PDPT, 0) = PD | PTE_P | PTE_W | PTE_U;
  ENT(mem, PD, 0) = PT | PTE_P | PTE_W | PTE_U;             // 0..2 MiB, 4 KiB pages
  ENT(mem, PD, 1) = 0x200000 | PTE_P | PTE_W | PTE_U | PTE_PS;  // 2..4 MiB
  for (uint64_t i = 0; i < 512; i++)
    ENT(mem, PT, i) = (i << 12) | PTE_P | PTE_W | PTE_U;
  // The one page ring 3 may not touch. Everything else, code and stacks
  // included, is user-accessible: isolation is m3/m4's subject, not this one's.
  ENT(mem, PT, SUPER_PAGE >> 12) = SUPER_PAGE | PTE_P | PTE_W;
}

static void build_gdt(uint8_t *mem) {
  uint64_t *g = (uint64_t *)(mem + GDT);
  g[0] = 0;                      // null
  g[1] = 0x00AF9A000000FFFFULL;  // 0x08: 64-bit code, DPL 0
  g[2] = 0x00CF92000000FFFFULL;  // 0x10: data, DPL 0
  g[3] = 0x00CFF2000000FFFFULL;  // 0x18: data, DPL 3 (sysret: STAR[63:48]+8)
  g[4] = 0x00AFFA000000FFFFULL;  // 0x20: 64-bit code, DPL 3 (STAR[63:48]+16)
  // 0x28: 64-bit TSS, 16-byte descriptor. Interrupt delivery from ring 3
  // switches to the stack in TSS.RSP0.
  uint64_t base = TSS, limit = 0x67;
  g[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
         (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
  g[6] = base >> 32;
  uint64_t rsp0 = KSTACK_TOP;
  memcpy(mem + TSS + 4, &rsp0, 8);
  *(uint16_t *)(mem + TSS + 102) = 0x68;  // I/O bitmap offset: none
}

static void idt_gate(uint8_t *mem, int vec, uint64_t handler) {
  // 64-bit interrupt gate: present, DPL 0, selector KCODE, no IST.
  uint8_t *e = mem + IDT + vec * 16;
  *(uint16_t *)(e + 0) = handler & 0xffff;
  *(uint16_t *)(e + 2) = KCODE;
  e[4] = 0;
  e[5] = 0x8e;
  *(uint16_t *)(e + 6) = (handler >> 16) & 0xffff;
  *(uint32_t *)(e + 8) = handler >> 32;
}

static void stop_stub(uint8_t *mem, uint64_t addr, uint32_t vec) {
  // Ring-0 stub that records which vector fired and halts to the host:
  //   mov qword [STOP_VEC], vec    48 C7 04 25 28 90 00 00 <vec32>
  //   hlt                          F4
  uint8_t stub[] = {0x48, 0xC7, 0x04, 0x25, 0x28, 0x90, 0x00, 0x00,
                    0, 0, 0, 0, 0xF4};
  memcpy(&stub[8], &vec, 4);
  memcpy(mem + addr, stub, sizeof(stub));
}

static void build_code(uint8_t *mem) {
  // Ring-0 entry into ring 3, run at the start of every sub-run (r14 = user
  // entry). Verified by disassembly.
  //   push UDATA           6A 1B              SS
  //   push USTACK_TOP      68 00 00 1F 00     RSP
  //   push 2               6A 02              RFLAGS (IF=0)
  //   push UCODE           6A 23              CS
  //   push r14             41 56              RIP
  //   iretq                48 CF
  const uint8_t enter_user[] = {0x6A, 0x1B, 0x68, 0x00, 0x00, 0x1F, 0x00,
                                0x6A, 0x02, 0x6A, 0x23, 0x41, 0x56, 0x48, 0xCF};
  memcpy(mem + ENTER_USER, enter_user, sizeof(enter_user));

  // Ring-3 syscall loop (r12 = iterations). Timed in-guest with RDTSC; the
  // cycle count goes to TSC_OUT and a final syscall with rax = 1 ends the
  // sub-run. rax = 0 during the loop keeps the handler on its sysretq path.
  //   rdtsc                      0F 31
  //   shl rdx, 32                48 C1 E2 20
  //   or rax, rdx                48 09 D0
  //   mov r13, rax               49 89 C5
  //   xor eax, eax               31 C0
  // L:syscall                    0F 05
  //   dec r12                    49 FF CC
  //   jnz L                      75 F9
  //   rdtsc                      0F 31
  //   shl rdx, 32                48 C1 E2 20
  //   or rax, rdx                48 09 D0
  //   sub rax, r13               4C 29 E8
  //   mov [TSC_OUT], rax         48 89 04 25 00 90 00 00
  //   mov eax, 1                 B8 01 00 00 00
  //   syscall                    0F 05
  const uint8_t user_sys[] = {
      0x0F, 0x31, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0x49, 0x89, 0xC5,
      0x31, 0xC0,
      0x0F, 0x05,
      0x49, 0xFF, 0xCC, 0x75, 0xF9,
      0x0F, 0x31, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0x4C, 0x29, 0xE8,
      0x48, 0x89, 0x04, 0x25, 0x00, 0x90, 0x00, 0x00,
      0xB8, 0x01, 0x00, 0x00, 0x00,
      0x0F, 0x05,
  };
  memcpy(mem + USER_SYS, user_sys, sizeof(user_sys));

  // Ring-3 faulting-load loop (rbx = SUPER_PAGE, r12 = iterations). Same shape
  // with the syscall replaced by a 2-byte load that always raises #PF.
  //   rdtsc                      0F 31
  //   shl rdx, 32                48 C1 E2 20
  //   or rax, rdx                48 09 D0
  //   mov r13, rax               49 89 C5
  // L:mov eax, [rbx]             8B 03        #PF: U/S violation
  //   dec r12                    49 FF CC
  //   jnz L                      75 F9
  //   rdtsc                      0F 31
  //   shl rdx, 32                48 C1 E2 20
  //   or rax, rdx                48 09 D0
  //   sub rax, r13               4C 29 E8
  //   mov [TSC_OUT], rax         48 89 04 25 00 90 00 00
  //   mov eax, 1                 B8 01 00 00 00
  //   syscall                    0F 05
  const uint8_t user_pf[] = {
      0x0F, 0x31, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0x49, 0x89, 0xC5,
      0x8B, 0x03,
      0x49, 0xFF, 0xCC, 0x75, 0xF9,
      0x0F, 0x31, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0x4C, 0x29, 0xE8,
      0x48, 0x89, 0x04, 0x25, 0x00, 0x90, 0x00, 0x00,
      0xB8, 0x01, 0x00, 0x00, 0x00,
      0x0F, 0x05,
  };
  memcpy(mem + USER_PF, user_pf, sizeof(user_pf));

  // LSTAR handler, ring 0. rcx holds the return RIP and r11 the RFLAGS that
  // sysretq restores; neither is touched. rax = 1 is the stop request that
  // ends a sub-run; the test and the predicted branch are the only work
  // beyond the counter on the measured path.
  //   inc qword [SYS_COUNT]      48 FF 04 25 08 90 00 00
  //   test rax, rax              48 85 C0
  //   jnz stop                   75 03
  //   sysretq                    48 0F 07
  // stop:
  //   mov qword [STOP_VEC], 1    48 C7 04 25 28 90 00 00 01 00 00 00
  //   hlt                        F4
  const uint8_t sys_handler[] = {
      0x48, 0xFF, 0x04, 0x25, 0x08, 0x90, 0x00, 0x00,
      0x48, 0x85, 0xC0,
      0x75, 0x03,
      0x48, 0x0F, 0x07,
      0x48, 0xC7, 0x04, 0x25, 0x28, 0x90, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0xF4,
  };
  memcpy(mem + SYS_HANDLER, sys_handler, sizeof(sys_handler));

  // #PF handler, ring 0, on the TSS.RSP0 stack. The frame is
  // [rsp]=error code, [rsp+8]=RIP, [rsp+16]=CS, then RFLAGS, RSP, SS. The only
  // instruction in the guest that faults is the 2-byte `mov eax, [rbx]`, so
  // stepping RIP by 2 is exact. rax is dead in the faulting code.
  //   inc qword [PF_COUNT]       48 FF 04 25 10 90 00 00
  //   mov rax, [rsp]             48 8B 04 24
  //   mov [PF_ERR], rax          48 89 04 25 18 90 00 00
  //   mov rax, [rsp+16]          48 8B 44 24 10
  //   mov [PF_CS], rax           48 89 04 25 20 90 00 00
  //   add qword [rsp+8], 2       48 83 44 24 08 02   skip the load
  //   add rsp, 8                 48 83 C4 08         drop the error code
  //   iretq                      48 CF
  const uint8_t pf_handler[] = {
      0x48, 0xFF, 0x04, 0x25, 0x10, 0x90, 0x00, 0x00,
      0x48, 0x8B, 0x04, 0x24,
      0x48, 0x89, 0x04, 0x25, 0x18, 0x90, 0x00, 0x00,
      0x48, 0x8B, 0x44, 0x24, 0x10,
      0x48, 0x89, 0x04, 0x25, 0x20, 0x90, 0x00, 0x00,
      0x48, 0x83, 0x44, 0x24, 0x08, 0x02,
      0x48, 0x83, 0xC4, 0x08,
      0x48, 0xCF,
  };
  memcpy(mem + PF_HANDLER, pf_handler, sizeof(pf_handler));

  stop_stub(mem, STOP_UD, 6);
  stop_stub(mem, STOP_DF, 8);
  stop_stub(mem, STOP_GP, 13);

  // Ring-0 port-I/O loop (r12 = iterations): one VM exit per iteration,
  // milestone 5's measurement inside this VM.
  //   rdtsc                      0F 31
  //   shl rdx, 32                48 C1 E2 20
  //   or rax, rdx                48 09 D0
  //   mov r13, rax               49 89 C5
  // L:out HYPERCALL_PORT, al     E6 11
  //   dec r12                    49 FF CC
  //   jnz L                      75 F9
  //   rdtsc                      0F 31
  //   shl rdx, 32                48 C1 E2 20
  //   or rax, rdx                48 09 D0
  //   sub rax, r13               4C 29 E8
  //   mov [TSC_OUT], rax         48 89 04 25 00 90 00 00
  //   hlt                        F4
  const uint8_t exit_loop[] = {
      0x0F, 0x31, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0x49, 0x89, 0xC5,
      0xE6, HYPERCALL_PORT,
      0x49, 0xFF, 0xCC, 0x75, 0xF9,
      0x0F, 0x31, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0x4C, 0x29, 0xE8,
      0x48, 0x89, 0x04, 0x25, 0x00, 0x90, 0x00, 0x00,
      0xF4,
  };
  memcpy(mem + EXIT_LOOP, exit_loop, sizeof(exit_loop));
}

static void setup_sregs(struct kvm_sregs *s) {
  s->cr3 = PML4;
  s->cr4 = CR4_PAE;
  s->cr0 = CR0_PE | CR0_PG;
  s->efer = EFER_LME | EFER_LMA | EFER_SCE;
  s->gdt.base = GDT;
  s->gdt.limit = 7 * 8 - 1;
  s->idt.base = IDT;
  s->idt.limit = 256 * 16 - 1;
  struct kvm_segment cs = {.base = 0, .limit = 0xffffffff, .selector = KCODE,
                           .type = 11, .present = 1, .s = 1, .l = 1, .g = 1};
  struct kvm_segment ds = {.base = 0, .limit = 0xffffffff, .selector = KDATA,
                           .type = 3, .present = 1, .s = 1, .db = 1, .g = 1};
  struct kvm_segment tr = {.base = TSS, .limit = 0x67, .selector = TSS_SEL,
                           .type = 11 /* busy 64-bit TSS */, .present = 1};
  s->cs = cs;
  s->ds = s->es = s->fs = s->gs = s->ss = ds;
  s->tr = tr;
}

// ---- running and measuring --------------------------------------------------

struct vm {
  int vcpu;
  struct kvm_run *run;
  uint8_t *mem;
  struct stats st;
};

static uint64_t guest_u64(const struct vm *vm, uint64_t addr) {
  uint64_t v;
  memcpy(&v, vm->mem + addr, 8);
  return v;
}

// Runs the guest at ring 0 from `rip` with r12 = iters (and r14 = user entry
// for ENTER_USER) until it halts. Port-I/O exits from EXIT_LOOP are resumed.
// Returns the host ns spent in KVM_RUN and the per-vCPU stat deltas.
static double run_guest(struct vm *vm, uint64_t rip, uint64_t r14,
                        uint64_t iters, uint64_t delta[NSTATS]) {
  struct kvm_regs regs = {.rip = rip, .rsp = KSTACK_TOP, .rflags = 0x2,
                          .rbx = SUPER_PAGE, .r12 = iters, .r14 = r14};
  if (ioctl(vm->vcpu, KVM_SET_REGS, &regs) < 0) die("KVM_SET_REGS");
  memset(vm->mem + STOP_VEC, 0, 8);
  uint64_t before[NSTATS], after[NSTATS];
  stats_read(&vm->st, before);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    if (ioctl(vm->vcpu, KVM_RUN, 0) < 0) die("KVM_RUN");
    if (vm->run->exit_reason == KVM_EXIT_IO &&
        vm->run->io.direction == KVM_EXIT_IO_OUT &&
        vm->run->io.port == HYPERCALL_PORT)
      continue;
    if (vm->run->exit_reason == KVM_EXIT_HLT) break;
    if (vm->run->exit_reason == KVM_EXIT_SHUTDOWN) {
      fprintf(stderr, "SHUTDOWN (triple fault)\n");
      exit(1);
    }
    fprintf(stderr, "unexpected exit %u\n", vm->run->exit_reason);
    exit(1);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  stats_read(&vm->st, after);
  for (int i = 0; i < NSTATS; i++) delta[i] = after[i] - before[i];
  return (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
}

static void check_stopped_by_syscall(const struct vm *vm) {
  if (guest_u64(vm, STOP_VEC) != 1) {
    fprintf(stderr, "guest halted through exception vector %llu, not the "
            "syscall stop request\n",
            (unsigned long long)guest_u64(vm, STOP_VEC));
    exit(1);
  }
}

struct trap_result {
  uint64_t iters;          // round trips in kept sub-runs
  uint64_t cycles;         // TSC cycles over those, measured in-guest
  double host_ns;          // host clock over those, KVM_RUN overhead removed
  int kept, discarded, attempts;
  uint64_t kept_exits, kept_halt;
  uint64_t discarded_exits, discarded_irq, discarded_other;
  double min_per, max_per; // per-round-trip cycles, best and worst kept sub-run
};

static void measure_trap(struct vm *vm, uint64_t entry, double overhead_ns,
                         struct trap_result *r) {
  memset(r, 0, sizeof *r);
  r->min_per = 1e30;
  uint64_t d[NSTATS];
  run_guest(vm, ENTER_USER, entry, WARM_ITERS, d);  // first touches, NPT fills
  check_stopped_by_syscall(vm);
  while (r->kept < KEEP_RUNS) {
    if (++r->attempts > MAX_ATTEMPTS)
      fail("too few sub-runs free of host interrupts; lower SUB_ITERS");
    double ns = run_guest(vm, ENTER_USER, entry, SUB_ITERS, d);
    check_stopped_by_syscall(vm);
    uint64_t cycles = guest_u64(vm, TSC_OUT);
    // These would mean the trap itself left the VM, on any sub-run.
    if (d[ST_IO] || d[ST_PF_GUEST] || d[ST_EMUL] || d[ST_HALT] != 1) {
      fprintf(stderr, "sub-run: exits=%llu halt=%llu io=%llu pf_guest=%llu "
              "insn_emulation=%llu: the trap path left the VM\n",
              (unsigned long long)d[ST_EXITS], (unsigned long long)d[ST_HALT],
              (unsigned long long)d[ST_IO], (unsigned long long)d[ST_PF_GUEST],
              (unsigned long long)d[ST_EMUL]);
      exit(1);
    }
    if (d[ST_EXITS] == 1) {
      r->kept++;
      r->iters += SUB_ITERS;
      r->cycles += cycles;
      r->host_ns += ns - overhead_ns;
      r->kept_exits += d[ST_EXITS];
      r->kept_halt += d[ST_HALT];
      double per = (double)cycles / SUB_ITERS;
      if (per < r->min_per) r->min_per = per;
      if (per > r->max_per) r->max_per = per;
    } else {
      r->discarded++;
      r->discarded_exits += d[ST_EXITS];
      r->discarded_irq += d[ST_IRQ];
      r->discarded_other += d[ST_EXITS] - d[ST_IRQ] - 1;
    }
  }
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double tsc_ghz(void) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  uint64_t c0 = __builtin_ia32_rdtsc();
  double ns;
  do {
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
  } while (ns < 200e6);
  uint64_t c1 = __builtin_ia32_rdtsc();
  return (double)(c1 - c0) / ns;
}

static void report_trap(const char *title, const struct trap_result *r,
                        double ghz) {
  double cyc = (double)r->cycles / r->iters;
  printf("%s\n", title);
  printf("  %llu round trips in %d kept sub-runs of %d; %d sub-runs discarded "
         "(they contained %llu host-interrupt exits, %llu other)\n",
         (unsigned long long)r->iters, r->kept, SUB_ITERS, r->discarded,
         (unsigned long long)r->discarded_irq,
         (unsigned long long)r->discarded_other);
  printf("  per round trip: %.1f TSC cycles = %.1f ns; host clock %.1f ns\n",
         cyc, cyc / ghz, r->host_ns / r->iters);
  printf("  spread over kept sub-runs: %.1f .. %.1f cycles\n", r->min_per,
         r->max_per);
  printf("  VM exits during kept sub-runs: %llu = the one HLT ending each of "
         "the %d sub-runs (halt_exits %llu); zero exits inside the measured "
         "loop\n",
         (unsigned long long)r->kept_exits, r->kept,
         (unsigned long long)r->kept_halt);
}

int main(void) {
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) die("open /dev/kvm");
  int vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vmfd < 0) die("KVM_CREATE_VM");
  const size_t mem_size = 4 * 1024 * 1024;
  uint8_t *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) die("mmap");
  struct kvm_userspace_memory_region region = {
      .slot = 0, .guest_phys_addr = 0,
      .memory_size = mem_size, .userspace_addr = (uint64_t)mem};
  if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) die("memregion");

  build_tables(mem);
  build_gdt(mem);
  memset(mem + IDT, 0, 256 * 16);
  idt_gate(mem, 6, STOP_UD);
  idt_gate(mem, 8, STOP_DF);
  idt_gate(mem, 13, STOP_GP);
  idt_gate(mem, 14, PF_HANDLER);
  build_code(mem);

  struct vm vm;
  vm.mem = mem;
  vm.vcpu = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  if (vm.vcpu < 0) die("KVM_CREATE_VCPU");
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  vm.run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vm.vcpu, 0);
  if (vm.run == MAP_FAILED) die("mmap kvm_run");
  stats_open(&vm.st, vm.vcpu);

  struct kvm_sregs sregs;
  if (ioctl(vm.vcpu, KVM_GET_SREGS, &sregs) < 0) die("KVM_GET_SREGS");
  setup_sregs(&sregs);
  if (ioctl(vm.vcpu, KVM_SET_SREGS, &sregs) < 0) die("KVM_SET_SREGS");
  // syscall loads CS/SS from STAR[47:32] (0x08/0x10); sysretq from
  // STAR[63:48]+16/+8 with RPL 3 (0x23/0x1B).
  set_msr(vm.vcpu, MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)KCODE << 32));
  set_msr(vm.vcpu, MSR_LSTAR, SYS_HANDLER);
  set_msr(vm.vcpu, MSR_SYSCALL_MASK, 0);

  double ghz = tsc_ghz();

  // Warm up, then price a sub-run's fixed cost (KVM_RUN entry, the ring-0 to
  // ring-3 stub, the stop syscall and the HLT exit) with one-iteration runs,
  // so it can be taken out of the host-clock figures. The in-guest TSC figures
  // never include it.
  uint64_t d[NSTATS];
  run_guest(&vm, ENTER_USER, USER_SYS, WARM_ITERS, d);
  check_stopped_by_syscall(&vm);
  double overhead[OVERHEAD_RUNS];
  for (int i = 0; i < OVERHEAD_RUNS; i++)
    overhead[i] = run_guest(&vm, ENTER_USER, USER_SYS, 1, d);
  qsort(overhead, OVERHEAD_RUNS, sizeof overhead[0], cmp_double);
  double overhead_ns = overhead[OVERHEAD_RUNS / 2];

  struct trap_result sys, pf;
  measure_trap(&vm, USER_SYS, overhead_ns, &sys);
  measure_trap(&vm, USER_PF, overhead_ns, &pf);

  // Reference: VM-exit round trip in this VM, milestone 5's method at ring 0.
  run_guest(&vm, EXIT_LOOP, 0, 1000, d);
  double exit_ns = run_guest(&vm, EXIT_LOOP, 0, EXIT_ITERS, d);
  uint64_t exit_cycles = guest_u64(&vm, TSC_OUT);
  if (d[ST_IO] != EXIT_ITERS) fail("port-I/O loop did not exit once per iteration");
  double exit_per_ns = exit_ns / EXIT_ITERS;
  double exit_per_cyc = (double)exit_cycles / EXIT_ITERS;

  // The handlers must have run exactly once per trap issued from ring 3. Every
  // run of either ring-3 loop ends with one extra stop syscall.
  uint64_t user_runs = 2 + OVERHEAD_RUNS + sys.attempts + 1 + pf.attempts;
  uint64_t sys_issued = 2 * WARM_ITERS + OVERHEAD_RUNS +
                        (uint64_t)sys.attempts * SUB_ITERS + user_runs;
  uint64_t pf_issued = WARM_ITERS + (uint64_t)pf.attempts * SUB_ITERS;
  uint64_t sys_count = guest_u64(&vm, SYS_COUNT);
  uint64_t pf_count = guest_u64(&vm, PF_COUNT);
  uint64_t pf_err = guest_u64(&vm, PF_ERR);
  uint64_t pf_cs = guest_u64(&vm, PF_CS);
  uint64_t pte = ENT(mem, PT, SUPER_PAGE >> 12);
  if (sys_count != sys_issued) fail("LSTAR handler count != syscalls issued");
  if (pf_count != pf_issued) fail("#PF handler count != faulting loads issued");
  if (pf_err != 0x5) fail("#PF error code is not P|U (a user-mode access)");
  if (pf_cs != UCODE) fail("#PF did not come from the ring-3 code segment");

  printf("TSC %.3f GHz (host RDTSC against CLOCK_MONOTONIC); fixed cost per "
         "sub-run %.2f us, subtracted from host-clock figures only\n\n",
         ghz, overhead_ns / 1e3);
  report_trap("explicit trap: ring 3 syscall -> LSTAR handler -> sysretq", &sys,
              ghz);
  printf("  LSTAR handler ran %llu times = every syscall issued, stop "
         "requests included\n\n",
         (unsigned long long)sys_count);
  report_trap("fault-driven: ring 3 load from a supervisor-only page -> #PF -> "
              "iretq", &pf, ghz);
  printf("  #PF handler ran %llu times = every load issued; error code 0x%llx "
         "(present, user-mode) from CS 0x%llx (ring 3); PTE 0x%llx (P|W, U "
         "clear)\n\n",
         (unsigned long long)pf_count, (unsigned long long)pf_err,
         (unsigned long long)pf_cs, (unsigned long long)pte);
  printf("reference in the same VM: ring-0 port-I/O VM exit round trip (m5's "
         "method)\n  %u exits: %.0f ns = %.0f TSC cycles per round trip\n\n",
         EXIT_ITERS, exit_per_ns, exit_per_cyc);
  printf("VM exit / syscall trap: %.1fx    VM exit / #PF trap: %.1fx\n",
         exit_per_cyc / ((double)sys.cycles / sys.iters),
         exit_per_cyc / ((double)pf.cycles / pf.iters));
  printf("\nPASS: both ring 3 -> ring 0 round trips measured with zero VM "
         "exits inside the loop\n");
  return 0;
}
