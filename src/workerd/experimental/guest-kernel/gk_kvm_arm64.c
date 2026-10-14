// gk: arm64 under KVM: ONE_REG register access, the vDSO, root loads from the
// host, host-backing faults and the run loop.
#include "gk_internal.h"
#if defined(__aarch64__) && !GK_HVF

// KVM backs a memslot from the host mapping as the guest touches it, so a
// page needs nothing put into stage 2 before its first PTE (see gk_hvf.c for
// the hypervisor that does).
static int s2_ensure(gk_region *rg, uintptr_t hva) {
  (void)rg; (void)hva;
  return 0;
}

// ---- VM setup (see gk_init) --------------------------------------------------
// The host facts the host-frame wall needs (see stack_wall_bounds): the size of
// glibc's struct pthread.
static void wall_probe(void) {
  // glibc publishes sizeof(struct pthread) for libthread_db (GLIBC_PRIVATE).
  const uint32_t *psz = dlsym(RTLD_DEFAULT, "_thread_db_sizeof_pthread");
  G->pthread_size = psz ? *psz : 0;
  if (!G->pthread_size)
    fprintf(stderr, "[gk] _thread_db_sizeof_pthread not found: gk_run_here_user builds no "
                    "host-frame wall on pthread stacks\n");
}

// The VM, its kvm_run size, and the vDSO's data pages, which every region must
// avoid (see the vDSO section), so before the first one.
static int vm_create(void) {
  if (kvm_open() < 0) return -1;
  // The largest guest-physical space KVM offers (see the guest-physical
  // allocator), and TCR_EL1.IPS to match.
  int ipa = ioctl(G->kvm, KVM_CHECK_EXTENSION, KVM_CAP_ARM_VM_IPA_SIZE);
  if (ipa <= 0) ipa = 40;
  if (ipa > 48) ipa = 48;
  static const int ips_bits[] = {32, 36, 40, 42, 44, 48};
  for (G->ips = 0; G->ips < 5 && ips_bits[G->ips + 1] <= ipa; G->ips++) {}
  ipa = ips_bits[G->ips];
  G->vmfd = ioctl(G->kvm, KVM_CREATE_VM, KVM_VM_TYPE_ARM_IPA_SIZE(ipa));
  if (G->vmfd < 0) { G->err = "KVM_CREATE_VM"; return -1; }
  G->gpa_next = GK_BACK_WIN;  // guest-physical 0 stays unused
  G->gpa_limit = 1ULL << ipa;
  // The vector handlers' HVCs come to the host as KVM_EXIT_HYPERCALL; without
  // the filter KVM would take them as (failed) PSCI calls itself.
  struct kvm_smccc_filter filter = {.base = GK_HC_BASE, .nr_functions = GK_HC_COUNT,
                                    .action = KVM_SMCCC_FILTER_FWD_TO_USER};
  struct kvm_device_attr fa = {.group = KVM_ARM_VM_SMCCC_CTRL, .attr = KVM_ARM_VM_SMCCC_FILTER,
                               .addr = (uint64_t)(uintptr_t)&filter};
  if (ioctl(G->vmfd, KVM_SET_DEVICE_ATTR, &fa) < 0) { G->err = "KVM_ARM_VM_SMCCC_FILTER"; return -1; }
  // The guest runs the host's vDSO, which reads the virtual counter: with a
  // zero offset it reads the host's, as the zero TSC offset does on x86-64.
  // Best effort: older kernels lack the ioctl, and the guest's clock is then
  // off by the VM's start time.
  struct kvm_arm_counter_offset co = {.counter_offset = 0};
  if (ioctl(G->vmfd, KVM_ARM_SET_COUNTER_OFFSET, &co) < 0)
    fprintf(stderr, "[gk] KVM_ARM_SET_COUNTER_OFFSET failed (%s): guest clocks are off\n",
            strerror(errno));
  if (ioctl(G->vmfd, KVM_ARM_PREFERRED_TARGET, &G->vcpu_target) < 0) {
    G->err = "KVM_ARM_PREFERRED_TARGET";
    return -1;
  }
  G->run_size = ioctl(G->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  vdso_setup();
  return 0;
}

// ---- the vDSO (arm64) --------------------------------------------------------
// The vDSO's functions (clock_gettime and friends) read the kernel's data
// pages, [vvar], which are a PFNMAP mapping, and arm64 KVM refuses a memslot
// over one: those pages cannot be the guest's. So the vDSO does not run in the
// guest. Its text is refused, like gk's own memory, and a call into it, which
// faults on the fetch of the function's first instruction, is carried out by
// the host instead: the very function at that address, with the guest's
// argument registers, its result returned to the guest's link register as the
// function itself would (see vdso_call). Only the vDSO's exported entry
// points are called, never its signal return trampoline, which only the
// kernel's signal delivery uses (and no signal is delivered inside the guest).
// The pointers the guest passes are its own memory, as for a forwarded
// syscall. [vvar] is refused and a hole for the region windows (see
// region_ensure).
static void vdso_setup(void) {
  FILE *f = fopen("/proc/self/maps", "re");
  if (f) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      unsigned long s, e;
      char name[64] = "";
      if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %63s", &s, &e, name) < 3) continue;
      if (strncmp(name, "[vvar", 5) == 0 && G->hole_n < (int)(sizeof G->holes / sizeof G->holes[0])) {
        G->holes[G->hole_n++] = (gk_prot_range){s, e, GK_PROT_REFUSE};
        prot_add(s, e, GK_PROT_REFUSE);
      } else if (strcmp(name, "[vdso]") == 0) {
        G->vdso_lo = s;
        G->vdso_hi = e;
        prot_add(s, e, GK_PROT_REFUSE);
      }
    }
    fclose(f);
  }
  uintptr_t base = (uintptr_t)getauxval(AT_SYSINFO_EHDR);
  if (!base) return;
  const Elf64_Ehdr *eh = (const void *)base;
  const Elf64_Phdr *ph = (const void *)(base + eh->e_phoff);
  uintptr_t bias = base;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0) bias = base - ph[i].p_vaddr;
  const Elf64_Shdr *sh = (const void *)(base + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_DYNSYM) continue;
    const Elf64_Sym *sym = (const void *)(base + sh[i].sh_offset);
    const char *str = (const char *)(base + sh[sh[i].sh_link].sh_offset);
    size_t n = sh[i].sh_size / sizeof *sym;
    for (size_t k = 0; k < n; k++) {
      if (ELF64_ST_TYPE(sym[k].st_info) != STT_FUNC || sym[k].st_shndx == SHN_UNDEF) continue;
      if (strstr(str + sym[k].st_name, "sigreturn")) continue;
      if (G->vdso_n < (int)(sizeof G->vdso_fn / sizeof G->vdso_fn[0]))
        G->vdso_fn[G->vdso_n++] = bias + sym[k].st_value;
    }
  }
  if (G->dbg)
    fprintf(stderr, "[gk] vDSO [%#lx,%#lx), %d functions, run on the host; %d data range(s) "
            "kept out of memslots\n", (unsigned long)G->vdso_lo, (unsigned long)G->vdso_hi,
            G->vdso_n, G->hole_n);
}

