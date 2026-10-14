// gk: the macOS host and the Hypervisor.framework backend: host syscalls and
// Mach traps, stage 2, the vCPU registers, syscall forwarding and the run loop.
#include "gk_internal.h"
#if GK_HVF
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

// One macOS syscall or Mach trap, with the registers a caller has at svc #0x80:
// x0..x7 from `a`, the number in x16 (negative for a Mach trap). Returns x0 and
// sets *x1_out to x1 and *carry to the carry flag: a BSD syscall reports failure
// through the carry, with the errno in x0; a Mach trap returns a kern_return_t
// in x0 and leaves the carry as it was.
static long hvf_syscall(long nr, const uint64_t a[8], uint64_t *x1_out, int *carry) {
  register uint64_t x0 __asm__("x0") = a[0];
  register uint64_t x1 __asm__("x1") = a[1];
  register uint64_t x2 __asm__("x2") = a[2];
  register uint64_t x3 __asm__("x3") = a[3];
  register uint64_t x4 __asm__("x4") = a[4];
  register uint64_t x5 __asm__("x5") = a[5];
  register uint64_t x6 __asm__("x6") = a[6];
  register uint64_t x7 __asm__("x7") = a[7];
  register long x16 __asm__("x16") = nr;
  uint64_t cf;
  __asm__ __volatile__("svc #0x80\n\tcset %[cf], cs"
                       : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6),
                         "+r"(x7), "+r"(x16), [cf] "=r"(cf)
                       :
                       : "memory", "cc");
  *x1_out = x1;
  *carry = (int)cf;
  return (long)x0;
}
// gk's own host calls, in the Linux shape: the result, or -errno.
static long host_syscall(long nr, long a1, long a2, long a3, long a4, long a5,
                         long a6) {
  uint64_t x1;
  int carry;
  long r = hvf_syscall(nr, (uint64_t[8]){(uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4,
                                         (uint64_t)a5, (uint64_t)a6, 0, 0}, &x1, &carry);
  return carry ? -r : r;
}

// Copies `n` bytes from a guest-supplied address into host memory, without
// faulting the host if the address is unmapped or not readable: the kernel
// does the access and reports failure instead. Returns 0, or -1 if any byte
// could not be read.
static int copy_in(void *dst, uintptr_t src, size_t n) {
  mach_vm_size_t got = 0;
  kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)src, n,
                                            (mach_vm_address_t)(uintptr_t)dst, &got);
  return kr == KERN_SUCCESS && got == n ? 0 : -1;
}

// ---- Hypervisor.framework ------------------------------------------------------
// On macOS the VM is Hypervisor.framework's and the guest model is the same as
// on Linux: an arm64 guest at EL1/EL0 with a 4KiB stage-1 granule, the host's
// address space as its own, syscalls forwarded. What HVF lacks is KVM's tie
// between a memslot and the host page tables. KVM backs a memslot from the host
// mapping as the guest touches it and follows the host's later changes; HVF's
// hv_vm_map pins the host pages it is given into stage 2 there and then, fails
// on a range any part of which the host has not mapped, and refuses pages
// whose maximum protection excludes writing (this binary's text and read-only
// data, the dyld shared cache's text). So a region (see the MMU layer) gets no
// stage-2 mapping when it is created; its host pages enter stage 2 one at a
// time, each when it gets its first guest PTE (map4k_root), which is exactly
// when gk has just read the host mapping it lies in. A bitmap over the
// guest-physical space (s2_map, one bit per host page) records which pages are
// in, and each region node counts its own.
//
// A page hv_vm_map refuses is shadowed: a copy of it, from a pool of pages
// that is refused to the guest, is mapped at the page's guest-physical address
// instead, readable and executable but never writable, since a write there
// would diverge from the host's page without any fault. The pages this covers
// are immutable text and data, and the commpage: the two read-only pages the
// kernel maps above the user range for every task (time and CPU facts libc
// reads), which are not in the task's VM map at all and whose copies are
// therefore refreshed before every turn (commpage_refresh).
//
// A stage-2 page stays the physical page it was when it was mapped. A host
// protection change or a decommit (madvise) of the page leaves that page in
// place and is seen by the guest; replacing the mapping (munmap or
// vm_deallocate followed by a new mapping at the same address, a fixed mmap
// over it) gives the host a new page while the guest keeps the old one,
// reading stale data and writing where the host never looks. Changes the
// guest makes to its own mappings go through forward_syscall, which drops the
// range from stage 2 (s2_unmap) along with its PTEs, so the next access maps
// the host's then-current page; the one such change host threads make as a
// matter of course, a thread's stack mapping coming and going, is handled at
// thread entry and exit (stack_refresh). Any other replacement made outside
// the guest is not seen, where KVM would follow the host (see the host-backing
// section).
static const uintptr_t gk_commpage_addr[GK_COMMPAGE_N] = {0x0000000fffff4000ULL, 0x0000000fffffc000ULL};
static gk_region *region_succ(uintptr_t s);  // see the MMU layer

// ---- VM setup (see gk_init) --------------------------------------------------
// The host-frame wall needs no host facts on macOS: the thread descriptor is
// pthread_self (see stack_wall_bounds).
static void wall_probe(void) {}

