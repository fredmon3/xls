// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/frontend/ast_utils.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/variant.h"
#include "xls/common/casts.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/token_utils.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_system/type_info.h"

namespace xls::dslx {
namespace {

// Has to be an enum or builtin-type name, given the context we're in: looking
// for _values_ hanging off, e.g. in service of a `::` ref.
absl::StatusOr<std::variant<EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*>>
ResolveTypeAliasToDirectColonRefSubject(ImportData* import_data,
                                        const TypeInfo* type_info,
                                        TypeAlias* type_def) {
  XLS_VLOG(5) << "ResolveTypeDefToDirectColonRefSubject; type_def: `"
              << type_def->ToString() << "`";

  TypeDefinition td = type_def;
  while (std::holds_alternative<TypeAlias*>(td)) {
    TypeAlias* type_def = std::get<TypeAlias*>(td);
    XLS_VLOG(5) << "TypeAlias: `" << type_def->ToString() << "`";
    TypeAnnotation* type = type_def->type_annotation();
    XLS_VLOG(5) << "TypeAnnotation: `" << type->ToString() << "`";

    if (auto* bti = dynamic_cast<BuiltinTypeAnnotation*>(type);
        bti != nullptr) {
      return bti->builtin_name_def();
    }
    if (auto* ata = dynamic_cast<ArrayTypeAnnotation*>(type); ata != nullptr) {
      return ata;
    }

    TypeRefTypeAnnotation* type_ref_type =
        dynamic_cast<TypeRefTypeAnnotation*>(type);
    // TODO(rspringer): We'll need to collect parametrics from type_ref_type to
    // support parametric TypeDefs.
    XLS_RET_CHECK(type_ref_type != nullptr)
        << type->ToString() << " :: " << type->GetNodeTypeName();
    XLS_VLOG(5) << "TypeRefTypeAnnotation: `" << type_ref_type->ToString()
                << "`";

    td = type_ref_type->type_ref()->type_definition();
  }

  if (std::holds_alternative<ColonRef*>(td)) {
    ColonRef* colon_ref = std::get<ColonRef*>(td);
    XLS_ASSIGN_OR_RETURN(auto subject, ResolveColonRefSubjectForTypeChecking(
                                           import_data, type_info, colon_ref));
    XLS_RET_CHECK(std::holds_alternative<Module*>(subject));
    Module* module = std::get<Module*>(subject);
    XLS_ASSIGN_OR_RETURN(td, module->GetTypeDefinition(colon_ref->attr()));

    if (std::holds_alternative<TypeAlias*>(td)) {
      // We need to get the right type info for the enum's containing module. We
      // can get the top-level module since [currently?] enums can't be
      // parameterized.
      type_info = import_data->GetRootTypeInfo(module).value();
      return ResolveTypeAliasToDirectColonRefSubject(import_data, type_info,
                                                     std::get<TypeAlias*>(td));
    }
  }

  if (!std::holds_alternative<EnumDef*>(td)) {
    return absl::InternalError(
        "ResolveTypeDefToDirectColonRefSubject() can only be called when the "
        "TypeAlias "
        "directory or indirectly refers to an EnumDef.");
  }

  return std::get<EnumDef*>(td);
}

void FlattenToSetInternal(const AstNode* node,
                          absl::flat_hash_set<const AstNode*>* the_set) {
  the_set->insert(node);
  for (const AstNode* child : node->GetChildren(/*want_types=*/true)) {
    FlattenToSetInternal(child, the_set);
  }
}

}  // namespace

bool IsParametricFunction(const AstNode* n) {
  // Convenience so we can check things like "definer" when the definer may be
  // unspecified in the AST. This generally only happens with programmatically
  // built ASTs.
  if (n == nullptr) {
    return false;
  }

  const auto* f = dynamic_cast<const Function*>(n);
  return f != nullptr && f->IsParametric();
}

bool ParentIsInvocationWithCallee(const NameRef* n) {
  XLS_CHECK(n != nullptr);
  const AstNode* parent = n->parent();
  XLS_CHECK(parent != nullptr);
  const auto* invocation = dynamic_cast<const Invocation*>(parent);
  return invocation != nullptr && invocation->callee() == n;
}

bool IsBuiltinFn(Expr* callee, std::optional<std::string_view> target) {
  NameRef* name_ref = dynamic_cast<NameRef*>(callee);
  if (name_ref == nullptr) {
    return false;
  }

  if (!std::holds_alternative<BuiltinNameDef*>(name_ref->name_def())) {
    return false;
  }

  if (target.has_value()) {
    auto* bnd = std::get<BuiltinNameDef*>(name_ref->name_def());
    return bnd->identifier() == target.value();
  }

  return true;
}

absl::StatusOr<std::string> GetBuiltinName(Expr* callee) {
  if (!IsBuiltinFn(callee)) {
    return absl::InvalidArgumentError("Callee is not a builtin function.");
  }

  NameRef* name_ref = dynamic_cast<NameRef*>(callee);
  return name_ref->identifier();
}

absl::StatusOr<Function*> ResolveFunction(Expr* callee,
                                          const TypeInfo* type_info) {
  if (NameRef* name_ref = dynamic_cast<NameRef*>(callee); name_ref != nullptr) {
    return name_ref->owner()->GetMemberOrError<Function>(
        name_ref->identifier());
  }

  auto* colon_ref = dynamic_cast<ColonRef*>(callee);
  XLS_RET_CHECK_NE(colon_ref, nullptr);
  std::optional<Import*> import = colon_ref->ResolveImportSubject();
  XLS_RET_CHECK(import.has_value())
      << "ColonRef did not refer to an import: " << colon_ref->ToString();
  std::optional<const ImportedInfo*> imported_info =
      type_info->GetImported(*import);
  return imported_info.value()->module->GetMemberOrError<Function>(
      colon_ref->attr());
}

static absl::StatusOr<StructDef*> ResolveLocalStructDef(
    TypeAnnotation* type_annotation, const TypeDefinition& td) {
  auto error = [&](const AstNode* latest) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Could not resolve local struct definition from %s -- "
                        "%s was not a struct definition",
                        ToAstNode(td)->ToString(), latest->ToString()));
  };

  TypeRefTypeAnnotation* type_ref_type_annotation =
      dynamic_cast<TypeRefTypeAnnotation*>(type_annotation);
  if (type_ref_type_annotation == nullptr) {
    return error(type_annotation);
  }

  return ResolveLocalStructDef(
      type_ref_type_annotation->type_ref()->type_definition());
}