// A guest instruction fetch fault at `pc`: if it is the entry of a vDSO
// function, call that function on the host with the frame's arguments and
// return the guest to its caller (1). 0 otherwise.
static int vdso_call(gk_frame *f, uint64_t pc) {
  if (pc < G->vdso_lo || pc >= G->vdso_hi) return 0;
  for (int i = 0; i < G->vdso_n; i++) {
    if (G->vdso_fn[i] != pc) continue;
    long (*fn)(long, long, long, long, long, long) = (void *)(uintptr_t)pc;
    f->x[0] = (uint64_t)fn((long)f->x[0], (long)f->x[1], (long)f->x[2], (long)f->x[3],
                           (long)f->x[4], (long)f->x[5]);
    f->elr = f->x[30];
    return 1;
  }
  return 0;
}
static int vcpu_set(int fd, uint64_t id, uint64_t v) {
  struct kvm_one_reg r = {.id = id, .addr = (uint64_t)(uintptr_t)&v};
  return ioctl(fd, KVM_SET_ONE_REG, &r);
}

static int vcpu_get(int fd, uint64_t id, uint64_t *v) {
  *v = 0;
  struct kvm_one_reg r = {.id = id, .addr = (uint64_t)(uintptr_t)v};
  return ioctl(fd, KVM_GET_ONE_REG, &r);
}
// cr3_load_host on arm64: the switch and a full flush, as KVM does for a CR3
// set from the host, both carried out by the dispatcher before the vCPU runs.
static int cr3_load_host(gk_thread *t, uint64_t cr3) {
  struct gk_vcb *v = VCB(t);
  if (ttbr0_preload(t, cr3) < 0) return -1;
  v->ttbr = cr3;
  v->action |= GK_ACT_TTBR;
  flush_run(t);
  t->tlb_touched = 1;
  return 0;
}
// The host-backing paths on arm64. The stopped vCPU's state is in the CPU,
// not in its vcb, so the retry copies it there by ioctl (a rare path) and
// restarts the instruction through the dispatcher with a full flush, which
// is what the CR3 swing does on x86-64; ring 3 or not, the dispatcher runs at
// EL1. The failure puts the vCPU back at the dispatcher, where every turn
// starts.
static int host_backing_retry(gk_thread *t) {
  gk_arena *a = t->active_arena;
  if (!a || t->backing_retries >= GK_BACKING_RETRIES) return 0;
  uint64_t pstate;
  if (vcpu_get(t->fd, GK_CORE_REG(regs.pstate), &pstate) < 0 || (pstate & 0xf) == (PSR_EL1H & 0xf))
    return 0;  // stopped inside a handler or the dispatcher: nothing to restart
  t->backing_retries++;
  pthread_mutex_lock(&G->lock);
  unmap_range_root(a->pml4, a->base, a->end);
  pthread_mutex_unlock(&G->lock);
  struct gk_vcb *v = VCB(t);
  gk_frame f;
  for (int i = 0; i < 31; i++)
    if (vcpu_get(t->fd, GK_CORE_REG(regs.regs[i]), &f.x[i]) < 0) return 0;
  if (vcpu_get(t->fd, GK_CORE_REG(regs.sp), &f.sp) < 0 ||
      vcpu_get(t->fd, GK_CORE_REG(regs.pc), &f.elr) < 0)
    return 0;
  f.spsr = pstate;
  f.esr = f.far = 0;
  v->f = f;
  v->action = GK_ACT_FLUSH;  // resume, after the flush
  atomic_fetch_add_explicit(&G->tlb_flushes, 1, memory_order_relaxed);
  if (vcpu_reset_pc(t) < 0) return 0;
  if (G->dbg) {  // fault path: raw output only
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": host backing gone under an arena PTE; retrying as a guest fault (");
    db_dec(&d, t->backing_retries); db_str(&d, ")");
    db_flush(&d);
  }
  return 1;
}

