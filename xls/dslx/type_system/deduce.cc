// Copyright 2020 The XLS Authors
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

#include "xls/dslx/type_system/deduce.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/casts.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/bytecode/bytecode_emitter.h"
#include "xls/dslx/bytecode/bytecode_interpreter.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/constexpr_evaluator.h"
#include "xls/dslx/errors.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_node.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/frontend/token_utils.h"
#include "xls/dslx/interp_bindings.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/concrete_type.h"
#include "xls/dslx/type_system/concrete_type_zero_value.h"
#include "xls/dslx/type_system/deduce_ctx.h"
#include "xls/dslx/type_system/parametric_constraint.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/parametric_expression.h"
#include "xls/dslx/type_system/parametric_instantiator.h"
#include "xls/dslx/type_system/type_and_parametric_env.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/unwrap_meta_type.h"
#include "xls/dslx/warning_kind.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {
namespace {

bool IsNameRefTo(const Expr* e, const NameDef* name_def) {
  if (auto* name_ref = dynamic_cast<const NameRef*>(e)) {
    const AnyNameDef any_name_def = name_ref->name_def();
    return std::holds_alternative<const NameDef*>(any_name_def) &&
           std::get<const NameDef*>(any_name_def) == name_def;
  }
  return false;
}

// Deduces the concrete types of the arguments to a parametric function or
// proc and returns them to the caller.
absl::Status InstantiateParametricArgs(
    const Instantiation* inst, const Expr* callee, absl::Span<Expr* const> args,
    DeduceCtx* ctx, std::vector<InstantiateArg>* instantiate_args) {
  for (Expr* arg : args) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                         DeduceAndResolve(arg, ctx));
    XLS_VLOG(5) << "InstantiateParametricArgs; arg: `" << arg->ToString()
                << "` deduced: `" << type->ToString() << "` @ " << arg->span();
    XLS_RET_CHECK(!type->IsMeta()) << "parametric arg: " << arg->ToString()
                                   << " type: " << type->ToString();
    instantiate_args->push_back(InstantiateArg{std::move(type), arg->span()});
  }

  return absl::OkStatus();
}

ParametricEnv GetCurrentParametricEnv(DeduceCtx* ctx) {
  return ctx->fn_stack().empty() ? ParametricEnv()
                                 : ctx->fn_stack().back().parametric_env();
}

// Attempts to convert an expression from the full DSL AST into the
// ParametricExpression sub-AST (a limited form that we can embed into a
// ConcreteTypeDim for later instantiation).
absl::StatusOr<std::unique_ptr<ParametricExpression>> ExprToParametric(
    const Expr* e, DeduceCtx* ctx) {
  if (auto* n = dynamic_cast<const ConstRef*>(e)) {
    XLS_RETURN_IF_ERROR(ctx->Deduce(n).status());
    XLS_ASSIGN_OR_RETURN(InterpValue constant,
                         ctx->type_info()->GetConstExpr(n));
    return std::make_unique<ParametricConstant>(std::move(constant));
  }
  if (auto* n = dynamic_cast<const NameRef*>(e)) {
    return std::make_unique<ParametricSymbol>(n->identifier(), n->span());
  }
  if (auto* n = dynamic_cast<const Binop*>(e)) {
    XLS_ASSIGN_OR_RETURN(auto lhs, ExprToParametric(n->lhs(), ctx));
    XLS_ASSIGN_OR_RETURN(auto rhs, ExprToParametric(n->rhs(), ctx));
    switch (n->binop_kind()) {
      case BinopKind::kMul:
        return std::make_unique<ParametricMul>(std::move(lhs), std::move(rhs));
      case BinopKind::kAdd:
        return std::make_unique<ParametricAdd>(std::move(lhs), std::move(rhs));
      default:
        return absl::InvalidArgumentError(
            "Cannot convert expression to parametric: " + e->ToString());
    }
  }
  if (auto* n = dynamic_cast<const Number*>(e)) {
    auto default_type = BitsType::MakeU32();
    XLS_ASSIGN_OR_RETURN(
        InterpValue constexpr_value,
        ConstexprEvaluator::EvaluateToValue(
            ctx->import_data(), ctx->type_info(), ctx->warnings(),
            GetCurrentParametricEnv(ctx), n, default_type.get()));
    return std::make_unique<ParametricConstant>(std::move(constexpr_value));
  }
  return absl::InvalidArgumentError(
      "Cannot convert expression to parametric: " + e->ToString());
}

// Record that the current function being checked has a side effect and will
// require an implicit token when converted to IR.
void UseImplicitToken(DeduceCtx* ctx) {
  Function* caller = ctx->fn_stack().back().f();
  // Note: caller could be nullptr; e.g. when we're calling a function that
  // can fail!() from the top level of a module; e.g. in a module-level const
  // expression.
  if (caller != nullptr) {
    ctx->type_info()->NoteRequiresImplicitToken(caller, true);
  }

  // TODO(rspringer): 2021-09-01: How to fail! from inside a proc?
}

absl::StatusOr<InterpValue> InterpretExpr(
    DeduceCtx* ctx, const Expr* expr,
    const absl::flat_hash_map<std::string, InterpValue>& env) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<BytecodeFunction> bf,
                       BytecodeEmitter::EmitExpression(
                           ctx->import_data(), ctx->type_info(), expr, env,
                           ctx->fn_stack().back().parametric_env()));

  return BytecodeInterpreter::Interpret(ctx->import_data(), bf.get(),
                                        /*args=*/{});
}

// Creates a function invocation on the first element of the given array.
//
// We need to create a fake invocation to deduce the type of a function
// in the case where map is called with a builtin as the map function. Normally,
// map functions (including parametric ones) have their types deduced when their
// ast.Function nodes are encountered (where a similar fake ast.Invocation node
// is created).
//
// Builtins don't have ast.Function nodes, so that inference can't occur, so we
// essentually perform that synthesis and deduction here.
//
// Args:
//   module: AST node owner.
//   span_: The location in the code where analysis is occurring.
//   callee: The function to be invoked.
//   arg_array: The array of arguments (at least one) to the function.
//
// Returns:
//   An invocation node for the given function when called with an element in
//   the argument array.
Invocation* CreateElementInvocation(Module* module, const Span& span,
                                    Expr* callee, Expr* arg_array) {
  auto* name = module->GetOrCreateBuiltinNameDef("u32");
  auto* annotation =
      module->Make<BuiltinTypeAnnotation>(span, BuiltinType::kU32, name);
  auto* index_number =
      module->Make<Number>(span, "0", NumberKind::kOther, annotation);
  auto* index = module->Make<Index>(span, arg_array, index_number);
  return module->Make<Invocation>(span, callee, std::vector<Expr*>{index});
}

// Returns an AST node typed T from module "m", resolved via name "name".
//
// Errors are attributed to span "span".
//
// Prefer this function to Module::GetMemberOrError(), as this gives a
// positional type-inference-error as its status result when the requested
// resolution cannot be performed.
template <typename T>
absl::StatusOr<T*> GetMemberOrTypeInferenceError(Module* m,
                                                 std::string_view name,
                                                 const Span& span) {
  std::optional<ModuleMember*> member = m->FindMemberWithName(name);
  if (!member.has_value()) {
    return TypeInferenceErrorStatus(
        span, nullptr,
        absl::StrFormat("Name '%s' does not exist in module `%s`", name,
                        m->name()));
  }

  if (!std::holds_alternative<T*>(*member.value())) {
    return TypeInferenceErrorStatus(
        span, nullptr,
        absl::StrFormat(
            "Name '%s' in module `%s` refers to a %s but a %s is required",
            name, m->name(), GetModuleMemberTypeName(*member.value()),
            T::GetDebugTypeName()));
  }

  T* result = std::get<T*>(*member.value());
  XLS_RET_CHECK(result != nullptr);
  return result;
}

// Resolves "ref" to an AST function.
absl::StatusOr<Function*> ResolveColonRefToFnForInvocation(ColonRef* ref,
                                                           DeduceCtx* ctx) {
  std::optional<Import*> import = ref->ResolveImportSubject();
  if (!import.has_value()) {
    return TypeInferenceErrorStatus(
        ref->span(), nullptr,
        absl::StrFormat("Colon-reference subject `%s` did not refer to a "
                        "module, so `%s` cannot be invoked.",
                        ToAstNode(ref->subject())->ToString(),
                        ref->ToString()));
  }
  XLS_RET_CHECK(import.has_value())
      << "ColonRef did not refer to an import: " << ref->ToString();
  std::optional<const ImportedInfo*> imported_info =
      ctx->type_info()->GetImported(*import);
  XLS_RET_CHECK(imported_info.has_value());
  Module* module = imported_info.value()->module;
  return GetMemberOrTypeInferenceError<Function>(module, ref->attr(),
                                                 ref->span());
}

// Resolves "ref" to an AST proc.
absl::StatusOr<Proc*> ResolveColonRefToProc(const ColonRef* ref,
                                            DeduceCtx* ctx) {
  std::optional<Import*> import = ref->ResolveImportSubject();
  XLS_RET_CHECK(import.has_value())
      << "ColonRef did not refer to an import: " << ref->ToString();
  std::optional<const ImportedInfo*> imported_info =
      ctx->type_info()->GetImported(*import);
  return GetMemberOrTypeInferenceError<Proc>(imported_info.value()->module,
                                             ref->attr(), ref->span());
}

// If the width is known for "type", checks that "number" fits in that type.
absl::Status TryEnsureFitsInType(const Number& number, const BitsType& type) {
  XLS_VLOG(5) << "TryEnsureFitsInType; number: " << number.ToString() << " @ "
              << number.span() << " type: " << type;

  // Characters have a `u8` type. They can support the dash (negation symbol).
  if (number.number_kind() != NumberKind::kCharacter &&
      number.text()[0] == '-' && !type.is_signed()) {
    return TypeInferenceErrorStatus(
        number.span(), &type,
        absl::StrFormat("Number %s invalid: "
                        "can't assign a negative value to an unsigned type.",
                        number.ToString()));
  }

  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim bits_dim, type.GetTotalBitCount());
  if (!std::holds_alternative<InterpValue>(bits_dim.value())) {
    // We have to wait for the dimension to be fully resolved before we can
    // check that the number is compliant.
    return absl::OkStatus();
  }

  XLS_ASSIGN_OR_RETURN(int64_t bit_count, bits_dim.GetAsInt64());

  // Helper to give an informative error on the appropriate range when we
  // determine the numerical value given doesn't fit into the type.
  auto does_not_fit = [&number, &type, bit_count]() {
    std::string low;
    std::string high;
    if (type.is_signed()) {
      low = BitsToString(Bits::MinSigned(bit_count),
                         FormatPreference::kSignedDecimal);
      high = BitsToString(Bits::MaxSigned(bit_count),
                          FormatPreference::kSignedDecimal);
    } else {
      low = BitsToString(Bits(bit_count), FormatPreference::kUnsignedDecimal);
      high = BitsToString(Bits::AllOnes(bit_count),
                          FormatPreference::kUnsignedDecimal);
    }

    return TypeInferenceErrorStatus(
        number.span(), &type,
        absl::StrFormat("Value '%s' does not fit in "
                        "the bitwidth of a %s (%d). "
                        "Valid values are [%s, %s].",
                        number.text(), type.ToString(), bit_count, low, high));
  };

  XLS_ASSIGN_OR_RETURN(bool fits_in_type, number.FitsInType(bit_count));
  if (!fits_in_type) {
    return does_not_fit();
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceUnop(const Unop* node,
                                                         DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> operand_type,
                       ctx->Deduce(node->operand()));

  if (dynamic_cast<BitsType*>(operand_type.get()) == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), operand_type.get(),
        absl::StrFormat(
            "Unary operation `%s` can only be applied to bits-typed operands.",
            UnopKindToString(node->unop_kind())));
  }

  return operand_type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceProcMember(
    const ProcMember* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(auto concrete_type,
                       ctx->Deduce(node->type_annotation()));
  auto* meta_type = dynamic_cast<MetaType*>(concrete_type.get());
  std::unique_ptr<ConcreteType>& param_type = meta_type->wrapped();
  return std::move(param_type);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceParam(const Param* node,
                                                          DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(auto concrete_type,
                       ctx->Deduce(node->type_annotation()));
  auto* meta_type = dynamic_cast<MetaType*>(concrete_type.get());
  std::unique_ptr<ConcreteType>& param_type = meta_type->wrapped();

  Function* f = dynamic_cast<Function*>(node->parent());
  if (f == nullptr) {
    return std::move(param_type);
  }

  // Special case handling for parameters to config functions. These must be
  // made constexpr.
  //
  // When deducing a proc at top level, we won't have constexpr values for its
  // config params, which will cause Spawn deduction to fail, so we need to
  // create dummy InterpValues for its parameter channels.
  // Other types of params aren't allowed, example: a proc member could be
  // assigned a constexpr value based on the sum of dummy values.
  // Stack depth 2: Module "<top>" + the config function being looked at.
  bool is_root_proc =
      f->tag() == Function::Tag::kProcConfig && ctx->fn_stack().size() == 2;
  bool is_channel_param =
      dynamic_cast<ChannelType*>(param_type.get()) != nullptr;
  bool is_param_constexpr = ctx->type_info()->IsKnownConstExpr(node);
  if (is_root_proc && is_channel_param && !is_param_constexpr) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue value,
        ConstexprEvaluator::CreateChannelValue(param_type.get()));
    ctx->type_info()->NoteConstExpr(node, value);
    ctx->type_info()->NoteConstExpr(node->name_def(), value);
  }

  return std::move(param_type);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceConstantDef(
    const ConstantDef* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "Noting constant: " << node->ToString();
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> result,
                       ctx->Deduce(node->value()));
  const FnStackEntry& peek_entry = ctx->fn_stack().back();
  std::optional<FnCtx> fn_ctx;
  if (peek_entry.f() != nullptr) {
    fn_ctx.emplace(FnCtx{peek_entry.module()->name(), peek_entry.name(),
                         peek_entry.parametric_env()});
  }

  ctx->type_info()->SetItem(node, *result);
  ctx->type_info()->SetItem(node->name_def(), *result);

  if (node->type_annotation() != nullptr) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> annotated,
                         ctx->Deduce(node->type_annotation()));
    XLS_ASSIGN_OR_RETURN(
        annotated,
        UnwrapMetaType(std::move(annotated), node->type_annotation()->span(),
                       "numeric literal type-prefix"));
    if (*annotated != *result) {
      return ctx->TypeMismatchError(node->span(), node->type_annotation(),
                                    *annotated, node->value(), *result,
                                    "Constant definition's annotated type did "
                                    "not match its expression's type");
    }
  }

  XLS_ASSIGN_OR_RETURN(
      InterpValue constexpr_value,
      ConstexprEvaluator::EvaluateToValue(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          GetCurrentParametricEnv(ctx), node->value(), result.get()));
  ctx->type_info()->NoteConstExpr(node, constexpr_value);
  ctx->type_info()->NoteConstExpr(node->value(), constexpr_value);
  ctx->type_info()->NoteConstExpr(node->name_def(), constexpr_value);
  return result;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceTupleIndex(
    const TupleIndex* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> lhs_type,
                       ctx->Deduce(node->lhs()));
  TupleType* tuple_type = dynamic_cast<TupleType*>(lhs_type.get());
  if (tuple_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), lhs_type.get(),
        absl::StrCat("Attempted to use tuple indexing on a non-tuple: ",
                     node->ToString()));
  }

  ctx->set_in_typeless_number_ctx(true);
  absl::Cleanup cleanup = [ctx]() { ctx->set_in_typeless_number_ctx(false); };
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> index_type,
                       ctx->Deduce(node->index()));
  std::move(cleanup).Cancel();

  // TupleIndex RHSs are always constexpr numbers.
  XLS_ASSIGN_OR_RETURN(
      InterpValue index_value,
      ConstexprEvaluator::EvaluateToValue(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          GetCurrentParametricEnv(ctx), node->index(), index_type.get()));
  XLS_ASSIGN_OR_RETURN(int64_t index, index_value.GetBitValueViaSign());
  if (index >= tuple_type->size()) {
    return TypeInferenceErrorStatus(
        node->span(), tuple_type,
        absl::StrCat("Out-of-bounds tuple index specified: ",
                     node->index()->ToString()));
  }

  return tuple_type->GetMemberType(index).CloneToUnique();
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceTypeRef(const TypeRef* node,
                                                            DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       ctx->Deduce(ToAstNode(node->type_definition())));
  XLS_RET_CHECK(type->IsMeta()) << type->ToString();
  return type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceTypeAlias(
    const TypeAlias* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       ctx->Deduce(node->type_annotation()));
  XLS_RET_CHECK(type->IsMeta());
  ctx->type_info()->SetItem(node->name_def(), *type);
  return type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceXlsTuple(
    const XlsTuple* node, DeduceCtx* ctx) {
  // Give a warning if the tuple is on a single line, is more than one element,
  // but has a trailing comma.
  //
  // Note: warning diagnostics and type checking are currently fused together,
  // but this is a pure post-parsing warning -- currently type checking the pass
  // that has a warning collector available.
  if (node->span().start().lineno() == node->span().limit().lineno() &&
      node->members().size() > 1 && node->has_trailing_comma()) {
    ctx->warnings()->Add(
        node->span(), WarningKind::kSingleLineTupleTrailingComma,
        absl::StrFormat("Tuple expression (with >1 element) is on a single "
                        "line, but has a trailing comma."));
  }

  std::vector<std::unique_ptr<ConcreteType>> members;
  for (Expr* e : node->members()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> m, ctx->Deduce(e));
    members.push_back(std::move(m));
  }
  return std::make_unique<TupleType>(std::move(members));
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceNumber(const Number* node,
                                                           DeduceCtx* ctx) {
  std::unique_ptr<ConcreteType> concrete_type;
  ParametricEnv bindings = GetCurrentParametricEnv(ctx);
  if (node->type_annotation() == nullptr) {
    switch (node->number_kind()) {
      case NumberKind::kBool: {
        auto type = BitsType::MakeU1();
        ctx->type_info()->SetItem(node, *type);
        return type;
      }
      case NumberKind::kCharacter: {
        auto type = BitsType::MakeU8();
        ctx->type_info()->SetItem(node, *type);
        return type;
      }
      default:
        break;
    }

    if (ctx->in_typeless_number_ctx()) {
      concrete_type = BitsType::MakeU32();
    } else {
      return TypeInferenceErrorStatus(node->span(), nullptr,
                                      "Could not infer a type for "
                                      "this number, please annotate a type.");
    }
  } else {
    XLS_ASSIGN_OR_RETURN(concrete_type, ctx->Deduce(node->type_annotation()));
    XLS_ASSIGN_OR_RETURN(concrete_type,
                         UnwrapMetaType(std::move(concrete_type),
                                        node->type_annotation()->span(),
                                        "numeric literal type-prefix"));
  }

  XLS_ASSIGN_OR_RETURN(concrete_type, Resolve(*concrete_type, ctx));
  XLS_RET_CHECK(!concrete_type->IsMeta());
  BitsType* bits_type = dynamic_cast<BitsType*>(concrete_type.get());
  if (bits_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), concrete_type.get(),
        "Non-bits type used to define a numeric literal.");
  }

  XLS_RETURN_IF_ERROR(TryEnsureFitsInType(*node, *bits_type));
  ctx->type_info()->SetItem(node, *concrete_type);
  return concrete_type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceString(const String* string,
                                                           DeduceCtx* ctx) {
  auto dim =
      ConcreteTypeDim::CreateU32(static_cast<uint32_t>(string->text().size()));
  return std::make_unique<ArrayType>(BitsType::MakeU8(), std::move(dim));
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceConditional(
    const Conditional* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> test_type,
                       ctx->Deduce(node->test()));
  XLS_ASSIGN_OR_RETURN(test_type, Resolve(*test_type, ctx));
  auto test_want = BitsType::MakeU1();
  if (*test_type != *test_want) {
    return ctx->TypeMismatchError(node->span(), node->test(), *test_type,
                                  nullptr, *test_want,
                                  "Test type for conditional expression is not "
                                  "\"bool\"");
  }

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> consequent_type,
                       ctx->Deduce(node->consequent()));
  XLS_ASSIGN_OR_RETURN(consequent_type, Resolve(*consequent_type, ctx));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> alternate_type,
                       ctx->Deduce(ToAstNode(node->alternate())));
  XLS_ASSIGN_OR_RETURN(alternate_type, Resolve(*alternate_type, ctx));

  if (*consequent_type != *alternate_type) {
    return ctx->TypeMismatchError(
        node->span(), node->consequent(), *consequent_type,
        ToAstNode(node->alternate()), *alternate_type,
        "Conditional consequent type (in the 'then' clause) "
        "did not match alternative type (in the 'else' clause)");
  }
  return consequent_type;
}

static absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceConcat(
    const Binop* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> lhs,
                       DeduceAndResolve(node->lhs(), ctx));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> rhs,
                       DeduceAndResolve(node->rhs(), ctx));

  auto* lhs_array = dynamic_cast<ArrayType*>(lhs.get());
  auto* rhs_array = dynamic_cast<ArrayType*>(rhs.get());
  bool lhs_is_array = lhs_array != nullptr;
  bool rhs_is_array = rhs_array != nullptr;

  if (lhs_is_array != rhs_is_array) {
    return ctx->TypeMismatchError(node->span(), node->lhs(), *lhs, node->rhs(),
                                  *rhs,
                                  "Attempting to concatenate array/non-array "
                                  "values together.");
  }

  if (lhs_is_array && lhs_array->element_type() != rhs_array->element_type()) {
    return ctx->TypeMismatchError(
        node->span(), nullptr, *lhs, nullptr, *rhs,
        "Array concatenation requires element types to be the same.");
  }

  if (lhs_is_array) {
    XLS_ASSIGN_OR_RETURN(ConcreteTypeDim new_size,
                         lhs_array->size().Add(rhs_array->size()));
    return std::make_unique<ArrayType>(
        lhs_array->element_type().CloneToUnique(), new_size);
  }

  auto* lhs_bits = dynamic_cast<BitsType*>(lhs.get());
  auto* rhs_bits = dynamic_cast<BitsType*>(rhs.get());
  bool lhs_is_bits = lhs_bits != nullptr;
  bool rhs_is_bits = rhs_bits != nullptr;
  if (!lhs_is_bits || !rhs_is_bits) {
    return ctx->TypeMismatchError(node->span(), node->lhs(), *lhs, node->rhs(),
                                  *rhs,
                                  "Concatenation requires operand types to be "
                                  "either both-arrays or both-bits");
  }

  XLS_RET_CHECK(lhs_bits != nullptr);
  XLS_RET_CHECK(rhs_bits != nullptr);
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim new_size,
                       lhs_bits->size().Add(rhs_bits->size()));
  return std::make_unique<BitsType>(/*signed=*/false, /*size=*/new_size);
}

// Shift operations are binary operations that require bits types as their
// operands.
static absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceShift(
    const Binop* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> lhs,
                       DeduceAndResolve(node->lhs(), ctx));

  std::optional<uint64_t> number_value;
  if (auto* number = dynamic_cast<Number*>(node->rhs());
      number != nullptr && number->type_annotation() == nullptr) {
    // Infer RHS node as bit type and retrieve bit width.
    const std::string& number_str = number->text();
    XLS_RET_CHECK(!number_str.empty()) << "Number literal empty.";
    if (number_str[0] == '-') {
      return TypeInferenceErrorStatus(
          number->span(), nullptr,
          absl::StrFormat("Negative literal values cannot be used as shift "
                          "amounts; got: %s",
                          number_str));
    }
    XLS_ASSIGN_OR_RETURN(number_value, number->GetAsUint64());
    ctx->type_info()->SetItem(
        number, BitsType(/*is_signed=*/false,
                         Bits::MinBitCountUnsigned(number_value.value())));
  }

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> rhs,
                       DeduceAndResolve(node->rhs(), ctx));

  // Validate bits type for lhs and rhs.
  BitsType* lhs_bit_type = dynamic_cast<BitsType*>(lhs.get());
  if (lhs_bit_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->lhs()->span(), lhs.get(),
        "Shift operations can only be applied to bits-typed operands.");
  }
  BitsType* rhs_bit_type = dynamic_cast<BitsType*>(rhs.get());
  if (rhs_bit_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->rhs()->span(), rhs.get(),
        "Shift operations can only be applied to bits-typed operands.");
  }

  if (rhs_bit_type->is_signed()) {
    return TypeInferenceErrorStatus(node->rhs()->span(), rhs.get(),
                                    "Shift amount must be unsigned.");
  }

  if (number_value.has_value()) {
    const ConcreteTypeDim& lhs_size = lhs_bit_type->size();
    XLS_CHECK(!lhs_size.IsParametric()) << "Shift amount type not inferred.";
    XLS_ASSIGN_OR_RETURN(int64_t lhs_bit_count, lhs_size.GetAsInt64());
    if (lhs_bit_count < number_value.value()) {
      return TypeInferenceErrorStatus(
          node->rhs()->span(), rhs.get(),
          absl::StrFormat(
              "Shift amount is larger than shift value bit width of %d.",
              lhs_bit_count));
    }
  }

  return lhs;
}

// Returns a set of the kinds of binary operations that are comparisons; that
// is, they are `(T, T) -> bool` typed.
static const absl::flat_hash_set<BinopKind>& GetBinopComparisonKinds() {
  static const auto* set = [] {
    return new absl::flat_hash_set<BinopKind>{
        BinopKind::kEq, BinopKind::kNe, BinopKind::kGt,
        BinopKind::kGe, BinopKind::kLt, BinopKind::kLe,
    };
  }();
  return *set;
}

// Returns a set of the kinds of binary operations that it's ok to use on an
// enum value.
static const absl::flat_hash_set<BinopKind>& GetEnumOkKinds() {
  static const auto* set = []() {
    return new absl::flat_hash_set<BinopKind>{
        BinopKind::kEq,
        BinopKind::kNe,
    };
  }();
  return *set;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceBinop(const Binop* node,
                                                          DeduceCtx* ctx) {
  if (node->binop_kind() == BinopKind::kConcat) {
    return DeduceConcat(node, ctx);
  }

  if (GetBinopShifts().contains(node->binop_kind())) {
    return DeduceShift(node, ctx);
  }

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> lhs,
                       DeduceAndResolve(node->lhs(), ctx));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> rhs,
                       DeduceAndResolve(node->rhs(), ctx));

  if (*lhs != *rhs) {
    return ctx->TypeMismatchError(
        node->span(), node->lhs(), *lhs, node->rhs(), *rhs,
        absl::StrFormat("Could not deduce type for "
                        "binary operation '%s'",
                        BinopKindFormat(node->binop_kind())));
  }

  if (auto* enum_type = dynamic_cast<EnumType*>(lhs.get());
      enum_type != nullptr && !GetEnumOkKinds().contains(node->binop_kind())) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Cannot use '%s' on values with enum type %s.",
                        BinopKindFormat(node->binop_kind()),
                        enum_type->nominal_type().identifier()));
  }

  if (GetBinopComparisonKinds().contains(node->binop_kind())) {
    return BitsType::MakeU1();
  }

  if (dynamic_cast<BitsType*>(lhs.get()) == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), lhs.get(),
        "Binary operations can only be applied to bits-typed operands.");
  }

  return lhs;
}

// Returns whether "node" is a "bare" number (without an explicit type
// annotation on it).
static const Number* IsBareNumber(const AstNode* node,
                                  bool* is_boolean = nullptr) {
  if (const Number* number = dynamic_cast<const Number*>(node)) {
    if (is_boolean != nullptr) {
      *is_boolean = number->number_kind() == NumberKind::kBool;
    }
    if (number->type_annotation() == nullptr) {
      return number;
    }
    return nullptr;
  }

  return nullptr;
}

// Checks that "number" can legitmately conform to type "type".
static absl::Status ValidateNumber(const Number& number,
                                   const ConcreteType& type) {
  XLS_VLOG(5) << "Validating " << number.ToString() << " vs " << type;
  const BitsType* bits_type = dynamic_cast<const BitsType*>(&type);
  if (bits_type == nullptr) {
    return TypeInferenceErrorStatus(
        number.span(), &type,
        absl::StrFormat("Non-bits type (%s) used to define a numeric literal.",
                        type.GetDebugTypeName()));
  }
  return TryEnsureFitsInType(number, *bits_type);
}

// When enums have no type annotation explicitly placed on them we infer the
// width of the enum from the values contained inside of its definition.
static absl::StatusOr<std::unique_ptr<ConcreteType>>
DeduceEnumSansUnderlyingType(const EnumDef* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "Deducing enum without underlying type: " << node->ToString();
  std::vector<std::pair<const EnumMember*, std::unique_ptr<ConcreteType>>>
      deduced;
  for (const EnumMember& member : node->values()) {
    bool is_boolean = false;
    if (IsBareNumber(member.value, &is_boolean) != nullptr && !is_boolean) {
      continue;  // We'll validate these below.
    }
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> t,
                         ctx->Deduce(member.value));
    deduced.emplace_back(&member, std::move(t));
  }
  if (deduced.empty()) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr, "Could not deduce underlying type for enum.");
  }
  const ConcreteType& target = *deduced.front().second;
  for (int64_t i = 1; i < deduced.size(); ++i) {
    const ConcreteType& got = *deduced.at(i).second;
    if (target != got) {
      return ctx->TypeMismatchError(
          deduced.at(i).first->GetSpan(), nullptr, target, nullptr, got,
          "Inconsistent member types in enum definition.");
    }
  }

  XLS_VLOG(5) << "Underlying type of EnumDef " << node->identifier() << ": "
              << target;

  // Note the deduced type for all the "bare number" members.
  for (const EnumMember& member : node->values()) {
    if (const Number* number = IsBareNumber(member.value)) {
      XLS_RETURN_IF_ERROR(ValidateNumber(*number, target));
      ctx->type_info()->SetItem(number, target);
    }
  }

  return std::move(deduced.front().second);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceEnumDef(const EnumDef* node,
                                                            DeduceCtx* ctx) {
  std::unique_ptr<ConcreteType> type;
  if (node->type_annotation() == nullptr) {
    XLS_ASSIGN_OR_RETURN(type, DeduceEnumSansUnderlyingType(node, ctx));
  } else {
    XLS_ASSIGN_OR_RETURN(type, DeduceAndResolve(node->type_annotation(), ctx));
    XLS_ASSIGN_OR_RETURN(
        type, UnwrapMetaType(std::move(type), node->type_annotation()->span(),
                             "enum underlying type"));
  }

  auto* bits_type = dynamic_cast<BitsType*>(type.get());
  if (bits_type == nullptr) {
    return TypeInferenceErrorStatus(node->span(), bits_type,
                                    "Underlying type for an enum "
                                    "must be a bits type.");
  }

  // Grab the bit count of the Enum's underlying type.
  const ConcreteTypeDim& bit_count = bits_type->size();

  std::vector<InterpValue> members;
  members.reserve(node->values().size());
  for (const EnumMember& member : node->values()) {
    if (const Number* number = dynamic_cast<const Number*>(member.value);
        number != nullptr && number->type_annotation() == nullptr) {
      XLS_RETURN_IF_ERROR(ValidateNumber(*number, *type));
      ctx->type_info()->SetItem(number, *type);
      XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          GetCurrentParametricEnv(ctx), number, type.get()));
    } else {
      XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> t,
                           ctx->Deduce(member.value));
      if (*t != *bits_type) {
        return ctx->TypeMismatchError(
            member.value->span(), nullptr, *t, nullptr, *bits_type,
            "Enum-member type did not match the enum's underlying type.");
      }
    }

    XLS_ASSIGN_OR_RETURN(
        InterpValue value,
        ConstexprEvaluator::EvaluateToValue(
            ctx->import_data(), ctx->type_info(), ctx->warnings(),
            GetCurrentParametricEnv(ctx), member.value, nullptr));
    members.push_back(value);
  }

  auto enum_type = std::make_unique<EnumType>(*node, bit_count,
                                              bits_type->is_signed(), members);

  for (const EnumMember& member : node->values()) {
    ctx->type_info()->SetItem(member.name_def, *enum_type);
  }

  auto meta_type = std::make_unique<MetaType>(std::move(enum_type));
  ctx->type_info()->SetItem(node->name_def(), *meta_type);
  ctx->type_info()->SetItem(node, *meta_type);
  return meta_type;
}