absl::StatusOr<StructDef*> ResolveLocalStructDef(TypeDefinition td) {
  auto error = [&](const AstNode* latest) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Could not resolve local struct definition from %s -- "
                        "%s was not a struct definition",
                        ToAstNode(td)->ToString(), latest->ToString()));
  };

  return absl::visit(
      Visitor{
          [&](TypeAlias* n) -> absl::StatusOr<StructDef*> {
            return ResolveLocalStructDef(n->type_annotation(), td);
          },
          [&](StructDef* n) -> absl::StatusOr<StructDef*> { return n; },
          [&](EnumDef* n) -> absl::StatusOr<StructDef*> { return error(n); },
          [&](ColonRef* n) -> absl::StatusOr<StructDef*> { return error(n); },
      },
      td);
}

absl::StatusOr<Proc*> ResolveProc(Expr* callee, const TypeInfo* type_info) {
  if (NameRef* name_ref = dynamic_cast<NameRef*>(callee); name_ref != nullptr) {
    return name_ref->owner()->GetMemberOrError<Proc>(name_ref->identifier());
  }

  auto* colon_ref = dynamic_cast<ColonRef*>(callee);
  XLS_RET_CHECK_NE(colon_ref, nullptr);
  std::optional<Import*> import = colon_ref->ResolveImportSubject();
  XLS_RET_CHECK(import.has_value())
      << "ColonRef did not refer to an import: " << colon_ref->ToString();
  std::optional<const ImportedInfo*> imported_info =
      type_info->GetImported(*import);
  return imported_info.value()->module->GetMemberOrError<Proc>(
      colon_ref->attr());
}