static int vm_create(void) {
  // The VM, with the largest guest-physical space the framework offers (see
  // the guest-physical allocator), and the shadow pool (see the HVF section).
  hv_vm_config_t cfg = hv_vm_config_create();
  uint32_t ipa = 36;
  if (!cfg || hv_vm_config_get_max_ipa_size(&ipa) != HV_SUCCESS ||
      hv_vm_config_set_ipa_size(cfg, ipa) != HV_SUCCESS || hv_vm_create(cfg) != HV_SUCCESS) {
    G->err = "hv_vm_create (is the binary signed with the hypervisor entitlement?)";
    return -1;
  }
  os_release(cfg);
  ipa = (uint32_t)gpa_space_init((int)ipa);
  void *pool = mmap(NULL, (size_t)GK_MAX_SHADOWS * GK_HOST_PAGE, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (pool == MAP_FAILED) { G->err = "mmap shadow pool"; return -1; }
  G->shadow_pool = (uintptr_t)pool;
  prot_add(G->shadow_pool, G->shadow_pool + (size_t)GK_MAX_SHADOWS * GK_HOST_PAGE, GK_PROT_REFUSE);
  size_t map_bytes = HOST_CEIL(G->gpa_limit / GK_HOST_PAGE / 8);  // touched only where used
  void *map = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (map == MAP_FAILED) { G->err = "mmap stage-2 map"; return -1; }
  G->s2_map = map;
  prot_add((uintptr_t)map, (uintptr_t)map + map_bytes, GK_PROT_REFUSE);
  if (G->dbg) fprintf(stderr, "[gk] Hypervisor.framework VM, %u-bit guest-physical space\n", ipa);
  return 0;
}

// The s2_map bit of the guest-physical host page holding `gpa`.
static int s2_test(uint64_t gpa) {
  uint64_t b = gpa / GK_HOST_PAGE;
  return (G->s2_map[b >> 6] >> (b & 63)) & 1;
}

// Put the host page holding `hva`, inside region `rg` and mapped readable on
// the host, into stage 2 if it is not there yet. Caller holds G->lock. Returns
// 0, or -1 if neither the page nor a shadow of it could be mapped.
static int s2_ensure(gk_region *rg, uintptr_t hva) {
  uintptr_t pg = HOST_FLOOR(hva);
  uint64_t gpa = rg->gpa + (pg - rg->start);
  if (s2_test(gpa)) return 0;
  if (hv_vm_map((void *)pg, gpa, GK_HOST_PAGE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) !=
      HV_SUCCESS) {
    if (G->shadow_used >= GK_MAX_SHADOWS) return -1;
    void *copy = (void *)(G->shadow_pool + (uintptr_t)G->shadow_used * GK_HOST_PAGE);
    if (mprotect(copy, GK_HOST_PAGE, PROT_READ | PROT_WRITE) != 0) return -1;
    memcpy(copy, (const void *)pg, GK_HOST_PAGE);
    if (hv_vm_map(copy, gpa, GK_HOST_PAGE, HV_MEMORY_READ | HV_MEMORY_EXEC) != HV_SUCCESS) {
      if (G->dbg) {
        gk_dbuf d = {.n = 0};
        db_str(&d, "[gk] stage 2: neither "); db_hex(&d, pg);
        db_str(&d, " nor its shadow could be mapped at "); db_hex(&d, gpa);
        db_flush(&d);
      }
      return -1;
    }
    G->shadow_used++;
    G->s2_shadowed++;
    for (int i = 0; i < GK_COMMPAGE_N; i++)
      if (pg == gk_commpage_addr[i] && G->commpage_n < GK_COMMPAGE_N)
        G->commpage[G->commpage_n++] = (gk_shadow){pg, copy};
  }
  uint64_t b = gpa / GK_HOST_PAGE;
  G->s2_map[b >> 6] |= 1ULL << (b & 63);
  rg->s2_n++;
  G->s2_pages++;
  return 0;
}

// Drop the guest-physical range [gpa, gpa + len) of region `rg` from stage 2:
// each run of pages that are in goes with one hv_vm_unmap, whose range is then
// exactly what is mapped. A run the hypervisor will not unmap stays, bits and
// all, and the call returns -1: the guest-physical pages still lead to the
// host pages they were mapped with, so they must not be handed out again.
// Returns 0 if everything went.
static int s2_drop(gk_region *rg, uint64_t gpa, uint64_t len) {
  int r = 0;
  uint64_t b = gpa / GK_HOST_PAGE, end = (gpa + len) / GK_HOST_PAGE;
  while (b < end) {
    if ((b & 63) == 0 && !G->s2_map[b >> 6]) { b += 64; continue; }
    if (!((G->s2_map[b >> 6] >> (b & 63)) & 1)) { b++; continue; }
    uint64_t run = b + 1;
    while (run < end && ((G->s2_map[run >> 6] >> (run & 63)) & 1)) run++;
    uint64_t lo = b * GK_HOST_PAGE, n = run - b;
    if (hv_vm_unmap(lo, n * GK_HOST_PAGE) != HV_SUCCESS) {
      if (G->dbg) {
        gk_dbuf d = {.n = 0};
        db_str(&d, "[gk] stage 2: unmap of guest-physical "); db_hex(&d, lo);
        db_str(&d, " +"); db_hex(&d, n * GK_HOST_PAGE); db_str(&d, " failed");
        db_flush(&d);
      }
      r = -1;
    } else {
      for (uint64_t i = b; i < run; i++) G->s2_map[i >> 6] &= ~(1ULL << (i & 63));
      rg->s2_n -= (long)n;
      G->s2_pages -= (long)n;
    }
    b = run;
  }
  return r;
}

// Drop the host pages of [lo, hi), host-page-aligned, from stage 2, so the
// guest's next access to one maps the host's then-current page. A shadow's pool
// page is not reused. Caller holds G->lock.
static void s2_unmap(uintptr_t lo, uintptr_t hi) {
  gk_region *rg = region_find(lo);
  if (!rg) rg = region_succ(lo);
  for (; rg && rg->start < hi; rg = region_succ(rg->start)) {
    uintptr_t s = lo > rg->start ? lo : rg->start, e = hi < rg->end ? hi : rg->end;
    if (s < e && rg->s2_n) s2_drop(rg, rg->gpa + (s - rg->start), e - s);
  }
}

// A region is going away (region_remove): its stage-2 pages with it. Returns
// -1 if some stayed (see s2_drop).
static int s2_release(gk_region *rg) {
  return rg->s2_n ? s2_drop(rg, rg->gpa, rg->end - rg->start) : 0;
}

// A region has no memslot under HVF: its window enters stage 2 page by page
// (see s2_ensure).
static int memslot_add(int slot, uint64_t gpa, uintptr_t s, uintptr_t e) {
  (void)slot; (void)gpa; (void)s; (void)e;
  return 0;
}

// The region goes either way (the host memory is going) and its stage-2 pages
// with it; guest-physical pages stage 2 still maps (see s2_drop) are
// quarantined (*keep_gpa) rather than reused.
static int memslot_remove(gk_region *n, int *keep_gpa) {
  if (s2_release(n) < 0) *keep_gpa = 1;
  return 0;
}

// Bring the commpage shadows up to date with the kernel's pages, before a turn.
static void commpage_refresh(void) {
  for (int i = 0; i < G->commpage_n; i++)
    memcpy(G->commpage[i].copy, (const void *)G->commpage[i].hva, GK_HOST_PAGE);
}
// On macOS the lookup is a Mach VM region query, descending into submaps (the
// dyld shared cache is one). The commpage (see the HVF section) is not in the
// map and is reported as a readable page.
static int host_region(uintptr_t page, uintptr_t *rs, uintptr_t *re, int *perms) {
  for (int i = 0; i < GK_COMMPAGE_N; i++) {
    uintptr_t c = gk_commpage_addr[i];
    if (page >= c && page < c + GK_HOST_PAGE) { *rs = c; *re = c + GK_HOST_PAGE; *perms = 1; return 1; }
  }
  natural_t depth = 0;
  for (int i = 0; i < 16; i++) {
    mach_vm_address_t addr = page;
    mach_vm_size_t size = 0;
    struct vm_region_submap_info_64 info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                               (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS ||
        addr > page)
      return 0;
    if (info.is_submap) { depth++; continue; }
    *rs = addr; *re = addr + size; *perms = info.protection & 7;
    return 1;
  }
  return 0;
}
static long gettid_raw(void) { return host_syscall(SYS_thread_selfid, 0, 0, 0, 0, 0, 0); }

// An HVF vCPU belongs to the thread that created it and cannot be run or
// destroyed by another, so it goes with the thread (vcpu_detach, before the
// park); its id, vcb and stacks are pooled and the next thread creates a vCPU
// around them (vcpu_init).
static void vcpu_detach(gk_thread *t) { hv_vcpu_destroy(t->vcpu); }
static void vcpu_park_hw(gk_thread *t, gk_parked_vcpu *p) { (void)t; (void)p; }

// The record of the host thread whose pthread key destructor runs. macOS may
// have finalized the thread's __thread storage (tls_rec) before the
// destructor, so the key's value `p` is checked instead the way thread_cur
// checks tls_rec: a record in the pool that this thread, by kernel tid, owns.
static gk_thread *thread_ending(void *p) {
  gk_thread *t = (gk_thread *)p;
  if (t < G->threads || t >= G->threads + GK_MAX_VCPUS ||
      ((uintptr_t)t - (uintptr_t)G->threads) % sizeof *t != 0 || t->tid != gettid_raw())
    gk_fatal("thread record does not belong to the ending thread");
  return t;
}

// A host thread's stack is a fresh mapping, often at an address an earlier
// thread's stack had, and stage 2 keeps the pages it was given (see the HVF
// section): the pages a guest turn on the earlier thread put there would
// stand in for the new thread's. So when a thread first enters gk, and again
// when it ends, its stack mapping is dropped from stage 2 and from every
// root, and its pages are taken from the host anew when next touched. The
// bump of root_gen has every vCPU flush before its next turn (sync_cr3).
static void stack_refresh(void) {
  pthread_t self = pthread_self();
  uintptr_t re = HOST_CEIL((uintptr_t)pthread_get_stackaddr_np(self));
  uintptr_t rs = HOST_FLOOR(re - pthread_get_stacksize_np(self));
  pthread_mutex_lock(&G->lock);
  unmap_range_all(rs, re);
  s2_unmap(rs, re);
  __atomic_fetch_add(&G->root_gen, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&G->lock);
}
// HVF names registers by enum and reports failure by return code; these give
// the KVM helpers' shape (0 or -1).
static int vcpu_set_sys(hv_vcpu_t v, hv_sys_reg_t id, uint64_t val) {
  return hv_vcpu_set_sys_reg(v, id, val) == HV_SUCCESS ? 0 : -1;
}
static int vcpu_set_reg(hv_vcpu_t v, hv_reg_t id, uint64_t val) {
  return hv_vcpu_set_reg(v, id, val) == HV_SUCCESS ? 0 : -1;
}
// Set one of the registers the shared arm64 code names (VREG_*): a general
// register or a system register, in HVF's two id spaces.
static int vreg_set(gk_thread *t, int reg, uint64_t v) {
  static const struct { int sys; uint16_t id; } ids[VREG_COUNT] = {
      [VREG_PC] = {0, HV_REG_PC},
      [VREG_PSTATE] = {0, HV_REG_CPSR},
      [VREG_FPCR] = {0, HV_REG_FPCR},
      [VREG_FPSR] = {0, HV_REG_FPSR},
      [VREG_SP_EL1] = {1, HV_SYS_REG_SP_EL1},
      [VREG_SCTLR] = {1, HV_SYS_REG_SCTLR_EL1},
      [VREG_TCR] = {1, HV_SYS_REG_TCR_EL1},
      [VREG_MAIR] = {1, HV_SYS_REG_MAIR_EL1},
      [VREG_CPACR] = {1, HV_SYS_REG_CPACR_EL1},
      [VREG_VBAR] = {1, HV_SYS_REG_VBAR_EL1},
      [VREG_CNTKCTL] = {1, HV_SYS_REG_CNTKCTL_EL1},
      [VREG_TTBR0] = {1, HV_SYS_REG_TTBR0_EL1},
      [VREG_TPIDR] = {1, HV_SYS_REG_TPIDR_EL0},
      [VREG_TPIDRRO] = {1, HV_SYS_REG_TPIDRRO_EL0},
  };
  return ids[reg].sys ? vcpu_set_sys(t->vcpu, (hv_sys_reg_t)ids[reg].id, v)
                      : vcpu_set_reg(t->vcpu, (hv_reg_t)ids[reg].id, v);
}

// A pooled id brings only its vcb and stacks along (see vcpu_park).
static void vcpu_unpark_hw(gk_thread *t, gk_parked_vcpu *p) { (void)t; (void)p; }

// Every thread creates its own HVF vCPU (see vcpu_park), so it is always new:
// returns 1, or -1 on failure.
static int vcpu_hw_create(gk_thread *t, int id, int reused) {
  (void)id; (void)reused;
  hv_vcpu_t vcpu;
  hv_vcpu_exit_t *exit;
  if (hv_vcpu_create(&vcpu, &exit, NULL) != HV_SUCCESS) { G->err = "hv_vcpu_create"; return -1; }
  uint64_t mmfr0 = 0;
  if (hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64MMFR0_EL1, &mmfr0) != HV_SUCCESS ||
      ((mmfr0 >> 28) & 0xf) == 0xf) {  // TGran4 (see the page-sizes section)
    G->err = "CPU has no 4KiB translation granule";
    hv_vcpu_destroy(vcpu);
    return -1;
  }
  // The guest reads the host's clock: CNTVCT_EL0 is mach_absolute_time.
  if (hv_vcpu_set_vtimer_offset(vcpu, 0) != HV_SUCCESS) {
    G->err = "hv_vcpu_set_vtimer_offset";
    hv_vcpu_destroy(vcpu);
    return -1;
  }
  t->vcpu = vcpu;
  t->exit = exit;
  return 1;
}