// Typechecks the name def tree items against type, putting the corresponding
// type information for the AST nodes within the name_def_tree as corresponding
// to the types within "type" (recursively).
//
// For example:
//
//    (a, (b, c))  vs (u8, (u4, u2))
//
// Will put a correspondence of {a: u8, b: u4, c: u2} into the mapping in ctx.
static absl::Status BindNames(const NameDefTree* name_def_tree,
                              const ConcreteType& type, DeduceCtx* ctx,
                              std::optional<InterpValue> constexpr_value) {
  if (name_def_tree->is_leaf()) {
    AstNode* name_def = ToAstNode(name_def_tree->leaf());

    ctx->type_info()->SetItem(name_def, type);
    if (constexpr_value.has_value()) {
      ctx->type_info()->NoteConstExpr(name_def, constexpr_value.value());
    }
    return absl::OkStatus();
  }

  auto* tuple_type = dynamic_cast<const TupleType*>(&type);
  if (tuple_type == nullptr) {
    return TypeInferenceErrorStatus(
        name_def_tree->span(), &type,
        absl::StrFormat("Expected a tuple type for these names, but "
                        "got %s.",
                        type.ToString()));
  }

  if (name_def_tree->nodes().size() != tuple_type->size()) {
    return TypeInferenceErrorStatus(
        name_def_tree->span(), &type,
        absl::StrFormat("Could not bind names, names are mismatched "
                        "in number vs type; at this level of the tuple: %d "
                        "names, %d types.",
                        name_def_tree->nodes().size(), tuple_type->size()));
  }

  for (int64_t i = 0; i < name_def_tree->nodes().size(); ++i) {
    NameDefTree* subtree = name_def_tree->nodes()[i];
    const ConcreteType& subtype = tuple_type->GetMemberType(i);
    ctx->type_info()->SetItem(subtree, subtype);

    std::optional<InterpValue> sub_value;
    if (constexpr_value.has_value()) {
      sub_value = constexpr_value.value().GetValuesOrDie()[i];
    }
    XLS_RETURN_IF_ERROR(BindNames(subtree, subtype, ctx, sub_value));
  }

  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceLet(const Let* node,
                                                        DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> rhs,
                       DeduceAndResolve(node->rhs(), ctx));

  if (node->type_annotation() != nullptr) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> annotated,
                         DeduceAndResolve(node->type_annotation(), ctx));
    XLS_ASSIGN_OR_RETURN(
        annotated,
        UnwrapMetaType(std::move(annotated), node->type_annotation()->span(),
                       "let type-annotation"));
    if (*rhs != *annotated) {
      return ctx->TypeMismatchError(
          node->type_annotation()->span(), nullptr, *annotated, nullptr, *rhs,
          "Annotated type did not match inferred type "
          "of right hand side expression.");
    }
  }

  std::optional<InterpValue> maybe_constexpr_value;
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(),
      GetCurrentParametricEnv(ctx), node->rhs(), rhs.get()));
  if (ctx->type_info()->IsKnownConstExpr(node->rhs())) {
    XLS_ASSIGN_OR_RETURN(maybe_constexpr_value,
                         ctx->type_info()->GetConstExpr(node->rhs()));
  }

  XLS_RETURN_IF_ERROR(
      BindNames(node->name_def_tree(), *rhs, ctx, maybe_constexpr_value));

  if (node->name_def_tree()->IsWildcardLeaf()) {
    ctx->warnings()->Add(
        node->name_def_tree()->span(), WarningKind::kUselessLetBinding,
        "`let _ = expr;` statement can be simplified to `expr;` -- there is no "
        "need for a `let` binding here");
  }

  if (node->is_const()) {
    TypeInfo* ti = ctx->type_info();
    XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                         ti->GetConstExpr(node->rhs()));
    ti->NoteConstExpr(node, constexpr_value);
    // Reminder: we don't allow name destructuring in constant defs, so this
    // is expected to never fail.
    XLS_RET_CHECK_EQ(node->name_def_tree()->GetNameDefs().size(), 1);
    ti->NoteConstExpr(node->name_def_tree()->GetNameDefs()[0],
                      ti->GetConstExpr(node->rhs()).value());
  }

  return ConcreteType::MakeUnit();
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceFor(const For* node,
                                                        DeduceCtx* ctx) {
  // Type of the init value to the for loop (also the accumulator type).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> init_type,
                       DeduceAndResolve(node->init(), ctx));

  // Type of the iterable (whose elements are being used as the induction
  // variable in the for loop).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> iterable_type,
                       DeduceAndResolve(node->iterable(), ctx));
  auto* iterable_array_type = dynamic_cast<ArrayType*>(iterable_type.get());
  if (iterable_array_type == nullptr) {
    return TypeInferenceErrorStatus(node->iterable()->span(),
                                    iterable_type.get(),
                                    "For loop iterable value is not an array.");
  }
  const ConcreteType& iterable_element_type =
      iterable_array_type->element_type();

  std::vector<std::unique_ptr<ConcreteType>> target_annotated_type_elems;
  target_annotated_type_elems.push_back(iterable_element_type.CloneToUnique());
  target_annotated_type_elems.push_back(init_type->CloneToUnique());
  auto target_annotated_type =
      std::make_unique<TupleType>(std::move(target_annotated_type_elems));

  // If there was an explicitly annotated type, ensure it matches our inferred
  // one.
  if (node->type_annotation() != nullptr) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> annotated_type,
                         DeduceAndResolve(node->type_annotation(), ctx));
    XLS_ASSIGN_OR_RETURN(annotated_type,
                         UnwrapMetaType(std::move(annotated_type),
                                        node->type_annotation()->span(),
                                        "for-loop annotated type"));

    if (*target_annotated_type != *annotated_type) {
      return ctx->TypeMismatchError(
          node->span(), node->type_annotation(), *annotated_type, nullptr,
          *target_annotated_type,
          "For-loop annotated type did not match inferred type.");
    }
  }

  // Bind the names to their associated types for use in the body.
  NameDefTree* bindings = node->names();

  if (!bindings->IsIrrefutable()) {
    return TypeInferenceErrorStatus(
        bindings->span(), nullptr,
        absl::StrFormat("for-loop bindings must be irrefutable (i.e. the "
                        "pattern must match all possible values)"));
  }

  XLS_RETURN_IF_ERROR(
      BindNames(bindings, *target_annotated_type, ctx, std::nullopt));

  // Now we can deduce the body.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> body_type,
                       DeduceAndResolve(node->body(), ctx));

  if (*init_type != *body_type) {
    return ctx->TypeMismatchError(node->span(), node->init(), *init_type,
                                  node->body(), *body_type,
                                  "For-loop init value type did not match "
                                  "for-loop body's result type.");
  }

  return init_type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceUnrollFor(
    const UnrollFor* node, DeduceCtx* ctx) {
  XLS_RETURN_IF_ERROR(ctx->Deduce(node->iterable()).status());
  XLS_RETURN_IF_ERROR(ctx->Deduce(node->types()).status());
  return ctx->Deduce(node->body());
}

// Returns true if the cast-conversion from "from" to "to" is acceptable (i.e.
// should not cause a type error to occur).
static bool IsAcceptableCast(const ConcreteType& from, const ConcreteType& to) {
  auto is_bits = [](const ConcreteType& ct) -> bool {
    return dynamic_cast<const BitsType*>(&ct) != nullptr;
  };
  auto is_enum = [](const ConcreteType& ct) -> bool {
    return dynamic_cast<const EnumType*>(&ct) != nullptr;
  };
  auto is_bits_array = [&](const ConcreteType& ct) -> bool {
    const ArrayType* at = dynamic_cast<const ArrayType*>(&ct);
    if (at == nullptr) {
      return false;
    }
    if (is_bits(at->element_type())) {
      return true;
    }
    return false;
  };
  if ((is_bits_array(from) && is_bits(to)) ||
      (is_bits(from) && is_bits_array(to))) {
    return from.GetTotalBitCount() == to.GetTotalBitCount();
  }
  if ((is_bits(from) || is_enum(from)) && is_bits(to)) {
    return true;
  }
  if (is_bits(from) && is_enum(to)) {
    return true;
  }
  return false;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceCast(const Cast* node,
                                                         DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       DeduceAndResolve(node->type_annotation(), ctx));
  XLS_ASSIGN_OR_RETURN(
      type, UnwrapMetaType(std::move(type), node->type_annotation()->span(),
                           "cast type"));

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> expr,
                       DeduceAndResolve(node->expr(), ctx));

  if (!IsAcceptableCast(/*from=*/*expr, /*to=*/*type)) {
    return ctx->TypeMismatchError(
        node->span(), node->expr(), *expr, node->type_annotation(), *type,
        absl::StrFormat("Cannot cast from expression type %s to %s.",
                        expr->ToErrorString(), type->ToErrorString()));
  }
  return type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceConstAssert(
    const ConstAssert* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       DeduceAndResolve(node->arg(), ctx));
  auto want = BitsType::MakeU1();
  if (*type != *want) {
    return ctx->TypeMismatchError(
        node->span(), /*lhs_node=*/node->arg(), *type, nullptr, *want,
        "const_assert! takes a (constexpr) boolean argument");
  }

  const ParametricEnv parametric_env = GetCurrentParametricEnv(ctx);
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(), parametric_env,
      node->arg(), type.get()));
  if (!ctx->type_info()->IsKnownConstExpr(node->arg())) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("const_assert! expression is not constexpr"));
  }

  XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                       ctx->type_info()->GetConstExpr(node->arg()));
  if (constexpr_value.IsFalse()) {
    XLS_ASSIGN_OR_RETURN(
        auto constexpr_map,
        MakeConstexprEnv(ctx->import_data(), ctx->type_info(), ctx->warnings(),
                         node->arg(), parametric_env));
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("const_assert! failure: `%s` constexpr environment: %s",
                        node->arg()->ToString(),
                        EnvMapToString(constexpr_map)));
  }

  return type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceStructDef(
    const StructDef* node, DeduceCtx* ctx) {
  for (const ParametricBinding* parametric : node->parametric_bindings()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> parametric_binding_type,
                         ctx->Deduce(parametric->type_annotation()));
    XLS_ASSIGN_OR_RETURN(parametric_binding_type,
                         UnwrapMetaType(std::move(parametric_binding_type),
                                        parametric->type_annotation()->span(),
                                        "parametric binding type annotation"));
    if (parametric->expr() != nullptr) {
      XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> expr_type,
                           ctx->Deduce(parametric->expr()));
      if (*expr_type != *parametric_binding_type) {
        return ctx->TypeMismatchError(
            node->span(), parametric->expr(), *expr_type,
            parametric->type_annotation(), *parametric_binding_type,
            "Annotated type of "
            "parametric value did not match inferred type.");
      }
    }
    ctx->type_info()->SetItem(parametric->name_def(), *parametric_binding_type);
  }

  std::vector<std::unique_ptr<ConcreteType>> members;
  for (auto [name_def, type] : node->members()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> concrete,
                         DeduceAndResolve(type, ctx));
    XLS_ASSIGN_OR_RETURN(concrete,
                         UnwrapMetaType(std::move(concrete), type->span(),
                                        "struct member type"));
    members.push_back(std::move(concrete));
  }
  auto wrapped = std::make_unique<StructType>(std::move(members), *node);
  auto result = std::make_unique<MetaType>(std::move(wrapped));
  ctx->type_info()->SetItem(node->name_def(), *result);
  XLS_VLOG(5) << absl::StreamFormat("Deduced type for struct %s => %s",
                                    node->ToString(), result->ToString());
  return result;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceArray(const Array* node,
                                                          DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceArray; node: " << node->ToString();

  std::vector<std::unique_ptr<ConcreteType>> member_types;
  for (Expr* member : node->members()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> member_type,
                         DeduceAndResolve(member, ctx));
    member_types.push_back(std::move(member_type));
  }

  for (int64_t i = 1; i < member_types.size(); ++i) {
    if (*member_types[0] != *member_types[i]) {
      return ctx->TypeMismatchError(
          node->span(), nullptr, *member_types[0], nullptr, *member_types[i],
          "Array member did not have same type as other members.");
    }
  }

  if (node->has_ellipsis() && node->members().empty()) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        "Array cannot have an ellipsis without an element to repeat; please "
        "add at least one element");
  }

  auto member_types_dim =
      ConcreteTypeDim::CreateU32(static_cast<uint32_t>(member_types.size()));

  // Try to infer the array type from the first member.
  std::unique_ptr<ArrayType> inferred;
  if (!member_types.empty()) {
    inferred = std::make_unique<ArrayType>(member_types[0]->CloneToUnique(),
                                           member_types_dim);
  }

  if (node->type_annotation() == nullptr) {
    if (inferred != nullptr) {
      return inferred;
    }

    return TypeInferenceErrorStatus(
        node->span(), nullptr, "Cannot deduce the type of an empty array.");
  }

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> annotated,
                       ctx->Deduce(node->type_annotation()));
  XLS_ASSIGN_OR_RETURN(annotated,
                       UnwrapMetaType(std::move(annotated), node->span(),
                                      "array type-prefix position"));
  auto* array_type = dynamic_cast<ArrayType*>(annotated.get());
  if (array_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), annotated.get(),
        "Array was not annotated with an array type.");
  }

  if (array_type->HasParametricDims()) {
    return TypeInferenceErrorStatus(
        node->type_annotation()->span(), array_type,
        absl::StrFormat("Annotated type for array "
                        "literal must be constexpr; type has dimensions that "
                        "cannot be resolved."));
  }

  // If we were presented with the wrong number of elements (vs what the
  // annotated type expected), flag an error.
  if (array_type->size() != member_types_dim && !node->has_ellipsis()) {
    std::string message = absl::StrFormat(
        "Annotated array size %s does not match inferred array size %d.",
        array_type->size().ToString(), member_types.size());
    if (inferred == nullptr) {
      // No type to compare our expectation to, as there was no member to infer
      // the type from.
      return TypeInferenceErrorStatus(node->span(), array_type, message);
    }
    return ctx->TypeMismatchError(node->span(), nullptr, *array_type, nullptr,
                                  *inferred, message);
  }

  // Implementation note: we can only do this after we've checked that the size
  // is correct (zero elements provided and zero elements expected).
  if (member_types.empty()) {
    return annotated;
  }

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> resolved_element_type,
                       Resolve(array_type->element_type(), ctx));
  if (*resolved_element_type != *member_types[0]) {
    return ctx->TypeMismatchError(
        node->members().at(0)->span(), nullptr, *resolved_element_type, nullptr,
        *member_types[0],
        "Annotated element type did not match inferred "
        "element type.");
  }

  if (node->has_ellipsis()) {
    // Need to constexpr evaluate here - while we have the concrete type - or
    // else we'd infer the wrong array size.
    XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
        ctx->import_data(), ctx->type_info(), ctx->warnings(),
        GetCurrentParametricEnv(ctx), node, array_type));
    return annotated;
  }

  return inferred;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceAttr(const Attr* node,
                                                         DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       ctx->Deduce(node->lhs()));
  auto* struct_type = dynamic_cast<StructType*>(type.get());
  if (struct_type == nullptr) {
    return TypeInferenceErrorStatus(node->span(), type.get(),
                                    absl::StrFormat("Expected a struct for "
                                                    "attribute access; got %s",
                                                    type->ToString()));
  }

  std::string_view attr_name = node->attr();
  if (!struct_type->HasNamedMember(attr_name)) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Struct '%s' does not have a "
                        "member with name "
                        "'%s'",
                        struct_type->nominal_type().identifier(), attr_name));
  }

  std::optional<const ConcreteType*> result =
      struct_type->GetMemberTypeByName(attr_name);
  XLS_RET_CHECK(result.has_value());  // We checked above we had named member.

  auto result_type = result.value()->CloneToUnique();
  return result_type;
}

// Returns whether "e" is definitely a meaningless expression-statement; i.e. if
// in statement context definitely has no side-effects and thus should be
// flagged.
//
// Note that some invocations of functions will have no side-effects and will be
// meaningless, but because we don't look inside of callees to see if they are
// side-effecting, we conservatively mark those as potentially useful.
static bool DefinitelyMeaninglessExpression(Expr* e) {
  absl::StatusOr<std::vector<AstNode*>> nodes_under_e =
      CollectUnder(e, /*want_types=*/true);
  if (!nodes_under_e.ok()) {
    XLS_LOG(WARNING) << "Could not collect nodes under `" << e->ToString()
                     << "`; status: " << nodes_under_e.status();
    return false;
  }
  for (AstNode* n : nodes_under_e.value()) {
    // In the DSL side effects can only be caused by invocations or
    // invocation-like AST nodes.
    switch (n->kind()) {
      case AstNodeKind::kInvocation:
      case AstNodeKind::kFormatMacro:
      case AstNodeKind::kSpawn:
        return false;
      default:
        continue;
    }
  }
  return true;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceStatement(
    const Statement* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> result,
                       ctx->Deduce(ToAstNode(node->wrapped())));
  return result;
}