using ColonRefSubjectT =
    std::variant<Module*, EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*,
                 StructDef*, ColonRef*>;

// When a ColonRef's subject is a NameRef, this resolves the entity referred to
// by that ColonRef. In a valid program that can only be a limited set of
// things, which is reflected in the return type provided.
//
// e.g.
//
//    A::B
//    ^
//    \- subject name_ref
//
// Args:
//  name_ref: The subject in the colon ref.
//
// Returns the entity the subject name_ref is referring to.
static absl::StatusOr<ColonRefSubjectT> ResolveColonRefNameRefSubject(
    NameRef* name_ref, ImportData* import_data, const TypeInfo* type_info) {
  XLS_VLOG(5) << "ResolveColonRefNameRefSubject for `" << name_ref->ToString()
              << "`";

  std::variant<const NameDef*, BuiltinNameDef*> any_name_def =
      name_ref->name_def();
  if (std::holds_alternative<BuiltinNameDef*>(any_name_def)) {
    return std::get<BuiltinNameDef*>(any_name_def);
  }

  const NameDef* name_def = std::get<const NameDef*>(any_name_def);
  AstNode* definer = name_def->definer();
  XLS_VLOG(5) << " ResolveColonRefNameRefSubject definer: `"
              << definer->ToString()
              << "` type: " << definer->GetNodeTypeName();

  if (Import* import = dynamic_cast<Import*>(definer); import != nullptr) {
    std::optional<const ImportedInfo*> imported =
        type_info->GetImported(import);
    if (!imported.has_value()) {
      return absl::InternalError(absl::StrCat(
          "Could not find Module for Import: ", import->ToString()));
    }
    return imported.value()->module;
  }

  // If the LHS isn't an Import, then it has to be an EnumDef (possibly via a
  // TypeAlias).
  if (EnumDef* enum_def = dynamic_cast<EnumDef*>(definer);
      enum_def != nullptr) {
    return enum_def;
  }

  TypeAlias* type_alias = dynamic_cast<TypeAlias*>(definer);
  XLS_RET_CHECK(type_alias != nullptr);

  if (type_alias->owner() != type_info->module()) {
    // We need to get the right type info for the enum's containing module. We
    // can get the top-level module since [currently?] enums can't be
    // parameterized (and we know this must be an enum, per the above).
    type_info = import_data->GetRootTypeInfo(type_alias->owner()).value();
  }
  XLS_ASSIGN_OR_RETURN(auto resolved, ResolveTypeAliasToDirectColonRefSubject(
                                          import_data, type_info, type_alias));
  return WidenVariantTo<ColonRefSubjectT>(resolved);
}

absl::StatusOr<ColonRefSubjectT> ResolveColonRefSubjectForTypeChecking(
    ImportData* import_data, const TypeInfo* type_info,
    const ColonRef* colon_ref) {
  XLS_VLOG(5) << "ResolveColonRefSubject for " << colon_ref->ToString();

  if (std::holds_alternative<NameRef*>(colon_ref->subject())) {
    NameRef* name_ref = std::get<NameRef*>(colon_ref->subject());
    return ResolveColonRefNameRefSubject(name_ref, import_data, type_info);
  }

  XLS_RET_CHECK(std::holds_alternative<ColonRef*>(colon_ref->subject()));
  ColonRef* subject = std::get<ColonRef*>(colon_ref->subject());
  XLS_ASSIGN_OR_RETURN(
      auto resolved_subject,
      ResolveColonRefSubjectForTypeChecking(import_data, type_info, subject));
  // Has to be a module, since it's a ColonRef inside a ColonRef.
  XLS_RET_CHECK(std::holds_alternative<Module*>(resolved_subject));
  Module* module = std::get<Module*>(resolved_subject);

  // And the subject has to be a type, namely an enum, since the ColonRef must
  // be of the form: <MODULE>::SOMETHING::SOMETHING_ELSE. Keep in mind, though,
  // that we might have to traverse an EnumDef.
  XLS_ASSIGN_OR_RETURN(TypeDefinition td,
                       module->GetTypeDefinition(subject->attr()));

  using ReturnT = absl::StatusOr<ColonRefSubjectT>;

  return absl::visit(
      Visitor{
          [&](TypeAlias* type_alias) -> ReturnT {
            XLS_ASSIGN_OR_RETURN(auto resolved,
                                 ResolveTypeAliasToDirectColonRefSubject(
                                     import_data, type_info, type_alias));
            return WidenVariantTo<ColonRefSubjectT>(resolved);
          },
          [](StructDef* struct_def) -> ReturnT { return struct_def; },
          [](EnumDef* enum_def) -> ReturnT { return enum_def; },
          [](ColonRef* colon_ref) -> ReturnT { return colon_ref; },
      },
      td);
}