static long host_backing_fail(gk_thread *t, int err) {
  uint64_t addr = 0;
  vcpu_get(t->fd, GK_CORE_REG(regs.pc), &addr);
  int exact = 0;
#ifdef KVM_EXIT_MEMORY_FAULT
  if (t->run->exit_reason == KVM_EXIT_MEMORY_FAULT) {
    pthread_mutex_lock(&G->lock);
    gk_region *r = region_by_gpa(G->regions, t->run->memory_fault.gpa);
    if (r) {
      addr = r->start + (t->run->memory_fault.gpa - r->gpa);
      exact = 1;
    }
    pthread_mutex_unlock(&G->lock);
  }
#endif
  if (G->dbg) {  // fault path: raw output only
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": KVM_RUN failed, errno "); db_dec(&d, err);
    if (err == EFAULT)
      db_str(&d, exact ? ": host backing gone at " : ": host backing gone under an access at pc ");
    else
      db_str(&d, ": pc ");
    db_hex(&d, addr);
    db_flush(&d);
  }
  VCB(t)->action = 0;
  vcpu_reset_pc(t);
  if (err != EFAULT) return -1;
  G->fault_addr = addr;
  return GK_EFAULT;
}
// run_vcpu on arm64: every exit is a hypercall from the vector handlers,
// with the exception's state in the vcb (see vcb_exception). Whatever ends
// the turn leaves the vCPU at the dispatcher, where the next turn's launch
// finds it.
static long run_vcpu(gk_thread *t) {
  for (;;) {
    t->tlb_touched = 1;  // the vCPU runs: its TLB is no longer known (see wall_raise)
    tls_in_run = 1;
    int rc = vcpu_run_ioctl(t);
    tls_in_run = 0;
    if (rc < 0) {
      int err = errno;
      if (err == EINTR) continue;
      if (err == EFAULT && host_backing_retry(t)) continue;
      return host_backing_fail(t, err);
    }
    if (t->run->exit_reason == KVM_EXIT_SYSTEM_EVENT) return GK_ESHUTDOWN;
    if (t->run->exit_reason != KVM_EXIT_HYPERCALL) {
      if (G->dbg) fprintf(stderr, "[gk] unexpected exit reason=%u\n", t->run->exit_reason);
      vcpu_reset_pc(t);
      return -1;
    }
    if (t->run->hypercall.nr != GK_HC_EXC) return vcb_bad(t);
    long ret;
    if (vcb_exception(t, &ret)) return ret;
  }
}

