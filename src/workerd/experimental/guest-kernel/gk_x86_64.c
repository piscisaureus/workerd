// gk: x86-64 (KVM): descriptor tables, vCPU setup, registers, TLB flushes, root
// switches, protection-key registers, host-backing faults and the run loop.
#include "gk_internal.h"
#if defined(__x86_64__)

// ---- page-table entries -----------------------------------------------------
// The leaf PTE map4k_root installs for `va` (see there for `flags`): the frame
// is the page itself (guest-physical == host-virtual), U unless the page is
// supervisor, W and NX from the protection, and the page's protection key.
static int pte_leaf(uint64_t va, uint64_t flags, int pkey, int super, uint64_t *out) {
  uint64_t pte = (va & ~0xfffULL) | PTE_P;
  if (!super) pte |= PTE_U;
  if (flags & 2) pte |= PTE_W;
  if (flags & GK_MAP_WALLED) pte |= PTE_WALL;
  if (!(flags & 4)) pte |= PTE_NX;
  pte |= ((uint64_t)pkey << PTE_PKEY_SHIFT) & PTE_PKEY_MASK;
  *out = pte;
  return 0;
}

// A present leaf rewritten for the protection of `rp` (see gk_reprot): W and
// NX follow it, and a writable page at `v` inside the host-frame wall gets
// PTE_WALL instead of W.
static uint64_t pte_reprotect(uint64_t e, const gk_reprot *rp, uintptr_t v) {
  uint64_t n = e & ~(PTE_W | PTE_NX | PTE_WALL);
  if (!(rp->prot & 4)) n |= PTE_NX;
  if (rp->prot & 2) n |= (v >= rp->wall_lo && v < rp->wall_hi) ? PTE_WALL : PTE_W;
  return n;
}

// ---- guest-physical addresses -----------------------------------------------
// Guest-physical is host-virtual on x86-64 (see the MMU layer): a region's
// guest-physical address is its start, and nothing is allocated or kept.
static int gpa_assign(uintptr_t s, uintptr_t e, uint64_t *gpa) {
  (void)e;
  *gpa = s;
  return 0;
}
static void gpa_unassign(uintptr_t s, uintptr_t e, uint64_t gpa) { (void)s; (void)e; (void)gpa; }
static void gpa_retire(gk_region *n) { (void)n; }
static void region_set_gpa(gk_region *n, uint64_t gpa) { (void)n; (void)gpa; }
static inline uint64_t region_gpa(const gk_region *n) { return n->start; }
// No host range is kept out of the memslots on x86-64 (see region_ensure).
static int region_clip_holes(uintptr_t *s, uintptr_t *gap_end) {
  (void)s; (void)gap_end;
  return 0;
}


// Every gate runs its handler on IST1, the vCPU's exception stack (see
// vcpu_init), never on the interrupted code's stack.
static void set_idt_gate(uint8_t *idt, int vec, uint64_t h, int ist) {
  uint8_t *e = idt + vec * 16;
  *(uint16_t *)(e + 0) = h & 0xffff;
  *(uint16_t *)(e + 2) = 0x08;
  e[4] = (uint8_t)ist;  // IST index
  e[5] = 0x8e;
  *(uint16_t *)(e + 6) = (h >> 16) & 0xffff;
  *(uint32_t *)(e + 8) = (h >> 32);
}
// Load the flat ring-0 code and data descriptors (GDT indices 1 and 2) into
// s's CS and SS/DS/ES: the privilege every guest entry starts at, and the one
// the stubs and handlers need.
static void sregs_ring0(struct kvm_sregs *s) {
  struct kvm_segment cs = {.base = 0, .limit = 0xffffffff, .selector = 0x08,
                           .type = 11, .present = 1, .s = 1, .l = 1, .g = 1};
  struct kvm_segment ds = {.base = 0, .limit = 0xffffffff, .selector = 0x10,
                           .type = 3, .present = 1, .s = 1, .db = 1, .g = 1};
  s->cs = cs;
  s->ds = s->es = s->ss = ds;
}

