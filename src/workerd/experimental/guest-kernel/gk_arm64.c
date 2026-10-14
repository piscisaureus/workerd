// gk: arm64, under either hypervisor: the guest-physical allocator, W^X at EL1,
// vCPU setup, TLB flushes, root switches, guest entry and exception decoding.
#include "gk_internal.h"
#if defined(__aarch64__)

// arm64 never lets EL1 execute a page EL0 can write (an implicit PXN), so ring
// 0 cannot run code from a writable and executable user page, which is what
// V8's code space is on arm64. On such a fault the page's W is withheld,
// emulating W^X: a later write takes an ordinary permission fault, and
// demand_map maps the page writable again from the host protection. Returns 1
// if the PTE was changed, 0 if the fault has some other cause.
static int pte_exec_at_el1(gk_thread *t, uintptr_t va) {
  int changed = 0;
  pthread_mutex_lock(&G->lock);
  gk_arena *in = arena_containing(va);
  uint64_t *root = in ? (in == t->active_arena ? in->pml4 : NULL) : G->pml4;
  uint64_t e = root ? pte_lookup(root, va) : 0;
  if ((e & PTE_P) && (e & PTE_U) && PTE_IS_W(e) && !(e & PTE_PXN)) {
    uint64_t *pt = root;
    for (int shift = 39; shift > 12; shift -= 9) pt = PTE_TABLE_PTR(pt[(va >> shift) & 0x1ff]);
    pt[(va >> 12) & 0x1ff] = PTE_CLR_W(e);
    changed = 1;
  }
  pthread_mutex_unlock(&G->lock);
  return changed;
}

// A region's guest-physical range: whole GK_BACK_WIN windows, even for a
// region a hole (see region_ensure) cut short, at the region's own offset into
// them.
static int gpa_assign(uintptr_t s, uintptr_t e, uint64_t *gpa) {
  uintptr_t wlo = s & ~(GK_BACK_WIN - 1), whi = (e + GK_BACK_WIN - 1) & ~(GK_BACK_WIN - 1);
  if (gpa_alloc(whi - wlo, gpa) < 0) {
    if (G->dbg) write_stderr("[gk] guest-physical space exhausted\n");
    return -1;
  }
  *gpa += s - wlo;
  return 0;
}

// Give back the range gpa_assign handed out for [s, e) at `gpa`.
static void gpa_unassign(uintptr_t s, uintptr_t e, uint64_t gpa) {
  uintptr_t wlo = s & ~(GK_BACK_WIN - 1), whi = (e + GK_BACK_WIN - 1) & ~(GK_BACK_WIN - 1);
  gpa_release(gpa - (s - wlo), whi - wlo);
}

// A deleted region's range goes back to the allocator.
static void gpa_retire(gk_region *n) { gpa_unassign(n->start, n->end, n->gpa); }

static void region_set_gpa(gk_region *n, uint64_t gpa) { n->gpa = gpa; }
static inline uint64_t region_gpa(const gk_region *n) { return n->gpa; }

// Clip the window [*s, *gap_end) region_ensure is about to back around the
// holes (see the vDSO section): KVM refuses a memslot over the kernel's data
// pages. Returns 1 if *s was inside one, which then moves *s past it.
static int region_clip_holes(uintptr_t *s, uintptr_t *gap_end) {
  for (int i = 0; i < G->hole_n; i++) {
    const gk_prot_range *h = &G->holes[i];
    if (*s >= h->start && *s < h->end) { *s = h->end; return 1; }
    if (h->start > *s && h->start < *gap_end) *gap_end = h->start;
  }
  return 0;
}

// ---- VM setup (see gk_init) --------------------------------------------------
// The host page size: any power of two the windows are multiples of (see the
// page-sizes section).
static int page_size_init(void) {
  long pagesz = sysconf(_SC_PAGESIZE);
  if (pagesz < 4096 || (pagesz & (pagesz - 1)) || GK_NEIGHBOR_WIN % (unsigned long)pagesz) {
    G->err = "unsupported host page size";
    return -1;
  }
  G->host_page = (size_t)pagesz;
  return 0;
}