// Warns if the next-to-last statement in a block has a trailing semi and the
// last statement is a nil tuple expression, as this is redundant; i.e.
//
//    {
//      foo;
//      ()  <-- useless, semi on previous statement implies it
//    }
static void DetectUselessTrailingTuplePattern(const Block* block,
                                              DeduceCtx* ctx) {
  // TODO(https://github.com/google/xls/issues/1124) 2023-08-31 Proc config
  // parsing functions synthesize a tuple at the end, and we don't want to flag
  // that since the user didn't even create it.
  if (block->parent()->kind() == AstNodeKind::kFunction &&
      dynamic_cast<const Function*>(block->parent())->tag() ==
          Function::Tag::kProcConfig) {
    return;
  }

  // Need at least a statement (i.e. with semicolon after it) and an
  // expression-statement at the end to match this pattern.
  if (block->statements().size() < 2) {
    return;
  }

  // Trailing statement has to be an expression-statement.
  const Statement* last_stmt = block->statements().back();
  if (!std::holds_alternative<Expr*>(last_stmt->wrapped())) {
    return;
  }

  // It has to be a tuple.
  const auto* last_expr = std::get<Expr*>(last_stmt->wrapped());
  auto* trailing_tuple = dynamic_cast<const XlsTuple*>(last_expr);
  if (trailing_tuple == nullptr) {
    return;
  }

  // Tuple has to be nil.
  if (!trailing_tuple->empty()) {
    return;
  }

  ctx->warnings()->Add(
      trailing_tuple->span(), WarningKind::kTrailingTupleAfterSemi,
      absl::StrFormat("Block has a trailing nil (empty) tuple after a "
                      "semicolon -- this is implied, please remove it"));
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceBlock(const Block* node,
                                                          DeduceCtx* ctx) {
  std::unique_ptr<ConcreteType> last;
  for (const Statement* s : node->statements()) {
    XLS_ASSIGN_OR_RETURN(last, ctx->Deduce(s));
  }
  // If there's a trailing semicolon this block always yields unit `()`.
  if (node->trailing_semi()) {
    last = ConcreteType::MakeUnit();
  }

  // We only want to check the last statement for "useless expression-statement"
  // property if it is not yielding a value from a block; e.g.
  //
  //    {
  //      my_invocation!();
  //      u32:42  // <- ok, no trailing semi
  //    }
  //
  // vs
  //
  //    {
  //      my_invocation!();
  //      u32:42;  // <- useless, trailing semi means block yields nil
  //    }
  const bool should_check_last_statement = node->trailing_semi();
  for (int64_t i = 0; i < static_cast<int64_t>(node->statements().size()) -
                              (should_check_last_statement ? 0 : 1);
       ++i) {
    const Statement* s = node->statements()[i];
    if (std::holds_alternative<Expr*>(s->wrapped()) &&
        DefinitelyMeaninglessExpression(std::get<Expr*>(s->wrapped()))) {
      Expr* e = std::get<Expr*>(s->wrapped());
      ctx->warnings()->Add(e->span(), WarningKind::kUselessExpressionStatement,
                           absl::StrFormat("Expression statement `%s` appears "
                                           "useless (i.e. has no side-effects)",
                                           e->ToString()));
    }
  }

  DetectUselessTrailingTuplePattern(node, ctx);
  return last;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceConstantArray(
    const ConstantArray* node, DeduceCtx* ctx) {
  if (node->type_annotation() == nullptr) {
    return DeduceArray(node, ctx);
  }

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       ctx->Deduce(node->type_annotation()));
  XLS_ASSIGN_OR_RETURN(
      type, UnwrapMetaType(std::move(type), node->type_annotation()->span(),
                           "array type-prefix position"));

  auto* array_type = dynamic_cast<ArrayType*>(type.get());
  if (array_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->type_annotation()->span(), type.get(),
        absl::StrFormat("Annotated type for array "
                        "literal must be an array type; got %s %s",
                        type->GetDebugTypeName(),
                        node->type_annotation()->ToString()));
  }

  const ConcreteType& element_type = array_type->element_type();
  for (Expr* member : node->members()) {
    XLS_RET_CHECK(IsConstant(member));
    if (Number* number = dynamic_cast<Number*>(member);
        number != nullptr && number->type_annotation() == nullptr) {
      ctx->type_info()->SetItem(member, element_type);
      const BitsType* bits_type = dynamic_cast<const BitsType*>(&element_type);
      XLS_RET_CHECK(bits_type != nullptr);
      XLS_RETURN_IF_ERROR(TryEnsureFitsInType(*number, *bits_type));
    }
  }

  XLS_RETURN_IF_ERROR(DeduceArray(node, ctx).status());
  return type;
}

static bool IsPublic(const ModuleMember& member) {
  if (std::holds_alternative<Function*>(member)) {
    return std::get<Function*>(member)->is_public();
  }
  if (std::holds_alternative<Proc*>(member)) {
    return std::get<Proc*>(member)->is_public();
  }
  if (std::holds_alternative<TypeAlias*>(member)) {
    return std::get<TypeAlias*>(member)->is_public();
  }
  if (std::holds_alternative<StructDef*>(member)) {
    return std::get<StructDef*>(member)->is_public();
  }
  if (std::holds_alternative<ConstantDef*>(member)) {
    return std::get<ConstantDef*>(member)->is_public();
  }
  if (std::holds_alternative<EnumDef*>(member)) {
    return std::get<EnumDef*>(member)->is_public();
  }
  if (std::holds_alternative<TestFunction*>(member)) {
    return false;
  }
  if (std::holds_alternative<TestProc*>(member)) {
    return false;
  }
  if (std::holds_alternative<QuickCheck*>(member)) {
    return false;
  }
  if (std::holds_alternative<Import*>(member)) {
    return false;
  }
  XLS_LOG(FATAL) << "Unhandled ModuleMember variant.";
}

// Deduces a colon-ref in the particular case when the subject is known to be an
// import.
static absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceColonRefToModule(
    const ColonRef* node, Module* module, DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceColonRefToModule; node: `" << node->ToString() << "`";
  std::optional<ModuleMember*> elem = module->FindMemberWithName(node->attr());
  if (!elem.has_value()) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Attempted to refer to module %s member '%s' "
                        "which does not exist.",
                        module->name(), node->attr()));
  }
  if (!IsPublic(*elem.value())) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Attempted to refer to module member %s that "
                        "is not public.",
                        ToAstNode(*elem.value())->ToString()));
  }

  XLS_ASSIGN_OR_RETURN(TypeInfo * imported_type_info,
                       ctx->import_data()->GetRootTypeInfo(module));
  if (std::holds_alternative<Function*>(*elem.value())) {
    auto* f = std::get<Function*>(*elem.value());
    if (!imported_type_info->Contains(f->name_def())) {
      XLS_VLOG(2) << "Function name not in imported_type_info; indicates it is "
                     "parametric.";
      XLS_RET_CHECK(f->IsParametric());
      // We don't type check parametric functions until invocations.
      // Let's typecheck this imported parametric function with respect to its
      // module (this will only get the type signature, the body gets
      // typechecked after parametric instantiation).
      std::unique_ptr<DeduceCtx> imported_ctx =
          ctx->MakeCtx(imported_type_info, module);
      const FnStackEntry& peek_entry = ctx->fn_stack().back();
      imported_ctx->AddFnStackEntry(peek_entry);
      XLS_RETURN_IF_ERROR(ctx->typecheck_function()(f, imported_ctx.get()));
      imported_type_info = imported_ctx->type_info();
    }
  }

  AstNode* member_node = ToAstNode(*elem.value());
  std::optional<ConcreteType*> type = imported_type_info->GetItem(member_node);
  XLS_RET_CHECK(type.has_value()) << member_node->ToString();
  return type.value()->CloneToUnique();
}

static absl::StatusOr<std::unique_ptr<ConcreteType>>
DeduceColonRefToBuiltinNameDef(BuiltinNameDef* builtin_name_def,
                               const ColonRef* node) {
  const auto& sized_type_keywords = GetSizedTypeKeywordsMetadata();
  if (auto it = sized_type_keywords.find(builtin_name_def->identifier());
      it != sized_type_keywords.end()) {
    auto [is_signed, size] = it->second;
    if (node->attr() == "MAX") {
      return std::make_unique<BitsType>(is_signed, size);
    }
    if (node->attr() == "ZERO") {
      return std::make_unique<BitsType>(is_signed, size);
    }
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Builtin type '%s' does not have attribute '%s'.",
                        builtin_name_def->identifier(), node->attr()));
  }
  return TypeInferenceErrorStatus(
      node->span(), nullptr,
      absl::StrFormat("Builtin '%s' has no attributes.",
                      builtin_name_def->identifier()));
}

static absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceColonRefToArrayType(
    ArrayTypeAnnotation* array_type, const ColonRef* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> resolved,
                       ctx->Deduce(array_type));
  XLS_ASSIGN_OR_RETURN(
      resolved,
      UnwrapMetaType(std::move(resolved), array_type->span(), "array type"));
  if (!IsBits(*resolved)) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Cannot use '::' on type %s -- only bits types support "
                        "'::' attributes",
                        resolved->ToString()));
  }
  if (node->attr() != "MAX" && node->attr() != "ZERO") {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Type '%s' does not have attribute '%s'.",
                        array_type->ToString(), node->attr()));
  }
  return resolved;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceColonRef(
    const ColonRef* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "Deducing type for ColonRef @ " << node->span().ToString();

  ImportData* import_data = ctx->import_data();
  XLS_ASSIGN_OR_RETURN(auto subject, ResolveColonRefSubjectForTypeChecking(
                                         import_data, ctx->type_info(), node));

  using ReturnT = absl::StatusOr<std::unique_ptr<ConcreteType>>;
  Module* subject_module = ToAstNode(subject)->owner();
  XLS_ASSIGN_OR_RETURN(TypeInfo * subject_type_info,
                       import_data->GetRootTypeInfo(subject_module));
  auto subject_ctx = ctx->MakeCtx(subject_type_info, subject_module);
  const FnStackEntry& peek_entry = ctx->fn_stack().back();
  subject_ctx->AddFnStackEntry(peek_entry);
  return absl::visit(
      Visitor{
          [&](Module* module) -> ReturnT {
            return DeduceColonRefToModule(node, module, subject_ctx.get());
          },
          [&](EnumDef* enum_def) -> ReturnT {
            if (!enum_def->HasValue(node->attr())) {
              return TypeInferenceErrorStatus(
                  node->span(), nullptr,
                  absl::StrFormat("Name '%s' is not defined by the enum %s.",
                                  node->attr(), enum_def->identifier()));
            }
            XLS_ASSIGN_OR_RETURN(auto enum_type,
                                 DeduceEnumDef(enum_def, subject_ctx.get()));
            return UnwrapMetaType(std::move(enum_type), node->span(),
                                  "enum type");
          },
          [&](BuiltinNameDef* builtin_name_def) -> ReturnT {
            return DeduceColonRefToBuiltinNameDef(builtin_name_def, node);
          },
          [&](ArrayTypeAnnotation* type) -> ReturnT {
            return DeduceColonRefToArrayType(type, node, subject_ctx.get());
          },
          [&](StructDef* struct_def) -> ReturnT {
            return TypeInferenceErrorStatus(
                node->span(), nullptr,
                absl::StrFormat("Struct definitions (e.g. '%s') cannot have "
                                "constant items.",
                                struct_def->identifier()));
          },
          [&](ColonRef* colon_ref) -> ReturnT {
            // Note: this should be unreachable, as it's a colon-reference that
            // refers *directly* to another colon-ref. Generally you need an
            // intervening construct, like a type alias.
            return absl::InternalError(
                "Colon-reference subject was another colon-reference.");
          },
      },
      subject);
}

// Returns (start, width), resolving indices via DSLX bit slice semantics.
static absl::StatusOr<StartAndWidth> ResolveBitSliceIndices(
    int64_t bit_count, std::optional<int64_t> start_opt,
    std::optional<int64_t> limit_opt) {
  XLS_RET_CHECK_GE(bit_count, 0);
  int64_t start = 0;
  int64_t limit = bit_count;

  if (start_opt.has_value()) {
    start = *start_opt;
  }
  if (limit_opt.has_value()) {
    limit = *limit_opt;
  }

  if (start < 0) {
    start += bit_count;
  }
  if (limit < 0) {
    limit += bit_count;
  }

  limit = std::min(std::max(limit, int64_t{0}), bit_count);
  start = std::min(std::max(start, int64_t{0}), limit);
  XLS_RET_CHECK_GE(start, 0);
  XLS_RET_CHECK_GE(limit, start);
  return StartAndWidth{start, limit - start};
}

static absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceWidthSliceType(
    const Index* node, const BitsType& subject_type,
    const WidthSlice& width_slice, DeduceCtx* ctx) {
  // Start expression; e.g. in `x[a+:u4]` this is `a`.
  Expr* start = width_slice.start();

  // Determined type of the start expression (must be bits kind).
  std::unique_ptr<ConcreteType> start_type_owned;
  BitsType* start_type;

  if (Number* start_number = dynamic_cast<Number*>(start);
      start_number != nullptr && start_number->type_annotation() == nullptr) {
    // A literal number with no annotated type as the slice start.
    //
    // By default, we use the "subject" type (converted to unsigned) as the type
    // for the slice start.
    start_type_owned = subject_type.ToUBits();
    start_type = dynamic_cast<BitsType*>(start_type_owned.get());

    // Get the start number as an integral value, after we make sure it fits.
    XLS_ASSIGN_OR_RETURN(Bits start_bits, start_number->GetBits(64));
    XLS_ASSIGN_OR_RETURN(int64_t start_int, start_bits.ToInt64());

    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> resolved_start_type,
                         Resolve(*start_type, ctx));
    XLS_ASSIGN_OR_RETURN(ConcreteTypeDim bit_count_ctd,
                         resolved_start_type->GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t bit_count, bit_count_ctd.GetAsInt64());

    // Make sure the start_int literal fits in the type we determined.
    absl::Status fits_status = SBitsWithStatus(start_int, bit_count).status();
    if (!fits_status.ok()) {
      return TypeInferenceErrorStatus(
          node->span(), resolved_start_type.get(),
          absl::StrFormat("Cannot fit slice start %d in %d bits (width "
                          "inferred from slice subject).",
                          start_int, bit_count));
    }
    ctx->type_info()->SetItem(start, *resolved_start_type);
    XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
        ctx->import_data(), ctx->type_info(), ctx->warnings(),
        GetCurrentParametricEnv(ctx), start_number, resolved_start_type.get()));
  } else {
    // Aside from a bare literal (with no type) we should be able to deduce the
    // start expression's type.
    XLS_ASSIGN_OR_RETURN(start_type_owned, ctx->Deduce(start));
    start_type = dynamic_cast<BitsType*>(start_type_owned.get());
    if (start_type == nullptr) {
      return TypeInferenceErrorStatus(
          start->span(), start_type,
          "Start expression for width slice must be bits typed.");
    }
  }

  // Validate that the start is unsigned.
  if (start_type->is_signed()) {
    return TypeInferenceErrorStatus(
        node->span(), start_type,
        "Start index for width-based slice must be unsigned.");
  }

  // If the width of the width_type is bigger than the subject, we flag an
  // error (prevent requesting over-slicing at compile time).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> width_type,
                       ctx->Deduce(width_slice.width()));
  XLS_ASSIGN_OR_RETURN(width_type, UnwrapMetaType(std::move(width_type),
                                                  width_slice.width()->span(),
                                                  "width slice type"));

  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim width_ctd,
                       width_type->GetTotalBitCount());
  ConcreteTypeDim subject_ctd = subject_type.size();
  if (std::holds_alternative<InterpValue>(width_ctd.value()) &&
      std::holds_alternative<InterpValue>(subject_ctd.value())) {
    XLS_ASSIGN_OR_RETURN(int64_t width_bits, width_ctd.GetAsInt64());
    XLS_ASSIGN_OR_RETURN(int64_t subject_bits, subject_ctd.GetAsInt64());
    if (width_bits > subject_bits) {
      return ctx->TypeMismatchError(
          start->span(), nullptr, subject_type, nullptr, *width_type,
          absl::StrFormat("Slice type must have <= original number of bits; "
                          "attempted slice from %d to %d bits.",
                          subject_bits, width_bits));
    }
  }

  // Validate that the width type is bits-based (e.g. no enums, since sliced
  // value could be out of range of the valid enum values).
  if (dynamic_cast<BitsType*>(width_type.get()) == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), width_type.get(),
        "A bits type is required for a width-based slice.");
  }

  // The width type is the thing returned from the width-slice.
  return width_type;
}