static int vcpu_init(gk_thread *t, int flags) {
  if (t->inited) return 0;
  int id, fd, reused = 0;
  struct kvm_run *run = NULL;
  uint64_t stack_top = 0, ist = 0;
  void *side_stack = NULL;
  gk_runner *runner = NULL;
  gk_park *park = NULL;
  int efd = -1, stats_fd = -1;
  pthread_mutex_lock(&G->lock);
  if (G->parked_n > 0) {
    gk_parked_vcpu *p = &G->parked[--G->parked_n];
    fd = p->fd; id = p->id; run = p->run; stack_top = p->stack_top; ist = p->ist;
    side_stack = p->side_stack;
    runner = p->runner;
    park = p->park;
    efd = p->efd;
    stats_fd = p->stats_fd;
    reused = 1;
  }
  pthread_mutex_unlock(&G->lock);
  if (!reused) {
    id = atomic_fetch_add(&G->vcpus_created, 1);
    fd = ioctl(G->vmfd, KVM_CREATE_VCPU, id);
    if (fd < 0) { G->err = "KVM_CREATE_VCPU"; return -1; }
    if (ioctl(fd, KVM_SET_CPUID2, GK_CPUID) < 0) { G->err = "KVM_SET_CPUID2"; return -1; }
    run = mmap(NULL, G->run_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (run == MAP_FAILED) { G->err = "mmap kvm_run"; return -1; }
#ifdef KVM_GET_STATS_FD
    // Taken now and kept: a vCPU ioctl waits behind a run in progress, and a
    // resident vCPU is in one between turns (see gk_debug_vcpu_stat).
    stats_fd = ioctl(fd, KVM_GET_STATS_FD, NULL);
#endif
    if (G->runner_mode && !(runner = runner_start(fd))) return -1;

    // Exception stack. The guest runs ordinary user-mode code, which keeps
    // locals in the 128-byte red zone below rsp; the ABI promises that zone
    // survives asynchronous events. A ring-0 exception without a stack switch
    // would push its frame right there, so every gate switches to this vCPU's
    // IST1 stack, selected through a per-vCPU TSS that lives in the last page
    // of the region. Eager-mapped: the switch itself must never fault.
    uint8_t *ir = mmap(NULL, GK_IST_BYTES, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ir == MAP_FAILED) { G->err = "mmap exception stack"; return -1; }
    pthread_mutex_lock(&G->lock);
    // kvm_run is host/KVM shared state; the IST region holds the exception
    // stack, TSS and RSP0 stack. Ring 3 must reach neither: refuse the former,
    // map the latter supervisor (registered before mapping so U is cleared).
    prot_add((uintptr_t)run, (uintptr_t)run + G->run_size, GK_PROT_REFUSE);
    prot_add((uintptr_t)ir, (uintptr_t)ir + GK_IST_BYTES, GK_PROT_SUPER);
    int irc = mmu_map_range((uintptr_t)ir, (uintptr_t)ir + GK_IST_BYTES, 1 | 2);
    pthread_mutex_unlock(&G->lock);
    if (irc < 0) { G->err = "map exception stack"; return -1; }
    ist = (uint64_t)(uintptr_t)ir;
    uint8_t *tss = ir + GK_IST_BYTES - 0x1000;
    *(uint64_t *)(tss + 36) = (uint64_t)(uintptr_t)tss;  // IST1: stack top is just below the TSS
    *(uint64_t *)(tss + 44) = (uint64_t)(uintptr_t)ir + GK_IST2_TOP;  // IST2: the NMI's
    // RSP0: the ring-0 stack the CPU switches to on a ring-3 -> ring-0 gate that
    // does not use an IST (all gk gates use an IST, so this is defensive). The
    // second page of the region, disjoint from the ISTs' downward growth; a
    // resident vCPU's park loop runs on it too (the loop pushes nothing).
    *(uint64_t *)(tss + 4) = (uint64_t)(uintptr_t)ir + GK_PARK_STACK_TOP;
    *(uint16_t *)(tss + 102) = 0x68;                     // I/O map base past the limit
    // A resident vCPU (see the runner section): its park frame in the
    // region's first page, its eventfd and the ioeventfd that signals it from
    // the park loop, and its own SYSCALL entry. The ids an xAPIC NMI cannot
    // address, and a failed ioeventfd registration (KVM's PIO bus is full),
    // leave the vCPU with a plain runner.
    if (G->resident_mode && id <= GK_PARK_MAX_ID) {
      efd = eventfd(0, EFD_CLOEXEC);
      struct kvm_ioeventfd ie = {0};
      ie.addr = GK_PARK_PORT_BASE + (uint64_t)id;
      ie.len = 1;
      ie.fd = efd;
      ie.flags = KVM_IOEVENTFD_FLAG_PIO;
      // The LAPIC KVM gave the vCPU is hardware-enabled but KVM's map of
      // APIC ids, which an MSI is matched against, does not list it until
      // its state is set; this sets it as it is, software-enabled (SVR bit
      // 8), and makes the vCPU a destination.
      // With a LAPIC, KVM also starts every vCPU but the first as an
      // application processor waiting for INIT and SIPI, in which state
      // KVM_RUN blocks; this makes it runnable as the first one is.
      struct kvm_lapic_state lapic;
      struct kvm_mp_state mp = {.mp_state = KVM_MP_STATE_RUNNABLE};
      int lapic_ok = ioctl(fd, KVM_GET_LAPIC, &lapic) == 0;
      if (lapic_ok) {
        *(uint32_t *)(lapic.regs + 0xf0) |= 0x100;
        lapic_ok = ioctl(fd, KVM_SET_LAPIC, &lapic) == 0 && ioctl(fd, KVM_SET_MP_STATE, &mp) == 0;
      }
      if (efd >= 0 && lapic_ok && ioctl(G->vmfd, KVM_IOEVENTFD, &ie) == 0) {
        park = (gk_park *)ir;
        park->spin = G->runner_spin;
        park->port = ie.addr;
        park->stack = (uint64_t)(uintptr_t)ir + GK_PARK_STACK_TOP;
        runner->park = park;
        runner->efd = efd;
        pthread_mutex_lock(&G->lock);
        G->resident_n++;
        pthread_mutex_unlock(&G->lock);
      } else {
        if (G->dbg)
          fprintf(stderr, "[gk] vcpu %d: not resident: %s failed: %s\n", id,
                  efd < 0 ? "eventfd" : !lapic_ok ? "KVM_SET_LAPIC" : "KVM_IOEVENTFD",
                  strerror(errno));
        if (efd >= 0) close(efd);
        efd = -1;
      }
    } else if (G->resident_mode && G->dbg) {
      fprintf(stderr, "[gk] vcpu %d: not resident: id past %d\n", id, GK_PARK_MAX_ID);
    }
  }

  // A private guest stack for this vCPU, eager-mapped so interrupt delivery
  // (which pushes a frame) never itself faults. A parked vCPU may bring one
  // along (and a gk_run_here side stack); they are kept (and parked again
  // later) even if this thread has no use for them, so stacks are neither
  // leaked nor left with dangling PTEs.
  const size_t stksz = 2 * 1024 * 1024;
  if ((flags & GK_VCPU_STACK) && stack_top == 0) {
    uint8_t *stk = mmap(NULL, stksz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) { G->err = "mmap guest stack"; return -1; }
    pthread_mutex_lock(&G->lock);
    int mrc = mmu_map_range((uintptr_t)stk, (uintptr_t)stk + stksz, 1 | 2);  // rw, NX
    pthread_mutex_unlock(&G->lock);
    if (mrc < 0) { G->err = "map guest stack"; return -1; }
    stack_top = (uintptr_t)stk + stksz;
  }

  struct kvm_sregs s;
  ioctl(fd, KVM_GET_SREGS, &s);
  s.cr3 = (uint64_t)(uintptr_t)G->pml4;
  // PGE makes the G bit of a PTE take effect (see the global-pages section).
  s.cr4 = CR4_PAE | CR4_PGE | CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE | CR4_PKE;
  // The guest runs in ring 0, where the CPU ignores a PTE's write-protect bit
  // and a protection key's write-disable bit unless CR0.WP is set. It is, so
  // read-only pages and write-disabled keys hold for the guest as they would
  // for a user-mode process.
  s.cr0 = CR0_PE | CR0_WP | CR0_PG;
  s.efer = EFER_LME | EFER_LMA | EFER_SCE | EFER_NXE;
  sregs_ring0(&s);
  uint64_t fsbase = 0, gsbase = 0;
  host_syscall(SYS_arch_prctl, 0x1003, (long)&fsbase, 0, 0, 0, 0);  // ARCH_GET_FS
  host_syscall(SYS_arch_prctl, 0x1004, (long)&gsbase, 0, 0, 0, 0);  // ARCH_GET_GS
  s.fs = s.ds; s.fs.base = fsbase;
  s.gs = s.ds; s.gs.base = gsbase;
  if (park) park->user_gs = gsbase;  // what the park loop swaps back in for the turn
  // TR comes from the vCPU's cached descriptor, set here; the GDT has no TSS
  // entry because the guest never executes ltr (iretq reloads only CS and SS).
  struct kvm_segment tr = {.base = ist + GK_IST_BYTES - 0x1000, .limit = 0x67,
                           .selector = 0x18, .type = 11, .present = 1};
  s.tr = tr;
  s.gdt.base = G->gdt_va; s.gdt.limit = 6 * 8 - 1;  // through the ring-3 descriptors (0x28)
  s.idt.base = G->idt_va; s.idt.limit = 256 * 16 - 1;
  if (ioctl(fd, KVM_SET_SREGS, &s) < 0) { G->err = "KVM_SET_SREGS"; return -1; }
  if (G->sync_regs) {
    // From here the vCPU's registers go through kvm_run (see regs_get). Its
    // sregs copy is made current now, as sync_cr3 may read it before this vCPU
    // first runs; and a dirty flag left by a previous thread's write that never
    // ran must not load that thread's registers over what was just set.
    ioctl(fd, KVM_GET_SREGS, &run->s.regs.sregs);
    run->kvm_valid_regs = GK_SYNC_REGS;
    run->kvm_dirty_regs = 0;
  }

  if (reused) {
    // Fresh x87/SSE control state, as a new thread would start with; the
    // previous thread's register contents are otherwise irrelevant because
    // the guest entry point sets every register it relies on.
    struct kvm_fpu f = {0};
    f.fcw = 0x37f;
    f.mxcsr = 0x1f80;
    if (ioctl(fd, KVM_SET_FPU, &f) < 0) { G->err = "KVM_SET_FPU"; return -1; }
    goto ready;
  }

  // XCR0 as xcr0_select chose it from the CPUID set above (KVM_SET_XCRS
  // checks the value against that CPUID, so the order matters). It stays
  // with the vCPU through the pool: the reuse path above resets only the
  // x87/SSE control words.
  struct kvm_xcrs xcrs = {0};
  xcrs.nr_xcrs = 1; xcrs.xcrs[0].xcr = 0; xcrs.xcrs[0].value = G->xcr0;
  if (ioctl(fd, KVM_SET_XCRS, &xcrs) < 0) { G->err = "KVM_SET_XCRS"; return -1; }

  // KVM starts a new vCPU's TSC near zero, but the guest runs the host's vDSO,
  // whose clock_gettime derives time from rdtsc against the host's TSC. With an
  // offset TSC the vDSO sees the counter behind its last update and returns the
  // stale tick-granular base time. A zero TSC offset makes guest rdtsc equal
  // host rdtsc, so vDSO time is exact. Best effort: older kernels lack the attr.
  {
    int64_t zero = 0;
    struct kvm_device_attr ta = {.group = KVM_VCPU_TSC_CTRL,
                                 .attr = KVM_VCPU_TSC_OFFSET,
                                 .addr = (uint64_t)(uintptr_t)&zero};
    if (ioctl(fd, KVM_SET_DEVICE_ATTR, &ta) < 0 && G->dbg)
      fprintf(stderr, "[gk] vcpu %d: KVM_VCPU_TSC_OFFSET unsupported: %s\n", id,
              strerror(errno));
  }

  struct { struct kvm_msrs h; struct kvm_msr_entry e[4]; } m = {0};
  m.h.nmsrs = 4;
  m.e[0].index = MSR_EFER; m.e[0].data = EFER_LME | EFER_LMA | EFER_SCE | EFER_NXE;
  // STAR[47:32] = 0x08: SYSCALL enters ring-0 CS 0x08, SS 0x10.
  // STAR[63:48] = 0x1b: SYSRET adds 8 and 16 to this and uses the result
  // verbatim -- the RPL is not forced -- so the base must already carry RPL 3.
  // SS = 0x1b + 8 = 0x23 (GDT index 4, ring-3 data), CS = 0x1b + 16 = 0x2b
  // (GDT index 5, ring-3 code). A base of 0x18 (RPL 0) would return ring 3 with
  // RPL-0 selectors, and the IRETQ off a ring-3 fault would then #GP on SS.
  m.e[1].index = MSR_STAR; m.e[1].data = ((uint64_t)0x08 << 32) | ((uint64_t)0x1b << 48);
  m.e[2].index = MSR_LSTAR;
  m.e[2].data = park ? (uintptr_t)&gk_park_syscall_tramp : (uintptr_t)&gk_syscall_tramp;
  m.e[3].index = MSR_SYSCALL_MASK; m.e[3].data = 0x3f7fd5;
  if (ioctl(fd, KVM_SET_MSRS, &m) < 4) { G->err = "KVM_SET_MSRS"; return -1; }

ready:
  t->id = id;
  t->fd = fd;
  t->run = run;
  t->stack_top = stack_top;
  t->ist = ist;
  t->side_stack = side_stack;
  t->runner = runner;
  t->park = park;
  t->parked = 0;      // a fresh or pooled vCPU is out of KVM_RUN (see vcpu_park)
  t->turn_seq = park ? atomic_load_explicit(&park->go, memory_order_seq_cst) : 0;
  t->run_seq = runner ? atomic_load_explicit(&runner->go, memory_order_seq_cst) : 0;
  t->cr3_shadow = 0;
  t->efd = efd;
  t->stats_fd = stats_fd;
  t->root_gen = 0;    // a fresh or reused vCPU flushes its TLB at first entry
  t->last_fault = 0;
  t->fault_repeat = 0;
  t->backing_retries = 0;
  t->pkru_ready = 0;  // a fresh or reused vCPU gets its PKRU at first entry
  t->user_mode = 0;   // ring 0 until enter_guest or child_entry selects ring 3
  t->tlb_touched = 1; // no known TLB state and no previous wall: the first
  t->prev_ro_lo = t->prev_ro_hi = 0;  // wall_raise on this vCPU flushes
  if (!t->active_pml4) t->active_pml4 = G->pml4;
  t->inited = 1;
  if (G->dbg)
    fprintf(stderr, "[gk] vcpu %d %s by tid %ld\n", id, reused ? "reused" : "created",
            host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0));
  return 0;
}

