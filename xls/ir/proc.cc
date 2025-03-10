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

#include "xls/ir/proc.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/function.h"
#include "xls/ir/node_iterator.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"
#include "xls/ir/source_location.h"
#include "xls/ir/value.h"
#include "xls/ir/value_helpers.h"

namespace xls {

std::string Proc::DumpIr() const {
  std::string res =
      absl::StrFormat("proc %s(%s: %s", name(), TokenParam()->GetName(),
                      TokenParam()->GetType()->ToString());
  for (Param* param : StateParams()) {
    absl::StrAppendFormat(&res, ", %s: %s", param->GetName(),
                          param->GetType()->ToString());
  }
  absl::StrAppendFormat(
      &res, ", init={%s}) {\n",
      absl::StrJoin(InitValues(), ", ", UntypedValueFormatter));

  for (Node* node : TopoSort(const_cast<Proc*>(this))) {
    if (node->op() == Op::kParam) {
      continue;
    }
    absl::StrAppend(&res, "  ", node->ToString(), "\n");
  }
  absl::StrAppend(&res, "  next (", NextToken()->GetName());
  for (Node* node : next_state_) {
    absl::StrAppend(&res, ", ", node->GetName());
  }
  absl::StrAppend(&res, ")\n}\n");
  return res;
}

int64_t Proc::GetStateFlatBitCount() const {
  int64_t total = 0;
  for (Param* param : StateParams()) {
    total += param->GetType()->GetFlatBitCount();
  }
  return total;
}

absl::StatusOr<int64_t> Proc::GetStateParamIndex(Param* param) const {
  auto it = std::find(StateParams().begin(), StateParams().end(), param);
  if (it == StateParams().end()) {
    return absl::InvalidArgumentError(
        "Given param is a state parameter of this proc: " + param->ToString());
  }
  return std::distance(StateParams().begin(), it);
}

absl::btree_set<int64_t> Proc::GetNextStateIndices(Node* node) const {
  auto it = next_state_indices_.find(node);
  if (it == next_state_indices_.end()) {
    return absl::btree_set<int64_t>();
  }
  return it->second;
}

absl::Status Proc::SetNextToken(Node* next) {
  if (!next->GetType()->IsToken()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Cannot set next token to \"%s\", expected token type but has type %s",
        next->GetName(), next->GetType()->ToString()));
  }
  next_token_ = next;
  return absl::OkStatus();
}

absl::Status Proc::JoinNextTokenWith(absl::Span<Node* const> tokens) {
  std::vector<Node*> operands;
  if (NextToken()->Is<AfterAll>()) {
    operands.insert(operands.end(), NextToken()->operands().begin(),
                    NextToken()->operands().end());
  } else {
    operands.push_back(NextToken());
  }
  operands.insert(operands.end(), tokens.begin(), tokens.end());
  if (operands.size() == 1) {
    return SetNextToken(operands.front());
  }
  XLS_ASSIGN_OR_RETURN(Node * next,
                       MakeNode<AfterAll>(NextToken()->loc(), operands));
  Node* old_next = NextToken();
  XLS_RETURN_IF_ERROR(SetNextToken(next));
  if (old_next->Is<AfterAll>() && old_next->users().empty()) {
    XLS_RETURN_IF_ERROR(RemoveNode(old_next));
  }
  return absl::OkStatus();
}

absl::Status Proc::SetNextStateElement(int64_t index, Node* next) {
  if (next->GetType() != GetStateElementType(index)) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Cannot set next state element %d to \"%s\"; type %s does not match "
        "proc state element type %s",
        index, next->GetName(), next->GetType()->ToString(),
        GetStateElementType(index)->ToString()));
  }
  next_state_indices_[next_state_[index]].erase(index);
  next_state_[index] = next;
  next_state_indices_[next].insert(index);
  return absl::OkStatus();
}

