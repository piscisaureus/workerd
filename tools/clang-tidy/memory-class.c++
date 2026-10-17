// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "memory-class.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>

namespace workerd::clang_tidy {

namespace {

enum class MemoryClass { NONE, HOST, ISOLATE, BOUNDARY };

llvm::StringRef className(MemoryClass cls) {
  switch (cls) {
    case MemoryClass::HOST:
      return "host";
    case MemoryClass::ISOLATE:
      return "isolate";
    case MemoryClass::BOUNDARY:
      return "boundary";
    case MemoryClass::NONE:
      break;
  }
  return "";
}

MemoryClass opposite(MemoryClass cls) {
  switch (cls) {
    case MemoryClass::HOST:
      return MemoryClass::ISOLATE;
    case MemoryClass::ISOLATE:
      return MemoryClass::HOST;
    default:
      return MemoryClass::NONE;
  }
}

// The standing rule a diagnostic ends with.
llvm::StringRef explanation(MemoryClass cls) {
  switch (cls) {
    case MemoryClass::HOST:
      return "a host object names isolate state only by handle";
    case MemoryClass::ISOLATE:
      return "an isolate object reaches host state only through a handle or a message";
    case MemoryClass::BOUNDARY:
      return "a boundary record carries only integers, enums, bools, floats, arrays of those "
             "and other boundary records";
    case MemoryClass::NONE:
      break;
  }
  return "";
}

// Anchored suffix match: true iff `qualifiedName` equals `suffix` or is of the
// form `<prefix>::<suffix>`, so `jsg::Ref` does not match `jsg::RefCount` or
// `foo::jsg::Refrigerator`. The same idiom as jsg-visit-for-gc.
bool endsWithQualified(llvm::StringRef qualifiedName, llvm::StringRef suffix) {
  if (qualifiedName == suffix) return true;
  if (qualifiedName.size() < suffix.size() + 2) return false;
  if (!qualifiedName.ends_with(suffix)) return false;
  auto sep = qualifiedName.size() - suffix.size();
  return qualifiedName[sep - 1] == ':' && qualifiedName[sep - 2] == ':';
}

bool isAnyOf(llvm::StringRef qualifiedName, llvm::ArrayRef<llvm::StringRef> suffixes) {
  for (auto suffix: suffixes) {
    if (endsWithQualified(qualifiedName, suffix)) return true;
  }
  return false;
}

// Types that bottom out in a V8 handle or a jsg::Wrappable, and so can only
// live in the isolate's arena (memory-classes.md, rule R2).
const llvm::StringRef kIsolateSideTypes[] = {
  "v8::Global",
  "v8::TracedReference",
  "jsg::Data",
  "jsg::V8Ref",
  "jsg::Value",
  "jsg::JsRef",
  "jsg::Ref",
  "jsg::Promise",
  "jsg::Function",
  "jsg::JsContext",
};

// Event-loop and cross-thread primitives that the host drives, and so can only
// live outside every arena (memory-classes.md, rule R3).
const llvm::StringRef kHostSideTypes[] = {
  "kj::Promise",
  "kj::ForkedPromise",
  "kj::TaskSet",
  "kj::PromiseFulfiller",
  "kj::Canceler",
  "kj::MutexGuarded",
  "kj::Executor",
};

// Value types that rule R6 names as side-neutral: a thin wrapper over an
// integer, with no pointer and a trivial destructor, so it carries nothing
// into a boundary record that a plain integer would not. The structural checks
// above still apply, so one of these growing a vtable would be reported.
const llvm::StringRef kBoundaryValueTypes[] = {
  "kj::Date",
  "kj::Duration",
};

// Wrappers whose side is decided by what they hold. The owning ones carry a
// disposer or refcount that lives with the pointee, which is exactly what
// must not cross; the rest are plain containers.
const llvm::StringRef kOwningWrappers[] = {
  "kj::Own",
  "kj::Arc",
  "kj::Rc",
};
const llvm::StringRef kContainerWrappers[] = {
  "kj::Maybe",
  "kj::Array",
  "kj::Vector",
  "kj::OneOf",
};

constexpr llvm::StringRef kHostAnnotation = "workerd_memory_class_host";
constexpr llvm::StringRef kIsolateAnnotation = "workerd_memory_class_isolate";
constexpr llvm::StringRef kBoundaryAnnotation = "workerd_memory_class_boundary";

MemoryClass annotationOnDecl(const clang::Decl* decl) {
  for (const auto* attr: decl->specific_attrs<clang::AnnotateAttr>()) {
    auto annotation = attr->getAnnotation();
    if (annotation == kHostAnnotation) return MemoryClass::HOST;
    if (annotation == kIsolateAnnotation) return MemoryClass::ISOLATE;
    if (annotation == kBoundaryAnnotation) return MemoryClass::BOUNDARY;
  }
  return MemoryClass::NONE;
}

// The memory class `record` is annotated with, looking at every redeclaration
// and, for a template instantiation, at the template it was instantiated from.
MemoryClass annotationOf(const clang::CXXRecordDecl* record) {
  if (record == nullptr) return MemoryClass::NONE;
  for (const auto* redecl: record->redecls()) {
    auto cls = annotationOnDecl(redecl);
    if (cls != MemoryClass::NONE) return cls;
  }
  if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
    return annotationOf(spec->getSpecializedTemplate()->getTemplatedDecl());
  }
  if (const auto* pattern = record->getInstantiatedFromMemberClass()) {
    return annotationOf(pattern);
  }
  return MemoryClass::NONE;
}

// Strips references, cv-qualifiers and typedefs.
clang::QualType stripped(clang::QualType qt) {
  return qt.getNonReferenceType().getCanonicalType().getUnqualifiedType();
}

std::string spelled(clang::QualType qt, const clang::ASTContext& ctx) {
  return qt.getAsString(ctx.getPrintingPolicy());
}

// The template a type is a specialization of (by qualified name, e.g.
// "workerd::jsg::Ref"), or the qualified name of the class type itself, plus
// its type template arguments. The name is empty for anything that is not a
// class or class-template type, including a bare template parameter.
struct ClassTypeName {
  std::string name;
  llvm::SmallVector<clang::QualType, 4> args;
};

void collectTypeArgs(
    llvm::ArrayRef<clang::TemplateArgument> args, llvm::SmallVectorImpl<clang::QualType>& out) {
  for (const auto& arg: args) {
    if (arg.getKind() == clang::TemplateArgument::Type) out.push_back(arg.getAsType());
  }
}

ClassTypeName classTypeName(clang::QualType qt) {
  ClassTypeName result;
  // The arguments as written, when the type is spelled as a specialization of
  // the class template itself (not of an alias template), so that diagnostics
  // print them the way the source does.
  const auto* written = qt.getNonReferenceType()->getAs<clang::TemplateSpecializationType>();
  if (written != nullptr && written->isTypeAlias()) written = nullptr;

  auto bare = stripped(qt);
  if (const auto* rd = bare->getAsCXXRecordDecl()) {
    if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(rd)) {
      result.name = spec->getSpecializedTemplate()->getQualifiedNameAsString();
      if (written != nullptr) {
        collectTypeArgs(written->template_arguments(), result.args);
      } else {
        collectTypeArgs(spec->getTemplateArgs().asArray(), result.args);
      }
    } else {
      result.name = rd->getQualifiedNameAsString();
    }
    return result;
  }
  // A dependent specialization, such as kj::Promise<T> inside a class
  // template: the template is known even though the arguments are not.
  if (const auto* tst = bare->getAs<clang::TemplateSpecializationType>()) {
    if (const auto* td = tst->getTemplateName().getAsTemplateDecl()) {
      result.name = td->getQualifiedNameAsString();
      collectTypeArgs(tst->template_arguments(), result.args);
    }
  }
  return result;
}