// Eager-map the pages spanning the interrupt handlers and trampolines. They
// must be present before any fault, or delivering the first fault would itself
// fault and triple-fault. All live together in gk_asm_x86_64.S, so one small range
// covers them (the rest of the binary and V8 are demand-paged on execute).
static int map_handler_text(void) {
  uintptr_t hs[] = {
      (uintptr_t)&gk_exc_de, (uintptr_t)&gk_exc_ud, (uintptr_t)&gk_exc_df,
      (uintptr_t)&gk_exc_gp, (uintptr_t)&gk_exc_pf, (uintptr_t)&gk_syscall_tramp,
      (uintptr_t)&gk_resolve_tramp,
      (uintptr_t)&gk_exit_tramp, (uintptr_t)&gk_pkru_stub, (uintptr_t)&gk_flush_stub,
      (uintptr_t)&gk_flush_stub_invpcid, (uintptr_t)gk_invpcid_desc,
      (uintptr_t)&gk_user_syscall_resume, (uintptr_t)&gk_user_launch,
      (uintptr_t)&gk_launch_cr3, (uintptr_t)&gk_user_launch_cr3,
      (uintptr_t)&gk_user_child_launch};
  // gk_user_exit_tramp is deliberately absent: it runs in ring 3, so it must
  // stay a user page (its own page-aligned section, demand-paged as user r-x).
  uintptr_t lo = hs[0], hi = hs[0];
  for (size_t i = 1; i < sizeof hs / sizeof hs[0]; i++) {
    if (hs[i] < lo) lo = hs[i];
    if (hs[i] > hi) hi = hs[i];
  }
  lo &= ~0xfffUL;
  hi = (hi + 64 + 0xfff) & ~0xfffUL;
  // The addresses are resolved by the linker, so the layout assumption above
  // is checked here rather than at compile time: if the ring-3 trampoline's
  // page ever fell inside the supervisor span, every ring-3 exit would fault.
  uintptr_t tramp = (uintptr_t)&gk_user_exit_tramp & ~0xfffUL;
  if (tramp < hi && tramp + 0x1000 > lo)
    gk_fatal("gk_user_exit_tramp shares a page with the supervisor handler text");
  // The handlers run in ring 0 and must be unreadable to ring 3, so map them
  // supervisor r-x. Register before mapping so map4k_root clears U.
  prot_add(lo, hi, GK_PROT_SUPER);
  return mmu_map_range(lo, hi, 1 | 4);  // r-x
}

// Pick the TLB flush stub (see gk_asm_x86_64.S): INVPCID when the CPUID every vCPU
// gets (the host's, as KVM supports it) carries it in leaf 7 EBX bit 10 --
// KVM then lets the guest execute it natively under nested paging, one VM
// exit per flush -- and the CR4.PGE toggle otherwise, three. GK_NO_INVPCID in
// the environment forces the toggle, so the fallback stays testable on a CPU
// that has both. Both stubs drop every entry, global ones included (see the
// global-pages section).
static void flush_stub_select(void) {
  G->flush_stub = gk_flush_stub;
  for (unsigned i = 0; i < GK_CPUID->nent && !getenv("GK_NO_INVPCID"); i++) {
    const struct kvm_cpuid_entry2 *e = &GK_CPUID->entries[i];
    if (e->function == 7 && e->index == 0 && (e->ebx & (1u << 10))) {
      G->flush_stub = gk_flush_stub_invpcid;
      break;
    }
  }
  if (G->dbg)
    fprintf(stderr, "[gk] TLB flush stub: %s\n",
            G->flush_stub == gk_flush_stub_invpcid ? "invpcid" : "cr4.pge toggle");
}
// ---- VM setup (see gk_init) --------------------------------------------------
// The host facts the host-frame wall needs (see stack_wall_bounds): the size of
// glibc's static TLS block.
static void wall_probe(void) {
  *(void **)&G->tls_static_info = dlsym(RTLD_DEFAULT, "_dl_get_tls_static_info");
  if (!G->tls_static_info)
    fprintf(stderr, "[gk] _dl_get_tls_static_info not found: gk_run_here_user builds no "
                    "host-frame wall on pthread stacks\n");
}

// The host page size, which on x86-64 must be the guest's (see the page-sizes
// section).
static int page_size_init(void) {
  if (sysconf(_SC_PAGESIZE) != 4096) { G->err = "host page size is not 4KiB"; return -1; }
  return 0;
}

