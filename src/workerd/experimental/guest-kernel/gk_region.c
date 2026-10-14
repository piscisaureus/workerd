// gk: the MMU layer: the interval tree of memslot-backed regions.
#include "gk_internal.h"

// ---- the MMU layer: interval-tree memslot manager --------------------------
// guest-physical == host-virtual everywhere, so a KVM memslot maps a range of
// guest-physical to the identical host-virtual range. Backed ranges ("regions")
// are tracked in an interval tree (a treap keyed by start) of non-overlapping
// [start, end) windows; every mapped PTE must have a backing memslot, or KVM
// takes the MMIO/emulation path and KVM_RUN returns EFAULT.
//
// Memory is backed one aligned GK_BACK_WIN window at a time (region_ensure
// rounds outward to the window), rather than one memslot per exact host mapping.
// This is a cost choice, not a correctness one. Exact per-page memslots are
// safe (given the malloc-free fault path -- see demand_map's signal-safety
// note), but each KVM_SET_USER_MEMORY_REGION costs on the order of 20us, and a
// V8-sized heap backed by 4K slots would blow past KVM's ~32764-memslot limit.
// A window amortizes both: one create backs 2MB, and a mapping's later growth
// usually lands in a window that already exists. Because everything is
// identity-mapped, an oversized window that spills into an unmapped hole is
// harmless -- the guest never has a PTE there, so KVM never faults it in.
// Protection is not cached on the region; the PTE carries the live host
// protection, re-read on every fault.
//
// Memslots are created once and deleted only when the arena they lie in is
// destroyed (see gk_arena_destroy). An identity memslot validly backs its
// window whether or not the host currently has memory there, so mprotect and
// munmap only need to drop the guest PTEs (see forward_syscall), not the
// memslot -- and a delete is not free: KVM invalidates every nested-page-table
// root when a memslot is removed, whereas adding an abutting memslot zaps
// nothing. An arena's teardown is the exception because its address space is
// given back for good: keeping its windows would let memslots grow without
// bound under isolate churn (KVM allows ~32K per VM), so they are deleted and
// their ids and pool nodes reused.
//
// TODO: 2MB windows over-back sparsely committed reservations. A production MMU
// could track exact VMAs (cf. FreeBSD's vm_map RB tree, sys/vm, 2-clause BSD)
// and issue cross-vCPU TLB shootdowns. Exact bounds are already safe, so this is
// a density/cost improvement, not a correctness fix.
// Region nodes come from a static pool because the fault path must not allocate.
// The host side of a guest fault runs on the SAME host thread whose guest half
// took the fault, and that thread may be in the middle of glibc malloc -- in
// fact the store that faults is often sysmalloc writing the freshly-grown heap
// top chunk's size field, so at that instant the arena's top chunk has size 0
// and its invariants are momentarily broken. If the fault handler then allocates
// it re-enters that same arena: single-threaded, glibc skips the arena lock and
// the nested malloc corrupts the arena (aborting later inside sysmalloc, at
// malloc.c's top-chunk assertion); multi-threaded, it deadlocks on the arena
// lock the guest half holds. So the fault path -- and the host side of any
// syscall malloc itself forwards, e.g. the brk/mmap sysmalloc issues -- must be
// async-signal-safe with respect to the guest thread: no malloc/free, no
// allocating stdio, no lock the guest may hold. That is why host_region reads
// /proc/self/maps with raw open/read rather than fopen, and region nodes come
// from this pool rather than calloc. (This corruption was once attributed to
// KVM's on-demand memslot creation; the memslot ioctl is not involved -- an
// added, abutting memslot invalidates nothing. See the backing-window note.)

// xorshift32 PRNG for treap priorities. Caller holds G->lock.
static unsigned gk_rand(void) {
  unsigned s = G->rand_state;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  G->rand_state = s;
  return s;
}

// Treap: BST ordered by `start`, max-heap ordered by `prio`.
static gk_region *rot_r(gk_region *n) {
  gk_region *l = n->l;
  n->l = l->r;
  l->r = n;
  return l;
}
static gk_region *rot_l(gk_region *n) {
  gk_region *r = n->r;
  n->r = r->l;
  r->l = n;
  return r;
}
static gk_region *treap_insert(gk_region *root, gk_region *node) {
  if (!root) return node;
  if (node->start < root->start) {
    root->l = treap_insert(root->l, node);
    if (root->l->prio > root->prio) root = rot_r(root);
  } else {
    root->r = treap_insert(root->r, node);
    if (root->r->prio > root->prio) root = rot_l(root);
  }
  return root;
}

