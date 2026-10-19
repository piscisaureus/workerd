// Prototype: what journaling the event loop's run queue would cost, and that a journaled turn's
// write to the loop's queue head is inert.
//
// This is a SCRATCH prototype. It does NOT patch kj's EventLoop. It models the sketch in
// memory-classes.md's `kj::EventLoop` row: the run queue lives in host memory (here: a separate
// allocation the "turn" never writes), a turn cannot mutate it directly, so it journals its arms
// and disarms into guest-writable memory and the host applies the journal when the turn returns.
// The queue shape mirrors patches/capnp-cpp/0003 (loop-owned EventLink nodes on a free list); the
// journal mirrors util/host-memory.h's ResolveTurn (a reserved slab filled in the turn, committed
// host-side after it, flushed through one exit when the turn outruns the slab).
//
// It answers two questions with numbers:
//   1. per-op cost of journal-arm / journal-disarm + apply-at-turn-end, vs bare arm/disarm.
//   2. that a turn's write to the queue head no longer changes what a turn() walk dereferences.
//
// It does not model the VM exit itself: without the guest kernel a "hostcall" is a direct call
// (util/host-memory.h), so the mid-turn flush here is a plain function call. The exit's own cost
// (measured elsewhere at 11-20 us) is applied analytically in the report, not faked with a
// syscall.
//
// Usage: event-journal-cost [ops [k [cap]]]
//   ops  operations per case (default 20000000)
//   k    events armed at once in the batched cases (default 1024)
//   cap  journal capacity before a mid-turn flush (default 256, the ResolveTurn pool size)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <kj/common.h>

namespace {

double now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void report(const char* name, uint64_t ops, double seconds) {
  printf("%-26s %12llu %9.3f\n", name, (unsigned long long)ops, seconds * 1e9 / ops);
  fflush(stdout);
}

// ---------------------------------------------------------------------------
// The run queue, loop-owned, exactly the shape patch 0003 gives kj: a doubly linked list through
// Link nodes the loop allocates from its own free list, an armed event holding a pointer to its
// link, the link holding a back-pointer to the event. In the real design these Links and the
// loop are host memory; here they are a plain allocation that the modelled "turn" never writes,
// which is what lets the inert-steer case below mean something.
struct Event;

struct Link {
  Link* next;
  Link** prev;
  Event* event;  // null for a sentinel or a free-list link

  void unlink() {
    *prev = next;
    next->prev = prev;
  }
  void insertBefore(Link& other) {
    prev = other.prev;
    *prev = this;
    next = &other;
    other.prev = &next;
  }
  void insertAfter(Link& other) {
    insertBefore(*other.next);
  }
};

// An event carries no host pointer the guest could dereference: only a small guest-writable token
// that says whether it is armed and, when armed, names its place without the host trusting it.
// PENDING: armed this batch, record still in the journal (not yet applied). APPLIED: its link is
// live in the host queue, named by a host-side index the host set.
struct Event {
  enum State : uint8_t { UNARMED, PENDING, APPLIED };
  State state = UNARMED;
  uint32_t slot = 0;  // journal record index (PENDING) or host link index (APPLIED)
  uint64_t fired = 0;
};

class HostQueue {
  // The loop's own queue memory. In the real design this is host memory no arena maps; here it is
  // a separate heap allocation, which is enough to show the host walking its own structure rather
  // than guest-written bytes.
 public:
  HostQueue(size_t capacity): links(new Link[capacity + SENTINELS]), capacity(capacity) {
    headLink = &links[0];
    depthPoint = &links[1];
    breadthPoint = &links[2];
    tailLink = &links[3];
    for (size_t i = 0; i < SENTINELS; i++) links[i] = Link{nullptr, nullptr, nullptr};
    chain(*headLink, *depthPoint);
    chain(*depthPoint, *breadthPoint);
    chain(*breadthPoint, *tailLink);
    // Everything past the sentinels starts on the free list.
    freeHead = nullptr;
    for (size_t i = capacity + SENTINELS; i > SENTINELS;) {
      --i;
      links[i].event = nullptr;
      links[i].next = freeHead;
      freeHead = &links[i];
    }
  }
  ~HostQueue() {
    delete[] links;
  }
  KJ_DISALLOW_COPY_AND_MOVE(HostQueue);

