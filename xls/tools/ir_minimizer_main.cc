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

#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/file/temp_file.h"
#include "xls/common/init_xls.h"
#include "xls/common/logging/log_lines.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/subprocess.h"
#include "xls/data_structures/binary_search.h"
#include "xls/data_structures/inline_bitmap.h"
#include "xls/interpreter/function_interpreter.h"
#include "xls/ir/call_graph.h"
#include "xls/ir/channel.h"
#include "xls/ir/channel_ops.h"
#include "xls/ir/events.h"
#include "xls/ir/function_base.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/node.h"
#include "xls/ir/node_iterator.h"
#include "xls/ir/node_util.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/source_location.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/ir/value_helpers.h"
#include "xls/ir/verifier.h"
#include "xls/jit/function_jit.h"
#include "xls/passes/arith_simplification_pass.h"
#include "xls/passes/array_simplification_pass.h"
#include "xls/passes/bit_slice_simplification_pass.h"
#include "xls/passes/concat_simplification_pass.h"
#include "xls/passes/constant_folding_pass.h"
#include "xls/passes/cse_pass.h"
#include "xls/passes/dce_pass.h"
#include "xls/passes/dfe_pass.h"
#include "xls/passes/inlining_pass.h"
#include "xls/passes/map_inlining_pass.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/optimization_pass_pipeline.h"
#include "xls/passes/pass_base.h"
#include "xls/passes/proc_state_flattening_pass.h"
#include "xls/passes/proc_state_optimization_pass.h"
#include "xls/passes/tuple_simplification_pass.h"
#include "xls/passes/unroll_pass.h"

const char* kUsage = R"(
Tool for reducing IR to a minimal test case based on an external test.

Selectively removes nodes from the graph and performs various simplifications
(e.g., CSE, DCE, etc.) while preserving the feature/bug indicated by an
external test.

Note that this is currently specialized to reduce fuzz functions, which are
fairly self contained. Reduction may not be as effective with larger samples
(like real user packages).

Currently the algorithm is such that the number of live nodes in the function
should go down monotonically.

The reducer supports two modes of operation. In the first mode, an external test
executable is passed in. This test should return a zero exit status if the IR
exhibits the bug. Example invocation:

  ir_minimizer_main --test_executable=/foo/test.sh IR_FILE

The second mode specifically reduces a test case where the JIT results differ
from the interpreter results. Example invocation:

  ir_minimizer_main --test_llvm_jit --use_optimization_pipeline \
    --input='bits[32]:42; bits[1]:0' IR_FILE

)";

ABSL_FLAG(bool, can_remove_params, false,
          "Whether parameters can be removed during the minimization process. "
          "If the test executable interprets the IR using a fixed set of "
          "arguments, parameters should not be removed.");
ABSL_FLAG(int64_t, failed_attempt_limit, 256,
          "Failed simplification attempts (in a row) before we conclude we're "
          "done reducing.");
ABSL_FLAG(int64_t, total_attempt_limit, 16384,
          "Limit on total number of attempts to try before bailing.");
ABSL_FLAG(
    std::string, test_executable, "",
    "Path to test executable to run during minimization. The test accepts "
    "a single command line argument which is the path to the textual IR "
    "file. The test should return a zero exit code (success) if the IR "
    "exhibits the bug in question. Note, if testing for a crash of a "
    "particular binary this is the opposite return code polarity.");
ABSL_FLAG(std::vector<std::string>, test_executable_args, {},
          "Flags to pass to the '--test_executable'.");
ABSL_FLAG(bool, test_executable_crash_is_bug, false,
          "If true a crash of the test executable is considered a bug repro, "
          "normal termination is considered a passing run.");
ABSL_FLAG(bool, test_llvm_jit, false,
          "Tests for differences between results from the JIT and the "
          "interpreter as the reduction test case. Must specify --input with "
          "this flag.");
ABSL_FLAG(std::string, input, "",
          "Input to use when invoking the JIT and the interpreter. Must be "
          "used with --test_llvm_jit.");
ABSL_FLAG(
    std::string, test_only_inject_jit_result, "",
    "Test-only flag for injecting the result produced by the JIT. Used to "
    "force mismatches between JIT and interpreter for testing purposed.");
ABSL_FLAG(bool, use_optimization_pipeline, false,
          "If true, then include the standard optimization pipeline as a "
          "simplification option during reduction. This option should *not* be "
          "used if trying to reduce a crash in the optimization pipeline "
          "itself as the minimizer will then crash.");
ABSL_FLAG(
    bool, use_optimization_passes, true,
    "If true, then as a simplification option, run a pass selected randomly "
    "from a subset of optimization passes. This flag differs from"
    "--use_optimization_pipeline in that only a subset of passes is "
    "used rather than the entire pipeline. So this flag can be used to reduce "
    "an input which crashes an optimization outside this selected subset. "
    "Also, because this option runs a single pass at a time it often results "
    "in more minimization than --use_optimization_pipeline which "
    "which might optimize away the problematic bit of IR entirely.");
ABSL_FLAG(bool, can_remove_sends, false,
          "If true, then the minimizer may remove sends.");
ABSL_FLAG(bool, can_remove_receives, false,
          "If true, then the minimizer may remove receives.");
ABSL_FLAG(std::vector<std::string>, preserve_channels, {},
          "Preserve IO ops on the given channel names during minimization. "
          "This is useful when minimizing with a script that runs the "
          "scheduler with IO constraints.");
ABSL_FLAG(std::string, top, "",
          "The name of the top entity. Currently, only procs and functions are "
          "supported. Entry function to use during minimization.");
ABSL_FLAG(int64_t, simplifications_between_tests, 1,
          "Number of simplifications to do in-between tests. Increasing this "
          "value may speed minimization for large designs, especially when "
          "--test_executable is long-running.");
ABSL_FLAG(int64_t, failed_attempts_between_tests_limit, 16,
          "Failed simplification attempts between tests before we conclude we "
          "need to check our changes so far.");