// The region containing `addr`, or NULL. Caller holds G->lock.
static struct gk_region *region_find(uintptr_t addr) {
  gk_region *n = G->regions;
  while (n) {
    if (addr < n->start) n = n->l;
    else if (addr >= n->end) n = n->r;
    else return n;
  }
  return NULL;
}

// The region with the smallest start strictly greater than `s`, or NULL (the
// in-order successor of `s` in the start-ordered tree). Caller holds G->lock.
static gk_region *region_succ(uintptr_t s) {
  gk_region *n = G->regions, *best = NULL;
  while (n) {
    if (n->start > s) { best = n; n = n->l; }
    else n = n->r;
  }
  return best;
}

// Unlink `node` (which is in the tree) by rotating it down to a leaf, keeping
// the heap order among the others. Caller holds G->lock.
static gk_region *treap_remove(gk_region *root, gk_region *node) {
  if (!root) return NULL;
  if (node->start < root->start) {
    root->l = treap_remove(root->l, node);
  } else if (node->start > root->start) {
    root->r = treap_remove(root->r, node);
  } else {
    if (!root->l) return root->r;
    if (!root->r) return root->l;
    if (root->l->prio > root->r->prio) {
      root = rot_r(root);
      root->r = treap_remove(root->r, node);
    } else {
      root = rot_l(root);
      root->l = treap_remove(root->l, node);
    }
  }
  return root;
}


// Create one memslot-backed region for [s, e). Caller holds G->lock and has
// ensured [s, e) does not overlap any existing region. A pool node freed by a
// region deletion is reused first, with the KVM memslot id it was created
// with, so both are bounded by the peak number of live regions.
static int region_add(uintptr_t s, uintptr_t e) {
  gk_region *n;
  int slot, recycled = G->region_free != NULL;
  if (recycled) {
    n = G->region_free;
    slot = n->slot;
  } else {
    if (G->region_n >= GK_MAX_REGIONS) return -1;
    n = &G->region_pool[G->region_n];
    slot = G->next_slot;
  }
  uint64_t gpa = s;
#if defined(__aarch64__)
  // A region cut short by a hole (see region_ensure) still gets whole windows
  // of guest-physical space, at its own offset into them.
  uintptr_t wlo = s & ~(GK_BACK_WIN - 1), whi = (e + GK_BACK_WIN - 1) & ~(GK_BACK_WIN - 1);
  if (gpa_alloc(whi - wlo, &gpa) < 0) {
    if (G->dbg) write_stderr("[gk] guest-physical space exhausted\n");
    return -1;
  }
  gpa += s - wlo;
#endif
#if !GK_HVF  // under HVF the window enters stage 2 page by page (see the HVF section)
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)slot,
                                          .guest_phys_addr = gpa,
                                          .memory_size = e - s,
                                          .userspace_addr = s};
  if (ioctl(G->vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
#if defined(__aarch64__)
    gpa_release(gpa - (s - wlo), whi - wlo);
#endif
    if (G->dbg) {  // fault path: raw output only (no strerror, it may allocate)
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] memslot "); db_dec(&d, slot);
      db_str(&d, " ["); db_hex(&d, s); db_str(&d, ","); db_hex(&d, e);
#if defined(__aarch64__)
      db_str(&d, ") at guest-physical "); db_hex(&d, gpa);
      db_str(&d, " failed: errno "); db_dec(&d, errno);
#else
      db_str(&d, ") failed: errno "); db_dec(&d, errno);
#endif
      db_flush(&d);
    }
    return -1;  // the node stays where it was (free list or untouched pool tail)
  }
#endif
  if (recycled) {
    G->region_free = n->l;
  } else {
    G->region_n++;
    G->next_slot++;
  }
  memset(n, 0, sizeof *n);
  n->start = s;
  n->end = e;
  n->slot = slot;
#if defined(__aarch64__)
  n->gpa = gpa;
#endif
  n->prio = gk_rand();
  G->regions = treap_insert(G->regions, n);
  G->region_live++;
  return 0;
}