// A vcpu_init that failed after vcpu_hw_create destroys the vCPU, so the
// thread can create another.
static void vcpu_hw_abort(gk_thread *t) { hv_vcpu_destroy(t->vcpu); }

// svc #0 is gk's own (the turn exit and the ring probe); svc #0x80 is a macOS
// syscall (see forward_syscall); any other immediate is a genuine fault.
static int svc_class(const gk_frame *f) {
  unsigned imm = f->esr & 0xffff;
  return imm == 0 ? SVC_GK : imm == 0x80 ? SVC_HOST : SVC_BAD;
}

// macOS has no vDSO: its user code reads the commpage (see commpage_refresh).
static int vdso_call(gk_frame *f, uint64_t pc) {
  (void)f; (void)pc;
  return 0;
}

static uint64_t vcpu_reg(hv_vcpu_t v, hv_reg_t id) {
  uint64_t val = 0;
  hv_vcpu_get_reg(v, id, &val);
  return val;
}
// On macOS the guest's syscalls are BSD syscalls and Mach traps (svc #0x80,
// the number in x16; see hvf_syscall), and the results go back into the frame
// as the kernel would set them: x0, x1 and, for a BSD syscall, the carry flag
// in the frame's SPSR. Denials are EPERM with the carry set.
//
// Thread creation (bsdthread_create and the workqueue syscalls behind
// libdispatch) is refused: a thread the kernel starts on the host would run
// the guest's code outside the guest, and gk does not intercept Mach thread
// creation here as it does clone on Linux.
//
// Mapping changes are reflected as on Linux, with the Mach VM traps libc and
// libmalloc use alongside the BSD calls: a protection change rewrites or drops
// the range's PTEs; an unmap, a fixed (overwriting) map and a decommit drop
// them and the range's stage-2 pages too, since the host's physical pages
// behind the range are about to change (see the HVF section). A mapping the
// kernel places anywhere is treated the same way for the range it got, as it
// replaces whatever gk last knew about those addresses.
#define GK_SYS_munmap 73
#define GK_SYS_mprotect 74
#define GK_SYS_mmap 197
#define GK_SYS_bsdthread_create 360
#define GK_SYS_workq_open 367
#define GK_SYS_workq_kernreturn 368
#define GK_TRAP_vm_allocate (-10)
#define GK_TRAP_vm_deallocate (-12)
#define GK_TRAP_vm_protect (-14)
#define GK_TRAP_vm_map (-15)
#define GK_SPSR_C (1UL << 29)
enum { GK_REFLECT_NONE, GK_REFLECT_PROTECT, GK_REFLECT_UNMAP, GK_REFLECT_MAP };