// Something found on the wrong side of the boundary. `noun` names it ("isolate
// class 'Foo'", "a pointer to host class 'Bar'", "host-side type
// 'kj::Promise<void>'"); `wrapper` is the outermost type it was found inside,
// if it was not the member type itself; `predicate` is a complete clause used
// for boundary violations, whose shape differs ("is a pointer ('int*')").
struct Finding {
  std::string noun;
  std::string wrapper;
  bool owned = false;
  std::string predicate;
};

// Looks for the other side's state inside `qt`, for a class of class `cls`.
// Returns nullopt when there is none, or when the type is dependent and
// cannot be decided until instantiation, in which case `undecided` is set.
std::optional<Finding> findCrossing(
    clang::QualType qt, MemoryClass cls, const clang::ASTContext& ctx, bool& undecided) {
  auto other = opposite(cls);
  std::string otherName = className(other).str();

  // Pointers and references to a class of the other side. A pointee that is
  // still a template parameter is decided on each instantiation.
  bool isReference = qt->isReferenceType();
  auto bare = stripped(qt);
  if (isReference || bare->isAnyPointerType()) {
    auto pointee = stripped(isReference ? bare : bare->getPointeeType());
    if (const auto* rd = pointee->getAsCXXRecordDecl()) {
      if (annotationOf(rd) == other) {
        return Finding{.noun = std::string(isReference ? "a reference to " : "a pointer to ") +
              otherName + " class '" + rd->getQualifiedNameAsString() + "'"};
      }
    } else if (pointee->isDependentType()) {
      undecided = true;
    }
    return std::nullopt;
  }

  // An array: the element decides.
  const clang::Type* base = bare->getBaseElementTypeUnsafe();
  if (base != bare.getTypePtr()) {
    auto inner = findCrossing(clang::QualType(base, 0), cls, ctx, undecided);
    if (!inner) return std::nullopt;
    inner->wrapper = spelled(qt, ctx);
    return inner;
  }

  if (const auto* rd = bare->getAsCXXRecordDecl()) {
    if (annotationOf(rd) == other) {
      std::string noun = otherName + " class '" + rd->getQualifiedNameAsString() + "'";
      return Finding{.noun = noun, .predicate = "embeds " + noun};
    }
  }

  auto typeName = classTypeName(qt);
  if (typeName.name.empty()) {
    if (bare->isDependentType()) undecided = true;
    return std::nullopt;
  }

  auto sideTypes = other == MemoryClass::ISOLATE
      ? llvm::ArrayRef<llvm::StringRef>(kIsolateSideTypes)
      : llvm::ArrayRef<llvm::StringRef>(kHostSideTypes);
  if (isAnyOf(typeName.name, sideTypes)) {
    return Finding{.noun = otherName + "-side type '" + spelled(qt, ctx) + "'"};
  }

  bool owning = isAnyOf(typeName.name, kOwningWrappers);
  if (owning || isAnyOf(typeName.name, kContainerWrappers)) {
    for (auto arg: typeName.args) {
      auto inner = findCrossing(arg, cls, ctx, undecided);
      if (!inner) continue;
      inner->wrapper = spelled(qt, ctx);
      inner->owned = inner->owned || owning;
      return inner;
    }
  }
  return std::nullopt;
}

