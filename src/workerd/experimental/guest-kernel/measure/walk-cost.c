// The cost of one page walk inside the guest against one on the host.
//
// A pointer chase visits one word per 4 KiB page of a buffer far larger than
// the L2 TLB covers, in a random cyclic order, so every access misses the TLB
// and walks; the page-table entries themselves (8 bytes per page) stay cached,
// so what is timed is the walk, not memory. The same chase is run on the host
// and inside gk_run, where the guest's own 4 KiB walk is resolved through the
// host's nested tables at every level, and again on a buffer the host backs
// with 2 MiB pages (MADV_HUGEPAGE), which shortens only the nested side: the
// guest's PTEs stay 4 KiB. The difference between the host and guest figures
// is what a guest page walk costs over a native one.
//
// Build beside the library: cc -O2 -fno-stack-protector -fcf-protection=none
//   -pthread -o walk-cost walk-cost.c gk.c gk_asm.S
#include "gk.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <x86intrin.h>

#define PAGES 32768L   // 128 MiB at a 4 KiB stride
#define ROUNDS 4

typedef struct { uint64_t *start; long n; uint64_t ticks; } chase;

static long run_chase(void *arg) {
  chase *c = arg;
  uint64_t *p = c->start;
  for (long i = 0; i < c->n; i++) p = (uint64_t *)*p;  // warm: faults and caches
  uint64_t t0 = __rdtsc();
  for (long i = 0; i < c->n; i++) p = (uint64_t *)*p;
  c->ticks = __rdtsc() - t0;
  return (long)(p != NULL);
}

static uint64_t *make_buffer(int huge) {
  size_t bytes = PAGES * 4096;
  uint8_t *raw = mmap(NULL, bytes + (2 << 20), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) { perror("mmap"); exit(1); }
  uint8_t *base = (uint8_t *)(((uintptr_t)raw + (2 << 20) - 1) & ~((uintptr_t)(2 << 20) - 1));
  if (huge && madvise(base, bytes, MADV_HUGEPAGE) != 0) perror("madvise");
  // One slot per page at a page-private offset, linked in a random cycle.
  long *order = malloc(PAGES * sizeof *order);
  for (long i = 0; i < PAGES; i++) order[i] = i;
  uint32_t seed = 0x9e3779b9u;
  for (long i = PAGES - 1; i > 0; i--) {  // Sattolo: a single cycle
    seed = seed * 1103515245u + 12345u;
    long j = (seed >> 8) % i;
    long t = order[i]; order[i] = order[j]; order[j] = t;
  }
  uint64_t **slot = malloc(PAGES * sizeof *slot);
  for (long i = 0; i < PAGES; i++) {
    seed = seed * 1103515245u + 12345u;
    slot[i] = (uint64_t *)(base + i * 4096 + ((seed >> 8) % 64) * 64);
  }
  for (long i = 0; i < PAGES; i++) *slot[order[i]] = (uint64_t)slot[order[(i + 1) % PAGES]];
  uint64_t *start = slot[order[0]];
  free(order); free(slot);
  return start;
}

static double tsc_ghz(void) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  uint64_t t0 = __rdtsc();
  do clock_gettime(CLOCK_MONOTONIC, &b); while ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec) < 1e8);
  uint64_t t1 = __rdtsc();
  return (t1 - t0) / ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec));
}

static void report(const char *what, chase *c, double ghz) {
  printf("%-34s %7.1f ns per access  (%6.0f TSC ticks)\n", what, c->ticks / (double)c->n / ghz, c->ticks / (double)c->n);
}

int main(void) {
  double ghz = tsc_ghz();
  printf("TSC %.2f GHz; %ld pages at a 4 KiB stride, %ld accesses timed\n", ghz, PAGES, PAGES * ROUNDS);
  chase small = {make_buffer(0), PAGES * ROUNDS, 0};
  chase huge = {make_buffer(1), PAGES * ROUNDS, 0};
  run_chase(&small); report("host, 4 KiB host pages", &small, ghz);
  run_chase(&huge); report("host, 2 MiB host pages", &huge, ghz);
  if (gk_init() < 0) { fprintf(stderr, "gk_init: %s\n", gk_last_error()); return 1; }
  for (int r = 0; r < 2; r++) {  // the second run has every page mapped in the guest already
    if (gk_run(run_chase, &small) < 0) { fprintf(stderr, "gk_run: %s\n", gk_last_error()); return 1; }
    if (gk_run(run_chase, &huge) < 0) { fprintf(stderr, "gk_run: %s\n", gk_last_error()); return 1; }
  }
  report("guest, 4 KiB guest and host pages", &small, ghz);
  report("guest, 4 KiB guest, 2 MiB host pages", &huge, ghz);
  return 0;
}
