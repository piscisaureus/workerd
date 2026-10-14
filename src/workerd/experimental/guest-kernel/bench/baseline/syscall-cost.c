// Measures the round-trip cost of a trivial syscall and of a pkey_set pair,
// in nanoseconds and TSC cycles, on the current machine.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <x86intrin.h>

static double now(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void) {
  const int n = 2000000;
  for (int i = 0; i < 100000; i++) syscall(SYS_getppid);
  double t0 = now(); uint64_t c0 = __rdtsc();
  for (int i = 0; i < n; i++) syscall(SYS_getppid);
  uint64_t c1 = __rdtsc(); double t1 = now();
  printf("getppid syscall: %.1f ns, %.0f cycles\n", (t1 - t0) / n * 1e9, (double)(c1 - c0) / n);

  t0 = now(); c0 = __rdtsc();
  for (int i = 0; i < n; i++) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); }
  c1 = __rdtsc(); t1 = now();
  printf("clock_gettime (vDSO): %.1f ns, %.0f cycles\n", (t1 - t0) / n * 1e9, (double)(c1 - c0) / n);

  int key = pkey_alloc(0, 0);
  if (key < 0) { perror("pkey_alloc"); return 0; }
  t0 = now(); c0 = __rdtsc();
  for (int i = 0; i < n; i++) { pkey_set(key, PKEY_DISABLE_ACCESS); pkey_set(key, 0); }
  c1 = __rdtsc(); t1 = now();
  printf("pkey_set pair (WRPKRU x2): %.1f ns, %.0f cycles\n", (t1 - t0) / n * 1e9, (double)(c1 - c0) / n);
  return 0;
}
