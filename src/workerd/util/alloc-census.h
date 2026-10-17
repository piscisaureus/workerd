// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Measurement-only allocation census for the guest-kernel placement work. Off unless the
// binary is built with --//src/workerd/util:alloc_census=True, which also defines
// WD_ALLOC_CENSUS and adds the --wrap link options the implementation needs. Never in a
// shipping build:
//
//   bazel build -c opt --platform_suffix=_census \
//     --//src/workerd/util:alloc_census=True \
//     --//src/workerd/server:use_tcmalloc=False \
//     --copt=-g1 --copt=-fno-omit-frame-pointer --strip=never \
//     //src/workerd/server:workerd
//
// The census writes to WD_ALLOC_CENSUS_OUT (default /tmp/alloc-census.txt) at exit and on
// SIGUSR2, so a running server can be sampled; alloc-census-report.py symbolizes the result.
//
// It records, for every C++ and C heap allocation, the call site (the ten return addresses
// above the interposer), the isolate whose lock was held when the block was allocated, and the
// isolate whose lock was held when it was freed. That is what decides whether an allocation
// site may be placed in one isolate's arena: a site whose blocks are allocated under one
// isolate's lock and freed under another's cannot be, because the free runs the destructor over
// memory the other isolate's page-table root does not map.
//
// The isolate, not the arena, is the identity, so the census runs with or without the guest
// kernel; jsg::GuestArenaScope, which brackets the window in which the isolate's arena is the
// thread's active one, is where it is taken.

namespace workerd::allocCensus {

#ifdef WD_ALLOC_CENSUS
// Called by jsg::GuestArenaScope, which is entered for the length of an isolate lock.
void enterIsolate(const void* isolate);
void leaveIsolate();
// Called by jsg::_::runTrampolineInGuest around the guest entry.
void enterTurn();
void leaveTurn();
// Writes the census to the path in WD_ALLOC_CENSUS_OUT (default /tmp/alloc-census.txt).
// Also wired to SIGUSR2 and to atexit.
void dump(const char* why);
#else
inline void enterIsolate(const void*) {}
inline void leaveIsolate() {}
inline void enterTurn() {}
inline void leaveTurn() {}
inline void dump(const char*) {}
#endif

}  // namespace workerd::allocCensus