absl::Status Proc::ReplaceState(absl::Span<const std::string> state_param_names,
                                absl::Span<const Value> init_values) {
  for (int64_t i = GetStateElementCount() - 1; i >= 0; --i) {
    XLS_RETURN_IF_ERROR(RemoveStateElement(i));
  }
  if (state_param_names.size() != init_values.size()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Must specify equal number of state parameter names "
                        "(%d) and initial values (%d)",
                        state_param_names.size(), init_values.size()));
  }
  for (int64_t i = 0; i < state_param_names.size(); ++i) {
    XLS_RETURN_IF_ERROR(
        AppendStateElement(state_param_names[i], init_values[i]).status());
  }
  return absl::OkStatus();
}

absl::Status Proc::ReplaceState(absl::Span<const std::string> state_param_names,
                                absl::Span<const Value> init_values,
                                absl::Span<Node* const> next_state) {
  for (int64_t i = GetStateElementCount() - 1; i >= 0; --i) {
    XLS_RETURN_IF_ERROR(RemoveStateElement(i));
  }
  // Verify next values match the type of the initial values.
  if (state_param_names.size() != next_state.size() ||
      state_param_names.size() != init_values.size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Must specify equal number of state parameter names "
        "(%d), next state values (%d) and initial values (%d)",
        state_param_names.size(), next_state.size(), init_values.size()));
  }
  for (int64_t i = 0; i < state_param_names.size(); ++i) {
    XLS_RETURN_IF_ERROR(
        AppendStateElement(state_param_names[i], init_values[i], next_state[i])
            .status());
  }
  return absl::OkStatus();
}

absl::StatusOr<Param*> Proc::ReplaceStateElement(
    int64_t index, std::string_view state_param_name, const Value& init_value,
    std::optional<Node*> next_state) {
  XLS_RET_CHECK_LT(index, GetStateElementCount());

  // Copy name to a local variable to avoid the use-after-free footgun of
  // `state_param_name` referring to the existing to-be-removed state element.
  std::string s(state_param_name);

  // Check that it's safe to remove the current state param node.
  Param* old_param = GetStateParam(index);
  if (!old_param->users().empty()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Cannot remove state element %d of proc %s, existing "
                        "state param %s has uses",
                        index, name(), old_param->GetName()));
  }
  next_state_indices_[next_state_[index]].erase(index);
  next_state_[index] = nullptr;
  XLS_RETURN_IF_ERROR(RemoveNode(old_param));

  // Construct the new param node, and update all trackers.
  XLS_ASSIGN_OR_RETURN(
      Param * param,
      MakeNodeWithName<Param>(SourceInfo(), s,
                              package()->GetTypeForValue(init_value)));
  // Move the param into place (not forgetting the offset for state params,
  // since the token param is always at index 0).
  XLS_RETURN_IF_ERROR(MoveParamToIndex(param, index + 1));
  if (next_state.has_value() &&
      !ValueConformsToType(init_value, next_state.value()->GetType())) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Cannot add state element at %d, next state value %s (type %s) does "
        "not match type of initial value: %s",
        index, next_state.value()->GetName(),
        next_state.value()->GetType()->ToString(), init_value.ToString()));
  }
  init_values_[index] = init_value;
  next_state_[index] = next_state.value_or(param);
  next_state_indices_[next_state_[index]].insert(index);

  return param;
}

absl::Status Proc::RemoveStateElement(int64_t index) {
  XLS_RET_CHECK_LT(index, GetStateElementCount());
  if (index < GetStateElementCount() - 1) {
    for (auto& [_, indices] : next_state_indices_) {
      // Relabel all indices > `index`.
      auto it = indices.upper_bound(index);
      absl::btree_set<int64_t> relabeled_indices(indices.begin(), it);
      for (; it != indices.end(); ++it) {
        relabeled_indices.insert((*it) - 1);
      }
      indices = std::move(relabeled_indices);
    }
  }
  next_state_indices_[next_state_[index]].erase(index);
  next_state_.erase(next_state_.begin() + index);
  Param* old_param = GetStateParam(index);
  if (!old_param->users().empty()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Cannot remove state element %d of proc %s, existing "
                        "state param %s has uses",
                        index, name(), old_param->GetName()));
  }
  XLS_RETURN_IF_ERROR(RemoveNode(old_param));

  init_values_.erase(init_values_.begin() + index);
  return absl::OkStatus();
}