static int vm_create(void) {
  if (kvm_open() < 0) return -1;
  G->vmfd = ioctl(G->kvm, KVM_CREATE_VM, 0);
  if (G->vmfd < 0) { G->err = "KVM_CREATE_VM"; return -1; }
  // Resident vCPUs halt inside KVM_RUN and are woken by an NMI, both of
  // which need an in-kernel LAPIC: a split irqchip (LAPICs in the kernel,
  // no PIC or IOAPIC), enabled before any vCPU exists. Without it the
  // runners return to userspace after every turn as GK_RUNNER=1 does.
  if (G->resident_mode) {
    struct kvm_enable_cap cap = {.cap = KVM_CAP_SPLIT_IRQCHIP};
    cap.args[0] = 24;
    if (ioctl(G->vmfd, KVM_ENABLE_CAP, &cap) != 0) {
      fprintf(stderr, "[gk] GK_RUNNER=resident: KVM_CAP_SPLIT_IRQCHIP: %s; runners will return "
                      "to userspace after every turn\n", strerror(errno));
      G->resident_mode = 0;
    }
  }
  return 0;
}

// Guest-physical is host-virtual, so the page-table arena needs nothing before
// the first table is linked in; its memslots come with guest_tables_init.
static int pt_arena_init(void) { return 0; }

// The descriptor tables, the page-table arena's memslots, the handler text,
// and what every vCPU gets from KVM: CPUID, the flush stub, the kvm_run size
// and the register exchange.
static int guest_tables_init(void) {
  uint8_t *tables = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (tables == MAP_FAILED) { G->err = "mmap gdt/idt"; return -1; }
  G->gdt_va = (uintptr_t)tables;
  G->idt_va = (uintptr_t)tables + 0x1000;
  // The GDT and IDT are read by the CPU (a supervisor access) during exception
  // delivery and ring transitions; ring-3 code has no business touching them.
  prot_add(G->gdt_va, G->gdt_va + 0x2000, GK_PROT_SUPER);
  // GDT layout, in the order SYSCALL/SYSRET and the exception frames need:
  //   0x08 ring-0 code64   (SYSCALL loads CS from STAR[47:32] = 0x08)
  //   0x10 ring-0 data     (SYSCALL loads SS = 0x08 + 8)
  //   0x18 unused          (TR's nominal selector; TR uses a cached descriptor)
  //   0x20 ring-3 data     (SYSRET loads SS = STAR[63:48] + 8 = 0x1b + 8 = 0x23)
  //   0x28 ring-3 code64   (SYSRET loads CS = STAR[63:48] + 16 = 0x1b + 16 = 0x2b)
  // STAR[63:48] is 0x1b (RPL 3): SYSRET adds 8/16 and uses the RPL as-is, so
  // ring 3 runs with SS 0x23 and CS 0x2b. The CPU reloads both from the GDT on
  // the IRETQ that returns from a ring-3 fault, so the +8/+16 order, RPL and
  // DPL=3 must all be exact.
  uint64_t *gdt = (uint64_t *)(uintptr_t)G->gdt_va;
  gdt[0] = 0;
  gdt[1] = 0x00AF9A000000FFFFULL;  // ring-0 code64 (DPL 0, L=1)
  gdt[2] = 0x00CF92000000FFFFULL;  // ring-0 data   (DPL 0)
  gdt[3] = 0;
  gdt[4] = 0x00CFF2000000FFFFULL;  // ring-3 data   (DPL 3)
  gdt[5] = 0x00AFFA000000FFFFULL;  // ring-3 code64 (DPL 3, L=1)
  uint8_t *idt = (uint8_t *)(uintptr_t)G->idt_va;
  memset(idt, 0, 0x1000);
  set_idt_gate(idt, 0, (uintptr_t)&gk_exc_de, 1);
  // The NMI a halted resident vCPU is kicked with (see the runner section),
  // on its own stack: it can arrive while an exception handler is on IST1.
  set_idt_gate(idt, 2, (uintptr_t)&gk_exc_nmi, 2);
  set_idt_gate(idt, 6, (uintptr_t)&gk_exc_ud, 1);
  set_idt_gate(idt, 8, (uintptr_t)&gk_exc_df, 1);
  set_idt_gate(idt, 13, (uintptr_t)&gk_exc_gp, 1);
  set_idt_gate(idt, 14, (uintptr_t)&gk_exc_pf, 1);

  // The page-table arena is read by the CPU page walker via guest-physical
  // addresses, so it needs memslots (but no PTEs of its own).
  if (region_ensure(G->pt_base, G->pt_base + G->pt_bytes) < 0) { G->err = "memslot for PT arena"; return -1; }
  // GDT/IDT and the handler text must be present before the first fault.
  if (mmu_map_range(G->gdt_va, G->gdt_va + 0x2000, 1 | 2) < 0) { G->err = "map gdt/idt"; return -1; }
  if (map_handler_text() < 0) { G->err = "map handler text"; return -1; }
  // The resolve table is read by the trampoline at ring 0, on the caller's
  // (ring-3) stack; mapping it now, supervisor r/w, keeps that path free of
  // demand faults in every root.
  if (mmu_map_range(GK_RESOLVE_START, GK_RESOLVE_END, 1 | 2) < 0) { G->err = "map resolve table"; return -1; }

  GK_CPUID->nent = GK_CPUID_ENTRIES;
  if (ioctl(G->kvm, KVM_GET_SUPPORTED_CPUID, GK_CPUID) < 0) { G->err = "GET_SUPPORTED_CPUID"; return -1; }
  flush_stub_select();
  G->park_flush_kind = G->flush_stub == gk_flush_stub_invpcid ? 1 : 2;
  xcr0_select();
  G->run_size = ioctl(G->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  // Register exchange through kvm_run (see regs_get) when KVM offers it for
  // both register sets; GK_NO_SYNC_REGS in the environment forces the ioctls.
  if (!getenv("GK_NO_SYNC_REGS")) {
    long caps = ioctl(G->kvm, KVM_CHECK_EXTENSION, KVM_CAP_SYNC_REGS);
    G->sync_regs = caps > 0 && (caps & GK_SYNC_REGS) == GK_SYNC_REGS;
  }
  if (G->dbg) fprintf(stderr, "[gk] registers via %s\n", G->sync_regs ? "kvm_run" : "ioctl");
  return 0;
}

// ---- vCPU registers ---------------------------------------------------------
// With KVM_CAP_SYNC_REGS (G->sync_regs) a vCPU's general and special registers
// are exchanged through its mmap'd kvm_run instead of by ioctl: KVM stores both
// sets into run->s.regs at every KVM_RUN return, including a failed one
// (kvm_valid_regs, set once by vcpu_init), and at the next KVM_RUN loads the
// sets flagged in kvm_dirty_regs from there, before it completes a pending
// port write or runs the guest: exactly where an ioctl issued in between would
// have taken effect. A write is thus deferred to the next KVM_RUN on this
// vCPU, which every writer follows it with; sregs_set_now is for writes that
// must take effect back to back (host_backing_retry). kvm_run is refused to the
// guest, so it can no more forge a register here than an ioctl argument.
// Without the capability (or with GK_NO_SYNC_REGS set), the ioctls.
static void regs_get(gk_thread *t, struct kvm_regs *r) {
  if (G->sync_regs) *r = t->run->s.regs.regs;
  else ioctl(t->fd, KVM_GET_REGS, r);
}

static void regs_set(gk_thread *t, const struct kvm_regs *r) {
  if (G->sync_regs) {
    t->run->s.regs.regs = *r;
    t->run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
  } else {
    ioctl(t->fd, KVM_SET_REGS, r);
  }
}

static void sregs_get(gk_thread *t, struct kvm_sregs *s) {
  if (G->sync_regs) *s = t->run->s.regs.sregs;
  else ioctl(t->fd, KVM_GET_SREGS, s);
}

static int sregs_set(gk_thread *t, const struct kvm_sregs *s) {
  if (G->sync_regs) {
    t->run->s.regs.sregs = *s;
    t->run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS;
    return 0;
  }
  return ioctl(t->fd, KVM_SET_SREGS, s) < 0 ? -1 : 0;
}

// Loads the sregs into the vCPU right away, by ioctl, keeping the kvm_run copy
// current so the next sregs_get reads what was set.
static int sregs_set_now(gk_thread *t, const struct kvm_sregs *s) {
  if (ioctl(t->fd, KVM_SET_SREGS, s) < 0) return -1;
  if (G->sync_regs) {
    t->run->s.regs.sregs = *s;
    t->run->kvm_dirty_regs &= ~(uint64_t)KVM_SYNC_X86_SREGS;
  }
  return 0;
}

// Run one of the gk_asm_x86_64.S stubs on this vCPU until it reports on `port`.
// Clobbers the vCPU's general registers: the caller reprograms them afterwards
// (gk_run and child_entry set the entry registers; a syscall exit sets the
// snapshot back and, because KVM only completes the pending outb when RIP is
// unchanged, points RIP at gk_syscall_tramp_resume). Sregs are untouched.
static int run_stub(gk_thread *t, void (*stub)(void), uint64_t rdi, uint64_t rsi,
                    uint16_t port, const char *what) {
  park_leave(t);  // a resident vCPU between turns is inside KVM_RUN
  struct kvm_regs regs = {0};
  regs.rip = (uintptr_t)stub;
  regs.rdi = rdi;
  regs.rsi = rsi;
  regs.rflags = 0x2;
  regs_set(t, &regs);
  for (;;) {
    t->tlb_touched = 1;  // the vCPU runs: its TLB is no longer known (see wall_raise)
    if (vcpu_run_ioctl(t) < 0) {
      if (errno == EINTR) continue;
      if (G->dbg) fprintf(stderr, "[gk] vcpu %d: %s stub KVM_RUN: %s\n", t->id, what, strerror(errno));
      return -1;
    }
    if (t->run->exit_reason == KVM_EXIT_IO && t->run->io.direction == KVM_EXIT_IO_OUT &&
        t->run->io.port == port)
      return 0;
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d: %s stub: unexpected exit reason=%u\n", t->id, what,
              t->run->exit_reason);
    return -1;
  }
}

