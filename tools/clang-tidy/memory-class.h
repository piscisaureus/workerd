// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"

namespace workerd::clang_tidy {

// Enforces the guest-kernel memory classification described in
// src/workerd/experimental/guest-kernel/memory-classes.md on classes that
// carry one of the WD_MEMORY_CLASS_{HOST,ISOLATE,BOUNDARY} annotations from
// src/workerd/util/memory-class.h. Unannotated classes are never reported, so
// the classification is adopted one class at a time.
//
// For an annotated class, every non-static data member and every base class
// is checked, with typedefs, cv-qualifiers and references stripped and
// template specializations identified by the template's qualified name:
//
//   Host (H): a member must not be an Isolate class, a pointer or reference to
//   one, or one of the isolate-side handle types (v8::Global,
//   v8::TracedReference, jsg::Data, jsg::V8Ref, jsg::Value, jsg::JsRef,
//   jsg::Ref, jsg::Promise, jsg::Function, jsg::JsContext).
//
//   Isolate (I): a member must not be a Host class, a pointer or reference to
//   one, or one of the host-side primitives (kj::Promise, kj::ForkedPromise,
//   kj::TaskSet, kj::PromiseFulfiller, kj::Canceler, kj::MutexGuarded,
//   kj::Executor).
//
//   For both, kj::Own, kj::Arc, kj::Rc, kj::Maybe, kj::Array, kj::Vector and
//   kj::OneOf are looked through: a kj::Own<HostClass> in an Isolate class is
//   the disposer that must not live in the arena.
//
//   Boundary (B): a member must be an integer, enum, bool or floating-point
//   value, an array of those, or another Boundary class; nothing with a
//   pointer, a reference, a vtable or a non-trivial destructor. A Boundary
//   class is itself not polymorphic and derives only from Boundary classes.
//
// Class templates are checked on the template where the member type is
// already decided (kj::Promise<T> is host-side for every T) and on each
// instantiation where it is not (a member of type T).
//
// Static data members live in process-global memory whatever the class is
// annotated, and are not checked.
class MemoryClassCheck: public clang::tidy::ClangTidyCheck {
 public:
  MemoryClassCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;

 private:
  void checkRecord(const clang::CXXRecordDecl* record, const clang::ASTContext& ctx);
};

}  // namespace workerd::clang_tidy
