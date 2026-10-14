// gk: thread records and the vCPU pool.
#include "gk_internal.h"

// ---- thread records ----------------------------------------------------------

// The calling thread's record, or NULL if it has none yet. tls_rec is user
// memory, so the record it names is accepted only if it belongs to this thread
// by kernel tid; anything else is tampering and fatal. Called at every host-side
// entry into gk (a gk_run, an arena switch, a thread ending); the KVM-exit
// path passes the record along instead.
static gk_thread *thread_cur(void) {
  int i = tls_rec;
  if (i == 0) return NULL;
  if (i < 1 || i > GK_MAX_VCPUS || G->threads[i - 1].tid != gettid_raw())
    gk_fatal("thread record does not belong to the calling thread");
  return &G->threads[i - 1];
}

// Take a record from the pool for the calling thread (caller holds G->lock).
// NULL if the pool is exhausted.
static gk_thread *thread_alloc_locked(long tid) {
  gk_thread *t = G->thread_free;
  if (t) G->thread_free = t->next_free;
  else if (G->thread_n < GK_MAX_VCPUS) t = &G->threads[G->thread_n++];
  else return NULL;
  memset(t, 0, sizeof *t);
  t->tid = tid;
  t->active_pml4 = G->pml4;
  return t;
}

// Bind `t` (taken from the pool) to the calling thread. `host` says the thread
// is one that ends through pthread exit (and so gets the key destructor), as
// opposed to a guest-created thread, which ends through its exit syscall.
static void thread_bind(gk_thread *t, int host) {
  tls_rec = (int)(t - G->threads) + 1;
  if (host) pthread_setspecific(G->thread_key, t);
}


// The calling thread's record, created if it has none. NULL if the pool is
// exhausted.
static gk_thread *thread_get(void) {
  gk_thread *t = thread_cur();
  if (t) return t;
  pthread_mutex_lock(&G->lock);
  t = thread_alloc_locked(gettid_raw());
  pthread_mutex_unlock(&G->lock);
  if (!t) {
    if (G->dbg) fprintf(stderr, "[gk] thread record pool exhausted\n");
    return NULL;
  }
  thread_bind(t, 1);
  stack_refresh();
  return t;
}

// Select `a` (NULL: the base root) as the active arena of the thread `t`
// belongs to, returning the previous one.
static gk_arena *arena_enter(gk_thread *t, gk_arena *a) {
  gk_arena *prev = t->active_arena;
  if (prev == a) return prev;
  if (a) __atomic_fetch_add(&a->active_threads, 1, __ATOMIC_SEQ_CST);
  t->active_arena = a;
  t->active_pml4 = a ? a->pml4 : G->pml4;
  if (prev) __atomic_fetch_sub(&prev->active_threads, 1, __ATOMIC_SEQ_CST);
  return prev;
}

// Return the thread's vCPU (and guest stack, if any) to the pool.
static void vcpu_park(gk_thread *t) {
  if (!t->inited) return;
  vcpu_detach(t);
  pthread_mutex_lock(&G->lock);
  if (G->parked_n < GK_MAX_VCPUS) {
    gk_parked_vcpu *p = &G->parked[G->parked_n++];
    vcpu_park_hw(t, p);
    p->id = t->id;
    p->stack_top = t->stack_top;
    p->ist = t->ist;
    p->side_stack = t->side_stack;
  } else {  // cannot happen with KVM_CAP_MAX_VCPUS <= GK_MAX_VCPUS; be safe
    vcpu_park_hw(t, NULL);
  }
  pthread_mutex_unlock(&G->lock);
  if (G->dbg) fprintf(stderr, "[gk] vcpu %d parked\n", t->id);
  t->inited = 0;
}

// The thread `t` belongs to is ending: park its vCPU, drop it from its active
// arena's user count, and return the record to the pool.
static void thread_release(gk_thread *t) {
  vcpu_park(t);
  arena_enter(t, NULL);
  tls_rec = 0;
  stack_refresh();
  pthread_mutex_lock(&G->lock);
  t->tid = 0;
  t->next_free = G->thread_free;
  G->thread_free = t;
  pthread_mutex_unlock(&G->lock);
}

// pthread key destructor: a host thread with a record is ending. (Guest-created
// threads end through the exit syscall in forward_syscall instead, which
// releases explicitly.) The key's value is not trusted; thread_ending finds
// and checks the record.
static void thread_key_dtor(void *p) {
  gk_thread *t = thread_ending(p);
  if (t) thread_release(t);
}