// Flush this vCPU's whole TLB, global entries included, by running the flush
// stub gk_init selected (see flush_stub_select). Every full flush gk does
// goes through here, so gk_stats.tlb_flushes counts them all. No ioctl does
// this from the host: KVM flushes the guest TLB only when KVM_SET_SREGS
// changes a control register, and re-setting the same values is a no-op.
static int flush_run(gk_thread *t) {
  atomic_fetch_add_explicit(&G->tlb_flushes, 1, memory_order_relaxed);
  if (t->parked) {
    // The park loop flushes at its launch, before the turn's first
    // instruction, which is when every between-turn caller wants it.
    t->park->flush = G->park_flush_kind;
    return 0;
  }
  return run_stub(t, G->flush_stub, 0, 0, PORT_FLUSH, "flush");
}

// Flush this vCPU's TLB from a syscall exit, whose register snapshot is `r`:
// the guest runs the flush stub and resumes after the hypercall.
static void flush_tlb(gk_thread *t, struct kvm_regs *r) {
  if (flush_run(t) == 0) r->rip = (uintptr_t)&gk_syscall_tramp_resume;
}
// The host thread's PKRU is what KVM's page backing (get_user_pages) checks
// against a host VMA's key. No host VMA carries a key (pkey_mprotect reaches
// the host as mprotect), so the host PKRU cannot deny a backing; it is cleared
// anyway so that also holds for memory keyed outside gk's knowledge. The
// guest's protection is the guest PKRU, which KVM keeps separate from this.
static void host_pkru_allow_all(void) {
  __asm__ __volatile__("xor %%ecx,%%ecx\n\t"
                       "xor %%edx,%%edx\n\t"
                       "xor %%eax,%%eax\n\t"
                       "wrpkru"
                       ::: "eax", "ecx", "edx");
}

static uint32_t host_pkru(void) {
  uint32_t v;
  __asm__ __volatile__("rdpkru" : "=a"(v) : "c"(0) : "edx");
  return v;
}

// Set this vCPU's guest PKRU to (pkru & keep) | set and return the new value in
// *out (if non-NULL), by running gk_pkru_stub on the vCPU (see run_stub for the
// register contract). KVM's view of the guest PKRU (KVM_GET_XSAVE) is
// unreliable when the host PKRU is zero, as it is here: the XSAVE header bit
// that says PKRU is present is written from the host value, so the guest's
// value can be reported as zero. Running the stub reads and writes the real
// register.
static int guest_pkru_update(gk_thread *t, uint32_t keep, uint32_t set, uint32_t *out) {
  if (run_stub(t, gk_pkru_stub, set, keep, PORT_PKRU, "pkru") < 0) return -1;
  struct kvm_regs regs;
  regs_get(t, &regs);
  if (out) *out = (uint32_t)regs.rax;
  return 0;
}

// Ready this vCPU's root for an entry: bring an active arena's root up to date
// with the base root (see arena_sync_root) and find out whether the vCPU's CR3
// already names the thread's active root. If not, *load is set to the root
// the entry must load and 0 otherwise. The load itself is the guest's own MOV
// CR3 at the start of the turn (see enter_guest and the global-pages
// section), or cr3_load_host for an entry that does not go through
// enter_guest. The vCPU's CR3 is read back from KVM rather than remembered:
// a load from inside the guest, one from the host (host_backing_retry) and one
// that failed all leave it wherever the vCPU actually is, and an entry that
// assumed a root it does not have would run one isolate's turn under
// another's tables.
//
// A CR3 load drops every non-global translation. An unchanged CR3 drops
// nothing, so when page-table pages were freed since this vCPU last flushed
// (G->root_gen moved), the guest flushes explicitly (flush_run): the same
// root page may by now be a different arena's, or its subtrees may have been
// rebuilt from recycled tables, and the vCPU's cached translations would be
// stale. The freed tables were an arena's private ones, whose translations are
// never global, so on a changed CR3 the load alone covers the generation
// change too.
static int sync_cr3(gk_thread *t, uint64_t *load) {
  if (t->active_arena) arena_sync_if_stale(t->active_arena);
  unsigned gen = __atomic_load_n(&G->root_gen, __ATOMIC_ACQUIRE);
  uint64_t want_cr3 = (uint64_t)(uintptr_t)t->active_pml4;
  struct kvm_sregs s;
  // kvm_run's copy is as of the last return from KVM_RUN. A parked vCPU
  // has not returned since its last launch, which loaded cr3_shadow (or
  // left it), and its CR3 has changed only through launches since.
  if (t->parked) s.cr3 = t->cr3_shadow;
  else sregs_get(t, &s);
  *load = 0;
  if (s.cr3 != want_cr3) {
    *load = want_cr3;
  } else if (t->root_gen != gen) {
    if (flush_run(t) < 0) return -1;
  }
  t->cr3_shadow = *load ? *load : s.cr3;
  t->root_gen = gen;
  return 0;
}

