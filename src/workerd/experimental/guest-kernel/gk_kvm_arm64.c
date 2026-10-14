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


#endif  // defined(__aarch64__) && !GK_HVF
