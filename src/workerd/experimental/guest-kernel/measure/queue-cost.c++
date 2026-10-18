// What the event loop's run queue costs per step.
//
// Everything asynchronous in the runtime goes through kj's run queue, so the shape of its
// linkage is priced here rather than argued about. Each case times one of the operations the
// queue is made of and reports nanoseconds per operation; the cases below the promise machinery
// (then-chain, eval-chain) say what that is worth once the allocation and the node graph around
// it are included.
//
// One invocation is one launch. A single launch of anything on this box is not a number -- see
// measure/findings.md -- so run it several times and read the spread.
//
// Usage: queue-cost [ops [k]]
//   ops  operations per case (default 20000000)
//   k    events kept armed at once in the cases that queue more than one (default 1024)
// Output: one "name ops ns_per_op" line per case, then one "sizes" line of what the layout
// costs in bytes: the size of an event and the bytes of queue linkage per armed event.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <kj/async.h>
#include <kj/debug.h>

namespace {

double now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

long rssKb() {
  FILE* f = fopen("/proc/self/status", "r");
  if (f == nullptr) return -1;
  char line[256];
  long kb = -1;
  while (fgets(line, sizeof(line), f) != nullptr) {
    if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
  }
  fclose(f);
  return kb;
}

void report(const char* name, uint64_t ops, double seconds) {
  printf("%-14s %12llu %9.3f\n", name, (unsigned long long)ops, seconds * 1e9 / ops);
  fflush(stdout);
}

// An event that counts its firings and, if asked, arms itself again from fire(), which is what a
// continuation attached to an already settled promise does.
class CountEvent final: public kj::_::Event {
 public:
  enum Mode { ONCE, REARM_DEPTH, REARM_BREADTH };

  explicit CountEvent(Mode mode): kj::_::Event({}), mode(mode) {}

  void fire() override {
    ++fired;
    switch (mode) {
      case ONCE:
        break;
      case REARM_DEPTH:
        armDepthFirst();
        break;
      case REARM_BREADTH:
        armBreadthFirst();
        break;
    }
  }
  void traceEvent(kj::_::TraceBuilder& builder) override {}

  uint64_t fired = 0;

 private:
  Mode mode;
};

// One event that arms itself depth-first: the queue is one deep, and each turn writes the link
// it is about to read. This is the shape of a .then() chain.
void rearmDepth(kj::EventLoop& loop, uint64_t ops) {
  CountEvent event(CountEvent::REARM_DEPTH);
  event.armDepthFirst();
  double t0 = now();
  loop.run(ops);
  double t1 = now();
  KJ_ASSERT(event.fired == ops, event.fired, ops);
  event.disarm();
  report("rearm-depth", ops, t1 - t0);
}

// K events that each arm themselves breadth-first: the queue stays K deep, so the link a turn
// reads is not one it just wrote and the walk is spread over K of them.
void rearmBreadth(kj::EventLoop& loop, uint64_t ops, size_t k) {
  auto events = kj::heapArray<kj::Own<CountEvent>>(k);
  for (auto& e: events) e = kj::heap<CountEvent>(CountEvent::REARM_BREADTH);
  for (auto& e: events) e->armBreadthFirst();
  double t0 = now();
  loop.run(ops);
  double t1 = now();
  for (auto& e: events) e->disarm();
  report("rearm-breadth", ops, t1 - t0);
}

// K events armed and then drained, over and over: the arm and the turn, separated, so that a
// run of arms is not interleaved with the turns that consume them.
void armDrain(kj::EventLoop& loop, uint64_t ops, size_t k) {
  auto events = kj::heapArray<kj::Own<CountEvent>>(k);
  for (auto& e: events) e = kj::heap<CountEvent>(CountEvent::ONCE);
  uint64_t done = 0;
  double t0 = now();
  while (done < ops) {
    for (auto& e: events) e->armBreadthFirst();
    loop.run(k);
    done += k;
  }
  double t1 = now();
  report("arm-drain", done, t1 - t0);
}

// K events armed and then disarmed unfired, over and over: the disarm path on its own, which is
// the path a turn takes when it drops a promise graph whose events are still queued.
void armDisarm(kj::EventLoop& loop, uint64_t ops, size_t k) {
  auto events = kj::heapArray<kj::Own<CountEvent>>(k);
  for (auto& e: events) e = kj::heap<CountEvent>(CountEvent::ONCE);
  uint64_t done = 0;
  double t0 = now();
  while (done < ops) {
    for (auto& e: events) e->armBreadthFirst();
    for (auto& e: events) e->disarm();
    done += k;
  }
  double t1 = now();
  report("arm-disarm", done, t1 - t0);
}

// The queue under the promise machinery: a chain of .then()s on an already settled promise,
// built first and then run, with the two timed apart so that the allocation of the chain does
// not hide what running it costs.
void thenChain(kj::WaitScope& waitScope, uint64_t ops, size_t k) {
  uint64_t built = 0, ran = 0;
  double build = 0, run = 0;
  while (ran < ops) {
    double t0 = now();
    kj::Promise<void> p = kj::READY_NOW;
    for (size_t i = 0; i < k; i++) p = p.then([]() {});
    double t1 = now();
    p.wait(waitScope);
    double t2 = now();
    build += t1 - t0;
    run += t2 - t1;
    built += k;
    ran += k;
  }
  report("then-build", built, build);
  report("then-run", ran, run);
}

// evalLater() arms breadth-first rather than depth-first, so a chain of them walks the queue
// from the other insert point.
void evalChain(kj::WaitScope& waitScope, uint64_t ops, size_t k) {
  uint64_t ran = 0;
  double run = 0;
  while (ran < ops) {
    kj::Promise<void> p = kj::READY_NOW;
    for (size_t i = 0; i < k; i++) p = p.then([]() { return kj::evalLater([]() {}); });
    double t0 = now();
    p.wait(waitScope);
    double t1 = now();
    run += t1 - t0;
    ran += k;
  }
  report("eval-run", ran, run);
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t ops = argc > 1 ? strtoull(argv[1], nullptr, 10) : 20000000;
  size_t k = argc > 2 ? strtoull(argv[2], nullptr, 10) : 1024;

  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  printf("%-14s %12s %9s\n", "case", "ops", "ns/op");
  rearmDepth(loop, ops);
  rearmBreadth(loop, ops, k);
  armDrain(loop, ops, k);
  armDisarm(loop, ops, k);
  thenChain(waitScope, ops / 10, k);
  evalChain(waitScope, ops / 10, k);

  // What the layout costs per event and per armed event. The linkage figure is measured rather
  // than declared: a million events are armed at once and the resident set is read before and
  // after, which catches linkage the event does not carry inline.
  size_t armed = 1000000;
  auto events = kj::heapArray<kj::Own<CountEvent>>(armed);
  for (auto& e: events) e = kj::heap<CountEvent>(CountEvent::ONCE);
  long before = rssKb();
  for (auto& e: events) e->armBreadthFirst();
  long after = rssKb();
  for (auto& e: events) e->disarm();
  printf("sizes          event=%zu bytes  armed=%zu  rss_delta_kb=%ld  bytes_per_armed=%.1f\n",
      sizeof(CountEvent), armed, after - before, (after - before) * 1024.0 / armed);
  return 0;
}