static int pt_arena_init(void) {
  // Table entries hold guest-physical addresses (PTE_TABLE_ENTRY), so the
  // page-table arena's must be known before the first table is linked in: it
  // is one region, and one guest-physical range.
  if (region_ensure(G->pt_base, G->pt_base + G->pt_bytes) < 0) { G->err = "memslot for PT arena"; return -1; }
  gk_region *ptr = region_find(G->pt_base);
  if (!ptr || ptr->end < G->pt_base + G->pt_bytes) { G->err = "PT arena memslot split"; return -1; }
  G->pt_gpa = ptr->gpa + (G->pt_base - ptr->start);
  return 0;
}

static int guest_tables_init(void) {
  if (map_handler_text() < 0) { G->err = "map handler text"; return -1; }
  return 0;
}

// ---- page-table entries -----------------------------------------------------
// The leaf PTE map4k_root installs for `va` (see there for `flags`). The frame
// is the page's guest-physical address, through the memslot region backing it
// (every caller has ensured one), whose host page goes into stage 2 now if the
// hypervisor needs that (s2_ensure; the caller has read the host mapping).
// EL0 has no execute permission on a supervisor page whatever AP says, so UXN
// is set on every supervisor page; and the CPU itself denies EL1 execution of
// a page EL0 can write. No page carries a protection key. Returns -1 if the
// page has no region or cannot enter stage 2.
static int pte_leaf(uint64_t va, uint64_t flags, int pkey, int super, uint64_t *out) {
  (void)pkey;
  gk_region *rg = region_find(va);
  if (!rg) return -1;
  if (s2_ensure(rg, va) < 0) return -1;
  uint64_t pte = (rg->gpa + ((va & ~0xfffULL) - rg->start)) | PTE_P | PTE_TABLE | PTE_SH_INNER |
                 PTE_AF | PTE_NG | PTE_RDONLY;
  if (!super) pte |= PTE_U;
  if (flags & 2) pte &= ~PTE_RDONLY;
  if (flags & GK_MAP_WALLED) pte |= PTE_WALL;
  if (!(flags & 4)) pte |= PTE_PXN | PTE_UXN;
  else if (super) pte |= PTE_UXN;
  *out = pte;
  return 0;
}

// pte_reprotect as on x86-64 (see gk_x86_64.c). A supervisor page (U clear)
// stays not executable at EL0 (see pte_leaf).
static uint64_t pte_reprotect(uint64_t e, const gk_reprot *rp, uintptr_t v) {
  uint64_t n = (e & ~(PTE_PXN | PTE_UXN | PTE_WALL)) | PTE_RDONLY;
  if (!(rp->prot & 4)) n |= PTE_PXN | PTE_UXN;
  else if (!(n & PTE_U)) n |= PTE_UXN;
  if (rp->prot & 2) n = (v >= rp->wall_lo && v < rp->wall_hi) ? n | PTE_WALL : PTE_SET_W(n);
  return n;
}

// ---- the guest-physical allocator (arm64) -------------------------------------
// On arm64 the guest-physical space is smaller than the host-virtual one: KVM
// gives a VM at most KVM_CAP_ARM_VM_IPA_SIZE bits (40 on Apple silicon) while
// the host's user mappings reach up to 2^48. Guest-virtual is still host-
// virtual, so the guest runs the host's code and data at their own addresses,
// but each region's memslot is placed at a guest-physical address of its own,
// handed out here: a bump pointer with a sorted, coalescing free list of the
// ranges deleted regions gave back, taken first fit. Ranges are GK_BACK_WIN
// multiples at GK_BACK_WIN-aligned addresses, and a region's guest-physical
// address is congruent to its host-virtual one modulo GK_BACK_WIN, so KVM can
// still back an aligned 2MB with one stage-2 block (see arena_thp_advise).
// Guarded by G->lock.
static int gpa_alloc(uint64_t size, uint64_t *out) {
  for (int i = 0; i < G->gpa_free_n; i++) {
    gk_gpa_extent *x = &G->gpa_free[i];
    if (x->size < size) continue;
    *out = x->start;
    x->start += size;
    x->size -= size;
    if (x->size == 0) {
      memmove(x, x + 1, (size_t)(G->gpa_free_n - i - 1) * sizeof *x);
      G->gpa_free_n--;
    }
    return 0;
  }
  if (G->gpa_next + size > G->gpa_limit) return -1;
  *out = G->gpa_next;
  G->gpa_next += size;
  return 0;
}