// The Mach traps the guest may make. A Mach message can ask the kernel to
// do anything the sending task's ports allow, including reading and writing
// any memory of the process and starting host threads outside the VM, so
// mach_msg and every trap not listed here is refused. What remains acts only
// on the calling task: its port names and its address space (the port and VM
// traps, with the task's own port as the target; gk reflects the VM traps'
// effect), its semaphores, and time.
static int mach_trap_allowed(long nr, uint64_t target) {
  switch (nr) {
    case GK_TRAP_vm_allocate:
    case GK_TRAP_vm_deallocate:
    case GK_TRAP_vm_protect:
    case GK_TRAP_vm_map:
    case -16: case -17: case -18: case -19: case -20:  // _kernelrpc_mach_port_*
    case -21: case -22: case -23: case -24: case -25:
      return (mach_port_name_t)target == mach_task_self();
    case -26:  // mach_reply_port
    case -27:  // thread_self_trap
    case -28:  // task_self_trap
    case -29:  // host_self_trap
    case -33: case -34: case -35: case -36:  // semaphore_signal/_all/_thread, wait
    case -37: case -38: case -39:  // semaphore_wait_signal, timedwait, timedwait_signal
    case -46:  // pid_for_task
    case -50:  // thread_get_special_reply_port
    case -59: case -60: case -61: case -62:  // swtch_pri, swtch, thread_switch, clock_sleep
    case -89: case -90:  // mach_timebase_info, mach_wait_until
    case -91: case -92: case -93: case -94:  // mk_timer_create/destroy/arm/cancel
      return 1;
    default:
      return 0;
  }
}

