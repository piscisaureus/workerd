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
  uint64_t gpa;
  if (gpa_assign(s, e, &gpa) < 0) return -1;
  if (memslot_add(slot, gpa, s, e) < 0) {
    gpa_unassign(s, e, gpa);
    return -1;  // the node stays where it was (free list or untouched pool tail)
  }
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
  region_set_gpa(n, gpa);
  n->prio = gk_rand();
  G->regions = treap_insert(G->regions, n);
  G->region_live++;
  return 0;
}

// Delete a region's memslot and drop it from the tree; its node (and memslot
// id) go to the free list, and its guest-physical range back to the allocator
// unless the hypervisor may still map it. Caller holds G->lock and has made
// sure no root maps a page in the region any more, so the guest cannot reach
// it. `keep_gpa`: the guest-physical range is not reused even so, as a vCPU
// running under a destroyed arena's root may hold translations to it (see
// gk_arena_destroy). Returns -1 if the memslot could not be deleted: the
// region then stays.
static int region_remove(gk_region *n, int keep_gpa) {
  if (memslot_remove(n, &keep_gpa) < 0) return -1;
  G->regions = treap_remove(G->regions, n);
  G->region_live--;
  if (!keep_gpa) gpa_retire(n);
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
// how many were deleted (see region_remove for `keep_gpa`). Caller holds
// G->lock.
static int region_remove_range(uintptr_t s, uintptr_t e, int keep_gpa) {
  int n = 0;
  gk_region *r = region_lower_bound(s);
  while (r && r->start < e) {
    gk_region *next = region_succ(r->start);
    if (r->end <= e && region_remove(r, keep_gpa) == 0) n++;
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
    if (region_clip_holes(&s, &gap_end)) continue;
    if (region_add(s, gap_end) < 0) return -1;
    s = gap_end;
  }
  return 0;
}