absl::StatusOr<Param*> Proc::AppendStateElement(
    std::string_view state_param_name, const Value& init_value,
    std::optional<Node*> next_state) {
  return InsertStateElement(GetStateElementCount(), state_param_name,
                            init_value, next_state);
}

absl::StatusOr<Param*> Proc::InsertStateElement(
    int64_t index, std::string_view state_param_name, const Value& init_value,
    std::optional<Node*> next_state) {
  XLS_RET_CHECK_LE(index, GetStateElementCount());
  const bool is_append = (index == GetStateElementCount());
  XLS_ASSIGN_OR_RETURN(
      Param * param,
      MakeNodeWithName<Param>(SourceInfo(), state_param_name,
                              package()->GetTypeForValue(init_value)));
  XLS_RETURN_IF_ERROR(MoveParamToIndex(param, index + 1));
  if (next_state.has_value()) {
    if (!ValueConformsToType(init_value, next_state.value()->GetType())) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Cannot add state element at %d, next state value %s (type %s) does "
          "not match type of initial value: %s",
          index, next_state.value()->GetName(),
          next_state.value()->GetType()->ToString(), init_value.ToString()));
    }
  }
  if (!is_append) {
    for (auto& [_, indices] : next_state_indices_) {
      // Relabel all indices >= `index`.
      auto it = indices.lower_bound(index);
      absl::btree_set<int64_t> relabeled_indices(indices.begin(), it);
      for (; it != indices.end(); ++it) {
        relabeled_indices.insert((*it) + 1);
      }
      indices = std::move(relabeled_indices);
    }
  }
  next_state_.insert(next_state_.begin() + index, next_state.value_or(param));
  next_state_indices_[next_state_[index]].insert(index);
  init_values_.insert(init_values_.begin() + index, init_value);
  return param;
}

bool Proc::HasImplicitUse(Node* node) const {
  if (node == NextToken()) {
    return true;
  }
  if (auto it = next_state_indices_.find(node);
      it != next_state_indices_.end() && !it->second.empty()) {
    return true;
  }
  return false;
}