// Of the allowed Mach traps, the ones a ring-3 turn may use: those that carry
// no guest pointer (so there is nothing to confine) plus the VM traps, whose
// address range and output pointer are checked where they are reflected below.
// The port RPC traps (-16..-25), pid_for_task and mach_timebase_info write a
// result through a guest pointer gk does not validate, so ring-3 is refused
// them; ring-0 (gk's own code) still gets them through mach_trap_allowed.
static int mach_trap_ring3_ok(long nr) {
  switch (nr) {
    case GK_TRAP_vm_allocate: case GK_TRAP_vm_deallocate:
    case GK_TRAP_vm_protect: case GK_TRAP_vm_map:
    case -26: case -27: case -28: case -29:  // reply_port, thread/task/host self
    case -33: case -34: case -35: case -36: case -37: case -38: case -39:  // semaphores
    case -50:  // thread_get_special_reply_port
    case -59: case -60: case -61:  // swtch_pri, swtch, thread_switch
    case -90:  // mach_wait_until
    case -91: case -92: case -93:  // mk_timer_create/destroy/arm
      return 1;
    // clock_sleep (-62) and mk_timer_cancel (-94) write a result through a guest
    // pointer gk does not validate, and the port RPC traps, pid_for_task and
    // mach_timebase_info do the same, so ring 3 is refused all of them.
    default:
      return 0;
  }
}
// ---- the ring-3 syscall memory broker (macOS) -------------------------------
// As on Linux (see the broker in gk_linux.c), a ring-3 turn's forwarded BSD
// syscall may name only memory the turn could reach itself (guest_range_ok),
// and a BSD syscall gk has no descriptor for is refused. The Mach traps are
// policed separately (mach_trap_allowed / mach_trap_ring3_ok) and the
// address-taking VM traps have their range checked where the call is reflected
// below. Ring-0 turns are gk's own trusted code and are not brokered.
#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024
#endif

static int g_str(gk_thread *t, uintptr_t s) {
  if (s == 0) return 0;
  for (uintptr_t off = 0; off <= 4096;) {
    uintptr_t p = s + off;
    uintptr_t page_end = (p & ~(GK_HOST_PAGE - 1)) + GK_HOST_PAGE;
    if (!guest_range_ok(t, p, page_end - p, 0)) return 0;
    char buf[256];
    while (p < page_end && off <= 4096) {
      size_t n = page_end - p;
      if (n > sizeof buf) n = sizeof buf;
      if (copy_in(buf, p, n) < 0) return 0;
      for (size_t i = 0; i < n; i++)
        if (buf[i] == 0) return 1;
      p += n;
      off += n;
    }
  }
  return 1;
}

static int g_iov(gk_thread *t, uintptr_t iov, long cnt, int write) {
  if (cnt < 0 || cnt > UIO_MAXIOV) return 0;
  if (!guest_range_ok(t, iov, (uintptr_t)cnt * sizeof(struct iovec), 0)) return 0;
  for (long i = 0; i < cnt; i++) {
    struct iovec v;
    if (copy_in(&v, iov + (uintptr_t)i * sizeof v, sizeof v) < 0) return 0;
    if (!guest_range_ok(t, (uintptr_t)v.iov_base, v.iov_len, write)) return 0;
  }
  return 1;
}

// recvmsg writes the header fields and buffers, sendmsg only reads them, so
// `write` governs the header too (keeping a recvmsg from writing a header in
// the host-frame wall).
static int g_msg(gk_thread *t, uintptr_t mp, int write) {
  struct msghdr m;
  if (!guest_range_ok(t, mp, sizeof m, write) || copy_in(&m, mp, sizeof m) < 0) return 0;
  if (m.msg_name && !guest_range_ok(t, (uintptr_t)m.msg_name, m.msg_namelen, write)) return 0;
  if (m.msg_control &&
      !guest_range_ok(t, (uintptr_t)m.msg_control, m.msg_controllen, write))
    return 0;
  return g_iov(t, (uintptr_t)m.msg_iov, (long)m.msg_iovlen, write);
}