absl::StatusOr<
    std::variant<Module*, EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*>>
ResolveColonRefSubjectAfterTypeChecking(ImportData* import_data,
                                        const TypeInfo* type_info,
                                        const ColonRef* colon_ref) {
  XLS_ASSIGN_OR_RETURN(auto result, ResolveColonRefSubjectForTypeChecking(
                                        import_data, type_info, colon_ref));
  using ReturnT = absl::StatusOr<
      std::variant<Module*, EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*>>;
  return absl::visit(
      Visitor{
          [](Module* x) -> ReturnT { return x; },
          [](EnumDef* x) -> ReturnT { return x; },
          [](BuiltinNameDef* x) -> ReturnT { return x; },
          [](ArrayTypeAnnotation* x) -> ReturnT { return x; },
          [](StructDef*) -> ReturnT {
            return absl::InternalError(
                "After type checking colon-ref subject cannot be a StructDef");
          },
          [](ColonRef*) -> ReturnT {
            return absl::InternalError(
                "After type checking colon-ref subject cannot be a StructDef");
          },
      },
      result);
}

absl::Status VerifyParentage(const Module* module) {
  for (const ModuleMember member : module->top()) {
    if (std::holds_alternative<Function*>(member)) {
      return VerifyParentage(std::get<Function*>(member));
    }
    if (std::holds_alternative<Proc*>(member)) {
      return VerifyParentage(std::get<Proc*>(member));
    }
    if (std::holds_alternative<TestFunction*>(member)) {
      return VerifyParentage(std::get<TestFunction*>(member));
    }
    if (std::holds_alternative<TestProc*>(member)) {
      return VerifyParentage(std::get<TestProc*>(member));
    }
    if (std::holds_alternative<QuickCheck*>(member)) {
      return VerifyParentage(std::get<QuickCheck*>(member));
    }
    if (std::holds_alternative<TypeAlias*>(member)) {
      return VerifyParentage(std::get<TypeAlias*>(member));
    }
    if (std::holds_alternative<StructDef*>(member)) {
      return VerifyParentage(std::get<StructDef*>(member));
    }
    if (std::holds_alternative<ConstantDef*>(member)) {
      return VerifyParentage(std::get<ConstantDef*>(member));
    }
    if (std::holds_alternative<EnumDef*>(member)) {
      return VerifyParentage(std::get<EnumDef*>(member));
    }
    if (std::holds_alternative<Import*>(member)) {
      return VerifyParentage(std::get<Import*>(member));
    }
  }

  return absl::OkStatus();
}

absl::Status VerifyParentage(const AstNode* root) {
  XLS_CHECK(root != nullptr);

  if (const Module* module = dynamic_cast<const Module*>(root);
      module != nullptr) {
    return VerifyParentage(module);
  }

  for (const auto* child : root->GetChildren(/*want_types=*/true)) {
    XLS_CHECK(child != nullptr);
    XLS_RETURN_IF_ERROR(VerifyParentage(child));

    if (child->parent() == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Child \"%s\" (%s) of node \"%s\" (%s) had "
                          "no parent.",
                          child->ToString(), child->GetNodeTypeName(),
                          root->ToString(), root->GetNodeTypeName()));
    }

    if (child->parent() != root) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Child \"%s\" (%s) of node \"%s\" (%s) had "
          "node \"%s\" (%s) as its parent.",
          child->ToString(), child->GetNodeTypeName(), root->ToString(),
          root->GetNodeTypeName(), child->parent()->ToString(),
          child->parent()->GetNodeTypeName()));
    }
  }

  return absl::OkStatus();
}