ABSL_FLAG(
    bool, verify_ir, true,
    "Verify IR whenever parsing. In most cases, this is a good check that the "
    "simplifications the minimizer makes are sound, but in cases where mostly "
    "well-formed IR fails to parse, it is useful to disable IR verification so "
    "the minimizer can proceed and help you understand why it fails to "
    "verify.");
ABSL_FLAG(bool, simplify_top_only, false,
          "If true only the top function/proc/block will be actively "
          "simplified. Otherwise each simplification will use a function "
          "weighted by node-count and node selected randomly in the function.");
ABSL_FLAG(
    bool, can_inline_everything, true,
    "Whether inlining everything into TOP as a single operation is an allowed "
    "simplification. This is independent of --can_inline");
ABSL_FLAG(
    bool, can_inline, true,
    "Whether individual invokes & maps can be inlined as a simplification.");

namespace xls {
namespace {

absl::StatusOr<std::unique_ptr<Package>> ParsePackage(
    std::string_view ir_text) {
  std::optional<std::string_view> entry;
  if (!absl::GetFlag(FLAGS_top).empty()) {
    entry = absl::GetFlag(FLAGS_top);
  }
  // Do not verify unless the flag is set- we may want to minimize IR that fails
  // to minimize.
  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<Package> package,
      Parser::ParsePackageNoVerify(ir_text, /*filename=*/std::nullopt,
                                   /*entry=*/entry));
  if (absl::GetFlag(FLAGS_verify_ir)) {
    XLS_RETURN_IF_ERROR(VerifyPackage(package.get()));
  }
  return package;
}

// Checks whether we still fail when attempting to run function "f". Optional
// 'inputs' is required if --test_llvm_jit is used.
absl::StatusOr<bool> StillFailsHelper(
    std::string_view ir_text, std::optional<std::vector<Value>> inputs) {
  if (!absl::GetFlag(FLAGS_test_executable).empty()) {
    // Verify script exists and is executable.
    absl::Status exists_status =
        FileExists(absl::GetFlag(FLAGS_test_executable));
    XLS_QCHECK(exists_status.ok() || absl::IsNotFound(exists_status))
        << absl::StreamFormat("Unable to access test executable %s: %s",
                              absl::GetFlag(FLAGS_test_executable),
                              exists_status.message());
    XLS_QCHECK(!absl::IsNotFound(exists_status)) << absl::StreamFormat(
        "Test executable %s not found", absl::GetFlag(FLAGS_test_executable));
    XLS_ASSIGN_OR_RETURN(
        bool is_executable,
        FileIsExecutable(absl::GetFlag(FLAGS_test_executable)));
    XLS_QCHECK(is_executable)
        << absl::StreamFormat("Test executable %s is not executable",
                              absl::GetFlag(FLAGS_test_executable));

    // Test for bug using external executable.
    XLS_ASSIGN_OR_RETURN(TempFile temp_file,
                         TempFile::CreateWithContent(ir_text));
    std::string ir_path = temp_file.path().string();

    XLS_QCHECK(!absl::GetFlag(FLAGS_test_llvm_jit))
        << "Cannot specify --test_llvm_jit with --test_executable";
    XLS_QCHECK(absl::GetFlag(FLAGS_input).empty())
        << "Cannot specify --input with --test_executable";
    std::vector<std::string> argv;
    argv.reserve(2 + absl::GetFlag(FLAGS_test_executable_args).size());
    argv.push_back(absl::GetFlag(FLAGS_test_executable));
    absl::c_copy(absl::GetFlag(FLAGS_test_executable_args),
                 std::back_inserter(argv));
    argv.push_back(ir_path);

    XLS_ASSIGN_OR_RETURN(SubprocessResult subproc_result,
                         InvokeSubprocess(argv));

    if (subproc_result.exit_status != 0) {
      XLS_VLOG(2) << "stdout:  \"\"\"" << subproc_result.stdout << "\"\"\"";
      XLS_VLOG(2) << "stderr:  \"\"\"" << subproc_result.stderr << "\"\"\"";
      XLS_VLOG(2) << "retcode: " << subproc_result.exit_status;
    }
    if (absl::GetFlag(FLAGS_test_executable_crash_is_bug)) {
      return !subproc_result.normal_termination;
    }
    return subproc_result.exit_status == 0;
  }

  // Test for bugs by comparing the results of the JIT and interpreter.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> package, ParsePackage(ir_text));
  XLS_RET_CHECK(inputs.has_value());
  XLS_ASSIGN_OR_RETURN(Function * main, package->GetTopAsFunction());
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<FunctionJit> jit,
                       FunctionJit::Create(main));
  InterpreterResult<Value> jit_result;
  if (absl::GetFlag(FLAGS_test_only_inject_jit_result).empty()) {
    XLS_ASSIGN_OR_RETURN(jit_result, jit->Run(*inputs));
  } else {
    XLS_ASSIGN_OR_RETURN(jit_result.value,
                         Parser::ParseTypedValue(
                             absl::GetFlag(FLAGS_test_only_inject_jit_result)));
  }
  // TODO(https://github.com/google/xls/issues/506): 2021-10-12 Also compare
  // events once the JIT fully supports them. One potential concern in this area
  // is making sure that the kind of mismatch (value, assertion failure or trace
  // messages) stays the same as the code is minimized. Leaving the comparison
  // value-only avoids that issue for now.
  XLS_ASSIGN_OR_RETURN(InterpreterResult<Value> interpreter_result,
                       InterpretFunction(main, *inputs));
  return jit_result.value != interpreter_result.value;
}

// Wrapper around StillFails which memoizes the result. Optional test_cache is
// used to memoize the results of testing the given IR.
absl::StatusOr<bool> StillFails(
    std::string_view ir_text, std::optional<std::vector<Value>> inputs,
    absl::flat_hash_map<std::string, bool>* test_cache) {
  XLS_VLOG(1) << "=== Verifying contents still fails";
  XLS_VLOG_LINES(2, ir_text);

  if (test_cache != nullptr) {
    auto it = test_cache->find(ir_text);
    if (it != test_cache->end()) {
      XLS_LOG(INFO) << absl::StreamFormat("Found result in cache (failed = %d)",
                                          it->second);
      return it->second;
    }
  }

  XLS_ASSIGN_OR_RETURN(bool result, StillFailsHelper(ir_text, inputs));
  if (test_cache != nullptr) {
    (*test_cache)[ir_text] = result;
  }
  return result;
}

