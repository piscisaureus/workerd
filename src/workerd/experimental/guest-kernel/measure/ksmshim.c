// Marks the whole process mergeable so that KSM deduplicates its anonymous
// pages against the other processes carrying this shim.
//
// This stands in for the sharing a pre-forked process-per-tenant design would
// get from copy-on-write, without building one: KSM merges every page two
// tenants happen to hold identically, which is a superset of the pages that
// would still be shared after a fork, so the memory it leaves is a floor on
// what such a design could reach rather than an estimate of it.
//
// cc -O2 -shared -fPIC -o ksmshim.so ksmshim.c

#include <stdio.h>
#include <sys/prctl.h>

#ifndef PR_SET_MEMORY_MERGE
#define PR_SET_MEMORY_MERGE 67
#endif

__attribute__((constructor)) static void enable_merge(void) {
  if (prctl(PR_SET_MEMORY_MERGE, 1, 0, 0, 0) != 0) {
    fprintf(stderr, "ksmshim: PR_SET_MEMORY_MERGE failed\n");
  }
}