// Load `cr3` into this vCPU from the host, for an entry that does not go
// through enter_guest (a guest-created thread's first, see child_entry). KVM
// then flushes the whole guest TLB, global entries included, at the next
// KVM_RUN.
static int cr3_load_host(gk_thread *t, uint64_t cr3) {
  struct kvm_sregs s;
  sregs_get(t, &s);
  s.cr3 = cr3;
  t->tlb_touched = 1;  // the TLB changes under the host's hand (see wall_raise)
  return sregs_set(t, &s);
}
// Enter the guest at fn(arg) with a stack whose top (16-byte aligned) is
// `stack_top`, and run it to completion. `user` selects the privilege level:
//
//  ring 0 (user == 0): RIP is set straight to fn, which runs privileged, and
//    returns into gk_exit_tramp -> PORT_EXIT.
//  ring 3 (user == 1): RIP is set to gk_user_launch, which SYSRETs down to fn
//    at ring 3 (RCX = fn, R11 = flags). fn returns into gk_user_exit_tramp,
//    which leaves the guest through the sentinel syscall. Untrusted code runs
//    this way: it cannot execute privileged instructions, reload CR3 or reach
//    the supervisor/refused pages, so the arena walls hold against it.
//
// `cr3`, when nonzero, is the root the turn must run under (see sync_cr3):
// RIP then goes to the variant of the entry stub that loads it into CR3 first
// (RSI = the root; gk_launch_cr3 also takes RAX = fn), inside the turn's own
// KVM_RUN. The guest's MOV CR3 is not intercepted under nested paging and
// keeps the TLB's global entries, the shared runtime's translations, whereas a
// CR3 set through KVM makes KVM flush the whole guest TLB (see the
// global-pages section). Either way the return value comes back through
// run_vcpu.
static long enter_guest(gk_thread *t, long (*fn)(void *), void *arg, uint64_t stack_top,
                        int user, uint64_t cr3) {
  uint64_t sp = stack_top - 8;
  if (cr3) {
    atomic_fetch_add_explicit(&G->root_switches, 1, memory_order_relaxed);
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d: root switch to %#llx (%s)\n", t->id,
              (unsigned long long)cr3, t->active_arena ? "arena" : "base");
  }
  if (t->park) {
    // A resident vCPU (see the runner section): the entry goes through the
    // park frame, and the park loop loads the root and the registers. A
    // ring-0 turn returns into the parking exit stub; a ring-3 turn's exit
    // sentinel reaches the vCPU's own SYSCALL entry, which parks. A vCPU
    // that is not in the loop (fresh, or out of KVM_RUN since a fault ended
    // its last turn) is started at gk_park_entry, which parks and launches.
    *(uint64_t *)sp = user ? (uintptr_t)&gk_user_exit_tramp : (uintptr_t)&gk_park_exit_tramp;
    // Everything the turn reads goes before `go`: the loop launches the
    // instant it sees the word move, and a hostcall early in the turn
    // reads tls_in_guest to decide whether it is in the guest at all.
    t->user_mode = user;
    tls_in_guest = 1;
    gk_park *p = t->park;
    p->cr3 = cr3;
    p->rsp = sp;
    p->fn = (uintptr_t)fn;
    p->arg = (uintptr_t)arg;
    p->user = (uint64_t)user;
    t->turn_seq++;
    atomic_store_explicit(&p->go, t->turn_seq, memory_order_seq_cst);
    if (t->parked) {
      if (atomic_load_explicit(&p->halted, memory_order_seq_cst)) park_kick(t);
    } else {
      // The loop starts afresh, and walks its root from its first
      // instruction: the turn's root goes in through kvm_run, where KVM
      // loads it before the guest runs, rather than through the loop's
      // launch path, which would run under whatever root the vCPU last
      // had (gone, possibly, with its arena). KVM flushes the TLB for it.
      if (cr3) {
        struct kvm_sregs s;
        sregs_get(t, &s);
        s.cr3 = cr3;
        sregs_set(t, &s);
        t->tlb_touched = 1;
        p->cr3 = 0;
      }
      atomic_store_explicit(&p->reload, 0, memory_order_seq_cst);
      atomic_store_explicit(&p->cur_cr3, t->cr3_shadow, memory_order_seq_cst);
      // A run that returned on a signal while the loop was halted leaves
      // the vCPU halted as far as KVM knows, and a halted vCPU's KVM_RUN
      // blocks until an event; it is runnable from the loop's first
      // instruction here.
      struct kvm_mp_state mp = {.mp_state = KVM_MP_STATE_RUNNABLE};
      ioctl(t->fd, KVM_SET_MP_STATE, &mp);
      struct kvm_regs regs = {0};
      regs.rip = (uintptr_t)&gk_park_entry;
      regs.rdi = (uintptr_t)p;
      regs.rsp = p->stack;
      regs.rflags = 0x2;
      regs_set(t, &regs);
    }
    long r = run_vcpu(t);
    tls_in_guest = 0;
    return r;
  }
  *(uint64_t *)sp = user ? (uintptr_t)&gk_user_exit_tramp : (uintptr_t)&gk_exit_tramp;

  struct kvm_regs regs = {0};
  regs.rsp = sp;
  regs.rdi = (uintptr_t)arg;
  regs.rsi = cr3;
  regs.rflags = 0x2;
  if (user) {
    regs.rip = cr3 ? (uintptr_t)&gk_user_launch_cr3 : (uintptr_t)&gk_user_launch;
    regs.rcx = (uintptr_t)fn;   // SYSRET target RIP
    regs.r11 = 0x2;             // SYSRET target RFLAGS
  } else if (cr3) {
    regs.rip = (uintptr_t)&gk_launch_cr3;
    regs.rax = (uintptr_t)fn;
  } else {
    regs.rip = (uintptr_t)fn;
  }
  regs_set(t, &regs);
  t->user_mode = user;
  tls_in_guest = 1;
  long r = run_vcpu(t);
  tls_in_guest = 0;
  return r;
}
// ---- host-backing faults ----------------------------------------------------
// KVM_RUN fails with EFAULT when the guest accesses a page whose PTE is in
// place but whose host backing is gone: the host side replaced or decommitted
// the mapping (a fixed mmap or mprotect to PROT_NONE, an munmap) without the
// change passing through a forwarded syscall, so the PTE was never dropped
// (see unmap_range_all). KVM cannot back the guest-physical page, and the run
// stops on the faulting instruction with the vCPU at whatever privilege the
// guest had, not in a handler. The access was an EPT violation, not a guest
// #PF, so guest CR2 still holds the previous demand fault's address, and this
// kernel's kvm_run carries no address for it either.
//
// Inside an arena the fault is turned into the guest #PF it would have been
// had the change been reflected: every PTE of the arena is dropped from the
// arena root (a late version of the one-page drop a reflected decommit does),
// the vCPU's TLB is flushed from the host, and the instruction is restarted.
// Its access now demand-faults with CR2 exact, and demand_map applies the
// host's current protection: a decommitted page is refused and the turn ends
// with GK_EFAULT at that address through the path every genuine fault takes; a
// page the host merely downgraded is remapped with what it has, and a write to
// it then fails through the fault-repeat limit; a page the host has meanwhile
// recommitted is mapped and the turn simply goes on. Another vCPU under the
// same arena root demand-faults its pages back. The retries are bounded per
// invocation in case the host keeps pulling pages from under the guest.
// Returns 1 to retry KVM_RUN, 0 to give the fault up (host_backing_fail).
static int host_backing_retry(gk_thread *t) {
  gk_arena *a = t->active_arena;
  if (!a || t->backing_retries >= GK_BACKING_RETRIES) return 0;
  t->backing_retries++;
  pthread_mutex_lock(&G->lock);
  unmap_range_root(a->pml4, a->base, a->end);
  pthread_mutex_unlock(&G->lock);
  // A CR3 change through KVM makes it flush the whole guest TLB (see the
  // global-pages section), and the stopped vCPU may be at ring 3, where it
  // cannot run the flush stub: swing CR3 through the base root and back to the
  // arena's, two loads that must both take effect (so not through kvm_run,
  // which would keep only the last). Should the second fail, the vCPU is left
  // on the base root, which the next entry's sync_cr3 reads back and corrects.
  struct kvm_sregs s;
  sregs_get(t, &s);
  s.cr3 = (uint64_t)(uintptr_t)G->pml4;
  if (sregs_set_now(t, &s) < 0) return 0;
  s.cr3 = (uint64_t)(uintptr_t)a->pml4;
  if (sregs_set_now(t, &s) < 0) return 0;
  if (G->dbg) {  // fault path: raw output only
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": host backing gone under an arena PTE; retrying as a guest fault (");
    db_dec(&d, t->backing_retries); db_str(&d, ")");
    db_flush(&d);
  }
  return 1;
}