// Writes the IR out to a temporary file, runs the test executable on it, and
// returns 'true' if the test (still) fails on that IR text.  Optional test
// cache is used to memoize the results of testing the given IR.
absl::Status VerifyStillFails(
    std::string_view ir_text, std::optional<std::vector<Value>> inputs,
    std::string_view description,
    absl::flat_hash_map<std::string, bool>* test_cache) {
  XLS_ASSIGN_OR_RETURN(bool still_fails,
                       StillFails(ir_text, inputs, test_cache));

  if (!still_fails) {
    return absl::FailedPreconditionError(
        absl::StrCat("Unexpected PASS: ", description, "\n\nIR\n", ir_text));
  }

  XLS_VLOG(1) << "Confirmed: sample still fails.";
  return absl::OkStatus();
}

// Removes params with zero users from the function.
absl::StatusOr<bool> RemoveDeadParameters(FunctionBase* f) {
  if (f->IsProc()) {
    Proc* p = f->AsProcOrDie();
    absl::flat_hash_set<Node*> dead_state_params;
    for (Param* state_param : p->StateParams()) {
      if (state_param->IsDead()) {
        dead_state_params.insert(state_param);
      }
      XLS_ASSIGN_OR_RETURN(int64_t index, p->GetStateParamIndex(state_param));
      // Replace all uses of invariant state elements (i.e.: ones where
      // next[i] = param[i]) with a literal of the initial value.
      if (state_param == p->GetNextStateElement(index)) {
        Value init_value = p->GetInitValueElement(index);
        XLS_RETURN_IF_ERROR(
            state_param->ReplaceUsesWithNew<Literal>(init_value).status());
        dead_state_params.insert(state_param);
      }
    }
    bool changed = false;
    for (Node* dead : dead_state_params) {
      XLS_ASSIGN_OR_RETURN(int64_t index,
                           p->GetStateParamIndex(dead->As<Param>()));
      XLS_RETURN_IF_ERROR(p->RemoveStateElement(index));
      changed = true;
    }

    return changed;
  }
  if (f->IsFunction()) {
    // Remove dead parameters and get rid of the uses in the linked invokes.
    std::vector<Node*> invokes = GetNodesWhichCall(f->AsFunctionOrDie());
    if (!absl::c_all_of(invokes, [](Node* n) { return n->Is<Invoke>(); })) {
      // TODO(allight) 2023-11-16: Support removing params from non-invoke
      // function calls (map, counted-for, etc).
      return false;
    }
    std::vector<Param*> params(f->params().begin(), f->params().end());
    InlineBitmap removed(params.size(), /*fill=*/false);
    {
      int64_t i = 0;
      for (Param* p : params) {
        if (p->IsDead()) {
          XLS_RETURN_IF_ERROR(f->RemoveNode(p));
          removed.Set(i, true);
        }
        i++;
      }
    }
    if (params.size() == f->params().size()) {
      // Nothing changed.
      return false;
    }
    // Remove appropriate invoke arguments.
    for (Node* n : invokes) {
      // TODO(allight) 2023-11-16: Support removing params from non-invoke
      // function calls (map, counted-for, etc).
      if (n->As<Invoke>()->to_apply() == f) {
        std::vector<Node*> new_args;
        for (int64_t i = 0; i < n->operand_count(); ++i) {
          if (!removed.Get(i)) {
            new_args.push_back(n->operand(i));
          }
        }
        XLS_RETURN_IF_ERROR(
            n->ReplaceUsesWithNew<Invoke>(new_args, f->AsFunctionOrDie())
                .status());
        // Avoid illegal (albeit unused) invokes.
        XLS_RETURN_IF_ERROR(n->function_base()->RemoveNode(n));
      }
    }
    return true;
  }
  XLS_LOG(FATAL) << "RemoveDeadParameters only handles procs and functions";
}

enum class SimplificationResult {
  kCannotChange,  // Cannot simplify.
  kDidNotChange,  // Did not simplify, e.g. because RNG didn't come up that way.
  kDidChange,     // Did simplify in some way.
};

// Return a random subset of the given input.
template <typename T>
std::vector<T> PickRandomSubset(absl::Span<const T> input,
                                absl::BitGenRef rng) {
  std::vector<T> result;
  result.reserve(input.size());
  // About half the time, drop about half of the elements.
  if (absl::Bernoulli(rng, 0.5)) {
    for (const T& x : input) {
      if (absl::Bernoulli(rng, 0.5)) {
        result.push_back(x);
      }
    }
    return result;
  }

  // Otherwise drop 1 element.
  int64_t dropped = absl::Uniform<int64_t>(rng, 0, result.size());
  for (int64_t i = 0; i < dropped; ++i) {
    result.push_back(input[i]);
  }
  for (int64_t i = dropped + 1; i < input.size(); ++i) {
    result.push_back(input[i]);
  }
  return result;
}

absl::StatusOr<SimplificationResult> ReplaceImplicitUse(Node* node,
                                                        Node* replacement) {
  FunctionBase* fb = node->function_base();
  if (fb->IsFunction()) {
    Function* f = fb->AsFunctionOrDie();
    if (node != f->return_value()) {
      return SimplificationResult::kDidNotChange;
    }
    XLS_RETURN_IF_ERROR(f->set_return_value(replacement));
    return SimplificationResult::kDidChange;
  }
  if (fb->IsProc()) {
    Proc* p = fb->AsProcOrDie();
    SimplificationResult changed = SimplificationResult::kDidNotChange;
    if (!node->GetType()->IsEqualTo(replacement->GetType())) {
      return SimplificationResult::kDidNotChange;
    }
    if (p->NextToken() == node) {
      XLS_RETURN_IF_ERROR(p->SetNextToken(replacement));
      changed = SimplificationResult::kDidChange;
    }
    for (int64_t i = 0; i < p->GetStateElementCount(); ++i) {
      if (p->GetNextStateElement(i) == node) {
        XLS_RETURN_IF_ERROR(p->SetNextStateElement(i, replacement));
        changed = SimplificationResult::kDidChange;
      }
    }
    return changed;
  }
  return SimplificationResult::kDidNotChange;
}