// Return [start, start + size) to the allocator. A full free list leaks the
// range rather than fail; it is bounded by the number of regions anyway.
static void gpa_release(uint64_t start, uint64_t size) {
  int i = 0;
  while (i < G->gpa_free_n && G->gpa_free[i].start < start) i++;
  gk_gpa_extent *prev = i > 0 ? &G->gpa_free[i - 1] : NULL;
  gk_gpa_extent *next = i < G->gpa_free_n ? &G->gpa_free[i] : NULL;
  if (prev && prev->start + prev->size == start) {
    prev->size += size;
    if (next && start + size == next->start) {
      prev->size += next->size;
      memmove(next, next + 1, (size_t)(G->gpa_free_n - i - 1) * sizeof *next);
      G->gpa_free_n--;
    }
  } else if (next && start + size == next->start) {
    next->start = start;
    next->size += size;
  } else if (G->gpa_free_n < GK_MAX_REGIONS) {
    memmove(&G->gpa_free[i + 1], &G->gpa_free[i], (size_t)(G->gpa_free_n - i) * sizeof G->gpa_free[0]);
    G->gpa_free[i] = (gk_gpa_extent){start, size};
    G->gpa_free_n++;
  } else {
    return;
  }
  // The last free range ends at the bump pointer: give it back to the pointer.
  gk_gpa_extent *last = &G->gpa_free[G->gpa_free_n - 1];
  if (last->start + last->size == G->gpa_next) {
    G->gpa_next = last->start;
    G->gpa_free_n--;
  }
}

static uint64_t host_tpidr(int ro) {
  uint64_t v;
  if (ro) __asm__ __volatile__("mrs %0, tpidrro_el0" : "=r"(v));
  else __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(v));
  return v;
}

// Put the vCPU at the dispatcher (gk_dispatch), at EL1 on SP_EL1 = its vcb,
// as a fresh vCPU starts and as every exception the guest takes leaves it. For
// a vCPU stopped anywhere else: one that faulted inside a handler, or whose
// KVM_RUN failed (see host_backing_fail).
static int vcpu_reset_pc(gk_thread *t) {
  if (vreg_set(t, VREG_PC, (uintptr_t)&gk_dispatch) < 0 ||
      vreg_set(t, VREG_PSTATE, PSR_EL1H) < 0)
    return -1;
  return 0;
}