// A KVM_RUN failure that is not retried (no arena is active, so there is no
// bounded set of PTEs to drop; the retries are used up; or errno is not
// EFAULT). Reports EFAULT as GK_EFAULT like any other fault; the address is
// the faulting instruction's, the closest thing to the access's address KVM
// leaves behind (a kernel that fills kvm_run.memory_fault supplies the page
// itself). Other errnos stay -1: they are not guest faults.
//
// Either way the vCPU is then put back into the state a turn's end leaves it
// in, so the next entry on this thread runs a normal turn: ring 0, since the
// stubs and handlers run privileged (run_stub's code page is supervisor, so a
// vCPU left at ring 3 faults on its first instruction there), and no event
// pending, since a fault taken during exception delivery would be requeued
// and delivered at the next entry. The registers are set afresh by every
// entry.
static long host_backing_fail(gk_thread *t, int err) {
  struct kvm_regs r; struct kvm_sregs s;
  regs_get(t, &r);
  sregs_get(t, &s);
  uint64_t addr = r.rip;
  int exact = 0;
#ifdef KVM_EXIT_MEMORY_FAULT
  if (t->run->exit_reason == KVM_EXIT_MEMORY_FAULT) {
    addr = t->run->memory_fault.gpa;  // guest-physical == host-virtual
    exact = 1;
  }
#endif
  if (G->dbg) {  // fault path: raw output only
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": KVM_RUN failed, errno "); db_dec(&d, err);
    if (err == EFAULT)
      db_str(&d, exact ? ": host backing gone at " : ": host backing gone under an access at rip ");
    else
      db_str(&d, ": rip ");
    db_hex(&d, addr);
    db_str(&d, " (cr2 "); db_hex(&d, s.cr2); db_str(&d, " is the previous demand fault)");
    db_flush(&d);
  }
  sregs_ring0(&s);
  sregs_set_now(t, &s);
  struct kvm_vcpu_events ev = {0};
  ioctl(t->fd, KVM_SET_VCPU_EVENTS, &ev);
  if (err != EFAULT) return -1;
  G->fault_addr = addr;
  return GK_EFAULT;
}

// A KVM_RUN that failed with errno `err`: whether that ends the turn, with
// *result set, or the run is to be retried.
static int run_failed(gk_thread *t, int err, long *result) {
  if (err == EINTR) return 0;
  if (err == EFAULT && host_backing_retry(t)) return 0;
  *result = host_backing_fail(t, err);
  return 1;
}

// Handle the exit a returned KVM_RUN reports: whether it ends the turn, with
// *result set, or the vCPU is to be run again from where it stopped.
static int handle_exit(gk_thread *t, long *result) {
#define TURN_ENDS(v) do { *result = (v); return 1; } while (0)
    switch (t->run->exit_reason) {
      case KVM_EXIT_IO: {
        if (t->run->io.direction != KVM_EXIT_IO_OUT) break;
        uint16_t port = t->run->io.port;
        if (port == PORT_SYSCALL) {
          struct kvm_regs r;
          regs_get(t, &r);
          // A ring-3 guest leaves through the sentinel exit syscall (ring 3
          // cannot use PORT_EXIT's OUT): its result is in RDI.
          if (r.rax == GK_EXIT_SYSCALL) TURN_ENDS((long)r.rdi);
          // A hostcall (see the hostcalls section) is answered here, never
          // forwarded and never shown to the filter.
          if (r.rax == GK_HOSTCALL_SYSCALL) {
            r.rax = (uint64_t)hostcall_dispatch(t, r.rdi);
            if (G->dbg)
              fprintf(stderr, "[gk] vcpu %d hostcall %lu -> %ld\n", t->id, (unsigned long)r.rdi,
                      (long)r.rax);
          } else {
            r.rax = (uint64_t)forward_syscall(t, &r);
          }
          // A ring-3 guest's syscall trampoline ran in ring 0; return it to
          // ring 3 with SYSRET rather than the ring-0 jmp. forward_syscall left
          // RCX/R11 (the SYSCALL-saved return RIP and flags) intact and may
          // have already pointed RIP at a resume label; override it to the
          // ring-3 one. Ring-0 guests keep RIP at the OUT so KVM completes it.
          if (t->user_mode) r.rip = (uintptr_t)&gk_user_syscall_resume;
          regs_set(t, &r);
        } else if (port == PORT_EXIT) {
          struct kvm_regs r;
          regs_get(t, &r);
          TURN_ENDS((long)r.rax);
        } else if (port == PORT_UDRIP) {
          struct kvm_regs r;
          regs_get(t, &r);
          G->fault_addr = r.rax;
          if (G->dbg) fprintf(stderr, "[gk] #UD at rip=%#llx\n", (unsigned long long)r.rax);
          TURN_ENDS(GK_EFAULT);
        } else if (port == PORT_DEMAND) {
          struct kvm_sregs s;
          sregs_get(t, &s);
          uint64_t cr2 = s.cr2;
          int repeat = (cr2 == t->last_fault);
          if (repeat) {
            if (++t->fault_repeat > 2) { G->fault_addr = cr2; TURN_ENDS(GK_EFAULT); }
          } else { t->last_fault = cr2; t->fault_repeat = 0; }
          // A protection-key violation is the guest CPU enforcing the page's
          // key against this vCPU's PKRU: a genuine fault, unless the entry
          // this vCPU used carried the page's previous key (another vCPU
          // changed it without a shootdown). The fault invalidated that entry,
          // so one retry against the current PTE tells the two apart.
          struct kvm_regs pr;
          regs_get(t, &pr);
          // The #PF frame on the IST stack: [err, rip, cs, rflags, rsp].
          const uint64_t *pf_frame = (const uint64_t *)(uintptr_t)pr.rsp;
          uint64_t pf_err = pf_frame[0], pf_sp = pf_frame[4];
          int pk = (pf_err & PF_ERR_PK) != 0;
          if (pk && repeat) {
            if (G->dbg) {
              gk_dbuf d = {.n = 0};
              db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
              db_str(&d, ": protection-key violation at "); db_hex(&d, cr2);
              db_str(&d, " (err="); db_hex(&d, pf_err); db_str(&d, ")");
              db_flush(&d);
            }
            G->fault_addr = cr2;
            TURN_ENDS(GK_EFAULT);
          }
          // A write into this turn's host-frame wall (see run_here) is the wall
          // holding: genuine, whatever the host protection of the page. Reads
          // there fall through and are mapped without write permission.
          if ((pf_err & PF_ERR_WR) && cr2 >= t->ro_lo && cr2 < t->ro_hi) {
            if (G->dbg) {
              gk_dbuf d = {.n = 0};
              db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
              db_str(&d, ": write into the host-frame wall at "); db_hex(&d, cr2);
              db_str(&d, " (wall ["); db_hex(&d, t->ro_lo); db_str(&d, ", ");
              db_hex(&d, t->ro_hi); db_str(&d, "), err="); db_hex(&d, pf_err);
              db_str(&d, ", rip="); db_hex(&d, pf_frame[1]);
              db_str(&d, ", rsp="); db_hex(&d, pf_sp); db_str(&d, ")");
              db_flush(&d);
              // Best-effort frame-pointer walk of the faulting code's stack.
              uint64_t bp = pr.rbp, lo = pf_sp, hi = pf_sp + (8UL << 20);
              for (int i = 0; i < 24 && bp >= lo && bp + 16 <= hi; i++) {
                uint64_t *f = (uint64_t *)(uintptr_t)bp;
                db_str(&d, "[gk]   frame "); db_dec(&d, i);
                db_str(&d, ": bp="); db_hex(&d, bp);
                db_str(&d, " ret="); db_hex(&d, f[1]);
                db_flush(&d);
                if (f[0] <= bp) break;
                bp = f[0];
              }
            }
            G->fault_addr = cr2;
            TURN_ENDS(GK_EFAULT);
          }
          gk_dm_refusal why = {NULL, 0, 0, 0, 0};
          int guard = GK_HANDLER_ENTER();
          int dm = demand_map(t, (uintptr_t)cr2, (uintptr_t)pf_sp, &why);
          GK_HANDLER_LEAVE(guard);
          if (dm < 0) {
            if (G->dbg) {  // still the fault path: raw output only
              struct kvm_regs rr;
              regs_get(t, &rr);
              // The #PF frame on the guest stack: [err, rip, cs, rflags, rsp].
              uint64_t *frame = (uint64_t *)(uintptr_t)rr.rsp;
              gk_dbuf d = {.n = 0};
              db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
              db_str(&d, ": unmappable fault cr2="); db_hex(&d, cr2);
              db_str(&d, " rip="); db_hex(&d, frame[1]);
              db_str(&d, " err="); db_hex(&d, frame[0]);
              db_str(&d, " rsp="); db_hex(&d, frame[4]);
              db_str(&d, " rax="); db_hex(&d, rr.rax);
              if (why.reason) {
                db_str(&d, ": "); db_str(&d, why.reason);
                if (why.page_end) {
                  db_str(&d, "; the page is in ["); db_hex(&d, why.page_base);
                  db_str(&d, ","); db_hex(&d, why.page_end); db_str(&d, ")");
                }
                if (why.turn_end) {
                  db_str(&d, ", the turn runs under ["); db_hex(&d, why.turn_base);
                  db_str(&d, ","); db_hex(&d, why.turn_end); db_str(&d, ")");
                } else if (why.page_end) {
                  db_str(&d, ", the turn runs on the base root");
                }
              }
              db_flush(&d);
              // Best-effort frame-pointer walk of the guest stack (host memory,
              // identity mapped); stops when the chain leaves the stack.
              uint64_t bp = rr.rbp, lo = frame[4], hi = frame[4] + (8UL << 20);
              for (int i = 0; i < 16 && bp >= lo && bp + 16 <= hi; i++) {
                uint64_t *f = (uint64_t *)(uintptr_t)bp;
                db_str(&d, "[gk]   frame "); db_dec(&d, i);
                db_str(&d, ": ret="); db_hex(&d, f[1]);
                db_flush(&d);
                if (f[0] <= bp) break;
                bp = f[0];
              }
            }
            G->fault_addr = cr2;
            TURN_ENDS(GK_EFAULT);
          }
          // The retry re-walks the page tables: a page fault invalidates the
          // TLB entry it was taken through, so no explicit flush is needed.
        } else if (port == PORT_PARK_LEAVE) {
          // The park loop left KVM_RUN at park_leave's request between
          // turns; inside a turn it cannot, so this is only ever seen by a
          // run_vcpu that found the run returned (see above) and resumes.
        } else if (port == PORT_FAULT) {
          struct kvm_regs rr; struct kvm_sregs sr;
          regs_get(t, &rr);
          sregs_get(t, &sr);
          if (G->dbg) {
            gk_dbuf d = {.n = 0};
            db_str(&d, "[gk] fatal exception, handler_rax="); db_hex(&d, rr.rax);
            db_str(&d, " cr2="); db_hex(&d, sr.cr2);
            db_flush(&d);
          }
          G->fault_addr = rr.rax;
          TURN_ENDS(GK_EFAULT);
        }
        break;
      }
      case KVM_EXIT_HLT:
        TURN_ENDS(0);
      case KVM_EXIT_SHUTDOWN:
        TURN_ENDS(GK_ESHUTDOWN);
      default:
        if (G->dbg) fprintf(stderr, "[gk] unexpected exit reason=%u\n", t->run->exit_reason);
        TURN_ENDS(-1);
    }
    return 0;
#undef TURN_ENDS
}