// Whether ring-3 thread `t` may forward BSD syscall `nr` (>= 0) with arguments
// a[0..5]. Fail-closed: a syscall not named here is refused.
static int syscall_mem_ok(gk_thread *t, long nr, const uint64_t a[8]) {
#define RD(p, n) \
  do { if (!guest_range_ok(t, (uintptr_t)(p), (uintptr_t)(n), 0)) return 0; } while (0)
#define WR(p, n) \
  do { if (!guest_range_ok(t, (uintptr_t)(p), (uintptr_t)(n), 1)) return 0; } while (0)
#define STR(p) \
  do { if (!g_str(t, (uintptr_t)(p))) return 0; } while (0)
#define OPT_WR(p, n) \
  do { if ((p) && !guest_range_ok(t, (uintptr_t)(p), (uintptr_t)(n), 1)) return 0; } while (0)
  switch (nr) {
    // Scalar-only, plus the mapping calls whose range is checked where they are
    // reflected below. macOS exit() is process-wide (there is no guest-thread
    // exit on macOS, where thread creation is refused), so it is not allowed.
    case SYS_getpid: case SYS_getppid: case SYS_gettid: case SYS_close:
    case SYS_lseek: case SYS_socket: case SYS_shutdown: case SYS_listen: case SYS_fsync:
    case SYS_mprotect: case SYS_munmap: case SYS_mmap:
    case SYS_dup: case SYS_dup2: case SYS_fdatasync:
      return 1;
    case SYS_madvise: WR(a[0], a[1]); return 1;
    case SYS_fcntl:
      // Only the commands whose third argument is a scalar; the rest (F_GETPATH,
      // F_GETLK, ...) carry a pointer gk does not size.
      switch (a[1]) {
        case F_GETFD: case F_SETFD: case F_GETFL: case F_SETFL:
        case F_DUPFD: case F_DUPFD_CLOEXEC: case F_GETOWN: case F_SETOWN:
          return 1;
        default: return 0;
      }
    case SYS_read: case SYS_pread: WR(a[1], a[2]); return 1;
    case SYS_write: case SYS_pwrite: RD(a[1], a[2]); return 1;
    case SYS_readv: return g_iov(t, a[1], (long)a[2], 1);
    case SYS_writev: return g_iov(t, a[1], (long)a[2], 0);
    case SYS_getentropy: WR(a[0], a[1]); return 1;
    case SYS_recvfrom:
      WR(a[1], a[2]);
      if (a[4] && a[5]) {
        socklen_t sl = 0;
        if (copy_in(&sl, (uintptr_t)a[5], sizeof sl) < 0) return 0;
        WR(a[5], sizeof sl);
        WR(a[4], sl);
      }
      return 1;
    case SYS_sendto: RD(a[1], a[2]); if (a[4]) RD(a[4], a[5]); return 1;
    case SYS_recvmsg: return g_msg(t, a[1], 1);
    case SYS_sendmsg: return g_msg(t, a[1], 0);
    case SYS_connect: case SYS_bind: RD(a[1], a[2]); return 1;
    case SYS_setsockopt: RD(a[3], a[4]); return 1;
    case SYS_getsockopt:
      WR(a[4], sizeof(int));
      if (a[3]) {
        socklen_t sl = 0;
        if (copy_in(&sl, (uintptr_t)a[4], sizeof sl) < 0) return 0;
        WR(a[3], sl);
      }
      return 1;
    case SYS_open: STR(a[0]); return 1;
    case SYS_openat: STR(a[1]); return 1;
    case SYS_fstat64: WR(a[1], sizeof(struct stat)); return 1;
    case SYS_stat64: case SYS_lstat64: STR(a[0]); WR(a[1], sizeof(struct stat)); return 1;
    case SYS_fstatat64: STR(a[1]); WR(a[2], sizeof(struct stat)); return 1;
    default:
      if (G->dbg)
        fprintf(stderr, "[gk] vcpu %d ring-3 BSD syscall %ld has no memory descriptor: refused\n",
                t->id, nr);
      return 0;
  }
#undef RD
#undef WR
#undef STR
#undef OPT_WR
}