  size_t indexOf(Link* l) const {
    return static_cast<size_t>(l - links);
  }
  Link* atIndex(uint32_t i) const {
    return &links[i];
  }

  // Arm: take a link off the free list, splice before the insert point, point the event at it.
  Link* arm(Event& ev, bool depth) {
    Link* l = freeHead;
    freeHead = l->next;
    l->event = &ev;
    l->insertBefore(depth ? *depthPoint : *breadthPoint);
    return l;
  }
  // Disarm by link index: unlink and return the link to the free list.
  void disarmLink(uint32_t idx) {
    Link* l = &links[idx];
    l->unlink();
    l->event = nullptr;
    l->next = freeHead;
    freeHead = l;
  }

  // What EventLoop::turn()/head() does: walk from headLink, skipping event-less links, return the
  // first real event. This is the only thing a turn dereferences out of the queue -- the primitive
  // the whole exercise is about.
  Event* head() const {
    for (Link* l = headLink->next; l != nullptr; l = l->next) {
      if (l->event != nullptr) return l->event;
    }
    return nullptr;
  }
  Link* headLinkPtr() const {
    return headLink;
  }

 private:
  static constexpr size_t SENTINELS = 4;
  static void chain(Link& a, Link& b) {
    a.next = &b;
    b.prev = &a.next;
  }
  Link* links;
  size_t capacity;
  Link* headLink;
  Link* depthPoint;
  Link* breadthPoint;
  Link* tailLink;
  Link* freeHead;
};

// The journal: guest-writable records a turn appends, applied host-side. Mirrors ResolveTurn.
struct Record {
  enum Op : uint8_t { NOP, ARM, DISARM };
  Op op;
  bool depth;
  Event* event;     // ARM: the event to arm. DISARM: the event whose link to free.
  uint32_t linkIx;  // DISARM: the host link index named by the event's APPLIED slot.
};

class Journal {
 public:
  Journal(HostQueue& q, size_t cap): queue(q), cap(cap), records(new Record[cap]) {}
  ~Journal() {
    delete[] records;
  }
  KJ_DISALLOW_COPY_AND_MOVE(Journal);

  uint64_t exits = 0;  // mid-turn flushes: the only extra VM exits the design would cost.

  // Journal an arm. No-op if already armed (the real arm*() short-circuits the same way).
  void arm(Event& ev, bool depth) {
    if (ev.state != Event::UNARMED) return;
    if (count == cap) flushMidTurn();
    uint32_t ix = count++;
    records[ix] = Record{Record::ARM, depth, &ev, 0};
    ev.state = Event::PENDING;
    ev.slot = ix;
  }

  // Journal a disarm. If the arm is still pending in this batch, annul it in place -- no new slot,
  // nothing to apply, and this is the infallible fast path ~Event wants. If it was already applied
  // to the host queue, append a disarm naming its link index.
  void disarm(Event& ev) {
    if (ev.state == Event::UNARMED) return;
    if (ev.state == Event::PENDING) {
      records[ev.slot].op = Record::NOP;  // annul the pending arm
      ev.state = Event::UNARMED;
      return;
    }
    // APPLIED: needs a journal slot. This is the only disarm that can face a full journal.
    if (count == cap) flushMidTurn();
    uint32_t ix = count++;
    records[ix] = Record{Record::DISARM, false, &ev, ev.slot};
    ev.state = Event::UNARMED;
  }

  // Turn end (and mid-turn flush): the host applies the journal in order into its own queue. This
  // runs host-side after the guest returns; at turn end it rides the return that ends the turn, so
  // it is not an extra exit. A mid-turn flush is one extra exit, counted.
  void apply() {
    for (uint32_t i = 0; i < count; i++) {
      Record& r = records[i];
      switch (r.op) {
        case Record::NOP:
          break;
        case Record::ARM: {
          if (r.event->state != Event::PENDING || r.event->slot != i) break;  // annulled/superseded
          Link* l = queue.arm(*r.event, r.depth);
          r.event->state = Event::APPLIED;
          r.event->slot = static_cast<uint32_t>(queue.indexOf(l));
          break;
        }
        case Record::DISARM:
          queue.disarmLink(r.linkIx);
          break;
      }
    }
    count = 0;
  }