// ---- the host thread (see stack_wall_bounds) ---------------------------------
// The thread pointer, TPIDR_EL0, with glibc's struct pthread just below it and
// the TCB and static TLS block just above.
static uintptr_t thread_pointer(void) {
  uintptr_t tp;
  __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(tp));
  return tp;
}

// The lowest address of the thread's data around thread pointer `tp`: struct
// pthread (glibc's TLS_PRE_TCB_SIZE, plus slack for its alignment) below it.
// 0 if its size is unknown.
static uintptr_t thread_data_floor(uintptr_t tp) {
  if (!G->pthread_size) return 0;
  return GUEST_FLOOR(tp - G->pthread_size - 64);
}

// ---- the Linux syscall ABI and guest threads (see gk_linux.c) ----------------
// gk's own host calls, in the Linux shape: the result, or -errno.
static long host_syscall(long nr, long a1, long a2, long a3, long a4, long a5,
                         long a6) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a1;
  register long x1 __asm__("x1") = a2;
  register long x2 __asm__("x2") = a3;
  register long x3 __asm__("x3") = a4;
  register long x4 __asm__("x4") = a5;
  register long x5 __asm__("x5") = a6;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                       : "memory");
  return x0;
}

// A guest syscall's number and arguments from its register snapshot, as a[0]
// and a[1..6].
static void syscall_args(const gk_regs *r, long a[7]) {
  a[0] = (long)r->x[8];
  for (int i = 0; i < 6; i++) a[i + 1] = (long)r->x[i];
}

// The SVC exit resumes from the vcb whatever ran on the vCPU in between.
static void syscall_stub_ran(gk_regs *r) {
  (void)r;
}

// The start registers of a guest-created thread, from its parent's snapshot
// `c` at the clone: the clone returns 0 on the stack it named. ELR and SPSR
// stay the parent's: the child resumes after the SVC, at the parent's
// exception level (see child_launch).
static void child_regs_init(gk_regs *c, uint64_t sp) {
  c->x[0] = 0;
  c->sp = sp;
}

// Hand a guest-created thread's vCPU its start registers (see child_entry).
static void child_launch(gk_thread *t) {
  // The dispatcher resumes the parent's frame as the child's: after the SVC,
  // at the parent's exception level (the frame's SPSR), so a ring-3 creator's
  // child starts at ring 3 with no stub of its own.
  VCB(t)->f = t->start.regs;
  VCB(t)->action &= ~GK_ACT_LAUNCH;
  if (G->dbg)
    fprintf(stderr, "[gk] vcpu %d: guest thread tid %ld (from vcpu %d) pc=%#llx sp=%#llx ring %d\n",
            t->id, t->tid, t->start.parent_id, (unsigned long long)t->start.regs.elr,
            (unsigned long long)t->start.regs.sp, t->start.user ? 3 : 0);
}

#endif  // defined(__aarch64__) && !GK_HVF
