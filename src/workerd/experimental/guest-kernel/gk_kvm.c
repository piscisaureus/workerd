// gk: KVM, on either architecture: memslots.
#include "gk_internal.h"
#if !GK_HVF

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

#endif  // !GK_HVF
