// gk: KVM, on either architecture: memslots and parked vCPUs.
#include "gk_internal.h"
#if !GK_HVF

// /dev/kvm, at the API version gk is written against.
static int kvm_open(void) {
  G->kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (G->kvm < 0) { G->err = "open /dev/kvm"; return -1; }
  if (ioctl(G->kvm, KVM_GET_API_VERSION, 0) != 12) { G->err = "KVM API != 12"; return -1; }
  return 0;
}

// Create memslot `slot` mapping guest-physical `gpa` to the host range [s, e).
// Runs on the fault path: raw output only (no strerror, it may allocate).
static int memslot_add(int slot, uint64_t gpa, uintptr_t s, uintptr_t e) {
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)slot,
                                          .guest_phys_addr = gpa,
                                          .memory_size = e - s,
                                          .userspace_addr = s};
  if (ioctl(G->vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] memslot "); db_dec(&d, slot);
      db_str(&d, " ["); db_hex(&d, s); db_str(&d, ","); db_hex(&d, e);
      db_str(&d, ") at guest-physical "); db_hex(&d, gpa);
      db_str(&d, " failed: errno "); db_dec(&d, errno);
      db_flush(&d);
    }
    return -1;
  }
  return 0;
}

// Delete region `n`'s memslot. A deleted memslot maps nothing, so the
// guest-physical range is free to reuse (*keep_gpa stays 0). Returns -1 if
// KVM refused, in which case the region must stay.
static int memslot_remove(gk_region *n, int *keep_gpa) {
  (void)keep_gpa;
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)n->slot,
                                          .guest_phys_addr = region_gpa(n),
                                          .memory_size = 0,
                                          .userspace_addr = n->start};
  if (ioctl(G->vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (G->dbg)
      fprintf(stderr, "[gk] memslot %d [%#lx,%#lx) delete failed: %s\n", n->slot,
              (unsigned long)n->start, (unsigned long)n->end, strerror(errno));
    return -1;
  }
  return 0;
}

// Any host thread may run a KVM vCPU, so a vCPU outlives its thread: it is
// parked with its kvm_run mapping for the next thread that needs one (see the
// vCPU pool), or closed if the pool has no room (p NULL). Caller holds G->lock.
static void vcpu_detach(gk_thread *t) {
  // A pooled vCPU is out of KVM_RUN: the next thread to take it programs it
  // by ioctl, which would wait behind a run in progress.
  park_leave(t);
}
static void vcpu_park_hw(gk_thread *t, gk_parked_vcpu *p) {
  if (p) {
    p->fd = t->fd;
    p->run = t->run;
  } else {
    if (t->runner) runner_stop(t->runner);
    if (t->efd >= 0) close(t->efd);
    if (t->stats_fd >= 0) close(t->stats_fd);
    munmap(t->run, G->run_size);
    close(t->fd);
  }
}

// KVM follows a host remap of a thread's stack mapping (see the host-backing
// section), so a thread's stack needs no refresh when it comes or goes.
static void stack_refresh(void) {}

#endif  // !GK_HVF