// Checks a Boundary member type: a plain value, an array of plain values, or
// another Boundary record, with nothing a pointer, vtable or disposer could
// hide in. Returns the reason it fails, or nullopt if it passes or is
// dependent, in which case `undecided` is set.
std::optional<Finding> findBoundaryViolation(
    clang::QualType qt, const clang::ASTContext& ctx, bool& undecided) {
  if (qt->isReferenceType()) {
    return Finding{.predicate = "is a reference ('" + spelled(qt, ctx) + "')"};
  }
  auto bare = stripped(qt);
  const clang::Type* base = bare->getBaseElementTypeUnsafe();
  bool isArray = base != bare.getTypePtr();
  clang::QualType element(base, 0);
  std::string prefix = isArray ? "is an array of '" + spelled(element, ctx) + "', which " : "";

  if (element->isAnyPointerType() || element->isMemberPointerType()) {
    return Finding{
      .predicate = isArray ? prefix + "is a pointer" : "is a pointer ('" + spelled(qt, ctx) + "')"};
  }
  if (element->isDependentType()) {
    undecided = true;
    return std::nullopt;
  }
  if (element->isIntegralOrEnumerationType() || element->isFloatingType()) {
    return std::nullopt;
  }
  if (const auto* rd = element->getAsCXXRecordDecl()) {
    if (annotationOf(rd) == MemoryClass::BOUNDARY) return std::nullopt;
    std::string name = "'" + spelled(element, ctx) + "'";
    const auto* def = rd->getDefinition();
    if (def == nullptr) {
      return Finding{.predicate = prefix + "has type " + name + ", which is not a boundary class"};
    }
    if (def->isPolymorphic() || def->getNumVBases() != 0) {
      return Finding{.predicate = prefix + "has type " + name + ", which has a vtable"};
    }
    if (!def->hasTrivialDestructor()) {
      return Finding{
        .predicate = prefix + "has type " + name + ", which has a non-trivial destructor"};
    }
    std::string qualified = rd->getQualifiedNameAsString();
    for (llvm::StringRef allowed: kBoundaryValueTypes) {
      if (qualified == allowed) return std::nullopt;
    }
    return Finding{.predicate = prefix + "has type " + name + ", which is not a boundary class"};
  }
  return Finding{.predicate = prefix + "has type '" + spelled(element, ctx) +
        "', which is not an integer, enum, bool, floating-point value or boundary class"};
}

std::optional<Finding> findViolation(
    clang::QualType qt, MemoryClass cls, const clang::ASTContext& ctx, bool& undecided) {
  if (cls == MemoryClass::BOUNDARY) return findBoundaryViolation(qt, ctx, undecided);
  return findCrossing(qt, cls, ctx, undecided);
}

// Whether a member type is settled by the template pattern alone, so that
// instantiations need not look at it again.
bool decidedInPattern(clang::QualType qt, MemoryClass cls, const clang::ASTContext& ctx) {
  bool undecided = false;
  findViolation(qt, cls, ctx, undecided);
  return !undecided;
}