static long forward_syscall(gk_thread *t, gk_regs *r) {
  long nr = (long)r->x[16];
  uint64_t a[8];
  for (int i = 0; i < 8; i++) a[i] = r->x[i];
  long a1 = (long)a[0], a2 = (long)a[1], a3 = (long)a[2], a4 = (long)a[3], a5 = (long)a[4],
       a6 = (long)a[5];
  int deny = 0;
  if (G->filter && !G->filter(nr, a1, a2, a3, a4, a5, a6)) {
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d syscall %ld DENIED\n", t->id, nr);
    deny = 1;
  } else if (nr == GK_SYS_bsdthread_create || nr == GK_SYS_workq_open ||
             nr == GK_SYS_workq_kernreturn) {
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d syscall %ld: thread creation is not supported on macOS\n", t->id, nr);
    deny = 1;
  } else if (nr < 0 && !mach_trap_allowed(nr, a[0])) {
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d Mach trap %ld DENIED\n", t->id, nr);
    deny = 1;
  } else if (t->user_mode && nr < 0 && !mach_trap_ring3_ok(nr)) {
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d Mach trap %ld refused at ring 3\n", t->id, nr);
    deny = 1;
  } else if (t->user_mode && nr >= 0 && !syscall_mem_ok(t, nr, a)) {
    // A ring-3 BSD syscall may name only memory the turn could reach itself.
    deny = 1;
  }
  if (deny) {
    // A Mach trap reports failure in its return value alone; a BSD call in
    // the carry flag, with the errno.
    if (nr < 0) return KERN_PROTECTION_FAILURE;
    r->spsr |= GK_SPSR_C;
    return GK_EPERM;
  }
  // What the call does to the address space, decided before it runs: the
  // range and, for a protection change, the protection.
  int kind = GK_REFLECT_NONE, prot = 0;
  uintptr_t rs = 0, re = 0, addr_out = 0;
  size_t len = 0;
  int self = (mach_port_name_t)a[0] == mach_task_self();
  switch (nr) {
    case GK_SYS_mprotect: kind = GK_REFLECT_PROTECT; rs = a[0]; len = a[1]; prot = (int)a[2] & 7; break;
    case GK_SYS_munmap: kind = GK_REFLECT_UNMAP; rs = a[0]; len = a[1]; break;
    case GK_SYS_mmap: kind = GK_REFLECT_MAP; rs = a[0]; len = a[1]; break;
    case GK_TRAP_vm_protect:
      if (self && !a[3]) { kind = GK_REFLECT_PROTECT; rs = a[1]; len = a[2]; prot = (int)a[4] & 7; }
      else if (self && t->user_mode && a[2] && !guest_range_ok(t, a[1], a[2], 1))
        return KERN_PROTECTION_FAILURE;  // a max-protection change is not reflected, but still confined
      break;
    case GK_TRAP_vm_deallocate:
      if (self) { kind = GK_REFLECT_UNMAP; rs = a[1]; len = a[2]; }
      break;
    case GK_TRAP_vm_allocate:
    case GK_TRAP_vm_map:
      if (self && a[1]) {
        if (copy_in(&rs, a[1], sizeof rs) < 0) return KERN_INVALID_ADDRESS;
        kind = GK_REFLECT_MAP;
        addr_out = a[1];
        len = a[2];
      }
      break;
  }
  if (len == 0) kind = GK_REFLECT_NONE;
  // A ring-3 turn may only unmap or reprotect a range it could reach itself, and
  // may only place a new mapping where the kernel chooses: a fixed mmap is held
  // to a reachable range, and a vm_allocate/vm_map must be ANYWHERE (so it gets
  // fresh memory, not another arena) with its result-address pointer reachable.
  if (t->user_mode && kind) {
    int bad = 0;
    if (kind == GK_REFLECT_UNMAP || kind == GK_REFLECT_PROTECT) {
      bad = !guest_range_ok(t, rs, len, 1);
    } else if (nr == GK_SYS_mmap) {
      if (a[3] & MAP_FIXED) bad = !guest_range_ok(t, rs, len, 1);
    } else {  // vm_allocate / vm_map
      uint64_t flags = (nr == GK_TRAP_vm_map) ? a[4] : a[3];
      bad = !(flags & VM_FLAGS_ANYWHERE) || !guest_range_ok(t, addr_out, sizeof(uint64_t), 1);
    }
    if (bad) {
      if (nr < 0) return KERN_PROTECTION_FAILURE;
      r->spsr |= GK_SPSR_C;
      return GK_EPERM;
    }
  }
  if (kind) {
    rs = HOST_FLOOR(rs);
    re = HOST_CEIL(rs + len);
    pthread_mutex_lock(&G->lock);
  }
  uint64_t x1 = 0;
  int carry = 0;
  long ret = hvf_syscall(nr, a, &x1, &carry);
  int ok = nr < 0 ? ret == KERN_SUCCESS : !carry;
  if (kind) {
    if (ok) {
      if (kind == GK_REFLECT_MAP) {
        // The range the mapping got (a placed one: the one asked for).
        uintptr_t got = rs;
        if (nr == GK_SYS_mmap) got = (uintptr_t)ret;
        else if (copy_in(&got, addr_out, sizeof got) < 0) got = rs;
        rs = HOST_FLOOR(got);
        re = HOST_CEIL(got + len);
      }
      if (kind == GK_REFLECT_PROTECT && (prot & PROT_READ)) {
        reprotect_range_all(t, rs, re, prot);
      } else {
        unmap_range_all(rs, re);
        if (kind != GK_REFLECT_PROTECT) s2_unmap(rs, re);
      }
    }
    pthread_mutex_unlock(&G->lock);
    flush_tlb(t, r);
  }
  if (nr >= 0) {
    r->x[1] = x1;
    if (carry) r->spsr |= GK_SPSR_C; else r->spsr &= ~GK_SPSR_C;
  }
  if (G->dbg)
    fprintf(stderr, "[gk] vcpu %d syscall %ld(%#lx, %#lx, %#lx, %#lx) -> %ld%s\n", t->id, nr,
            (unsigned long)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4, ret,
            carry ? " (errno)" : "");
  return ret;
}
// A stage-2 abort under HVF: the guest reached a guest-physical page that is
// not in stage 2, because nothing has mapped it yet or because forward_syscall
// dropped it while this vCPU's TLB kept a translation to it (see the HVF
// section). If the host maps the page now it is put in and the access
// retried; otherwise the turn ends at the address, as a host-backing fault
// does under KVM. A page that is in by the time the lock is taken was put
// there by another vCPU's fault meanwhile, and the access is simply retried
// (the caller's repeat limit covers a page that keeps aborting). Caller holds
// no lock.
static int s2_fault(gk_thread *t, uint64_t gpa) {
  int ok = 0;
  const char *why = "no region";
  pthread_mutex_lock(&G->lock);
  gk_region *rg = region_by_gpa(G->regions, gpa);
  if (rg) {
    uintptr_t hva = rg->start + (gpa - rg->gpa), rs, re;
    int perms;
    if (s2_test(rg->gpa + (HOST_FLOOR(hva) - rg->start))) ok = 1;
    else if (!host_region(HOST_FLOOR(hva), &rs, &re, &perms) || !(perms & 1)) why = "host page unreadable";
    else if (s2_ensure(rg, hva) == 0) ok = 1;
    else why = "stage-2 map failed";
  }
  pthread_mutex_unlock(&G->lock);
  if (!ok && G->dbg) {
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, ": stage-2 abort at guest-physical "); db_hex(&d, gpa);
    db_str(&d, ": "); db_str(&d, why);
    db_flush(&d);
  }
  return ok;
}