// Attempts to resolve one of the bounds (start or limit) of slice into a
// DSLX-compile-time constant.
static absl::StatusOr<std::optional<int64_t>> TryResolveBound(
    Slice* slice, Expr* bound, std::string_view bound_name, ConcreteType* s32,
    const absl::flat_hash_map<std::string, InterpValue>& env, DeduceCtx* ctx) {
  if (bound == nullptr) {
    return std::nullopt;
  }

  absl::StatusOr<InterpValue> bound_or = InterpretExpr(ctx, bound, env);
  if (!bound_or.ok()) {
    const absl::Status& status = bound_or.status();
    if (absl::StrContains(status.message(), "could not find slot or binding")) {
      return TypeInferenceErrorStatus(
          bound->span(), nullptr,
          absl::StrFormat(
              "Unable to resolve slice %s to a compile-time constant.",
              bound_name));
    }
  }

  const InterpValue& value = bound_or.value();
  if (value.tag() != InterpValueTag::kSBits) {  // Error if bound is not signed.
    std::string error_suffix = ".";
    if (value.tag() == InterpValueTag::kUBits) {
      error_suffix = " -- consider casting to a signed value?";
    }
    return TypeInferenceErrorStatus(
        bound->span(), nullptr,
        absl::StrFormat(
            "Slice %s must be a signed compile-time-constant value%s",
            bound_name, error_suffix));
  }

  XLS_ASSIGN_OR_RETURN(int64_t as_64b, value.GetBitValueViaSign());
  XLS_VLOG(3) << absl::StreamFormat("Slice %s bound @ %s has value: %d",
                                    bound_name, bound->span().ToString(),
                                    as_64b);
  return as_64b;
}

// Deduces the concrete type for an Index AST node with a slice spec.
//
// Precondition: node->rhs() is either a Slice or a WidthSlice.
static absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceSliceType(
    const Index* node, DeduceCtx* ctx, std::unique_ptr<ConcreteType> lhs_type) {
  auto* bits_type = dynamic_cast<BitsType*>(lhs_type.get());
  if (bits_type == nullptr) {
    // TODO(leary): 2019-10-28 Only slicing bits types for now, and only with
    // Number AST nodes, generalize to arrays and constant expressions.
    return TypeInferenceErrorStatus(node->span(), lhs_type.get(),
                                    "Value to slice is not of 'bits' type.");
  }

  if (bits_type->is_signed()) {
    return TypeInferenceErrorStatus(node->span(), lhs_type.get(),
                                    "Bit slice LHS must be unsigned.");
  }

  if (std::holds_alternative<WidthSlice*>(node->rhs())) {
    auto* width_slice = std::get<WidthSlice*>(node->rhs());
    return DeduceWidthSliceType(node, *bits_type, *width_slice, ctx);
  }

  absl::flat_hash_map<std::string, InterpValue> env;
  XLS_ASSIGN_OR_RETURN(
      env,
      MakeConstexprEnv(ctx->import_data(), ctx->type_info(), ctx->warnings(),
                       node, ctx->fn_stack().back().parametric_env()));

  std::unique_ptr<BitsType> s32 = BitsType::MakeS32();
  auto* slice = std::get<Slice*>(node->rhs());

  // Constexpr evaluate start & limit, skipping deducing in the case of
  // undecorated literals.
  auto should_deduce = [](Expr* expr) {
    if (Number* number = dynamic_cast<Number*>(expr);
        number != nullptr && number->type_annotation() == nullptr) {
      return false;
    }
    return true;
  };

  if (slice->start() != nullptr) {
    if (should_deduce(slice->start())) {
      XLS_RETURN_IF_ERROR(Deduce(slice->start(), ctx).status());
    } else {
      // If the slice start is untyped, assume S32, and check it fits in that
      // size.
      XLS_RETURN_IF_ERROR(
          TryEnsureFitsInType(*down_cast<Number*>(slice->start()), *s32));
      ctx->type_info()->SetItem(slice->start(), *s32);
    }
  }
  XLS_ASSIGN_OR_RETURN(
      std::optional<int64_t> start,
      TryResolveBound(slice, slice->start(), "start", s32.get(), env, ctx));

  if (slice->limit() != nullptr) {
    if (should_deduce(slice->limit())) {
      XLS_RETURN_IF_ERROR(Deduce(slice->limit(), ctx).status());
    } else {
      // If the slice limit is untyped, assume S32, and check it fits in that
      // size.
      XLS_RETURN_IF_ERROR(
          TryEnsureFitsInType(*down_cast<Number*>(slice->limit()), *s32));
      ctx->type_info()->SetItem(slice->limit(), *s32);
    }
  }
  XLS_ASSIGN_OR_RETURN(
      std::optional<int64_t> limit,
      TryResolveBound(slice, slice->limit(), "limit", s32.get(), env, ctx));

  const ParametricEnv& fn_parametric_env =
      ctx->fn_stack().back().parametric_env();
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim lhs_bit_count_ctd,
                       lhs_type->GetTotalBitCount());
  int64_t bit_count;
  if (std::holds_alternative<ConcreteTypeDim::OwnedParametric>(
          lhs_bit_count_ctd.value())) {
    auto& owned_parametric =
        std::get<ConcreteTypeDim::OwnedParametric>(lhs_bit_count_ctd.value());
    ParametricExpression::Evaluated evaluated =
        owned_parametric->Evaluate(ToParametricEnv(fn_parametric_env));
    InterpValue v = std::get<InterpValue>(evaluated);
    bit_count = v.GetBitValueViaSign().value();
  } else {
    XLS_ASSIGN_OR_RETURN(bit_count, lhs_bit_count_ctd.GetAsInt64());
  }
  XLS_ASSIGN_OR_RETURN(StartAndWidth saw,
                       ResolveBitSliceIndices(bit_count, start, limit));
  ctx->type_info()->AddSliceStartAndWidth(slice, fn_parametric_env, saw);

  // Make sure the start and end types match and that the limit fits.
  std::unique_ptr<ConcreteType> start_type;
  std::unique_ptr<ConcreteType> limit_type;
  if (slice->start() == nullptr && slice->limit() == nullptr) {
    start_type = BitsType::MakeS32();
    limit_type = BitsType::MakeS32();
  } else if (slice->start() != nullptr && slice->limit() == nullptr) {
    XLS_ASSIGN_OR_RETURN(BitsType * tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->start()));
    start_type = tmp->CloneToUnique();
    limit_type = start_type->CloneToUnique();
  } else if (slice->start() == nullptr && slice->limit() != nullptr) {
    XLS_ASSIGN_OR_RETURN(BitsType * tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->limit()));
    limit_type = tmp->CloneToUnique();
    start_type = limit_type->CloneToUnique();
  } else {
    XLS_ASSIGN_OR_RETURN(BitsType * tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->start()));
    start_type = tmp->CloneToUnique();
    XLS_ASSIGN_OR_RETURN(tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->limit()));
    limit_type = tmp->CloneToUnique();
  }

  if (*start_type != *limit_type) {
    return TypeInferenceErrorStatus(
        node->span(), limit_type.get(),
        absl::StrFormat(
            "Slice limit type (%s) did not match slice start type (%s).",
            limit_type->ToString(), start_type->ToString()));
  }
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim type_width_dim,
                       start_type->GetTotalBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t type_width, type_width_dim.GetAsInt64());
  if (Bits::MinBitCountSigned(saw.start + saw.width) > type_width) {
    return TypeInferenceErrorStatus(
        node->span(), limit_type.get(),
        absl::StrFormat("Slice limit does not fit in index type: %d.",
                        saw.start + saw.width));
  }

  return std::make_unique<BitsType>(/*signed=*/false, saw.width);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceIndex(const Index* node,
                                                          DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> lhs_type,
                       ctx->Deduce(node->lhs()));

  if (std::holds_alternative<Slice*>(node->rhs()) ||
      std::holds_alternative<WidthSlice*>(node->rhs())) {
    return DeduceSliceType(node, ctx, std::move(lhs_type));
  }

  if (auto* tuple_type = dynamic_cast<TupleType*>(lhs_type.get())) {
    return TypeInferenceErrorStatus(
        node->span(), tuple_type,
        "Tuples should not be indexed with array-style syntax. "
        "Use `tuple.<number>` syntax instead.");
  }

  auto* array_type = dynamic_cast<ArrayType*>(lhs_type.get());
  if (array_type == nullptr) {
    return TypeInferenceErrorStatus(node->span(), lhs_type.get(),
                                    "Value to index is not an array.");
  }

  ctx->set_in_typeless_number_ctx(true);
  auto cleanup =
      absl::MakeCleanup([ctx]() { ctx->set_in_typeless_number_ctx(false); });
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> index_type,
                       ctx->Deduce(ToAstNode(node->rhs())));
  XLS_RET_CHECK(index_type != nullptr);
  auto* index_bits = dynamic_cast<BitsType*>(index_type.get());
  if (index_bits == nullptr) {
    return TypeInferenceErrorStatus(node->span(), index_type.get(),
                                    "Index is not (scalar) bits typed.");
  }
  return array_type->element_type().CloneToUnique();
}

// Ensures that the name_def_tree bindings are aligned with the type "other"
// (which is the type for the matched value at this name_def_tree level).
static absl::Status Unify(NameDefTree* name_def_tree, const ConcreteType& other,
                          DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> resolved_rhs_type,
                       Resolve(other, ctx));
  if (name_def_tree->is_leaf()) {
    NameDefTree::Leaf leaf = name_def_tree->leaf();
    if (std::holds_alternative<NameDef*>(leaf)) {
      // Defining a name in the pattern match, we accept all types.
      ctx->type_info()->SetItem(ToAstNode(leaf), *resolved_rhs_type);
    } else if (std::holds_alternative<WildcardPattern*>(leaf)) {
      // Nothing to do.
    } else if (std::holds_alternative<Number*>(leaf) ||
               std::holds_alternative<ColonRef*>(leaf)) {
      // For a reference (or literal) the types must be consistent.
      XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> resolved_leaf_type,
                           DeduceAndResolve(ToAstNode(leaf), ctx));
      if (*resolved_leaf_type != *resolved_rhs_type) {
        return ctx->TypeMismatchError(
            name_def_tree->span(), nullptr, *resolved_rhs_type, nullptr,
            *resolved_leaf_type,
            absl::StrFormat(
                "Conflicting types; pattern expects %s but got %s from value",
                resolved_rhs_type->ToString(), resolved_leaf_type->ToString()));
      }
    }
  } else {
    const NameDefTree::Nodes& nodes = name_def_tree->nodes();
    auto* type = dynamic_cast<const TupleType*>(&other);
    if (type == nullptr) {
      return TypeInferenceErrorStatus(
          name_def_tree->span(), &other,
          "Pattern expected matched-on type to be a tuple.");
    }
    if (type->size() != nodes.size()) {
      return TypeInferenceErrorStatus(
          name_def_tree->span(), &other,
          absl::StrFormat("Pattern wanted %d tuple elements, matched-on "
                          "value had %d element",
                          nodes.size(), type->size()));
    }
    for (int64_t i = 0; i < nodes.size(); ++i) {
      const ConcreteType& subtype = type->GetMemberType(i);
      NameDefTree* subtree = nodes[i];
      XLS_RETURN_IF_ERROR(Unify(subtree, subtype, ctx));
    }
  }
  return absl::OkStatus();
}

static std::string PatternsToString(MatchArm* arm) {
  return absl::StrJoin(arm->patterns(), " | ",
                       [](std::string* out, NameDefTree* ndt) {
                         absl::StrAppend(out, ndt->ToString());
                       });
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceMatch(const Match* node,
                                                          DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> matched,
                       ctx->Deduce(node->matched()));

  if (node->arms().empty()) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        "Match construct has no arms, cannot determine its type.");
  }

  absl::flat_hash_set<std::string> seen_patterns;
  for (MatchArm* arm : node->arms()) {
    // We opportunistically identify syntactically identical match arms -- this
    // is a user error since the first should always match, the latter is
    // totally redundant.
    std::string patterns_string = PatternsToString(arm);
    if (auto [it, inserted] = seen_patterns.insert(patterns_string);
        !inserted) {
      return TypeInferenceErrorStatus(
          arm->GetPatternSpan(), nullptr,
          absl::StrFormat("Exact-duplicate pattern match detected `%s` -- only "
                          "the first could possibly match",
                          patterns_string));
    }

    for (NameDefTree* pattern : arm->patterns()) {
      // Deduce types for all patterns with types that can be checked.
      //
      // Note that NameDef is handled in the Unify() call below, and
      // WildcardPattern has no type because it's a black hole.
      for (NameDefTree::Leaf leaf : pattern->Flatten()) {
        if (!std::holds_alternative<NameDef*>(leaf) &&
            !std::holds_alternative<WildcardPattern*>(leaf)) {
          XLS_RETURN_IF_ERROR(ctx->Deduce(ToAstNode(leaf)).status());
        }
      }

      XLS_RETURN_IF_ERROR(Unify(pattern, *matched, ctx));
    }
  }

  std::vector<std::unique_ptr<ConcreteType>> arm_types;
  for (MatchArm* arm : node->arms()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> arm_type,
                         DeduceAndResolve(arm, ctx));
    arm_types.push_back(std::move(arm_type));
  }

  for (int64_t i = 1; i < arm_types.size(); ++i) {
    if (*arm_types[i] != *arm_types[0]) {
      return ctx->TypeMismatchError(
          node->arms()[i]->span(), nullptr, *arm_types[i], nullptr,
          *arm_types[0],
          "This match arm did not have the same type as the "
          "preceding match arms.");
    }
  }

  return std::move(arm_types[0]);
}

struct ValidatedStructMembers {
  // Names seen in the struct instance; e.g. for a SplatStructInstance can be a
  // subset of the struct member names.
  //
  // Note: we use a btree set so we can do set differencing via c_set_difference
  // (which works on ordered sets).
  absl::btree_set<std::string> seen_names;

  std::vector<InstantiateArg> args;
  std::vector<std::unique_ptr<ConcreteType>> member_types;
};

// Validates a struct instantiation is a subset of 'members' with no dups.
//
// Args:
//  members: Sequence of members used in instantiation. Note this may be a
//    subset; e.g. in the case of splat instantiation.
//  struct_type: The deduced type for the struct (instantiation).
//  struct_text: Display name to use for the struct in case of an error.
//  ctx: Wrapper containing node to type mapping context.
//
// Returns:
//  A tuple containing:
//  * The set of struct member names that were instantiated
//  * The ConcreteTypes of the provided arguments
//  * The ConcreteTypes of the corresponding struct member definition.
static absl::StatusOr<ValidatedStructMembers> ValidateStructMembersSubset(
    absl::Span<const std::pair<std::string, Expr*>> members,
    const StructType& struct_type, std::string_view struct_text,
    DeduceCtx* ctx) {
  ValidatedStructMembers result;
  for (auto& [name, expr] : members) {
    if (!result.seen_names.insert(name).second) {
      return TypeInferenceErrorStatus(
          expr->span(), nullptr,
          absl::StrFormat(
              "Duplicate value seen for '%s' in this '%s' struct instance.",
              name, struct_text));
    }
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> expr_type,
                         DeduceAndResolve(expr, ctx));
    XLS_RET_CHECK(!expr_type->IsMeta())
        << "name: " << name << " expr: " << expr->ToString()
        << " type: " << expr_type->ToString();

    result.args.push_back(InstantiateArg{std::move(expr_type), expr->span()});
    std::optional<const ConcreteType*> maybe_type =
        struct_type.GetMemberTypeByName(name);

    if (maybe_type.has_value()) {
      XLS_RET_CHECK(!maybe_type.value()->IsMeta())
          << maybe_type.value()->ToString();
      result.member_types.push_back(maybe_type.value()->CloneToUnique());
    } else {
      return TypeInferenceErrorStatus(
          expr->span(), nullptr,
          absl::StrFormat("Struct '%s' has no member '%s', but it was provided "
                          "by this instance.",
                          struct_text, name));
    }
  }

  return result;
}