std::vector<Node*> ImplicitlyUsed(FunctionBase* fb) {
  if (fb->IsFunction()) {
    Function* f = fb->AsFunctionOrDie();
    return {f->return_value()};
  }
  if (fb->IsProc()) {
    Proc* p = fb->AsProcOrDie();
    std::vector<Node*> result(p->NextState().begin(), p->NextState().end());
    result.push_back(p->NextToken());
    return result;
  }
  XLS_LOG(FATAL) << "ImplicitlyUsed only supports functions and procs";
}

// Removes a random subset of elements from a compound typed value. That is,
// remove (potentially nested) tuple/array elements.
Value ReduceValue(const Value& value, absl::BitGenRef rng) {
  if (value.IsTuple() || value.IsArray()) {
    // Pick a random subset of the elements to keep.
    std::vector<Value> elements =
        PickRandomSubset<Value>(value.elements(), rng);
    if (value.IsTuple() || (value.IsArray() && elements.size() == 1)) {
      // Tuples and single element arrays can have their subelements
      // reduced. Multi-element arrays can't have subelements reduced because
      // all elements must be the same type.
      for (int64_t i = 0; i < elements.size(); ++i) {
        elements[i] = ReduceValue(elements[i], rng);
      }
    }
    if (value.IsTuple()) {
      return Value::Tuple(elements);
    }
    if (!elements.empty()) {
      return Value::ArrayOrDie(elements);
    }
  }
  return value;
}

absl::StatusOr<SimplificationResult> SimplifyReturnValue(
    FunctionBase* f, absl::BitGenRef rng, std::string* which_transform) {
  // For now we do not do the inter-procedural transforms needed to allow for
  // return-value simplification in non-top functions.
  // TODO(https://github.com/google/xls/issues/1207): This should be added at
  // some point.
  if (f != f->package()->GetTop()) {
    return SimplificationResult::kCannotChange;
  }

  Node* orig = nullptr;
  {
    std::vector<Node*> implicitly_used = ImplicitlyUsed(f);
    int64_t i = absl::Uniform<int64_t>(rng, 0, implicitly_used.size());
    orig = implicitly_used.at(i);
  }

  if (orig->GetType()->IsToken()) {
    return SimplificationResult::kDidNotChange;
  }

  // Try slicing array return values down to fewer elements.
  if (f->IsFunction() && orig->GetType()->IsArray() &&
      absl::Bernoulli(rng, 0.25) &&
      orig->GetType()->AsArrayOrDie()->size() > 1) {
    int64_t original_size = orig->GetType()->AsArrayOrDie()->size();
    int64_t new_size = absl::Uniform<int64_t>(rng, 1, original_size);
    XLS_ASSIGN_OR_RETURN(
        Node * zero,
        f->MakeNode<Literal>(orig->loc(),
                             ZeroOfType(f->package()->GetBitsType(1))));
    XLS_ASSIGN_OR_RETURN(
        Node * replacement,
        f->MakeNode<ArraySlice>(orig->loc(), orig, zero, new_size));
    XLS_ASSIGN_OR_RETURN(SimplificationResult changed,
                         ReplaceImplicitUse(orig, replacement));
    *which_transform = absl::StrFormat("array slice reduction: %d => %d",
                                       original_size, new_size);
    return changed;
  }

  // If the return value is a tuple, concat, or array, try to knock out some of
  // the operands which then become dead. Only possible for the top function.
  if (f->IsFunction() &&
      (orig->Is<Tuple>() || orig->Is<Concat>() || orig->Is<Array>()) &&
      absl::Bernoulli(rng, 0.5)) {
    std::vector<Node*> new_operands = PickRandomSubset(orig->operands(), rng);
    if (new_operands.size() < orig->operand_count()) {
      *which_transform =
          absl::StrFormat("return tuple/concat/array reduction: %d => %d",
                          orig->operand_count(), new_operands.size());
      Node* new_return_value;
      if (orig->Is<Tuple>()) {
        XLS_ASSIGN_OR_RETURN(new_return_value,
                             f->MakeNode<Tuple>(orig->loc(), new_operands));
      } else if (orig->Is<Array>()) {
        // XLS does not support empty arrays.
        if (new_operands.empty()) {
          return SimplificationResult::kDidNotChange;
        }
        XLS_ASSIGN_OR_RETURN(
            new_return_value,
            f->MakeNode<Array>(orig->loc(), new_operands,
                               new_operands.front()->GetType()));
      } else {
        XLS_RET_CHECK(orig->Is<Concat>());
        XLS_ASSIGN_OR_RETURN(new_return_value,
                             f->MakeNode<Concat>(orig->loc(), new_operands));
      }

      return ReplaceImplicitUse(orig, new_return_value);
    }
  }

  // If the return value is a tuple or array typed literal, try to remove some
  // of the elements.
  if (f->IsFunction() && orig->Is<Literal>() &&
      (orig->GetType()->IsTuple() || orig->GetType()->IsArray())) {
    const Value& orig_value = orig->As<Literal>()->value();
    Value reduced_value = ReduceValue(orig_value, rng);
    if (reduced_value != orig_value) {
      *which_transform = "return literal tuple/array size reduction";
      XLS_ASSIGN_OR_RETURN(Node * new_return_value,
                           f->MakeNode<Literal>(orig->loc(), reduced_value));
      return ReplaceImplicitUse(orig, new_return_value);
    }
  }

  // Try to replace the return value with an operand of the return value.
  if (orig->operand_count() > 0) {
    Node* replacement = nullptr;
    if (f->IsProc()) {
      std::vector<Node*> same_type_operands;
      for (Node* operand : orig->operands()) {
        if (operand->GetType()->IsEqualTo(orig->GetType())) {
          same_type_operands.push_back(operand);
        }
      }
      if (same_type_operands.empty()) {
        return SimplificationResult::kDidNotChange;
      }
      int64_t which = absl::Uniform<int64_t>(rng, 0, same_type_operands.size());
      replacement = same_type_operands.at(which);
      *which_transform =
          absl::StrFormat("replace next state node %s with operand %s",
                          orig->GetName(), replacement->GetName());
    }
    if (f->IsFunction()) {
      int64_t which_operand =
          absl::Uniform<int64_t>(rng, 0, orig->operand_count());
      replacement = orig->operand(which_operand);
      if (replacement->GetType() !=
              f->AsFunctionOrDie()->return_value()->GetType() &&
          f != f->package()->GetTop()) {
        // Can't change type of a non-top function.
        XLS_VLOG(1) << "Unable to change return type of a non-top function";
        return SimplificationResult::kDidNotChange;
      }
      *which_transform =
          absl::StrFormat("return operand %d of return value", which_operand);
    }
    XLS_CHECK_NE(replacement, nullptr);
    return ReplaceImplicitUse(orig, replacement);
  }

  XLS_VLOG(1) << "Unable to simplify return value node";
  return SimplificationResult::kDidNotChange;
}