absl::StatusOr<Proc*> Proc::Clone(
    std::string_view new_name, Package* target_package,
    absl::flat_hash_map<std::string, std::string> channel_remapping,
    absl::flat_hash_map<const FunctionBase*, FunctionBase*> call_remapping)
    const {
  absl::flat_hash_map<Node*, Node*> original_to_clone;
  if (target_package == nullptr) {
    target_package = package();
  }
  Proc* cloned_proc = target_package->AddProc(std::make_unique<Proc>(
      new_name, TokenParam()->GetName(), target_package));
  original_to_clone[TokenParam()] = cloned_proc->TokenParam();
  for (int64_t i = 0; i < GetStateElementCount(); ++i) {
    XLS_ASSIGN_OR_RETURN(Param * cloned_param, cloned_proc->AppendStateElement(
                                                   GetStateParam(i)->GetName(),
                                                   GetInitValueElement(i)));
    original_to_clone[GetStateParam(i)] = cloned_param;
  }
  for (Node* node : TopoSort(const_cast<Proc*>(this))) {
    std::vector<Node*> cloned_operands;
    for (Node* operand : node->operands()) {
      cloned_operands.push_back(original_to_clone.at(operand));
    }

    switch (node->op()) {
      case Op::kParam: {
        continue;
      }
      case Op::kReceive: {
        Receive* src = node->As<Receive>();
        std::string channel = channel_remapping.contains(src->channel_name())
                                  ? channel_remapping.at(src->channel_name())
                                  : src->channel_name();
        XLS_ASSIGN_OR_RETURN(original_to_clone[node],
                             cloned_proc->MakeNodeWithName<Receive>(
                                 src->loc(), cloned_operands[0],
                                 cloned_operands.size() == 2
                                     ? std::optional<Node*>(cloned_operands[1])
                                     : std::nullopt,
                                 channel, src->is_blocking(), src->GetName()));
        break;
      }
      case Op::kSend: {
        Send* src = node->As<Send>();
        std::string channel = channel_remapping.contains(src->channel_name())
                                  ? channel_remapping.at(src->channel_name())
                                  : src->channel_name();
        XLS_ASSIGN_OR_RETURN(
            original_to_clone[node],
            cloned_proc->MakeNodeWithName<Send>(
                src->loc(), cloned_operands[0], cloned_operands[1],
                cloned_operands.size() == 3
                    ? std::optional<Node*>(cloned_operands[2])
                    : std::nullopt,
                channel, src->GetName()));
        break;
      }
      // Remap CountedFor body.
      case Op::kCountedFor: {
        CountedFor* src = node->As<CountedFor>();
        auto remapped_call = call_remapping.find(src->body());
        if (remapped_call == call_remapping.end()) {
          return absl::InvalidArgumentError(absl::StrFormat(
              "Could not find mapping for CountedFor target %s.",
              src->GetName()));
        }
        Function* body = dynamic_cast<Function*>(remapped_call->second);
        if (body == nullptr) {
          return absl::InvalidArgumentError(absl::StrFormat(
              "CountedFor target was not mapped to a function."));
        }
        XLS_ASSIGN_OR_RETURN(
            original_to_clone[node],
            cloned_proc->MakeNodeWithName<CountedFor>(
                src->loc(), cloned_operands[0],
                absl::Span<Node*>(cloned_operands).subspan(1),
                src->trip_count(), src->stride(), body, src->GetName()));
        break;
      }
      // Remap Map to_apply.
      case Op::kMap: {
        Map* src = node->As<Map>();
        auto remapped_call = call_remapping.find(src->to_apply());
        if (remapped_call == call_remapping.end()) {
          return absl::InvalidArgumentError(absl::StrFormat(
              "Could not find mapping for Map target %s.", src->GetName()));
        }
        Function* to_apply = dynamic_cast<Function*>(remapped_call->second);
        if (to_apply == nullptr) {
          return absl::InvalidArgumentError(
              absl::StrFormat("Map target was not mapped to a function."));
        }
        XLS_ASSIGN_OR_RETURN(
            original_to_clone[node],
            cloned_proc->MakeNodeWithName<Map>(src->loc(), cloned_operands[0],
                                               to_apply, src->GetName()));
        break;
      }
      // Remap Invoke to_apply.
      case Op::kInvoke: {
        Invoke* src = node->As<Invoke>();
        auto remapped_call = call_remapping.find(src->to_apply());
        if (remapped_call == call_remapping.end()) {
          return absl::InvalidArgumentError(absl::StrFormat(
              "Could not find mapping for Invoke target %s.", src->GetName()));
        }
        Function* to_apply = dynamic_cast<Function*>(remapped_call->second);
        if (to_apply == nullptr) {
          return absl::InvalidArgumentError(
              absl::StrFormat("Invoke target was not mapped to a function."));
        }
        XLS_ASSIGN_OR_RETURN(
            original_to_clone[node],
            cloned_proc->MakeNodeWithName<Invoke>(src->loc(), cloned_operands,
                                                  to_apply, src->GetName()));
        break;
      }
      // Default clone.
      default: {
        XLS_ASSIGN_OR_RETURN(
            original_to_clone[node],
            node->CloneInNewFunction(cloned_operands, cloned_proc));
        break;
      }
    }
  }
  XLS_RETURN_IF_ERROR(
      cloned_proc->SetNextToken(original_to_clone.at(NextToken())));

  for (int64_t i = 0; i < GetStateElementCount(); ++i) {
    XLS_RETURN_IF_ERROR(cloned_proc->SetNextStateElement(
        i, original_to_clone.at(GetNextStateElement(i))));
  }
  return cloned_proc;
}

}  // namespace xls