// The arm64 vcpu_init, in the shape of the x86-64 one above: the pool, the
// guest stack and the per-thread state are the same. A new vCPU gets its vcb,
// the region SP_EL1 points at, and its static system registers; every thread
// gets the base root, its TLS registers, and the dispatcher.
static int vcpu_init(gk_thread *t, int flags) {
  if (t->inited) return 0;
  int id, reused = 0;
  uint64_t stack_top = 0, ist = 0;
  void *side_stack = NULL;
  pthread_mutex_lock(&G->lock);
  if (G->parked_n > 0) {
    gk_parked_vcpu *p = &G->parked[--G->parked_n];
    vcpu_unpark_hw(t, p);
    id = p->id; stack_top = p->stack_top; ist = p->ist;
    side_stack = p->side_stack;
    reused = 1;
  }
  pthread_mutex_unlock(&G->lock);
  if (!reused) id = atomic_fetch_add(&G->vcpus_created, 1);
  int fresh = vcpu_hw_create(t, id, reused);
  if (fresh < 0) return -1;
  if (!reused) {
    // The vcb, eager-mapped supervisor: the vector handlers store into it
    // before anything else, so it must never fault. Its mapping is a whole
    // number of host pages, all of them supervisor.
    size_t vcb_bytes = HOST_CEIL(GK_IST_BYTES);
    uint8_t *ir = mmap(NULL, vcb_bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ir == MAP_FAILED) { G->err = "mmap vcb"; vcpu_hw_abort(t); return -1; }
    pthread_mutex_lock(&G->lock);
    prot_add((uintptr_t)ir, (uintptr_t)ir + vcb_bytes, GK_PROT_SUPER);
    int irc = mmu_map_range((uintptr_t)ir, (uintptr_t)ir + vcb_bytes, 1 | 2);
    pthread_mutex_unlock(&G->lock);
    if (irc < 0) { G->err = "map vcb"; vcpu_hw_abort(t); return -1; }
    ist = (uint64_t)(uintptr_t)ir;
  }
  // The static system registers go into every new vCPU.
  if (fresh &&
      (vreg_set(t, VREG_SCTLR, GK_SCTLR) < 0 || vreg_set(t, VREG_TCR, GK_TCR(G->ips)) < 0 ||
       vreg_set(t, VREG_MAIR, 0xff) < 0 ||          // attribute 0: normal write-back
       vreg_set(t, VREG_CPACR, 3UL << 20) < 0 ||    // FP/SIMD at EL0 and EL1
       vreg_set(t, VREG_VBAR, G->vbar) < 0 ||
       vreg_set(t, VREG_CNTKCTL, 3) < 0 ||          // EL0 reads the counters (libc, the vDSO)
       vreg_set(t, VREG_SP_EL1, ist) < 0)) {
    G->err = "setting the vCPU's system registers";
    vcpu_hw_abort(t);
    return -1;
  }

  // A private guest stack for this vCPU (see the x86-64 vcpu_init).
  const size_t stksz = 2 * 1024 * 1024;
  if ((flags & GK_VCPU_STACK) && stack_top == 0) {
    uint8_t *stk = mmap(NULL, stksz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) { G->err = "mmap guest stack"; vcpu_hw_abort(t); return -1; }
    pthread_mutex_lock(&G->lock);
    int mrc = mmu_map_range((uintptr_t)stk, (uintptr_t)stk + stksz, 1 | 2);  // rw, NX
    pthread_mutex_unlock(&G->lock);
    if (mrc < 0) { G->err = "map guest stack"; vcpu_hw_abort(t); return -1; }
    stack_top = (uintptr_t)stk + stksz;
  }

  // The calling thread's state. A reused vCPU's TLB is flushed at its first
  // entry (root_gen below), like a fresh one's; the vcb asks for that flush
  // too, in case a previous thread left one pending.
  struct gk_vcb *v = (struct gk_vcb *)(uintptr_t)ist;
  memset(v, 0, sizeof *v);
  v->action = GK_ACT_FLUSH;
  v->cur_ttbr = PT_GPA(G->pml4);
  if (vreg_set(t, VREG_TTBR0, PT_GPA(G->pml4)) < 0 ||
      vreg_set(t, VREG_TPIDR, host_tpidr(0)) < 0 ||
      vreg_set(t, VREG_TPIDRRO, host_tpidr(1)) < 0 ||
      vreg_set(t, VREG_PC, (uintptr_t)&gk_dispatch) < 0 ||
      vreg_set(t, VREG_PSTATE, PSR_EL1H) < 0 ||
      vreg_set(t, VREG_FPCR, 0) < 0 || vreg_set(t, VREG_FPSR, 0) < 0) {
    G->err = "setting the vCPU's thread state";
    vcpu_hw_abort(t);
    return -1;
  }

  t->id = id;
  t->stack_top = stack_top;
  t->ist = ist;
  t->side_stack = side_stack;
  t->root_gen = 0;    // a fresh or reused vCPU flushes its TLB at first entry
  t->last_fault = 0;
  t->fault_repeat = 0;
  t->backing_retries = 0;
  t->pkru_ready = 0;
  t->user_mode = 0;   // ring 0 until enter_guest or child_entry selects ring 3
  t->tlb_touched = 1; // no known TLB state and no previous wall: the first
  t->prev_ro_lo = t->prev_ro_hi = 0;  // wall_raise on this vCPU flushes
  if (!t->active_pml4) t->active_pml4 = G->pml4;
  t->inited = 1;
  if (G->dbg)
    fprintf(stderr, "[gk] vcpu %d %s by tid %ld\n", id, reused ? "reused" : "created", gettid_raw());
  return 0;
}