// Runs a randomly selected optimization pass and returns whether the graph
// changed.
absl::StatusOr<SimplificationResult> RunRandomPass(
    FunctionBase* f, absl::BitGenRef rng, std::string* which_transform) {
  // All these passes have trivial construction costs.
  // TODO(allight): 2023-11-16: It might be nice to run these passes only on the
  // selected function-base (possibly with a different set for running on
  // everything).
  std::vector<std::unique_ptr<OptimizationPass>> passes;
  passes.push_back(std::make_unique<ArithSimplificationPass>());
  passes.push_back(std::make_unique<ArraySimplificationPass>());
  passes.push_back(std::make_unique<BitSliceSimplificationPass>());
  passes.push_back(std::make_unique<ConcatSimplificationPass>());
  passes.push_back(std::make_unique<ConstantFoldingPass>());
  passes.push_back(std::make_unique<CsePass>());
  passes.push_back(std::make_unique<TupleSimplificationPass>());
  passes.push_back(std::make_unique<UnrollPass>());
  if (absl::GetFlag(FLAGS_can_inline_everything)) {
    // Only can inline from the top.
    passes.push_back(std::make_unique<InliningPass>());
  }
  passes.push_back(std::make_unique<ProcStateFlatteningPass>());
  passes.push_back(std::make_unique<ProcStateOptimizationPass>());

  int64_t pass_no = absl::Uniform<int64_t>(rng, 0, passes.size());
  PassResults results;
  XLS_ASSIGN_OR_RETURN(bool changed,
                       passes.at(pass_no)->Run(
                           f->package(), OptimizationPassOptions(), &results));
  if (changed) {
    *which_transform = passes.at(pass_no)->short_name();
    return SimplificationResult::kDidChange;
  }
  XLS_LOG(INFO) << "Running " << passes.at(pass_no)->short_name()
                << " did not change graph.";
  return SimplificationResult::kDidNotChange;
}