absl::flat_hash_set<const AstNode*> FlattenToSet(const AstNode* node) {
  absl::flat_hash_set<const AstNode*> the_set;
  FlattenToSetInternal(node, &the_set);
  return the_set;
}

absl::StatusOr<InterpValue> GetBuiltinNameDefColonAttr(
    const BuiltinNameDef* builtin_name_def, std::string_view attr) {
  const auto& sized_type_keywords = GetSizedTypeKeywordsMetadata();
  auto it = sized_type_keywords.find(builtin_name_def->identifier());
  // We should have checked this was a valid type keyword in typechecking.
  XLS_RET_CHECK(it != sized_type_keywords.end());
  auto [is_signed, width] = it->second;
  if (attr == "ZERO") {
    return InterpValue::MakeZeroValue(is_signed, width);
  }
  if (attr == "MAX") {
    return InterpValue::MakeMaxValue(is_signed, width);
  }
  // We only support the above attributes on builtin types at the moment -- this
  // is checked during typechecking.
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid attribute of builtin name %s: %s",
                      builtin_name_def->identifier(), attr));
}

absl::StatusOr<InterpValue> GetArrayTypeColonAttr(
    const ArrayTypeAnnotation* array_type, uint64_t constexpr_dim,
    std::string_view attr) {
  auto* builtin_type =
      dynamic_cast<BuiltinTypeAnnotation*>(array_type->element_type());
  if (builtin_type == nullptr) {
    return absl::InvalidArgumentError(
        "Can only take '::' attributes of uN/sN/bits array types.");
  }
  bool is_signed;
  switch (builtin_type->builtin_type()) {
    case BuiltinType::kUN:
    case BuiltinType::kBits:
      is_signed = false;
      break;
    case BuiltinType::kSN:
      is_signed = true;
      break;
    default:
      return absl::InvalidArgumentError(
          "Can only take '::' attributes of uN/sN/bits array types.");
  }

  if (attr == "ZERO") {
    return InterpValue::MakeZeroValue(is_signed, constexpr_dim);
  }
  if (attr == "MAX") {
    return InterpValue::MakeMaxValue(is_signed, constexpr_dim);
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid attribute of builtin array type: %s", attr));
}

int64_t DetermineIndentLevel(const AstNode& n) {
  switch (n.kind()) {
    case AstNodeKind::kModule:
      return 0;
    case AstNodeKind::kBlock: {
      XLS_CHECK(n.parent() != nullptr);
      return DetermineIndentLevel(*n.parent()) + 1;
    }
    case AstNodeKind::kFunction: {
      const Function* function = down_cast<const Function*>(&n);
      switch (function->tag()) {
        case Function::Tag::kProcInit:
        case Function::Tag::kProcNext:
        case Function::Tag::kProcConfig:
          return 1;
        case Function::Tag::kNormal:
          return 0;
      }
    }
    default: {
      AstNode* parent = n.parent();
      XLS_CHECK(parent != nullptr);
      return DetermineIndentLevel(*parent);
    }
  }
}

std::optional<BitVectorMetadata> ExtractBitVectorMetadata(
    const TypeAnnotation* type_annotation) {
  bool is_enum = false;
  bool is_alias = false;
  const TypeAnnotation* type = type_annotation;
  while (dynamic_cast<const TypeRefTypeAnnotation*>(type) != nullptr) {
    auto type_ref = dynamic_cast<const TypeRefTypeAnnotation*>(type);
    if (std::holds_alternative<TypeAlias*>(
            type_ref->type_ref()->type_definition())) {
      is_alias = true;
      type = std::get<TypeAlias*>(type_ref->type_ref()->type_definition())
                 ->type_annotation();
    } else if (std::holds_alternative<EnumDef*>(
                   type_ref->type_ref()->type_definition())) {
      is_enum = true;
      type = std::get<EnumDef*>(type_ref->type_ref()->type_definition())
                 ->type_annotation();
    } else {
      break;
    }
  }

  BitVectorKind kind;
  if (is_enum && is_alias) {
    kind = BitVectorKind::kEnumTypeAlias;
  } else if (is_enum && !is_alias) {
    kind = BitVectorKind::kEnumType;
  } else if (!is_enum && is_alias) {
    kind = BitVectorKind::kBitTypeAlias;
  } else {
    kind = BitVectorKind::kBitType;
  }

  if (const BuiltinTypeAnnotation* builtin =
          dynamic_cast<const BuiltinTypeAnnotation*>(type);
      builtin != nullptr) {
    switch (builtin->builtin_type()) {
      case BuiltinType::kToken:
      case BuiltinType::kChannelIn:
      case BuiltinType::kChannelOut:
        return std::nullopt;
      default:
        break;
    }
    return BitVectorMetadata{.bit_count = builtin->GetBitCount(),
                             .is_signed = builtin->GetSignedness(),
                             .kind = kind};
  }
  if (const ArrayTypeAnnotation* array_type =
          dynamic_cast<const ArrayTypeAnnotation*>(type);
      array_type != nullptr) {
    // bits[..], uN[..], and sN[..] are bit-vector types but a represented with
    // ArrayTypeAnnotations.
    const BuiltinTypeAnnotation* builtin_element_type =
        dynamic_cast<const BuiltinTypeAnnotation*>(array_type->element_type());
    if (builtin_element_type == nullptr) {
      return std::nullopt;
    }
    if (builtin_element_type->builtin_type() == BuiltinType::kBits ||
        builtin_element_type->builtin_type() == BuiltinType::kUN ||
        builtin_element_type->builtin_type() == BuiltinType::kSN) {
      return BitVectorMetadata{
          .bit_count = array_type->dim(),
          .is_signed = builtin_element_type->builtin_type() == BuiltinType::kSN,
          .kind = kind};
    }
  }
  return std::nullopt;
}

absl::StatusOr<std::vector<AstNode*>> CollectUnder(AstNode* root,
                                                   bool want_types) {
  std::vector<AstNode*> nodes;

  class CollectVisitor : public AstNodeVisitor {
   public:
    explicit CollectVisitor(std::vector<AstNode*>& nodes) : nodes_(nodes) {}

#define DECLARE_HANDLER(__type)                           \
  absl::Status Handle##__type(const __type* n) override { \
    nodes_.push_back(const_cast<__type*>(n));             \
    return absl::OkStatus();                              \
  }
    XLS_DSLX_AST_NODE_EACH(DECLARE_HANDLER)
#undef DECLARE_HANDLER

   private:
    std::vector<AstNode*>& nodes_;
  } collect_visitor(nodes);

  XLS_RETURN_IF_ERROR(WalkPostOrder(root, &collect_visitor, want_types));
  return nodes;
}

absl::StatusOr<std::vector<const AstNode*>> CollectUnder(const AstNode* root,
                                                         bool want_types) {
  // Implementation note: delegate to non-const version and turn result values
  // back to const.
  XLS_ASSIGN_OR_RETURN(std::vector<AstNode*> got,
                       CollectUnder(const_cast<AstNode*>(root), want_types));

  std::vector<const AstNode*> result;
  result.reserve(got.size());
  for (AstNode* n : got) {
    result.push_back(n);
  }
  return result;
}

absl::StatusOr<std::vector<const NameDef*>> CollectReferencedUnder(
    const AstNode* root, bool want_types) {
  XLS_ASSIGN_OR_RETURN(std::vector<const AstNode*> nodes,
                       CollectUnder(root, want_types));
  std::vector<const NameDef*> name_defs;
  for (const AstNode* n : nodes) {
    if (const NameRef* nr = dynamic_cast<const NameRef*>(n)) {
      if (std::holds_alternative<const NameDef*>(nr->name_def())) {
        name_defs.push_back(std::get<const NameDef*>(nr->name_def()));
      }
    }
  }
  return name_defs;
}

}  // namespace xls::dslx