// Eager-map the supervisor text (gk_asm_arm64.S): the vector table, the handlers
// and the dispatcher, which must be present before the first exception. They
// sit in a page-aligned section of their own, so no other code shares their
// pages, and the ring-3 exit trampoline is outside it (checked here as on
// x86-64).
static int map_handler_text(void) {
  uintptr_t lo = (uintptr_t)gk_super_start, hi = (uintptr_t)gk_super_end;
  if (((lo | hi) & 0xfff) || hi <= lo) gk_fatal("gk_asm.S supervisor text is not page-aligned");
  uintptr_t tramp = (uintptr_t)&gk_user_exit_tramp & ~0xfffUL;
  if (tramp < hi && tramp + 0x1000 > lo)
    gk_fatal("gk_user_exit_tramp shares a page with the supervisor handler text");
  G->vbar = (uintptr_t)gk_vectors;
  prot_add(lo, hi, GK_PROT_SUPER);
  return mmu_map_range(lo, hi, 1 | 4);  // r-x
}
// On arm64 a vCPU's state lives in its vcb (see gk_asm_arm64.S): the registers of a
// stopped vCPU are those its handler saved there, and it resumes through the
// dispatcher, so no register is exchanged by ioctl on the common paths. A
// flush is an action the dispatcher carries out before the vCPU next runs
// guest code, which every caller follows with: it covers global entries too
// (TLBI VMALLE1), as the x86-64 stubs do.
static int flush_run(gk_thread *t) {
  atomic_fetch_add_explicit(&G->tlb_flushes, 1, memory_order_relaxed);
  VCB(t)->action |= GK_ACT_FLUSH;
  return 0;
}

static void flush_tlb(gk_thread *t, gk_regs *r) {
  (void)r;
  flush_run(t);
}
// No protection keys to virtualize (see GK_HAS_PKEYS): every guest PKRU is 0.
static void host_pkru_allow_all(void) {}
static uint32_t host_pkru(void) { return 0; }
static int guest_pkru_update(gk_thread *t, uint32_t keep, uint32_t set, uint32_t *out) {
  (void)t; (void)keep; (void)set;
  if (out) *out = 0;
  return 0;
}
// The vCPU waits at the dispatcher with its last root in TTBR0_EL1, and that
// root may be gone by the time the host asks for another (a destroyed arena's
// pml4, back in the table pool with a free-list link in its first entry). The
// dispatcher's own first fetch walks TTBR0 unless the TLB still holds the
// dispatcher's page, which neither HVF (on every entry) nor KVM (after a
// migration to another CPU, say) promises, so the new root goes into
// TTBR0_EL1 from the host before the vCPU runs; the dispatcher's load of the
// same value and its TLBI then take effect as on a switch the guest made
// itself. Returns 0, or -1 if the register could not be set, in which case
// the vCPU must not run.
static int ttbr0_preload(gk_thread *t, uint64_t root) {
  return vreg_set(t, VREG_TTBR0, root);
}

// sync_cr3 on arm64: the root is TTBR0's guest-physical address, which the
// dispatcher records in the vcb (cur_ttbr) every time it runs, so the vCPU's
// actual root is read back from there; a switch the host has asked for and
// the vCPU not yet made counts as made, since nothing runs before it. *load
// is the root to switch to, which the turn's dispatcher pass loads (see
// enter_guest), keeping global entries as the x86-64 MOV CR3 does.
static int sync_cr3(gk_thread *t, uint64_t *load) {
  if (t->active_arena) arena_sync_if_stale(t->active_arena);
  unsigned gen = __atomic_load_n(&G->root_gen, __ATOMIC_ACQUIRE);
  uint64_t want = PT_GPA(t->active_pml4);
  struct gk_vcb *v = VCB(t);
  uint64_t cur = (v->action & GK_ACT_TTBR) ? v->ttbr : v->cur_ttbr;
  *load = 0;
  if (cur != want) {
    *load = want;
  } else if (t->root_gen != gen) {
    if (flush_run(t) < 0) return -1;
  }
  t->root_gen = gen;
  return 0;
}