// Dereferences the "original" struct reference to a struct definition or
// returns an error.
//
// Args:
//  span: The span of the original construct trying to dereference the struct
//    (e.g. a StructInstance).
//  original: The original struct reference value (used in error reporting).
//  current: The current type definition being dereferenced towards a struct
//    definition (note there can be multiple levels of typedefs and such).
//  type_info: The type information that the "current" TypeDefinition resolves
//    against.
static absl::StatusOr<StructDef*> DerefToStruct(
    const Span& span, std::string_view original_ref_text,
    TypeDefinition current, TypeInfo* type_info) {
  while (true) {
    if (std::holds_alternative<StructDef*>(current)) {  // Done dereferencing.
      return std::get<StructDef*>(current);
    }
    if (std::holds_alternative<TypeAlias*>(current)) {
      auto* type_alias = std::get<TypeAlias*>(current);
      TypeAnnotation* annotation = type_alias->type_annotation();
      TypeRefTypeAnnotation* type_ref =
          dynamic_cast<TypeRefTypeAnnotation*>(annotation);
      if (type_ref == nullptr) {
        return TypeInferenceErrorStatus(
            span, nullptr,
            absl::StrFormat("Could not resolve struct from %s; found: %s @ %s",
                            original_ref_text, annotation->ToString(),
                            annotation->span().ToString()));
      }
      current = type_ref->type_ref()->type_definition();
      continue;
    }
    if (std::holds_alternative<ColonRef*>(current)) {
      auto* colon_ref = std::get<ColonRef*>(current);
      // Colon ref has to be dereferenced, may be a module reference.
      ColonRef::Subject subject = colon_ref->subject();
      // TODO(leary): 2020-12-12 Original logic was this way, but we should be
      // able to violate this assertion.
      XLS_RET_CHECK(std::holds_alternative<NameRef*>(subject));
      auto* name_ref = std::get<NameRef*>(subject);
      AnyNameDef any_name_def = name_ref->name_def();
      XLS_RET_CHECK(std::holds_alternative<const NameDef*>(any_name_def));
      const NameDef* name_def = std::get<const NameDef*>(any_name_def);
      AstNode* definer = name_def->definer();
      auto* import = dynamic_cast<Import*>(definer);
      if (import == nullptr) {
        return TypeInferenceErrorStatus(
            span, nullptr,
            absl::StrFormat("Could not resolve struct from %s; found: %s @ %s",
                            original_ref_text, name_ref->ToString(),
                            name_ref->span().ToString()));
      }
      std::optional<const ImportedInfo*> imported =
          type_info->GetImported(import);
      XLS_RET_CHECK(imported.has_value());
      Module* module = imported.value()->module;
      XLS_ASSIGN_OR_RETURN(current,
                           module->GetTypeDefinition(colon_ref->attr()));
      return DerefToStruct(span, original_ref_text, current,
                           imported.value()->type_info);
    }
    XLS_RET_CHECK(std::holds_alternative<EnumDef*>(current));
    auto* enum_def = std::get<EnumDef*>(current);
    return TypeInferenceErrorStatus(
        span, nullptr,
        absl::StrFormat("Expected struct reference, but found enum: %s",
                        enum_def->identifier()));
  }
}

// Wrapper around the DerefToStruct above (that works on TypeDefinitions) that
// takes a `TypeAnnotation` instead.
static absl::StatusOr<StructDef*> DerefToStruct(
    const Span& span, std::string_view original_ref_text,
    TypeAnnotation* type_annotation, TypeInfo* type_info) {
  auto* type_ref_type_annotation =
      dynamic_cast<TypeRefTypeAnnotation*>(type_annotation);
  if (type_ref_type_annotation == nullptr) {
    return TypeInferenceErrorStatus(
        span, nullptr,
        absl::StrFormat("Could not resolve struct from %s (%s) @ %s",
                        type_annotation->ToString(),
                        type_annotation->GetNodeTypeName(),
                        type_annotation->span().ToString()));
  }

  return DerefToStruct(span, original_ref_text,
                       type_ref_type_annotation->type_ref()->type_definition(),
                       type_info);
}

// Deduces the type for a ParametricBinding (via its type annotation).
//
// Note that this returns the type of the expression (i.e. parameter reference)
// not the metatype (i.e. not the type of the type-annotation).
static absl::StatusOr<std::unique_ptr<ConcreteType>> ParametricBindingToType(
    ParametricBinding* binding, DeduceCtx* ctx) {
  Module* binding_module = binding->owner();
  ImportData* import_data = ctx->import_data();
  XLS_ASSIGN_OR_RETURN(TypeInfo * binding_type_info,
                       import_data->GetRootTypeInfo(binding_module));
  auto binding_ctx = ctx->MakeCtx(binding_type_info, binding_module);
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       binding_ctx->Deduce(binding->type_annotation()));
  return UnwrapMetaType(std::move(type), binding->type_annotation()->span(),
                        "parametric binding type");
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceStructInstance(
    const StructInstance* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "Deducing type for struct instance: " << node->ToString();

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       ctx->Deduce(ToAstNode(node->struct_ref())));
  XLS_ASSIGN_OR_RETURN(type, UnwrapMetaType(std::move(type), node->span(),
                                            "struct instance type"));

  auto* struct_type = dynamic_cast<const StructType*>(type.get());
  if (struct_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), struct_type,
        "Expected a struct definition to instantiate");
  }

  // Note what names we expect to be present.
  XLS_ASSIGN_OR_RETURN(std::vector<std::string> names,
                       struct_type->GetMemberNames());
  absl::btree_set<std::string> expected_names(names.begin(), names.end());

  XLS_ASSIGN_OR_RETURN(
      ValidatedStructMembers validated,
      ValidateStructMembersSubset(node->GetUnorderedMembers(), *struct_type,
                                  node->struct_ref()->ToString(), ctx));
  if (validated.seen_names != expected_names) {
    absl::btree_set<std::string> missing_set;
    absl::c_set_difference(expected_names, validated.seen_names,
                           std::inserter(missing_set, missing_set.begin()));
    std::vector<std::string> missing(missing_set.begin(), missing_set.end());
    std::sort(missing.begin(), missing.end());
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat(
            "Struct instance is missing member(s): %s",
            absl::StrJoin(missing, ", ",
                          [](std::string* out, const std::string& piece) {
                            absl::StrAppendFormat(out, "'%s'", piece);
                          })));
  }

  TypeAnnotation* struct_ref = node->struct_ref();
  XLS_ASSIGN_OR_RETURN(StructDef * struct_def,
                       DerefToStruct(node->span(), struct_ref->ToString(),
                                     struct_ref, ctx->type_info()));

  XLS_ASSIGN_OR_RETURN(
      std::vector<ParametricConstraint> parametric_constraints,
      ParametricBindingsToConstraints(struct_def->parametric_bindings(), ctx));
  XLS_ASSIGN_OR_RETURN(
      TypeAndParametricEnv tab,
      InstantiateStruct(node->span(), *struct_type, validated.args,
                        validated.member_types, ctx, parametric_constraints));

  return std::move(tab.type);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceSplatStructInstance(
    const SplatStructInstance* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> struct_type_ct,
                       ctx->Deduce(ToAstNode(node->struct_ref())));
  XLS_ASSIGN_OR_RETURN(struct_type_ct,
                       UnwrapMetaType(std::move(struct_type_ct), node->span(),
                                      "splatted struct instance type"));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> splatted_type_ct,
                       ctx->Deduce(node->splatted()));

  // The splatted type should be (nominally) equivalent to the struct type,
  // because that's where we're filling in the default values from (those values
  // that were not directly provided by the user).
  auto* struct_type = dynamic_cast<StructType*>(struct_type_ct.get());
  auto* splatted_type = dynamic_cast<StructType*>(splatted_type_ct.get());

  // TODO(leary): 2020-12-13 Create a test case that hits this assertion, this
  // is a type error users can make; e.g. if they try to splat-instantiate a
  // tuple type alias.
  XLS_RET_CHECK(struct_type != nullptr);
  XLS_RET_CHECK(splatted_type != nullptr);
  if (&struct_type->nominal_type() != &splatted_type->nominal_type()) {
    return ctx->TypeMismatchError(
        node->span(), nullptr, *struct_type, nullptr, *splatted_type,
        absl::StrFormat("Attempting to fill values in '%s' instantiation from "
                        "a value of type '%s'",
                        struct_type->nominal_type().identifier(),
                        splatted_type->nominal_type().identifier()));
  }

  XLS_ASSIGN_OR_RETURN(
      ValidatedStructMembers validated,
      ValidateStructMembersSubset(node->members(), *struct_type,
                                  node->struct_ref()->ToString(), ctx));

  XLS_ASSIGN_OR_RETURN(std::vector<std::string> all_names,
                       struct_type->GetMemberNames());
  XLS_VLOG(5) << "SplatStructInstance @ " << node->span() << " seen names: ["
              << absl::StrJoin(validated.seen_names, ", ") << "] "
              << " all names: [" << absl::StrJoin(all_names, ", ") << "]";

  if (validated.seen_names.size() == all_names.size()) {
    ctx->warnings()->Add(
        node->splatted()->span(), WarningKind::kUselessStructSplat,
        absl::StrFormat("'Splatted' struct instance has all members of struct "
                        "defined, consider removing the `..%s`",
                        node->splatted()->ToString()));
  }

  for (const std::string& name : all_names) {
    // If we didn't see the name, it comes from the "splatted" argument.
    if (!validated.seen_names.contains(name)) {
      const ConcreteType& splatted_member_type =
          *splatted_type->GetMemberTypeByName(name).value();
      const ConcreteType& struct_member_type =
          *struct_type->GetMemberTypeByName(name).value();

      validated.args.push_back(InstantiateArg{
          splatted_member_type.CloneToUnique(), node->splatted()->span()});
      validated.member_types.push_back(struct_member_type.CloneToUnique());
    }
  }

  // At this point, we should have the same number of args compared to the
  // number of members defined in the struct.
  XLS_RET_CHECK_EQ(validated.args.size(), validated.member_types.size());

  TypeAnnotation* struct_ref = node->struct_ref();
  XLS_ASSIGN_OR_RETURN(StructDef * struct_def,
                       DerefToStruct(node->span(), struct_ref->ToString(),
                                     struct_ref, ctx->type_info()));

  XLS_ASSIGN_OR_RETURN(
      std::vector<ParametricConstraint> parametric_constraints,
      ParametricBindingsToConstraints(struct_def->parametric_bindings(), ctx));
  XLS_ASSIGN_OR_RETURN(
      TypeAndParametricEnv tab,
      InstantiateStruct(node->span(), *struct_type, validated.args,
                        validated.member_types, ctx, parametric_constraints));

  return std::move(tab.type);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceBuiltinTypeAnnotation(
    const BuiltinTypeAnnotation* node, DeduceCtx* ctx) {
  std::unique_ptr<ConcreteType> t;
  if (node->builtin_type() == BuiltinType::kToken) {
    t = std::make_unique<TokenType>();
  } else {
    t = std::make_unique<BitsType>(node->GetSignedness(), node->GetBitCount());
  }
  return std::make_unique<MetaType>(std::move(t));
}

// Converts an AST expression in "dimension position" (e.g. in an array type
// annotation's size) and converts it into a ConcreteTypeDim value that can be
// used in a ConcreteStruct. The result is either a constexpr-evaluated value or
// a ParametricSymbol (for a parametric binding that has not yet been defined).
//
// Note: this is not capable of expressing more complex ASTs -- it assumes
// something is either fully constexpr-evaluatable, or symbolic.
static absl::StatusOr<ConcreteTypeDim> DimToConcrete(const Expr* dim_expr,
                                                     DeduceCtx* ctx) {
  std::unique_ptr<BitsType> u32 = BitsType::MakeU32();
  auto validate_high_bit = [&u32](const Span& span, uint32_t value) {
    if ((value >> 31) == 0) {
      return absl::OkStatus();
    }
    return TypeInferenceErrorStatus(
        span, u32.get(),
        absl::StrFormat("Dimension value is too large, high bit is set: %#x; "
                        "was a negative number accidentally cast to a size?",
                        value));
  };

  // We allow numbers in dimension position to go without type annotations -- we
  // implicitly make the type of the dimension u32, as we generally do with
  // dimension values.
  if (auto* number = dynamic_cast<const Number*>(dim_expr)) {
    if (number->type_annotation() == nullptr) {
      XLS_RETURN_IF_ERROR(TryEnsureFitsInType(*number, *u32));
      ctx->type_info()->SetItem(number, *u32);
    } else {
      XLS_ASSIGN_OR_RETURN(auto dim_type, ctx->Deduce(number));
      if (*dim_type != *u32) {
        return ctx->TypeMismatchError(
            dim_expr->span(), nullptr, *dim_type, nullptr, *u32,
            absl::StrFormat(
                "Dimension %s must be a `u32` (soon to be `usize`, see "
                "https://github.com/google/xls/issues/450 for details).",
                dim_expr->ToString()));
      }
    }

    XLS_ASSIGN_OR_RETURN(int64_t value, number->GetAsUint64());
    const uint32_t value_u32 = static_cast<uint32_t>(value);
    XLS_RET_CHECK_EQ(value, value_u32);

    XLS_RETURN_IF_ERROR(validate_high_bit(number->span(), value_u32));

    // No need to use the ConstexprEvaluator here. We've already got the goods.
    // It'd have trouble anyway, since this number isn't type-decorated.
    ctx->type_info()->NoteConstExpr(dim_expr, InterpValue::MakeU32(value_u32));
    return ConcreteTypeDim::CreateU32(value_u32);
  }

  // First we check that it's a u32 (in the future we'll want it to be a usize).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> dim_type,
                       ctx->Deduce(dim_expr));
  if (*dim_type != *u32) {
    return ctx->TypeMismatchError(
        dim_expr->span(), nullptr, *dim_type, nullptr, *u32,
        absl::StrFormat(
            "Dimension %s must be a `u32` (soon to be `usize`, see "
            "https://github.com/google/xls/issues/450 for details).",
            dim_expr->ToString()));
  }

  // Now we try to constexpr evaluate it.
  const ParametricEnv parametric_env = GetCurrentParametricEnv(ctx);
  XLS_VLOG(5) << "Attempting to evaluate dimension expression: `"
              << dim_expr->ToString()
              << "` via parametric env: " << parametric_env;
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(), parametric_env,
      dim_expr, dim_type.get()));
  if (ctx->type_info()->IsKnownConstExpr(dim_expr)) {
    XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                         ctx->type_info()->GetConstExpr(dim_expr));
    XLS_ASSIGN_OR_RETURN(uint64_t int_value,
                         constexpr_value.GetBitValueViaSign());
    uint32_t u32_value = static_cast<uint32_t>(int_value);
    XLS_RETURN_IF_ERROR(validate_high_bit(dim_expr->span(), u32_value));
    XLS_RET_CHECK_EQ(u32_value, int_value);
    return ConcreteTypeDim::CreateU32(u32_value);
  }

  // If there wasn't a known constexpr we could evaluate it to at this point, we
  // attempt to turn it into a parametric expression.
  absl::StatusOr<std::unique_ptr<ParametricExpression>> parametric_expr_or =
      ExprToParametric(dim_expr, ctx);
  if (parametric_expr_or.ok()) {
    return ConcreteTypeDim(std::move(parametric_expr_or).value());
  }

  XLS_VLOG(3) << "Could not convert dim expr to parametric expr; status: "
              << parametric_expr_or.status();

  // If we can't evaluate it to a parametric expression we give an error.
  return TypeInferenceErrorStatus(
      dim_expr->span(), nullptr,
      absl::StrFormat(
          "Could not evaluate dimension expression `%s` to a constant value.",
          dim_expr->ToString()));
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceChannelTypeAnnotation(
    const ChannelTypeAnnotation* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceChannelTypeAnnotation; node: " << node->ToString();
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> payload_type,
                       Deduce(node->payload(), ctx));
  XLS_RET_CHECK(payload_type->IsMeta())
      << node->payload()->ToString() << " @ " << node->payload()->span();
  XLS_ASSIGN_OR_RETURN(payload_type, UnwrapMetaType(std::move(payload_type),
                                                    node->payload()->span(),
                                                    "channel type annotation"));
  std::unique_ptr<ConcreteType> node_type =
      std::make_unique<ChannelType>(std::move(payload_type), node->direction());
  if (node->dims().has_value()) {
    std::vector<Expr*> dims = node->dims().value();

    for (const auto& dim : dims) {
      XLS_ASSIGN_OR_RETURN(ConcreteTypeDim concrete_dim,
                           DimToConcrete(dim, ctx));
      node_type =
          std::make_unique<ArrayType>(std::move(node_type), concrete_dim);
    }
  }

  return std::make_unique<MetaType>(std::move(node_type));
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceTupleTypeAnnotation(
    const TupleTypeAnnotation* node, DeduceCtx* ctx) {
  std::vector<std::unique_ptr<ConcreteType>> members;
  for (TypeAnnotation* member : node->members()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                         ctx->Deduce(member));
    XLS_ASSIGN_OR_RETURN(type, UnwrapMetaType(std::move(type), member->span(),
                                              "tuple type member"));
    members.push_back(std::move(type));
  }
  auto t = std::make_unique<TupleType>(std::move(members));
  return std::make_unique<MetaType>(std::move(t));
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceArrayTypeAnnotation(
    const ArrayTypeAnnotation* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceArrayTypeAnnotation; node: " << node->ToString();
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim dim, DimToConcrete(node->dim(), ctx));

  std::unique_ptr<ConcreteType> t;
  if (auto* element_type =
          dynamic_cast<BuiltinTypeAnnotation*>(node->element_type());
      element_type != nullptr && element_type->GetBitCount() == 0) {
    t = std::make_unique<BitsType>(element_type->GetSignedness(),
                                   std::move(dim));
  } else {
    XLS_VLOG(5) << "DeduceArrayTypeAnnotation; element_type: "
                << node->element_type()->ToString();
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> e,
                         ctx->Deduce(node->element_type()));
    XLS_ASSIGN_OR_RETURN(
        e, UnwrapMetaType(std::move(e), node->element_type()->span(),
                          "array element type position"));
    t = std::make_unique<ArrayType>(std::move(e), std::move(dim));
    XLS_VLOG(4) << absl::StreamFormat("Array type annotation: %s => %s",
                                      node->ToString(), t->ToString());
  }
  auto result = std::make_unique<MetaType>(std::move(t));
  return result;
}