// Delete a region's memslot and drop it from the tree; its node (and memslot
// id) go to the free list. Caller holds G->lock and has made sure no root maps
// a page in the region any more, so the guest cannot reach it. Returns the
// ioctl's result: on failure the region stays.
static int region_remove(gk_region *n) {
#if GK_HVF
  // The region goes either way (the host memory is going); guest-physical
  // pages stage 2 still maps are quarantined below rather than reused.
  int keep_gpa = s2_release(n) < 0;
#else
  uint64_t gpa = n->start;
#if defined(__aarch64__)
  gpa = n->gpa;
#endif
  struct kvm_userspace_memory_region r = {.slot = (uint32_t)n->slot,
                                          .guest_phys_addr = gpa,
                                          .memory_size = 0,
                                          .userspace_addr = n->start};
  if (ioctl(G->vmfd, KVM_SET_USER_MEMORY_REGION, &r) < 0) {
    if (G->dbg)
      fprintf(stderr, "[gk] memslot %d [%#lx,%#lx) delete failed: %s\n", n->slot,
              (unsigned long)n->start, (unsigned long)n->end, strerror(errno));
    return -1;
  }
#endif
  G->regions = treap_remove(G->regions, n);
  G->region_live--;
#if defined(__aarch64__)
  // A vCPU still running under a destroyed arena's root (see gk_arena_destroy)
  // may hold translations to this range: it is not handed out again then.
#if GK_HVF
  if (!G->gpa_keep && !keep_gpa) {
#else
  if (!G->gpa_keep) {
#endif
    uintptr_t wlo = n->start & ~(GK_BACK_WIN - 1);
    uintptr_t whi = (n->end + GK_BACK_WIN - 1) & ~(GK_BACK_WIN - 1);
    gpa_release(n->gpa - (n->start - wlo), whi - wlo);
  }
#endif
  n->l = G->region_free;
  G->region_free = n;
  return 0;
}

// The region with the smallest start at or beyond `s`, or NULL. Caller holds
// G->lock.
static gk_region *region_lower_bound(uintptr_t s) {
  gk_region *n = G->regions, *best = NULL;
  while (n) {
    if (n->start >= s) { best = n; n = n->l; }
    else n = n->r;
  }
  return best;
}

// Delete every region lying within [s, e). Regions never straddle a 512GiB
// slot boundary (windows are created within host mappings and arena spans,
// both of which lie within slots), so for a whole-slot range this is every
// region that overlaps it; one that did straddle would be left alone. Returns
// how many were deleted. Caller holds G->lock.
static int region_remove_range(uintptr_t s, uintptr_t e) {
  int n = 0;
  gk_region *r = region_lower_bound(s);
  while (r && r->start < e) {
    gk_region *next = region_succ(r->start);
    if (r->end <= e && region_remove(r) == 0) n++;
    r = next;
  }
  return n;
}

// Ensure memslots back all of [s, e), rounded outward to GK_BACK_WIN windows.
// Additive: existing regions are kept and only the gaps between them get fresh
// memslots. Caller holds G->lock.
static int region_ensure(uintptr_t s, uintptr_t e) {
  s &= ~(GK_BACK_WIN - 1);
  e = (e + GK_BACK_WIN - 1) & ~(GK_BACK_WIN - 1);
  while (s < e) {
    gk_region *have = region_find(s);
    if (have) {  // already backed; skip past it
      s = have->end;
      continue;
    }
    uintptr_t gap_end = e;
    gk_region *nx = region_succ(s);  // stop the new region before the next one
    if (nx && nx->start < gap_end) gap_end = nx->start;
#if defined(__aarch64__)
    // Nor may it cover a hole (see the vDSO section): KVM refuses a memslot
    // over the kernel's data pages, so the window is cut around them.
    int in_hole = 0;
    for (int i = 0; i < G->hole_n; i++) {
      const gk_prot_range *h = &G->holes[i];
      if (s >= h->start && s < h->end) { s = h->end; in_hole = 1; break; }
      if (h->start > s && h->start < gap_end) gap_end = h->start;
    }
    if (in_hole) continue;
#endif
    if (region_add(s, gap_end) < 0) return -1;
    s = gap_end;
  }
  return 0;
}