absl::StatusOr<SimplificationResult> SimplifyNode(
    Node* n, absl::BitGenRef rng, std::string* which_transform) {
  FunctionBase* f = n->function_base();
  if (((n->Is<Receive>() && absl::GetFlag(FLAGS_can_remove_receives)) ||
       (n->Is<Send>() && absl::GetFlag(FLAGS_can_remove_sends))) &&
      absl::Bernoulli(rng, 0.3)) {
    XLS_ASSIGN_OR_RETURN(Channel * c, GetChannelUsedByNode(n));
    absl::flat_hash_set<std::string> preserved_channels;
    for (const std::string& chan : absl::GetFlag(FLAGS_preserve_channels)) {
      preserved_channels.insert(chan);
    }
    absl::flat_hash_map<Channel*, absl::flat_hash_set<Node*>> channel_to_nodes;
    for (Node* node : f->nodes()) {
      if (node->Is<Receive>() || node->Is<Send>()) {
        XLS_ASSIGN_OR_RETURN(Channel * c, GetChannelUsedByNode(node));
        channel_to_nodes[c].insert(node);
      }
    }
    if ((c->supported_ops() != ChannelOps::kSendReceive) &&
        !preserved_channels.contains(c->name())) {
      if (n->Is<Send>() && channel_to_nodes.at(c).size() == 1) {
        XLS_RETURN_IF_ERROR(n->ReplaceUsesWith(n->operand(0)));
        *which_transform = "remove send: %s" + n->GetName();
        XLS_RETURN_IF_ERROR(f->RemoveNode(n));
        XLS_RETURN_IF_ERROR(f->package()->RemoveChannel(c));
        return SimplificationResult::kDidChange;
      }
      if (n->Is<Receive>() && channel_to_nodes.at(c).size() == 1) {
        // A receive can have two possible types:
        //   blocking     : (token, <data type>)
        //   non-blocking : (token, <data type>, bits[1])
        // Create a tuple of the correct type containing the token and literal
        // values for the other elements.
        TupleType* tuple_type = n->GetType()->AsTupleOrDie();
        std::vector<Node*> tuple_elements = {n->operand(0)};
        for (int64_t i = 1; i < tuple_type->size(); ++i) {
          XLS_ASSIGN_OR_RETURN(
              Node * zero,
              f->MakeNode<Literal>(SourceInfo(),
                                   ZeroOfType(tuple_type->element_type(i))));
          tuple_elements.push_back(zero);
        }
        XLS_ASSIGN_OR_RETURN(Node * tuple,
                             f->MakeNode<Tuple>(SourceInfo(), tuple_elements));
        XLS_RETURN_IF_ERROR(n->ReplaceUsesWith(tuple));
        *which_transform = "remove receive: %s" + n->GetName();
        XLS_RETURN_IF_ERROR(f->RemoveNode(n));
        XLS_RETURN_IF_ERROR(f->package()->RemoveChannel(c));
        return SimplificationResult::kDidChange;
      }
    }
  }

  if (TypeHasToken(n->GetType())) {
    return SimplificationResult::kDidNotChange;
  }

  if (OpIsSideEffecting(n->op()) && !n->Is<Param>() && n->IsDead() &&
      absl::Bernoulli(rng, 0.3)) {
    *which_transform = "remove userless side-effecting node: " + n->GetName();
    XLS_RETURN_IF_ERROR(f->RemoveNode(n));
    return SimplificationResult::kDidChange;
  }

  if (!n->operands().empty() && absl::Bernoulli(rng, 0.3)) {
    // Try to replace a node with one of its (potentially truncated/extended)
    // operands.
    int64_t operand_no = absl::Uniform<int64_t>(rng, 0, n->operand_count());
    Node* operand = n->operand(operand_no);

    // If the chosen operand is the same type, just replace it.
    if (operand->GetType() == n->GetType()) {
      XLS_RETURN_IF_ERROR(n->ReplaceUsesWith(operand));
      *which_transform = "random replace with operand: " + n->GetName();
      return SimplificationResult::kDidChange;
    }

    // If the operand and node type are both bits, we can finagle the operand
    // type to match the node type.
    if (n->GetType()->IsBits() && operand->GetType()->IsBits()) {
      // If the chosen operand is a wider bits type, and this is not a bitslice
      // already, replace the node with a bitslice of its operand.
      if (operand->BitCountOrDie() > n->BitCountOrDie() && !n->Is<BitSlice>()) {
        XLS_RETURN_IF_ERROR(
            n->ReplaceUsesWithNew<BitSlice>(operand, /*start=*/0,
                                            /*width=*/n->BitCountOrDie())
                .status());
        *which_transform =
            "random replace with bitslice(operand): " + n->GetName();
        return SimplificationResult::kDidChange;
      }

      // If the chosen operand is a narrower bits type, and this is not a
      // zero-extend already, replace the node with a zero-extend of its
      // operand.
      if (operand->BitCountOrDie() < n->BitCountOrDie() &&
          n->op() != Op::kZeroExt) {
        XLS_RETURN_IF_ERROR(
            n->ReplaceUsesWithNew<ExtendOp>(
                 operand, /*new_bit_count=*/n->BitCountOrDie(), Op::kZeroExt)
                .status());
        *which_transform = "random replace with zext(operand): " + n->GetName();
        return SimplificationResult::kDidChange;
      }
    }
  }

  // Replace node with a constant (all zeros or all ones).
  if (n->Is<Param>() && n->IsDead()) {
    // Can't replace unused params with constant.
    XLS_VLOG(1) << "Candidate for constant-replacement is a dead parameter.";
    return SimplificationResult::kDidNotChange;
  }

  // (Rarely) replace non-literal node with an all ones.
  if (!n->Is<Literal>() && absl::Bernoulli(rng, 0.1)) {
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(AllOnesOfType(n->GetType())).status());
    *which_transform = "random replace with all-ones: " + n->GetName();
    return SimplificationResult::kDidChange;
  }

  // 50/50 replace invoke node with single-inlined version.
  if (absl::GetFlag(FLAGS_can_inline) && n->Is<Invoke>() &&
      absl::Bernoulli(rng, 0.5)) {
    // We will remove the invoke node so grab its name now.
    std::string transform_temp =
        absl::StrFormat("inline single invoke '%s'", n->ToString());
    XLS_RETURN_IF_ERROR(InliningPass::InlineOneInvoke(n->As<Invoke>()));
    *which_transform = transform_temp;
    return SimplificationResult::kDidChange;
  }

  // 50/50 replace map node with expanded invokes.
  if (absl::GetFlag(FLAGS_can_inline) && n->Is<Map>() &&
      absl::Bernoulli(rng, 0.5)) {
    // We will remove the map node so grab its name now.
    std::string transform_temp =
        absl::StrFormat("expand single map '%s'", n->ToString());
    XLS_RETURN_IF_ERROR(MapInliningPass::InlineOneMap(n->As<Map>()));
    *which_transform = transform_temp;
    return SimplificationResult::kDidChange;
  }

  // Otherwise replace with all zeros.
  if (n->Is<Literal>() && n->As<Literal>()->value().IsAllZeros()) {
    XLS_VLOG(1) << "Candidate for zero-replacement already a literal zero.";
    return SimplificationResult::kDidNotChange;
  }
  XLS_RETURN_IF_ERROR(
      n->ReplaceUsesWithNew<Literal>(ZeroOfType(n->GetType())).status());
  *which_transform = "random replace with zero: " + n->GetName();
  return SimplificationResult::kDidChange;
}

absl::StatusOr<SimplificationResult> Simplify(
    FunctionBase* f, std::optional<std::vector<Value>> inputs,
    absl::BitGenRef rng, std::string* which_transform) {
  if (absl::GetFlag(FLAGS_use_optimization_passes) &&
      absl::Bernoulli(rng, 0.2)) {
    XLS_ASSIGN_OR_RETURN(SimplificationResult pass_result,
                         RunRandomPass(f, rng, which_transform));
    if (pass_result != SimplificationResult::kDidNotChange) {
      return pass_result;
    }
  }

  if (absl::GetFlag(FLAGS_use_optimization_pipeline) &&
      absl::Bernoulli(rng, 0.05)) {
    // Try to run the sample through the entire optimization pipeline.
    XLS_ASSIGN_OR_RETURN(bool changed,
                         RunOptimizationPassPipeline(f->package()));
    if (changed) {
      *which_transform = "Optimization pipeline";
      return SimplificationResult::kDidChange;
    }
  }

  if (absl::Bernoulli(rng, 0.2)) {
    XLS_ASSIGN_OR_RETURN(SimplificationResult result,
                         SimplifyReturnValue(f, rng, which_transform));
    if (result == SimplificationResult::kDidChange) {
      return result;
    }
  }

  if (inputs.has_value() && absl::Bernoulli(rng, 0.3)) {
    // Try to replace a parameter with a literal equal to the respective input
    // value.
    int64_t param_no = absl::Uniform<int64_t>(rng, 0, f->params().size());
    Param* param = f->params()[param_no];
    if (!param->GetType()->IsToken()) {
      XLS_RETURN_IF_ERROR(
          param->ReplaceUsesWithNew<Literal>(inputs->at(param_no)).status());
      *which_transform = absl::StrFormat(
          "random replace parameter %d (%s) with literal of input value: %s",
          param_no, param->GetName(), inputs->at(param_no).ToString());
      return SimplificationResult::kDidChange;
    }
  }

  // Pick a random node and try to do something with it.
  int64_t i = absl::Uniform<int64_t>(rng, 0, f->node_count());
  Node* n = *std::next(f->nodes().begin(), i);
  return SimplifyNode(n, rng, which_transform);
}