// Returns concretized struct type using the provided bindings.
//
// For example, if we have a struct defined as `struct Foo<N: u32, M: u32>`,
// the default TupleType will be (N, M). If a type annotation provides bindings,
// (e.g. `Foo<A, 16>`), we will replace N, M with those values. In the case
// above, we will return `(A, 16)` instead.
//
// Args:
//   type_annotation: The provided type annotation for this parametric struct.
//   struct_def: The struct definition AST node.
//   base_type: The TupleType of the struct, based only on the struct
//    definition (before parametrics are applied).
static absl::StatusOr<std::unique_ptr<ConcreteType>> ConcretizeStructAnnotation(
    const TypeRefTypeAnnotation* type_annotation, const StructDef* struct_def,
    const ConcreteType& base_type, DeduceCtx* ctx) {
  XLS_VLOG(5) << "ConcreteStructAnnotation; type_annotation: "
              << type_annotation->ToString()
              << " struct_def: " << struct_def->ToString();

  // Note: if there are too *few* annotated parametrics, some of them may be
  // derived.
  if (type_annotation->parametrics().size() >
      struct_def->parametric_bindings().size()) {
    return TypeInferenceErrorStatus(
        type_annotation->span(), &base_type,
        absl::StrFormat("Expected %d parametric arguments for '%s'; got %d in "
                        "type annotation",
                        struct_def->parametric_bindings().size(),
                        struct_def->identifier(),
                        type_annotation->parametrics().size()));
  }

  absl::flat_hash_map<std::string, ConcreteTypeDim> parametric_env;

  for (int64_t i = 0; i < type_annotation->parametrics().size(); ++i) {
    ParametricBinding* defined_parametric =
        struct_def->parametric_bindings()[i];
    ExprOrType eot = type_annotation->parametrics()[i];
    XLS_RET_CHECK(std::holds_alternative<Expr*>(eot));
    Expr* annotated_parametric = std::get<Expr*>(eot);
    XLS_VLOG(5) << "annotated_parametric: `" << annotated_parametric->ToString()
                << "`";

    XLS_ASSIGN_OR_RETURN(ConcreteTypeDim ctd,
                         DimToConcrete(annotated_parametric, ctx));
    parametric_env.emplace(defined_parametric->identifier(), std::move(ctd));
  }

  // For the remainder of the formal parameterics (i.e. after the explicitly
  // supplied ones given as arguments) we have to see if they're derived
  // parametrics. If they're *not* derived via an expression, we should have
  // been supplied some value in the annotation, so we have to flag an error!
  for (int64_t i = type_annotation->parametrics().size();
       i < struct_def->parametric_bindings().size(); ++i) {
    ParametricBinding* defined_parametric =
        struct_def->parametric_bindings()[i];
    if (defined_parametric->expr() == nullptr) {
      return TypeInferenceErrorStatus(
          type_annotation->span(), &base_type,
          absl::StrFormat("No parametric value provided for '%s' in '%s'",
                          defined_parametric->identifier(),
                          struct_def->identifier()));
    }
  }

  ParametricExpression::Env env;
  for (const auto& [k, ctd] : parametric_env) {
    if (std::holds_alternative<InterpValue>(ctd.value())) {
      env[k] = std::get<InterpValue>(ctd.value());
    } else {
      env[k] = &ctd.parametric();
    }
  }

  // Now evaluate all the dimensions according to the values we've got.
  return base_type.MapSize([&](const ConcreteTypeDim& dim)
                               -> absl::StatusOr<ConcreteTypeDim> {
    if (std::holds_alternative<ConcreteTypeDim::OwnedParametric>(dim.value())) {
      auto& parametric =
          std::get<ConcreteTypeDim::OwnedParametric>(dim.value());
      return ConcreteTypeDim(parametric->Evaluate(env));
    }
    return dim;
  });
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceTypeRefTypeAnnotation(
    const TypeRefTypeAnnotation* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> base_type,
                       ctx->Deduce(node->type_ref()));
  TypeRef* type_ref = node->type_ref();
  TypeDefinition type_definition = type_ref->type_definition();

  // If it's a (potentially parametric) struct, we concretize it.
  absl::StatusOr<StructDef*> struct_def_or = DerefToStruct(
      node->span(), type_ref->ToString(), type_definition, ctx->type_info());
  if (struct_def_or.ok()) {
    auto* struct_def = struct_def_or.value();
    if (struct_def->IsParametric() && !node->parametrics().empty()) {
      XLS_ASSIGN_OR_RETURN(base_type, ConcretizeStructAnnotation(
                                          node, struct_def, *base_type, ctx));
    }
  }
  XLS_RET_CHECK(base_type->IsMeta());
  return std::move(base_type);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceMatchArm(
    const MatchArm* node, DeduceCtx* ctx) {
  return ctx->Deduce(node->expr());
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceChannelDecl(
    const ChannelDecl* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> element_type,
                       Deduce(node->type(), ctx));
  XLS_ASSIGN_OR_RETURN(
      element_type,
      UnwrapMetaType(std::move(element_type), node->type()->span(),
                     "channel declaration type"));
  std::unique_ptr<ConcreteType> producer = std::make_unique<ChannelType>(
      element_type->CloneToUnique(), ChannelDirection::kOut);
  std::unique_ptr<ConcreteType> consumer = std::make_unique<ChannelType>(
      std::move(element_type), ChannelDirection::kIn);

  if (node->dims().has_value()) {
    std::vector<Expr*> dims = node->dims().value();

    for (const auto& dim : dims) {
      XLS_ASSIGN_OR_RETURN(ConcreteTypeDim concrete_dim,
                           DimToConcrete(dim, ctx));
      producer = std::make_unique<ArrayType>(std::move(producer), concrete_dim);
      consumer = std::make_unique<ArrayType>(std::move(consumer), concrete_dim);
    }
  }

  if (node->fifo_depth().has_value()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> fifo_depth,
                         ctx->Deduce(node->fifo_depth().value()));
    auto want = BitsType::MakeU32();
    if (*fifo_depth != *want) {
      return ctx->TypeMismatchError(
          node->span(), node->fifo_depth().value(), *fifo_depth, nullptr, *want,
          "Channel declaration FIFO depth must be a u32.");
    }
  }

  std::vector<std::unique_ptr<ConcreteType>> elements;
  elements.push_back(std::move(producer));
  elements.push_back(std::move(consumer));
  return std::make_unique<TupleType>(std::move(elements));
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceRange(const Range* node,
                                                          DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> start_type,
                       ctx->Deduce(node->start()));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> end_type,
                       ctx->Deduce(node->end()));
  if (*start_type != *end_type) {
    return ctx->TypeMismatchError(node->span(), nullptr, *start_type, nullptr,
                                  *end_type,
                                  "Range start and end types didn't match.");
  }

  if (dynamic_cast<BitsType*>(start_type.get()) == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), start_type.get(),
        "Range start and end types must resolve to bits types.");
  }

  // Range implicitly defines a sized type, so it has to be constexpr
  // evaluatable.
  XLS_ASSIGN_OR_RETURN(
      InterpValue start_value,
      ConstexprEvaluator::EvaluateToValue(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          GetCurrentParametricEnv(ctx), node->start(), start_type.get()));
  XLS_ASSIGN_OR_RETURN(
      InterpValue end_value,
      ConstexprEvaluator::EvaluateToValue(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          GetCurrentParametricEnv(ctx), node->end(), end_type.get()));

  XLS_ASSIGN_OR_RETURN(InterpValue le, end_value.Le(start_value));
  if (le.IsTrue()) {
    ctx->warnings()->Add(
        node->span(), WarningKind::kEmptyRangeLiteral,
        absl::StrFormat("`%s` from `%s` to `%s` is an empty range",
                        node->ToString(), start_value.ToString(),
                        end_value.ToString()));
  }

  InterpValue array_size = InterpValue::MakeUnit();
  XLS_ASSIGN_OR_RETURN(InterpValue start_ge_end, start_value.Ge(end_value));
  if (start_ge_end.IsTrue()) {
    array_size = InterpValue::MakeU32(0);
  } else {
    XLS_ASSIGN_OR_RETURN(array_size, end_value.Sub(start_value));
  }
  return std::make_unique<ArrayType>(std::move(start_type),
                                     ConcreteTypeDim(array_size));
}