// enter_guest on arm64: the turn's entry state goes into the vcb and the
// dispatcher launches it, at EL1t (ring 0) or EL0t (ring 3), with LR =
// gk_user_exit_tramp, the turn exit for both rings. `cr3`, when nonzero, is
// the root the dispatcher loads first (see sync_cr3).
static long enter_guest(gk_thread *t, long (*fn)(void *), void *arg, uint64_t stack_top,
                        int user, uint64_t cr3) {
  struct gk_vcb *v = VCB(t);
  v->l_pc = (uintptr_t)fn;
  v->l_x0 = (uintptr_t)arg;
  v->l_sp = stack_top;
  v->l_lr = (uintptr_t)&gk_user_exit_tramp;
  v->l_spsr = user ? PSR_EL0T : PSR_EL1T;
  if (cr3) {
    if (ttbr0_preload(t, cr3) < 0) return -1;
    v->ttbr = cr3;
    v->action |= GK_ACT_TTBR;
    atomic_fetch_add_explicit(&G->root_switches, 1, memory_order_relaxed);
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d: root switch to %#llx (%s)\n", t->id,
              (unsigned long long)cr3, t->active_arena ? "arena" : "base");
  }
  v->action |= GK_ACT_LAUNCH;
  t->user_mode = user;
  return run_vcpu(t);
}
// The region whose memslot holds guest-physical `gpa`, for a failure report.
// Caller holds G->lock.
static gk_region *region_by_gpa(gk_region *n, uint64_t gpa) {
  if (!n) return NULL;
  if (gpa >= n->gpa && gpa < n->gpa + (n->end - n->start)) return n;
  gk_region *l = region_by_gpa(n->l, gpa);
  return l ? l : region_by_gpa(n->r, gpa);
}


// An exception gk does not take (gk_exc_bad): an interrupt, or a fault inside
// a handler or the dispatcher. The turn ends; the vCPU is put back at the
// dispatcher.
static long vcb_bad(gk_thread *t) {
  struct gk_vcb *v = VCB(t);
  if (G->dbg) {
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": fatal exception, elr="); db_hex(&d, v->n_elr);
    db_str(&d, " esr="); db_hex(&d, v->n_esr);
    db_str(&d, " far="); db_hex(&d, v->n_far);
    db_flush(&d);
  }
  G->fault_addr = v->n_elr;
  v->action = 0;
  vcpu_reset_pc(t);
  return GK_EFAULT;
}