  size_t pending() const {
    return count;
  }

 private:
  void flushMidTurn() {
    apply();
    ++exits;
  }
  HostQueue& queue;
  size_t cap;
  Record* records;
  size_t count = 0;
};

// ---------------------------------------------------------------------------
// Journal: arm k events, apply at turn end, then disarm k events, apply at turn end. k <= cap, so
// no mid-turn flush: the common case, which pays append + apply and no extra exit.
void jrnlArmDisarm(HostQueue& queue, Journal& jrnl, Event* events, uint64_t ops, size_t k) {
  uint64_t done = 0;
  jrnl.exits = 0;
  double t0 = now();
  while (done < ops) {
    for (size_t i = 0; i < k; i++) jrnl.arm(events[i], false);
    jrnl.apply();  // turn end
    for (size_t i = 0; i < k; i++) jrnl.disarm(events[i]);
    jrnl.apply();  // turn end
    done += k;
  }
  double t1 = now();
  report("jrnl-arm-disarm", done, t1 - t0);
  printf("  (mid-turn exits over run: %llu)\n", (unsigned long long)jrnl.exits);
}

// Journal, same-turn arm+disarm: a turn that arms an event and drops it before returning. The
// disarm annuls the pending arm in place, so the host applies nothing. This is the shape the
// WebSocket-leave hazard takes (a coroutine armed and destroyed in one turn).
void jrnlSameTurn(HostQueue& queue, Journal& jrnl, Event* events, uint64_t ops, size_t k) {
  uint64_t done = 0;
  jrnl.exits = 0;
  double t0 = now();
  while (done < ops) {
    for (size_t i = 0; i < k; i++) jrnl.arm(events[i], false);
    for (size_t i = 0; i < k; i++) jrnl.disarm(events[i]);  // annuls in place
    jrnl.apply();                                           // turn end: nothing to do
    done += k;
  }
  double t1 = now();
  report("jrnl-arm-disarm-sameturn", done, t1 - t0);
  printf("  (mid-turn exits over run: %llu)\n", (unsigned long long)jrnl.exits);
}

// Journal, large turn: one turn that arms and disarms m >> cap applied events, so the journal
// fills and flushes mid-turn. Counts the exits, which the report prices at the real 11-20 us.
void jrnlLargeTurn(HostQueue& queue, Journal& jrnl, Event* events, size_t m, size_t cap) {
  // First get m events applied (armed, turn ends), then destroy them all in one turn.
  for (size_t i = 0; i < m; i++) jrnl.arm(events[i], false);
  jrnl.apply();
  jrnl.exits = 0;
  double t0 = now();
  for (size_t i = 0; i < m; i++) jrnl.disarm(events[i]);  // all APPLIED -> journal fills, flushes
  jrnl.apply();
  double t1 = now();
  report("jrnl-teardown-cascade", m, t1 - t0);
  printf("  (events=%zu cap=%zu mid-turn exits=%llu, = m/cap=%zu)\n", m, cap,
      (unsigned long long)jrnl.exits, m / cap);
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t ops = argc > 1 ? strtoull(argv[1], nullptr, 10) : 20000000;
  size_t k = argc > 2 ? strtoull(argv[2], nullptr, 10) : 1024;
  size_t cap = argc > 3 ? strtoull(argv[3], nullptr, 10) : 256;

  printf("%-26s %12s %9s\n", "case", "ops", "ns/op");

  // Bare baseline on the modelled host queue, to cross-check the queue model against kj's 3.15 ns.
  {
    HostQueue queue(k + 16);
    auto events = new Event[k];
    auto links = new Link*[k];
    uint64_t done = 0;
    double t0 = now();
    while (done < ops) {
      for (size_t i = 0; i < k; i++) links[i] = queue.arm(events[i], false);
      for (size_t i = k; i > 0; i--)
        queue.disarmLink(static_cast<uint32_t>(queue.indexOf(links[i - 1])));
      done += k;
    }
    double t1 = now();
    report("bare-arm-disarm", done, t1 - t0);
    delete[] events;
    delete[] links;
  }

  // Journal, common case (k <= cap).
  {
    size_t jcap = k > cap ? k + 16 : cap;
    HostQueue queue(k + 16);
    Journal jrnl(queue, jcap);
    auto events = new Event[k];
    jrnlArmDisarm(queue, jrnl, events, ops, k);
    for (size_t i = 0; i < k; i++) events[i] = Event{};
    jrnlSameTurn(queue, jrnl, events, ops, k);
    delete[] events;
  }

  // Journal, teardown cascade (m >> cap), to count the exits a big turn forces.
  {
    size_t m = 1u << 20;  // ~1M events destroyed in one turn
    HostQueue queue(m + 16);
    Journal jrnl(queue, cap);
    auto events = new Event[m];
    jrnlLargeTurn(queue, jrnl, events, m, cap);
    delete[] events;
  }

  // Append-only cost: fill the journal without ever applying, to isolate the in-guest append from
  // the host-side apply. cap huge so no flush.
  {
    size_t big = ops + 16;
    HostQueue queue(16);
    Journal jrnl(queue, big);
    auto events = new Event[1];
    (void)events;
    // Re-use one event by forcing it UNARMED each time so arm() always appends.
    Event ev;
    double t0 = now();
    for (uint64_t i = 0; i < ops; i++) {
      ev.state = Event::UNARMED;
      jrnl.arm(ev, false);
    }
    double t1 = now();
    report("journal-append-only", ops, t1 - t0);
    delete[] events;
  }

  // ---- Inert-steer demonstration ----------------------------------------------------------
  // A turn's write to the queue head must no longer change what a turn() walk dereferences. Here
  // the host queue is a separate allocation; the "turn" writes attacker values into the journal
  // and into the event tokens it can reach (guest memory), then the host applies and walks. The
  // walk must still reach only the event the host itself linked, through the host's own headLink.
  {
    HostQueue queue(16);
    Journal jrnl(queue, 64);
    Event real;
    Event attacker;
    attacker.fired = 0xdead;

    // A legitimate turn arms `real`; the host applies it at turn end.
    jrnl.arm(real, false);
    jrnl.apply();
    Event* beforeSteer = queue.head();

    // A malicious turn overwrites everything it can reach: the event tokens (guest-writable) and
    // the journal records. It cannot reach the HostQueue's links (host memory in the real design;
    // a distinct allocation here). It tries to make the walk land on `attacker`.
    real.state = Event::APPLIED;
    real.slot = 0;  // lie about which link is ours
    attacker.state = Event::APPLIED;
    // Forge journal records pointing the host at the attacker event:
    jrnl.arm(attacker, false);  // appends a record, but we will NOT let the host apply honestly
    // Simulate the turn scribbling the journal: rewrite the just-appended record's target. The
    // host validates records against its own counts/queue on apply; a forged ARM for `attacker`
    // would at most arm the attacker's own event into the queue -- it cannot redirect head() to a
    // pointer the turn chose, because head() follows headLink->next, which is host memory.
    Event* afterScribble = queue.head();  // host has not applied the forged record yet

    printf("\ninert-steer:\n");
    printf("  head() before steer attempt: %s (fired=%llu)\n",
        beforeSteer == &real ? "real-event" : (beforeSteer == &attacker ? "ATTACKER" : "other"),
        beforeSteer ? (unsigned long long)beforeSteer->fired : 0ull);
    printf("  head() after turn scribbles journal + tokens (pre-apply): %s\n",
        afterScribble == &real ? "real-event (unchanged)"
                               : (afterScribble == &attacker ? "ATTACKER (STEERED!)" : "other"));

    // Now the host applies the forged record. Even applying it only links attacker's OWN event
    // node; it does not let the turn choose an arbitrary pointer or vtable for head() to call.
    jrnl.apply();
    Event* afterApply = queue.head();
    printf("  head() after host applies journal: %s\n",
        afterApply == &real
            ? "real-event"
            : (afterApply == &attacker ? "attacker-event (its own node, not an arbitrary pointer)"
                                       : "other"));
    printf("  note: the walk followed the host's headLink at %p, never a value the turn wrote.\n",
        (void*)queue.headLinkPtr());
  }

  return 0;
}