// Run this thread's vCPU from its current register state until the guest exits
// (via gk_exit_tramp, or the park loop for a resident vCPU) or faults.
static long run_vcpu(gk_thread *t) {
  long result;
  if (!t->park) {
    for (;;) {
      t->tlb_touched = 1;  // the vCPU runs: its TLB is no longer known (see wall_raise)
      if (vcpu_run_ioctl(t) < 0) {
        if (run_failed(t, errno, &result)) return result;
        continue;
      }
      if (handle_exit(t, &result)) return result;
    }
  }
  // Resident (see the runner section). A parked vCPU is inside KVM_RUN
  // already, launching the turn enter_guest wrote to its frame, unless its
  // run returned meanwhile (a signal), which is looked at before anything
  // else; a vCPU that is not parked is handed a run with the registers
  // enter_guest set. Then: the turn ends in the park loop, which leaves the
  // vCPU parked, or the run returns with an exit, which is handled as ever
  // and leaves the vCPU out of KVM_RUN until it is run again.
  int in_run = t->parked && !resident_run_returned(t);
  for (;;) {
    t->tlb_touched = 1;
    if (!in_run && !t->parked) resident_run_start(t);
    if (in_run || !t->parked) {
      if (resident_wait(t) == GK_EV_TURN_ENDED) {
        gk_park *p = t->park;
        if (atomic_load_explicit(&p->in_turn, memory_order_seq_cst)) {
          gk_dbuf d = {.n = 0};
          db_str(&d, "[gk] BUG: vcpu "); db_dec(&d, t->id);
          db_str(&d, ": turn "); db_dec(&d, (long)t->turn_seq);
          db_str(&d, " reported ended while the guest is in a turn (go ");
          db_dec(&d, (long)atomic_load_explicit(&p->go, memory_order_seq_cst));
          db_str(&d, ", done "); db_dec(&d, (long)atomic_load_explicit(&p->done, memory_order_seq_cst));
          db_str(&d, ", run "); db_dec(&d, (long)t->run_seq);
          db_str(&d, ", runner done "); db_dec(&d, (long)atomic_load_explicit(&t->runner->done, memory_order_seq_cst));
          db_str(&d, ", parked "); db_dec(&d, t->parked);
          db_str(&d, ", in_run "); db_dec(&d, in_run); db_str(&d, ")");
          db_flush(&d);
        }
        t->parked = 1;
        atomic_fetch_add_explicit(&G->park_turns, 1, memory_order_relaxed);
        return (long)p->result;
      }
    }
    in_run = 0;
    t->parked = 0;
    gk_runner *r = t->runner;
    if (r->ret < 0) {
      if (run_failed(t, r->err, &result)) return result;
      continue;
    }
    if (handle_exit(t, &result)) return result;
  }
}

// ---- the host thread (see stack_wall_bounds) ---------------------------------
// The thread pointer, the fs base: a pthread's TCB, which glibc places at the
// top of the thread's stack mapping, with its static TLS block just below.
static uintptr_t thread_pointer(void) {
  uintptr_t tp;
  __asm__ __volatile__("mov %%fs:0, %0" : "=r"(tp));
  return tp;
}

// The lowest address of the thread's data around thread pointer `tp`: the
// static TLS block below it. 0 if its size is unknown.
static uintptr_t thread_data_floor(uintptr_t tp) {
  if (!G->tls_static_info) return 0;
  size_t size = 0, align = 0;
  G->tls_static_info(&size, &align);
  return GUEST_FLOOR(tp - size);
}

// ---- diagnostics -------------------------------------------------------------
static int flush_uses_invpcid(void) { return G->flush_stub == gk_flush_stub_invpcid; }
static uintptr_t super_page(void) { return G->gdt_va; }

#endif  // defined(__x86_64__)