// Generic function to do the heavy lifting of deducing the type of an
// Invocation or Spawn's constituent functions.
static absl::StatusOr<TypeAndParametricEnv> DeduceInstantiation(
    DeduceCtx* ctx, const Invocation* invocation,
    const std::vector<InstantiateArg>& args,
    const std::function<absl::StatusOr<Function*>(const Instantiation*,
                                                  DeduceCtx*)>& resolve_fn,
    const absl::flat_hash_map<std::variant<const Param*, const ProcMember*>,
                              InterpValue>& constexpr_env = {}) {
  bool is_parametric_fn = false;
  // We can't resolve builtins as AST Functions, hence this check.
  if (!IsBuiltinFn(invocation->callee())) {
    XLS_ASSIGN_OR_RETURN(Function * f, resolve_fn(invocation, ctx));
    is_parametric_fn = f->IsParametric() || f->proc().has_value();
  }

  // If this is a parametric function invocation, then we need to typecheck
  // the resulting [function] instantiation before we can deduce its return
  // type (or else we won't find it in our TypeInfo).
  if (IsBuiltinFn(invocation->callee()) || is_parametric_fn) {
    return ctx->typecheck_invocation()(ctx, invocation, constexpr_env);
  }

  // If it's non-parametric, then we assume it's been already checked at module
  // top.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> callee_type,
                       ctx->Deduce(invocation->callee()));
  return TypeAndParametricEnv{down_cast<FunctionType*>(callee_type.get())
                                  ->return_type()
                                  .CloneToUnique(),
                              {}};
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceSpawn(const Spawn* node,
                                                          DeduceCtx* ctx) {
  const ParametricEnv& caller_parametric_env =
      ctx->fn_stack().back().parametric_env();
  XLS_VLOG(5) << "Deducing type for invocation: " << node->ToString()
              << " caller symbolic bindings: " << caller_parametric_env;

  auto resolve_proc = [](const Instantiation* node,
                         DeduceCtx* ctx) -> absl::StatusOr<Proc*> {
    Expr* callee = node->callee();
    Proc* proc;
    if (auto* colon_ref = dynamic_cast<ColonRef*>(callee)) {
      XLS_ASSIGN_OR_RETURN(proc, ResolveColonRefToProc(colon_ref, ctx));
    } else {
      auto* name_ref = dynamic_cast<NameRef*>(callee);
      XLS_RET_CHECK(name_ref != nullptr);
      const std::string& callee_name = name_ref->identifier();
      XLS_ASSIGN_OR_RETURN(
          proc, GetMemberOrTypeInferenceError<Proc>(ctx->module(), callee_name,
                                                    name_ref->span()));
    }
    return proc;
  };

  XLS_ASSIGN_OR_RETURN(Proc * proc, resolve_proc(node, ctx));
  auto resolve_config = [proc](const Instantiation* node,
                               DeduceCtx* ctx) -> absl::StatusOr<Function*> {
    return proc->config();
  };
  auto resolve_next = [proc](const Instantiation* node,
                             DeduceCtx* ctx) -> absl::StatusOr<Function*> {
    return proc->next();
  };

  auto resolve_init = [proc](const Instantiation* node,
                             DeduceCtx* ctx) -> absl::StatusOr<Function*> {
    return proc->init();
  };

  XLS_ASSIGN_OR_RETURN(
      TypeAndParametricEnv init_tab,
      DeduceInstantiation(ctx, down_cast<Invocation*>(node->next()->args()[0]),
                          {}, resolve_init, {}));

  // Gather up the type of all the (actual) arguments.
  std::vector<InstantiateArg> config_args;
  XLS_RETURN_IF_ERROR(InstantiateParametricArgs(
      node, node->callee(), node->config()->args(), ctx, &config_args));

  std::vector<InstantiateArg> next_args;
  next_args.push_back(
      InstantiateArg{std::make_unique<TokenType>(), node->span()});
  XLS_RETURN_IF_ERROR(InstantiateParametricArgs(
      node, node->callee(), node->next()->args(), ctx, &next_args));

  // For each [constexpr] arg, mark the associated Param as constexpr.
  absl::flat_hash_map<std::variant<const Param*, const ProcMember*>,
                      InterpValue>
      constexpr_env;
  size_t argc = node->config()->args().size();
  size_t paramc = proc->config()->params().size();
  if (argc != paramc) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("spawn had wrong argument count; want: %d got: %d",
                        paramc, argc));
  }
  for (int i = 0; i < node->config()->args().size(); i++) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue value,
        ConstexprEvaluator::EvaluateToValue(
            ctx->import_data(), ctx->type_info(), ctx->warnings(),
            GetCurrentParametricEnv(ctx), node->config()->args()[i], nullptr));
    constexpr_env.insert({proc->config()->params()[i], value});
  }

  // TODO(rspringer): 2022-05-26: We can't currently lazily evaluate `next` args
  // in the BytecodeEmitter, since that'd lead to a circular dependency between
  // it and the ConstexprEvaluator, so we have to do it eagerly here.
  // Un-wind that, if possible.
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(),
      GetCurrentParametricEnv(ctx),
      down_cast<Invocation*>(node->next()->args()[0]),
      /*concrete_type=*/nullptr));

  XLS_ASSIGN_OR_RETURN(TypeAndParametricEnv tab,
                       DeduceInstantiation(ctx, node->config(), config_args,
                                           resolve_config, constexpr_env));

  XLS_ASSIGN_OR_RETURN(TypeInfo * config_ti,
                       ctx->type_info()->GetInvocationTypeInfoOrError(
                           node->config(), tab.parametric_env));

  // Now we need to get the [constexpr] Proc member values so we can set them
  // when typechecking the `next` function. Those values are the elements in the
  // `config` function's terminating XlsTuple.
  // 1. Get the last statement in the `config` function.
  Function* config_fn = proc->config();
  Expr* last =
      std::get<Expr*>(config_fn->body()->statements().back()->wrapped());
  const XlsTuple* tuple = dynamic_cast<const XlsTuple*>(last);
  XLS_RET_CHECK_NE(tuple, nullptr);

  // 2. Extract the value of each element and associate with the corresponding
  // Proc member (in decl. order).
  constexpr_env.clear();
  XLS_RET_CHECK_EQ(tuple->members().size(), proc->members().size());
  for (int i = 0; i < tuple->members().size(); i++) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue value,
        ConstexprEvaluator::EvaluateToValue(
            ctx->import_data(), config_ti, ctx->warnings(),
            GetCurrentParametricEnv(ctx), tuple->members()[i], nullptr));
    constexpr_env.insert({proc->members()[i], value});
  }

  XLS_RETURN_IF_ERROR(DeduceInstantiation(ctx, node->next(), next_args,
                                          resolve_next, constexpr_env)
                          .status());
  return ConcreteType::MakeUnit();
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceMapInvocation(
    const Invocation* node, DeduceCtx* ctx) {
  const absl::Span<Expr* const>& args = node->args();
  if (args.size() != 2) {
    return ArgCountMismatchErrorStatus(
        node->span(),
        absl::StrFormat(
            "Expected 2 arguments to `map` builtin but got %d argument(s).",
            args.size()));
  }

  // First, get the input element type.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> arg0_type,
                       DeduceAndResolve(args[0], ctx));

  // Then get the type and bindings for the mapping fn.
  Invocation* element_invocation =
      CreateElementInvocation(ctx->module(), node->span(), args[1], args[0]);
  XLS_ASSIGN_OR_RETURN(TypeAndParametricEnv tab,
                       ctx->typecheck_invocation()(ctx, element_invocation,
                                                   /*constexpr_env=*/{}));
  const ParametricEnv& caller_bindings =
      ctx->fn_stack().back().parametric_env();
  ctx->type_info()->AddInvocationCallBindings(node, caller_bindings,
                                              tab.parametric_env);

  std::optional<TypeInfo*> dti = ctx->type_info()->GetInvocationTypeInfo(
      element_invocation, tab.parametric_env);
  if (dti.has_value()) {
    ctx->type_info()->SetInvocationTypeInfo(node, tab.parametric_env,
                                            dti.value());
  }

  ArrayType* arg0_array_type = dynamic_cast<ArrayType*>(arg0_type.get());
  return std::make_unique<ArrayType>(tab.type->CloneToUnique(),
                                     arg0_array_type->size());
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceInvocation(
    const Invocation* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "Deducing type for invocation: " << node->ToString();

  // Detect direct recursion. Indirect recursion is currently not syntactically
  // possible (as of 2023-08-22) since you cannot refer to a name that has not
  // yet been defined in the grammar.
  const FnStackEntry& entry = ctx->fn_stack().back();
  if (entry.f() != nullptr &&
      IsNameRefTo(node->callee(), entry.f()->name_def())) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Recursion of function `%s` detected -- recursion is "
                        "currently unsupported.",
                        node->callee()->ToString()));
  }

  // Map is special.
  if (IsBuiltinFn(node->callee(), "map")) {
    return DeduceMapInvocation(node, ctx);
  }

  // Gather up the type of all the (actual) arguments.
  std::vector<InstantiateArg> args;
  XLS_RETURN_IF_ERROR(InstantiateParametricArgs(node, node->callee(),
                                                node->args(), ctx, &args));

  // Find the callee as a DSLX Function from the expression.
  auto resolve_fn = [](const Instantiation* node,
                       DeduceCtx* ctx) -> absl::StatusOr<Function*> {
    Expr* callee = node->callee();
    Function* fn;
    if (auto* colon_ref = dynamic_cast<ColonRef*>(callee);
        colon_ref != nullptr) {
      XLS_ASSIGN_OR_RETURN(fn,
                           ResolveColonRefToFnForInvocation(colon_ref, ctx));
    } else if (auto* name_ref = dynamic_cast<NameRef*>(callee);
               name_ref != nullptr) {
      const std::string& callee_name = name_ref->identifier();
      XLS_ASSIGN_OR_RETURN(fn,
                           GetMemberOrTypeInferenceError<Function>(
                               ctx->module(), callee_name, name_ref->span()));
    } else {
      return TypeInferenceErrorStatus(
          node->span(), nullptr,
          absl::StrCat("An invocation callee must be either a name reference "
                       "or a colon reference; instead got: ",
                       AstNodeKindToString(callee->kind())));
    }
    return fn;
  };

  XLS_ASSIGN_OR_RETURN(TypeAndParametricEnv tab,
                       DeduceInstantiation(ctx, node, args, resolve_fn));

  ConcreteType* ct = ctx->type_info()->GetItem(node->callee()).value();
  FunctionType* ft = dynamic_cast<FunctionType*>(ct);
  if (args.size() != ft->params().size()) {
    return ArgCountMismatchErrorStatus(
        node->span(),
        absl::StrFormat("Expected %d parameter(s) but got %d arguments.",
                        ft->params().size(), node->args().size()));
  }

  for (int i = 0; i < args.size(); i++) {
    if (*args[i].type() != *ft->params()[i]) {
      return ctx->TypeMismatchError(
          args[i].span(), nullptr, *ft->params()[i], nullptr, *args[i].type(),
          "Mismatch between parameter and argument types.");
    }
  }

  // We can't blindly resolve the function or else we might fail due to look up
  // a parametric builtin.
  if (!IsBuiltinFn(node->callee())) {
    XLS_ASSIGN_OR_RETURN(Function * fn, resolve_fn(node, ctx));

    // If the callee function needs an implicit token type (e.g. because it has
    // a fail!() or cover!() operation transitively) then so do we.
    if (std::optional<bool> callee_opt = ctx->import_data()
                                              ->GetRootTypeInfoForNode(fn)
                                              .value()
                                              ->GetRequiresImplicitToken(fn);
        callee_opt.value()) {
      UseImplicitToken(ctx);
    }
  }

  return std::move(tab.type);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceFormatMacro(
    const FormatMacro* node, DeduceCtx* ctx) {
  int64_t arg_count = OperandsExpectedByFormat(node->format());

  if (arg_count != node->args().size()) {
    return ArgCountMismatchErrorStatus(
        node->span(),
        absl::StrFormat("%s macro expects %d argument(s) from format but has "
                        "%d argument(s)",
                        node->macro(), arg_count, node->args().size()));
  }

  for (Expr* arg : node->args()) {
    XLS_RETURN_IF_ERROR(DeduceAndResolve(arg, ctx).status());
  }

  // trace_fmt! (and any future friends) require threading implicit tokens for
  // control just like cover! and fail! do.
  UseImplicitToken(ctx);

  return std::make_unique<TokenType>();
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceZeroMacro(
    const ZeroMacro* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceZeroMacro; node: `" << node->ToString() << "`";
  // Note: since it's a macro the parser checks arg count and parametric count.
  //
  // This says the type of the parametric type arg is the type of the result.
  // However, we have to check that all of the transitive type within the
  // parametric argument type are "zero capable".
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> parametric_type,
                       DeduceAndResolve(ToAstNode(node->type()), ctx));
  XLS_ASSIGN_OR_RETURN(parametric_type,
                       UnwrapMetaType(std::move(parametric_type), node->span(),
                                      "zero! macro type"));
  XLS_RET_CHECK(!parametric_type->IsMeta());

  XLS_ASSIGN_OR_RETURN(
      InterpValue value,
      MakeZeroValue(*parametric_type, *ctx->import_data(), node->span()));
  ctx->type_info()->NoteConstExpr(node, value);
  return parametric_type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceNameRef(const NameRef* node,
                                                            DeduceCtx* ctx) {
  AstNode* name_def = ToAstNode(node->name_def());
  XLS_RET_CHECK(name_def != nullptr);

  std::optional<ConcreteType*> item = ctx->type_info()->GetItem(name_def);
  if (item.has_value()) {
    auto concrete_type = (*item)->CloneToUnique();
    return concrete_type;
  }

  // If this has no corresponding type because it is a parametric function that
  // is not being invoked, we give an error instead of propagating
  // "TypeMissing".
  if (IsParametricFunction(node->GetDefiner()) &&
      !ParentIsInvocationWithCallee(node)) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat(
            "Name '%s' is a parametric function, but it is not being invoked",
            node->identifier()));
  }

  return TypeMissingErrorStatus(/*node=*/*name_def, /*user=*/node);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceConstRef(
    const ConstRef* node, DeduceCtx* ctx) {
  XLS_VLOG(3) << "DeduceConstRef; node: `" << node->ToString() << "` @ "
              << node->span();
  // ConstRef is a subtype of NameRef, same deduction rule works.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       DeduceNameRef(node, ctx));
  XLS_ASSIGN_OR_RETURN(InterpValue value,
                       ctx->type_info()->GetConstExpr(node->name_def()));
  XLS_VLOG(3) << " DeduceConstRef; value: " << value.ToString();
  ctx->type_info()->NoteConstExpr(node, std::move(value));
  return type;
}

class DeduceVisitor : public AstNodeVisitor {
 public:
  explicit DeduceVisitor(DeduceCtx* ctx) : ctx_(ctx) {}

#define DEDUCE_DISPATCH(__type)                           \
  absl::Status Handle##__type(const __type* n) override { \
    result_ = Deduce##__type(n, ctx_);                    \
    return result_.status();                              \
  }

  DEDUCE_DISPATCH(Unop)
  DEDUCE_DISPATCH(Param)
  DEDUCE_DISPATCH(ProcMember)
  DEDUCE_DISPATCH(ConstantDef)
  DEDUCE_DISPATCH(Number)
  DEDUCE_DISPATCH(String)
  DEDUCE_DISPATCH(TypeRef)
  DEDUCE_DISPATCH(TypeAlias)
  DEDUCE_DISPATCH(XlsTuple)
  DEDUCE_DISPATCH(Conditional)
  DEDUCE_DISPATCH(Binop)
  DEDUCE_DISPATCH(EnumDef)
  DEDUCE_DISPATCH(Let)
  DEDUCE_DISPATCH(For)
  DEDUCE_DISPATCH(Cast)
  DEDUCE_DISPATCH(ConstAssert)
  DEDUCE_DISPATCH(StructDef)
  DEDUCE_DISPATCH(Array)
  DEDUCE_DISPATCH(Attr)
  DEDUCE_DISPATCH(Block)
  DEDUCE_DISPATCH(ChannelDecl)
  DEDUCE_DISPATCH(ConstantArray)
  DEDUCE_DISPATCH(ColonRef)
  DEDUCE_DISPATCH(Index)
  DEDUCE_DISPATCH(Match)
  DEDUCE_DISPATCH(Range)
  DEDUCE_DISPATCH(Spawn)
  DEDUCE_DISPATCH(SplatStructInstance)
  DEDUCE_DISPATCH(Statement)
  DEDUCE_DISPATCH(StructInstance)
  DEDUCE_DISPATCH(TupleIndex)
  DEDUCE_DISPATCH(UnrollFor)
  DEDUCE_DISPATCH(BuiltinTypeAnnotation)
  DEDUCE_DISPATCH(ChannelTypeAnnotation)
  DEDUCE_DISPATCH(ArrayTypeAnnotation)
  DEDUCE_DISPATCH(TupleTypeAnnotation)
  DEDUCE_DISPATCH(TypeRefTypeAnnotation)
  DEDUCE_DISPATCH(MatchArm)
  DEDUCE_DISPATCH(Invocation)
  DEDUCE_DISPATCH(FormatMacro)
  DEDUCE_DISPATCH(ZeroMacro)
  DEDUCE_DISPATCH(ConstRef)
  DEDUCE_DISPATCH(NameRef)

  // Unhandled nodes for deduction, either they are custom visited or not
  // visited "automatically" in the traversal process (e.g. top level module
  // members).
  absl::Status HandleProc(const Proc* n) override { return Fatal(n); }
  absl::Status HandleSlice(const Slice* n) override { return Fatal(n); }
  absl::Status HandleImport(const Import* n) override { return Fatal(n); }
  absl::Status HandleFunction(const Function* n) override { return Fatal(n); }
  absl::Status HandleQuickCheck(const QuickCheck* n) override {
    return Fatal(n);
  }
  absl::Status HandleTestFunction(const TestFunction* n) override {
    return Fatal(n);
  }
  absl::Status HandleTestProc(const TestProc* n) override { return Fatal(n); }
  absl::Status HandleModule(const Module* n) override { return Fatal(n); }
  absl::Status HandleWidthSlice(const WidthSlice* n) override {
    return Fatal(n);
  }
  absl::Status HandleNameDefTree(const NameDefTree* n) override {
    return Fatal(n);
  }
  absl::Status HandleNameDef(const NameDef* n) override { return Fatal(n); }
  absl::Status HandleBuiltinNameDef(const BuiltinNameDef* n) override {
    return Fatal(n);
  }
  absl::Status HandleParametricBinding(const ParametricBinding* n) override {
    return Fatal(n);
  }
  absl::Status HandleWildcardPattern(const WildcardPattern* n) override {
    return Fatal(n);
  }

  absl::StatusOr<std::unique_ptr<ConcreteType>>& result() { return result_; }

 private:
  absl::Status Fatal(const AstNode* n) {
    XLS_LOG(FATAL) << "Got unhandled AST node for deduction: " << n->ToString();
  }

  DeduceCtx* ctx_;
  absl::StatusOr<std::unique_ptr<ConcreteType>> result_;
};

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceInternal(
    const AstNode* node, DeduceCtx* ctx) {
  DeduceVisitor visitor(ctx);
  XLS_RETURN_IF_ERROR(node->Accept(&visitor));
  return std::move(visitor.result());
}

absl::StatusOr<std::unique_ptr<ConcreteType>> ResolveViaEnv(
    const ConcreteType& type, const ParametricEnv& parametric_env) {
  ParametricExpression::Env env;
  for (const auto& [k, v] : parametric_env.bindings()) {
    env[k] = v;
  }

  return type.MapSize([&](const ConcreteTypeDim& dim)
                          -> absl::StatusOr<ConcreteTypeDim> {
    if (std::holds_alternative<ConcreteTypeDim::OwnedParametric>(dim.value())) {
      const auto& parametric =
          std::get<ConcreteTypeDim::OwnedParametric>(dim.value());
      return ConcreteTypeDim(parametric->Evaluate(env));
    }
    return dim;
  });
}

}  // namespace

absl::StatusOr<std::unique_ptr<ConcreteType>> Resolve(const ConcreteType& type,
                                                      DeduceCtx* ctx) {
  XLS_RET_CHECK(!ctx->fn_stack().empty());
  const FnStackEntry& entry = ctx->fn_stack().back();
  const ParametricEnv& fn_parametric_env = entry.parametric_env();
  return ResolveViaEnv(type, fn_parametric_env);
}

absl::StatusOr<std::unique_ptr<ConcreteType>> Deduce(const AstNode* node,
                                                     DeduceCtx* ctx) {
  XLS_RET_CHECK(node != nullptr);
  if (std::optional<ConcreteType*> type = ctx->type_info()->GetItem(node)) {
    return (*type)->CloneToUnique();
  }
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type,
                       DeduceInternal(node, ctx));
  XLS_RET_CHECK(type != nullptr);
  ctx->type_info()->SetItem(node, *type);
  XLS_VLOG(5) << absl::StreamFormat(
      "Deduced type of `%s` @ %p (kind: %s) => %s in %p", node->ToString(),
      node, node->GetNodeTypeName(), type->ToString(), ctx->type_info());

  return type;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> DeduceAndResolve(
    const AstNode* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> deduced,
                       ctx->Deduce(node));
  return Resolve(*deduced, ctx);
}

// Converts a sequence of ParametricBinding AST nodes into a sequence of
// ParametricConstraints (which decorate the ParametricBinding nodes with their
// deduced ConcreteTypes).
absl::StatusOr<std::vector<ParametricConstraint>>
ParametricBindingsToConstraints(absl::Span<ParametricBinding* const> bindings,
                                DeduceCtx* ctx) {
  std::vector<ParametricConstraint> parametric_constraints;
  for (ParametricBinding* binding : bindings) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> binding_type,
                         ParametricBindingToType(binding, ctx));
    parametric_constraints.push_back(
        ParametricConstraint(*binding, std::move(binding_type)));
  }
  return parametric_constraints;
}

}  // namespace xls::dslx