// run_vcpu under HVF: the vector handlers' exits arrive as HVC exceptions
// with the state in the vcb, as the hypercalls do under KVM; stage-2 aborts
// and anything else the hypervisor traps stop the guest on the instruction
// itself, wherever it was, and end the turn from there unless the abort can
// be resolved.
static long run_vcpu(gk_thread *t) {
  struct gk_vcb *v = VCB(t);
  for (;;) {
    t->tlb_touched = 1;  // the vCPU runs: its TLB is no longer known (see wall_raise)
    hv_return_t hr = hv_vcpu_run(t->vcpu);
    if (hr != HV_SUCCESS) {
      if (G->dbg) {
        gk_dbuf d = {.n = 0};
        db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
        db_str(&d, ": hv_vcpu_run failed: "); db_hex(&d, (uint64_t)(uint32_t)hr);
        db_flush(&d);
      }
      v->action = 0;
      vcpu_reset_pc(t);
      return -1;
    }
    const hv_vcpu_exit_t *x = t->exit;
    if (x->reason == HV_EXIT_REASON_CANCELED) continue;
    if (x->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
      hv_vcpu_set_vtimer_mask(t->vcpu, true);  // the guest arms no timer
      continue;
    }
    if (x->reason != HV_EXIT_REASON_EXCEPTION) {
      if (G->dbg) fprintf(stderr, "[gk] unexpected exit reason=%u\n", (unsigned)x->reason);
      v->action = 0;
      vcpu_reset_pc(t);
      return -1;
    }
    unsigned ec = ESR_EC(x->exception.syndrome);
    if (ec == ESR_EC_HVC64) {
      if (vcpu_reg(t->vcpu, HV_REG_X0) != GK_HC_EXC) return vcb_bad(t);
      long ret;
      if (vcb_exception(t, &ret)) return ret;
      continue;
    }
    if (ec == ESR_EC_DABT_LOW || ec == ESR_EC_IABT_LOW) {
      // A stage-2 abort (the guest's EL1 and EL0 are both below the
      // hypervisor). The instruction is not completed; a retry re-runs it.
      uint64_t va = x->exception.virtual_address, gpa = x->exception.physical_address;
      int repeat = va == t->last_fault && ++t->fault_repeat > 2;
      if (va != t->last_fault) { t->last_fault = va; t->fault_repeat = 0; }
      if (!repeat && s2_fault(t, gpa)) continue;
      if (G->dbg) {
        gk_dbuf d = {.n = 0};
        db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
        db_str(&d, repeat ? ": stage-2 abort repeats at " : ": host backing gone at ");
        db_hex(&d, va); db_str(&d, " (guest-physical "); db_hex(&d, gpa);
        db_str(&d, ", pc "); db_hex(&d, vcpu_reg(t->vcpu, HV_REG_PC)); db_str(&d, ")");
        db_flush(&d);
      }
      v->action = 0;
      vcpu_reset_pc(t);
      G->fault_addr = va;
      return GK_EFAULT;
    }
    uint64_t pc = vcpu_reg(t->vcpu, HV_REG_PC);
    if (ec == ESR_EC_WFX) {  // WFI/WFE: nothing to wait for; step over it
      vcpu_set_reg(t->vcpu, HV_REG_PC, pc + 4);
      continue;
    }
    // Anything else the hypervisor traps (a system register access it does
    // not virtualize, say): genuine, at the instruction.
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": trapped exception class "); db_hex(&d, ec);
      db_str(&d, " at pc "); db_hex(&d, pc);
      db_str(&d, " (syndrome "); db_hex(&d, x->exception.syndrome); db_str(&d, ")");
      db_flush(&d);
    }
    v->action = 0;
    vcpu_reset_pc(t);
    G->fault_addr = pc;
    return GK_EFAULT;
  }
}

// A macOS stack is a mapping of its full size: nothing grows on demand.
static int host_stack_grow(uintptr_t page) {
  (void)page;
  return 0;
}

// ---- the host thread (see stack_wall_bounds) ---------------------------------
// The thread pointer: the thread descriptor itself (pthread_self), which holds
// the TLS and starts exactly at the top of a pthread's stack, in the stack's
// own mapping.
static uintptr_t thread_pointer(void) { return (uintptr_t)pthread_self(); }

// The descriptor and its TLS start at `tp` and extend upward.
static uintptr_t thread_data_floor(uintptr_t tp) { return GUEST_FLOOR(tp); }

// Before every turn: the commpage shadows follow the kernel's pages.
static void turn_prepare(void) { commpage_refresh(); }

#endif  // GK_HVF