// The clause after "field 'x' of host class 'Foo' ".
std::string memberClause(const Finding& finding) {
  if (!finding.wrapper.empty()) {
    return "has type '" + finding.wrapper + "', which " + (finding.owned ? "owns " : "holds ") +
        finding.noun;
  }
  if (!finding.predicate.empty()) return finding.predicate;
  if (finding.noun.starts_with("a pointer") || finding.noun.starts_with("a reference")) {
    return "is " + finding.noun;
  }
  return "has " + finding.noun;
}

// The non-static data members of `record`, with the members of unnamed
// structs and unions in place of the unnamed member itself: they are part of
// the enclosing object, and the classification applies to each of them.
void collectFields(
    const clang::CXXRecordDecl* record, llvm::SmallVectorImpl<const clang::FieldDecl*>& out) {
  for (const auto* field: record->fields()) {
    if (field->isAnonymousStructOrUnion()) {
      if (const auto* inner = stripped(field->getType())->getAsCXXRecordDecl()) {
        if (const auto* def = inner->getDefinition()) {
          collectFields(def, out);
          continue;
        }
      }
    }
    out.push_back(field);
  }
}

}  // namespace

void MemoryClassCheck::registerMatchers(clang::ast_matchers::MatchFinder* Finder) {
  using namespace clang::ast_matchers;
  Finder->addMatcher(cxxRecordDecl(isDefinition(), unless(isImplicit())).bind("record"), this);
}

void MemoryClassCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& Result) {
  const auto* record = Result.Nodes.getNodeAs<clang::CXXRecordDecl>("record");
  if (record == nullptr || Result.Context == nullptr) return;
  if (annotationOf(record) == MemoryClass::NONE) return;
  checkRecord(record, *Result.Context);
}

void MemoryClassCheck::checkRecord(
    const clang::CXXRecordDecl* record, const clang::ASTContext& ctx) {
  auto cls = annotationOf(record);
  auto clsName = className(cls);
  auto why = explanation(cls);
  std::string recordName = record->getQualifiedNameAsString();

  // An instantiation re-examines only what its pattern could not decide, so a
  // member that is wrong for every argument is reported once, on the template.
  const clang::CXXRecordDecl* pattern = record->getTemplateInstantiationPattern();
  if (pattern != nullptr) {
    pattern = pattern->getDefinition();
    if (pattern == nullptr) return;
    recordName = spelled(ctx.getCanonicalTagType(record), ctx);
  }

  if (cls == MemoryClass::BOUNDARY && pattern == nullptr &&
      (record->isPolymorphic() || record->getNumVBases() != 0)) {
    diag(record->getLocation(), "boundary class '%0' has a vtable; %1") << recordName << why;
  }

  // Base classes carry their members into this object.
  auto patternBase = pattern != nullptr ? pattern->bases_begin() : nullptr;
  for (const auto& base: record->bases()) {
    if (pattern != nullptr && patternBase != pattern->bases_end() &&
        decidedInPattern((patternBase++)->getType(), cls, ctx)) {
      continue;
    }
    std::optional<std::string> clause;
    if (cls == MemoryClass::BOUNDARY) {
      const auto* rd = stripped(base.getType())->getAsCXXRecordDecl();
      if (rd != nullptr && annotationOf(rd) != MemoryClass::BOUNDARY) {
        clause =
            "derives from '" + rd->getQualifiedNameAsString() + "', which is not a boundary class";
      }
    } else {
      bool undecided = false;
      if (auto finding = findCrossing(base.getType(), cls, ctx, undecided)) {
        clause = "derives from " + finding->noun;
      }
    }
    if (clause) {
      diag(base.getBeginLoc(), "%0 class '%1' %2; %3") << clsName << recordName << *clause << why;
    }
  }

  llvm::SmallVector<const clang::FieldDecl*, 16> fields;
  collectFields(record, fields);
  llvm::SmallVector<const clang::FieldDecl*, 16> patternFields;
  if (pattern != nullptr) collectFields(pattern, patternFields);
  for (size_t i = 0; i < fields.size(); i++) {
    const auto* field = fields[i];
    if (pattern != nullptr && i < patternFields.size() &&
        decidedInPattern(patternFields[i]->getType(), cls, ctx)) {
      continue;
    }
    bool undecided = false;
    if (auto finding = findViolation(field->getType(), cls, ctx, undecided)) {
      diag(field->getLocation(), "field '%0' of %1 class '%2' %3; %4")
          << field->getName() << clsName << recordName << memberClause(*finding) << why;
    }
  }
}

}  // namespace workerd::clang_tidy