// Runs removal of dead nodes (transitively), and then any dead parameters.
//
// Note removing dead parameters will not cause any additional nodes to be dead.
absl::Status CleanUp(FunctionBase* f, bool can_remove_params) {
  DeadCodeEliminationPass dce;
  DeadFunctionEliminationPass dfe;
  PassResults results;
  XLS_RETURN_IF_ERROR(
      dce.Run(f->package(), OptimizationPassOptions(), &results).status());
  if (can_remove_params) {
    XLS_RETURN_IF_ERROR(RemoveDeadParameters(f).status());
  }
  XLS_RETURN_IF_ERROR(
      dfe.Run(f->package(), OptimizationPassOptions(), &results).status());
  return absl::OkStatus();
}

absl::Status RealMain(std::string_view path, const int64_t failed_attempt_limit,
                      const int64_t total_attempt_limit,
                      const int64_t simplifications_between_tests,
                      const int64_t failed_attempts_between_tests_limit) {
  XLS_ASSIGN_OR_RETURN(std::string knownf_ir_text, GetFileContents(path));
  // Cache of test results to avoid duplicate invocations of the
  // test_executable.
  absl::flat_hash_map<std::string, bool> test_cache;

  // Parse inputs, if specified.
  std::optional<std::vector<xls::Value>> inputs;
  if (!absl::GetFlag(FLAGS_input).empty()) {
    inputs = std::vector<xls::Value>();
    XLS_QCHECK(absl::GetFlag(FLAGS_test_llvm_jit))
        << "Can only specify --input with --test_llvm_jit";
    for (const std::string_view& value_string :
         absl::StrSplit(absl::GetFlag(FLAGS_input), ';')) {
      XLS_ASSIGN_OR_RETURN(Value input, Parser::ParseTypedValue(value_string));
      inputs->push_back(input);
    }
  }

  // Check what the user gave us actually fails.
  XLS_RETURN_IF_ERROR(VerifyStillFails(
      knownf_ir_text, inputs,
      "Originally-provided main function provided does not fail", &test_cache));

  const bool can_remove_params = absl::GetFlag(FLAGS_can_remove_params);

  // Clean up any initial garbage and see if it still fails.
  {
    XLS_LOG(INFO) << "=== Cleaning up initial garbage";
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> package,
                         ParsePackage(knownf_ir_text));
    FunctionBase* main = package->GetTop().value();
    XLS_RETURN_IF_ERROR(CleanUp(main, can_remove_params));
    if (absl::GetFlag(FLAGS_verify_ir)) {
      XLS_RETURN_IF_ERROR(VerifyPackage(package.get()));
    }
    knownf_ir_text = package->DumpIr();
    XLS_RETURN_IF_ERROR(VerifyStillFails(
        knownf_ir_text, inputs,
        "Original main function does not fail after cleanup", &test_cache));
    XLS_LOG(INFO) << "=== Done cleaning up initial garbage";
  }

  // If so, we start simplifying via this seeded RNG.
  std::mt19937 rng;  // Default constructor uses deterministic seed.

  int64_t failed_simplification_attempts = 0;
  int64_t total_attempts = 0;
  int64_t simplification_iterations = 0;
  int64_t failed_attempts_between_tests = 0;
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> package,
                       ParsePackage(knownf_ir_text));

  struct CandidateChange {
    std::string which_transform;
    std::string package_ir_text;
    std::string candidate_ir_text;
    int64_t node_count;
  };
  std::vector<CandidateChange> candidate_changes;
  while (true) {
    if (failed_simplification_attempts >= failed_attempt_limit) {
      XLS_LOG(INFO) << "Hit failed-simplification-attempt-limit: "
                    << failed_simplification_attempts;
      // Used up all our attempts for this state.
      break;
    }
    XLS_LOG(INFO) << "Nodes left " << package->GetNodeCount() << " in "
                  << package->GetFunctionBases().size() << " FunctionBases";
    XLS_LOG(INFO) << "Total attempts " << total_attempts << "/"
                  << total_attempt_limit;
    XLS_LOG(INFO) << "Failed attempt count " << failed_simplification_attempts
                  << "/" << failed_attempt_limit;

    total_attempts++;
    if (total_attempts >= total_attempt_limit) {
      XLS_LOG(INFO) << "Hit total-attempt-limit: " << total_attempts;
      break;
    }

    XLS_VLOG(1) << "=== Simplification attempt " << total_attempts;

    FunctionBase* candidate;
    if (absl::GetFlag(FLAGS_simplify_top_only)) {
      candidate = package->GetTop().value();
    } else {
      std::vector<FunctionBase*> bases = package->GetFunctionBases();
      std::vector<int64_t> node_counts;
      node_counts.reserve(bases.size());
      absl::c_transform(bases, std::back_inserter(node_counts),
                        [](FunctionBase* f) { return f->node_count(); });
      std::discrete_distribution<int64_t> distribution(node_counts.cbegin(),
                                                       node_counts.cend());
      candidate = bases[distribution(rng)];
    }
    std::string candidate_name = candidate->name();
    XLS_VLOG_LINES(2,
                   "=== Candidate for simplification:\n" + candidate->DumpIr());

    // Simplify the function.
    std::string which_transform;
    XLS_ASSIGN_OR_RETURN(SimplificationResult simplification,
                         Simplify(candidate, inputs, rng, &which_transform));

    // If we cannot change it, we're done.
    if (simplification == SimplificationResult::kCannotChange) {
      XLS_LOG(INFO) << "Cannot simplify any further, done!";
      break;
    }

    // candidate might have been removed by DFE at this point. Be careful.
    auto candidate_ir = [&]() -> std::string {
      auto functions = package->GetFunctionBases();
      if (absl::c_find(functions, candidate) != functions.end()) {
        return candidate->DumpIr();
      }
      return "<Function removed>";
    };
    auto candidate_nodes = [&]() -> int64_t {
      auto functions = package->GetFunctionBases();
      if (absl::c_find(functions, candidate) != functions.end()) {
        return candidate->node_count();
      }
      return -1;
    };

    // If we happened to not change it (e.g. because the RNG said not to), keep
    // going until we do. We still bump the counter to make sure we don't end up
    // wedged in a state where we can't simplify anything.
    if (simplification == SimplificationResult::kDidNotChange) {
      XLS_VLOG(1) << "Did not change the sample.";
      failed_simplification_attempts++;
      if (simplification_iterations > 0) {
        failed_attempts_between_tests++;
      }
      if (failed_attempts_between_tests < failed_attempts_between_tests_limit) {
        // No need to test again yet.
        continue;
      }
      XLS_LOG(INFO) << "Hit failed-attempts-between-tests-limit, checking if "
                       "the candidate still fails: "
                    << failed_attempts_between_tests_limit;
      failed_attempts_between_tests = 0;
    } else {
      XLS_CHECK(simplification == SimplificationResult::kDidChange);
      XLS_LOG(INFO) << "Trying " << which_transform << " on " << candidate_name;
      XLS_RETURN_IF_ERROR(CleanUp(candidate, can_remove_params));
      XLS_VLOG_LINES(2, "=== After simplification [" + which_transform + "]\n" +
                            candidate_ir());
    }

    candidate_changes.push_back({
        .which_transform = which_transform,
        .package_ir_text = package->DumpIr(),
        .candidate_ir_text = candidate_ir(),
        .node_count = candidate_nodes(),
    });

    simplification_iterations++;
    if (simplification_iterations < simplifications_between_tests) {
      continue;
    }
    simplification_iterations = 0;
    failed_attempts_between_tests = 0;

    XLS_ASSIGN_OR_RETURN(bool still_fails,
                         StillFails(candidate_changes.back().package_ir_text,
                                    inputs, &test_cache));
    if (!still_fails) {
      // Test earlier changes to see if they were failing, and discard the ones
      // that aren't.
      XLS_LOG(INFO) << "Latest candidate no longer fails; trying earlier "
                       "untested candidates";
      XLS_ASSIGN_OR_RETURN(
          int64_t first_non_failing,
          BinarySearchMinTrueWithStatus(
              0, candidate_changes.size() - 1,
              [&](int64_t i) -> absl::StatusOr<bool> {
                XLS_ASSIGN_OR_RETURN(
                    bool still_fails,
                    StillFails(candidate_changes[i].package_ir_text, inputs,
                               &test_cache));
                return !still_fails;
              },
              BinarySearchAssumptions::kEndKnownTrue));
      XLS_LOG(INFO) << "Discarded "
                    << (candidate_changes.size() - first_non_failing)
                    << " candidates, leaving " << first_non_failing;
      candidate_changes.erase(candidate_changes.begin() + first_non_failing,
                              candidate_changes.end());
    }
    if (candidate_changes.empty()) {
      failed_simplification_attempts++;
      XLS_LOG(INFO) << "Sample no longer fails.";
      XLS_LOG(INFO) << "Failed simplification attempts now: "
                    << failed_simplification_attempts;
      // That simplification caused it to stop failing, but keep going with the
      // last known failing version and seeing if we can find something else
      // from there.
      XLS_ASSIGN_OR_RETURN(package, ParsePackage(knownf_ir_text));
      continue;
    }

    const CandidateChange& known_failure = candidate_changes.back();

    // We found something that definitely fails, update our "knownf" value and
    // reset our failed simplification attempt count since we see we've made
    // some forward progress.
    knownf_ir_text = known_failure.package_ir_text;
    std::cerr << "---\ntransforms: "
              << absl::StrJoin(
                     candidate_changes, ", ",
                     [](std::string* out, const CandidateChange& change) {
                       absl::StrAppend(out, change.which_transform);
                     })
              << " on " << candidate->name() << "\n"
              << (known_failure.node_count > 50
                      ? ""
                      : known_failure.candidate_ir_text)
              << "(" << known_failure.node_count << " nodes)\n";

    XLS_ASSIGN_OR_RETURN(package, ParsePackage(knownf_ir_text));
    failed_simplification_attempts = 0;
    candidate_changes.clear();
  }

  std::cout << knownf_ir_text;

  // Run the last test verification without the cache.
  XLS_RETURN_IF_ERROR(VerifyStillFails(knownf_ir_text, inputs,
                                       "Minimized function does not fail!",
                                       /*test_cache=*/nullptr));

  return absl::OkStatus();
}

}  // namespace
}  // namespace xls

int main(int argc, char** argv) {
  std::vector<std::string_view> positional_arguments =
      xls::InitXls(kUsage, argc, argv);

  if (positional_arguments.size() != 1 || positional_arguments[0].empty()) {
    XLS_LOG(QFATAL) << "Expected path argument with IR: " << argv[0]
                    << " <ir_path>";
  }

  XLS_QCHECK(!absl::GetFlag(FLAGS_test_executable).empty() ^
             absl::GetFlag(FLAGS_test_llvm_jit))
      << "Must specify either --test_executable or --test_llvm_jit";

  return xls::ExitStatus(xls::RealMain(
      positional_arguments[0], absl::GetFlag(FLAGS_failed_attempt_limit),
      absl::GetFlag(FLAGS_total_attempt_limit),
      absl::GetFlag(FLAGS_simplifications_between_tests),
      absl::GetFlag(FLAGS_failed_attempts_between_tests_limit)));
}