// The exception a vector handler saved in the vcb (gk_exc_sync, GK_HC_EXC):
// the cases of the x86-64 run_vcpu, the turn exit and forwarded syscalls
// (SVC), demand faults (translation and permission aborts), and genuine
// faults, which end the turn with GK_EFAULT. Returns 1 when the turn ends,
// with its result in *ret, and 0 to resume the guest through the dispatcher.
static int vcb_exception(gk_thread *t, long *ret) {
  struct gk_vcb *v = VCB(t);
  gk_frame *f = &v->f;
  unsigned ec = ESR_EC(f->esr);
  if (ec == ESR_EC_SVC64) {
    int c = svc_class(f);
    if (c == SVC_GK) {
      // The turn exit (gk_user_exit_tramp), for either ring: the result is in x0.
      if (f->x[8] == GK_EXIT_SYSCALL) { *ret = (long)f->x[0]; return 1; }
      if (f->x[8] == GK_DEBUG_RING_SYSCALL) {  // see gk.h
        f->x[0] = (f->spsr & 0xc) ? 0 : 3;
        return 0;
      }
    } else if (c == SVC_HOST) {
      // The dispatcher resumes after the SVC with x0 = the result (and, on
      // macOS, x1 and the carry forward_syscall sets).
      f->x[0] = (uint64_t)forward_syscall(t, f);
      return 0;
    }
    // Any other SVC is a genuine fault at the instruction.
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": svc #"); db_hex(&d, f->esr & 0xffff);
      db_str(&d, " at pc "); db_hex(&d, f->elr);
      db_flush(&d);
    }
    G->fault_addr = f->elr;
    *ret = GK_EFAULT;
    return 1;
  }
  if (ec != ESR_EC_DABT_LOW && ec != ESR_EC_DABT_CUR && ec != ESR_EC_IABT_LOW &&
      ec != ESR_EC_IABT_CUR) {
    // An undefined or trapped instruction (a privileged one at ring 3, say),
    // a breakpoint, a misaligned PC or SP: genuine, at the instruction.
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": exception class "); db_hex(&d, ec);
      db_str(&d, " at pc "); db_hex(&d, f->elr);
      db_str(&d, " (esr "); db_hex(&d, f->esr); db_str(&d, ")");
      db_flush(&d);
    }
    G->fault_addr = f->elr;
    *ret = GK_EFAULT;
    return 1;
  }
  int data = ec == ESR_EC_DABT_LOW || ec == ESR_EC_DABT_CUR;
  if (!data && vdso_call(f, f->elr)) return 0;
  unsigned fsc = f->esr & 0x3f;
  uint64_t far = f->far & 0x00ffffffffffffffULL;  // a tagged address's tag (TBI) is no part of it
  int write = data && (f->esr & ESR_WNR);
  // Translation, access-flag and permission faults, at any level, are what
  // demand paging resolves; anything else (alignment, an external abort, an
  // address-size fault, a FAR the CPU could not supply) is genuine.
  if ((data && (f->esr & ESR_FNV)) || fsc < 0x04 || fsc > 0x0f) {
    G->fault_addr = data && !(f->esr & ESR_FNV) ? far : f->elr;
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": abort fsc="); db_hex(&d, fsc);
      db_str(&d, " far="); db_hex(&d, far);
      db_str(&d, " pc="); db_hex(&d, f->elr);
      db_flush(&d);
    }
    *ret = GK_EFAULT;
    return 1;
  }
  if (ec == ESR_EC_IABT_CUR && fsc >= 0x0c && pte_exec_at_el1(t, (uintptr_t)far)) {
    v->tlbi_va = far;
    v->action |= GK_ACT_TLBI_VA;
    return 0;
  }
  int repeat = (far == t->last_fault);
  if (repeat) {
    if (++t->fault_repeat > 2) {
      if (G->dbg) {
        gk_dbuf d = {.n = 0};
        db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
        db_str(&d, ": fault repeats after mapping, far="); db_hex(&d, far);
        db_str(&d, " pc="); db_hex(&d, f->elr);
        db_str(&d, " esr="); db_hex(&d, f->esr);
        db_flush(&d);
      }
      G->fault_addr = far;
      *ret = GK_EFAULT;
      return 1;
    }
  } else { t->last_fault = far; t->fault_repeat = 0; }
  // A write into this turn's host-frame wall (see run_here) is the wall
  // holding: genuine, whatever the host protection of the page.
  if (write && far >= t->ro_lo && far < t->ro_hi) {
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": write into the host-frame wall at "); db_hex(&d, far);
      db_str(&d, " (wall ["); db_hex(&d, t->ro_lo); db_str(&d, ", ");
      db_hex(&d, t->ro_hi); db_str(&d, "), esr="); db_hex(&d, f->esr);
      db_str(&d, ", pc="); db_hex(&d, f->elr);
      db_str(&d, ", sp="); db_hex(&d, f->sp); db_str(&d, ")");
      db_flush(&d);
    }
    G->fault_addr = far;
    *ret = GK_EFAULT;
    return 1;
  }
  GK_HANDLER_ENTER();
  int dm = demand_map(t, (uintptr_t)far, (uintptr_t)f->sp);
  GK_HANDLER_LEAVE();
  if (dm < 0) {
    if (G->dbg) {  // still the fault path: raw output only
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": unmappable fault far="); db_hex(&d, far);
      db_str(&d, " pc="); db_hex(&d, f->elr);
      db_str(&d, " esr="); db_hex(&d, f->esr);
      db_str(&d, " sp="); db_hex(&d, f->sp);
      db_flush(&d);
      // Best-effort frame-record walk of the guest stack ([fp, lr] at x29).
      uint64_t bp = f->x[29], lo = f->sp, hi = f->sp + (8UL << 20);
      for (int i = 0; i < 16 && bp >= lo && bp + 16 <= hi; i++) {
        uint64_t *fr = (uint64_t *)(uintptr_t)bp;
        db_str(&d, "[gk]   frame "); db_dec(&d, i);
        db_str(&d, ": ret="); db_hex(&d, fr[1]);
        db_flush(&d);
        if (fr[0] <= bp) break;
        bp = fr[0];
      }
    }
    G->fault_addr = far;
    *ret = GK_EFAULT;
    return 1;
  }
  // An arm64 fault leaves the entry it was taken through in the TLB (a
  // permission fault's read-only one, say), so the retry would take it
  // again: the dispatcher invalidates the page first.
  v->tlbi_va = far;
  v->action |= GK_ACT_TLBI_VA;
  return 0;
}

// ---- diagnostics -------------------------------------------------------------
static int flush_uses_invpcid(void) { return 0; }
static uintptr_t super_page(void) { return G->vbar; }  // the vector table

#endif  // defined(__aarch64__)
