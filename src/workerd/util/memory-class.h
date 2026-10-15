// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Guest-kernel memory classes for runtime C++ objects. The classes, the rules that decide
// which one a class belongs to, and the inventory of the codebase against them are in
// src/workerd/experimental/guest-kernel/memory-classes.md.
//
// Each macro marks a class as living on one side of the isolation boundary:
//
//   WD_MEMORY_CLASS_ISOLATE   Isolate (I): lives in the isolate's private arena; the host never
//                             reads or writes it.
//   WD_MEMORY_CLASS_HOST      Host (H): lives outside every arena; unreachable from guest ring 3.
//   WD_MEMORY_CLASS_BOUNDARY  Boundary (B): the one region both sides touch; holds offsets,
//                             lengths and handles only, never pointers or disposers.
//
// The annotation goes between the class-key and the class name, which is where a C++ attribute
// appertains to the type itself:
//
//   class WD_MEMORY_CLASS_HOST V8System { ... };
//   struct WD_MEMORY_CLASS_BOUNDARY GuestTurnOutcome { ... };
//
// It compiles to nothing. The clang-tidy check `workerd-memory-class` (tools/clang-tidy/
// memory-class.h) reads it and rejects data members and base classes that put state on the wrong
// side: isolate-side handles (jsg::Ref, v8::Global, ...) in a Host class, host-side primitives
// (kj::Promise, kj::TaskSet, ...) or ownership of a Host class in an Isolate class, and anything
// that is not a plain value in a Boundary class. Unannotated classes are not checked, so classes
// are converted one at a time. A class that is genuinely used from both sides has no class until
// it is split, and must not be annotated.
#ifdef __clang__
#define WD_MEMORY_CLASS_ISOLATE __attribute__((annotate("workerd_memory_class_isolate")))
#define WD_MEMORY_CLASS_HOST __attribute__((annotate("workerd_memory_class_host")))
#define WD_MEMORY_CLASS_BOUNDARY __attribute__((annotate("workerd_memory_class_boundary")))
#else
#define WD_MEMORY_CLASS_ISOLATE
#define WD_MEMORY_CLASS_HOST
#define WD_MEMORY_CLASS_BOUNDARY
#endif
