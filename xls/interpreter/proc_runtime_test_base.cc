// Copyright 2022 The XLS Authors
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

#include "xls/interpreter/proc_runtime_test_base.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/channel.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"

namespace xls {
namespace {

using status_testing::IsOkAndHolds;
using status_testing::StatusIs;
using ::testing::HasSubstr;
using ::testing::Optional;

// Creates a proc which has a single send operation using the given channel
// which sends a sequence of U32 values starting at 'starting_value' and
// increasing byte 'step' each tick.
absl::StatusOr<Proc*> CreateIotaProc(std::string_view proc_name,
                                     int64_t starting_value, int64_t step,
                                     Channel* channel, Package* package) {
  ProcBuilder pb(proc_name, /*token_name=*/"tok", package);
  BValue st = pb.StateElement("st", Value(UBits(starting_value, 32)));
  BValue send_token = pb.Send(channel, pb.GetTokenParam(), st);

  BValue new_value = pb.Add(st, pb.Literal(UBits(step, 32)));
  return pb.Build(send_token, {new_value});
}

// Creates a proc which keeps a running sum of all values read through the input
// channel. The sum is sent via an output chanel each iteration.
absl::StatusOr<Proc*> CreateAccumProc(std::string_view proc_name,
                                      Channel* in_channel, Channel* out_channel,
                                      Package* package) {
  ProcBuilder pb(proc_name, /*token_name=*/"tok", package);
  BValue accum = pb.StateElement("accum", Value(UBits(0, 32)));
  BValue token_input = pb.Receive(in_channel, pb.GetTokenParam());
  BValue recv_token = pb.TupleIndex(token_input, 0);
  BValue input = pb.TupleIndex(token_input, 1);
  BValue next_accum = pb.Add(accum, input);
  BValue send_token = pb.Send(out_channel, recv_token, next_accum);
  return pb.Build(send_token, {next_accum});
}

// Creates a proc which simply passes through a received value to a send.
absl::StatusOr<Proc*> CreatePassThroughProc(std::string_view proc_name,
                                            Channel* in_channel,
                                            Channel* out_channel,
                                            Package* package) {
  ProcBuilder pb(proc_name, /*token_name=*/"tok", package);
  BValue token_input = pb.Receive(in_channel, pb.GetTokenParam());
  BValue recv_token = pb.TupleIndex(token_input, 0);
  BValue input = pb.TupleIndex(token_input, 1);
  BValue send_token = pb.Send(out_channel, recv_token, input);
  return pb.Build(send_token, {});
}

// Create a proc which reads tuples of (count: u32, char: u8) from in_channel,
// run-length decodes them, and sends the resulting char stream to
// out_channel. Run lengths of zero are allowed.
absl::StatusOr<Proc*> CreateRunLengthDecoderProc(std::string_view proc_name,
                                                 Channel* in_channel,
                                                 Channel* out_channel,
                                                 Package* package) {
  // Proc state is a two-tuple containing: character to write and remaining
  // number of times to write the character.
  ProcBuilder pb(proc_name, /*token_name=*/"tok", package);
  BValue last_char = pb.StateElement("last_char", Value(UBits(0, 8)));
  BValue num_remaining = pb.StateElement("num_remaining", Value(UBits(0, 32)));
  BValue receive_next = pb.Eq(num_remaining, pb.Literal(UBits(0, 32)));
  BValue receive_if =
      pb.ReceiveIf(in_channel, pb.GetTokenParam(), receive_next);
  BValue receive_if_data = pb.TupleIndex(receive_if, 1);
  BValue run_length =
      pb.Select(receive_next,
                /*cases=*/{num_remaining, pb.TupleIndex(receive_if_data, 0)});
  BValue this_char = pb.Select(
      receive_next, /*cases=*/{last_char, pb.TupleIndex(receive_if_data, 1)});
  BValue run_length_is_nonzero = pb.Ne(run_length, pb.Literal(UBits(0, 32)));
  BValue send = pb.SendIf(out_channel, pb.TupleIndex(receive_if, 0),
                          run_length_is_nonzero, this_char);
  BValue next_num_remaining =
      pb.Select(run_length_is_nonzero,
                /*cases=*/{pb.Literal(UBits(0, 32)),
                           pb.Subtract(run_length, pb.Literal(UBits(1, 32)))});

  return pb.Build(send, {this_char, next_num_remaining});
}

TEST_P(ProcRuntimeTestBase, EmptyProc) {
  auto package = CreatePackage();

  ProcBuilder pb(TestName(), /*token_name=*/"tok", package.get());
  XLS_ASSERT_OK(pb.Build(pb.GetTokenParam(), {}));

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());

  // Expecting no output should result in zero ticks because the output
  // condition is trivially satisfied.
  EXPECT_THAT(runtime->TickUntilOutput({}), IsOkAndHolds(0));

  // Ticking until blocked should immediately return because `TickUntilBlocked`
  // only considers procs with IO to determine if the system is blocked.
  XLS_ASSERT_OK(runtime->TickUntilBlocked(/*max_ticks=*/100));
}

TEST_P(ProcRuntimeTestBase, EmptyProcAndPassThroughProc) {
  auto package = CreatePackage();

  XLS_ASSERT_OK_AND_ASSIGN(Channel * in, package->CreateStreamingChannel(
                                             "in", ChannelOps::kReceiveOnly,
                                             package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel * out, package->CreateStreamingChannel(
                                              "out", ChannelOps::kSendOnly,
                                              package->GetBitsType(32)));
  XLS_ASSERT_OK(CreatePassThroughProc("feedback", /*in_channel=*/in,
                                      /*out_channel=*/out, package.get())
                    .status());

  // Create the empty proc in same package.
  ProcBuilder pb(TestName(), /*token_name=*/"tok", package.get());
  XLS_ASSERT_OK(pb.Build(pb.GetTokenParam(), {}));

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  EXPECT_THAT(runtime->TickUntilOutput({{out, 1}}, /*max_ticks=*/100),
              StatusIs(absl::StatusCode::kDeadlineExceeded,
                       HasSubstr("Exceeded limit of 100 ticks")));

  // Ticking until blocked should immediately return because the proc with IO is
  // blocked and the empty proc is not considered in the "is blocked" logic.
  XLS_ASSERT_OK(runtime->TickUntilBlocked(/*max_ticks=*/100));

  ChannelQueue& in_queue = runtime->queue_manager().GetQueue(in);
  ChannelQueue& out_queue = runtime->queue_manager().GetQueue(out);

  XLS_ASSERT_OK(in_queue.Write(Value(UBits(42, 32))));

  XLS_ASSERT_OK(runtime->TickUntilOutput({{out, 1}}, /*max_ticks=*/100));

  EXPECT_THAT(out_queue.Read(), Optional(Value(UBits(42, 32))));
}

TEST_P(ProcRuntimeTestBase, ProcIotaWithExplicitTicks) {
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * channel,
      package->CreateStreamingChannel("iota_out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK(CreateIotaProc("iota", /*starting_value=*/5, /*step=*/10,
                               channel, package.get())
                    .status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  ChannelQueue& queue = runtime->queue_manager().GetQueue(channel);

  EXPECT_TRUE(queue.IsEmpty());
  XLS_ASSERT_OK(runtime->Tick());
  EXPECT_EQ(queue.GetSize(), 1);

  EXPECT_THAT(queue.Read(), Optional(Value(UBits(5, 32))));

  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());

  EXPECT_EQ(queue.GetSize(), 3);

  EXPECT_THAT(queue.Read(), Optional(Value(UBits(15, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(25, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(35, 32))));
}

TEST_P(ProcRuntimeTestBase, ProcIotaWithTickUntilOutput) {
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * channel,
      package->CreateStreamingChannel("iota_out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK(CreateIotaProc("iota", /*starting_value=*/5, /*step=*/10,
                               channel, package.get())
                    .status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  ChannelQueue& queue = runtime->queue_manager().GetQueue(channel);
  XLS_ASSERT_OK_AND_ASSIGN(int64_t tick_count,
                           runtime->TickUntilOutput({{channel, 4}}));
  EXPECT_EQ(tick_count, 4);
  EXPECT_EQ(queue.GetSize(), 4);

  EXPECT_THAT(queue.Read(), Optional(Value(UBits(5, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(15, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(25, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(35, 32))));
}

TEST_P(ProcRuntimeTestBase, ProcIotaWithTickUntilBlocked) {
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * channel,
      package->CreateStreamingChannel("iota_out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK(CreateIotaProc("iota", /*starting_value=*/5, /*step=*/10,
                               channel, package.get())
                    .status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  EXPECT_THAT(runtime->TickUntilBlocked(/*max_ticks=*/100),
              StatusIs(absl::StatusCode::kDeadlineExceeded,
                       HasSubstr("Exceeded limit of 100 ticks")));
}

TEST_P(ProcRuntimeTestBase, IotaFeedingAccumulator) {
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * iota_accum_channel,
      package->CreateStreamingChannel("iota_accum", ChannelOps::kSendReceive,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out_channel,
      package->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK(CreateIotaProc("iota", /*starting_value=*/0, /*step=*/1,
                               iota_accum_channel, package.get())
                    .status());
  XLS_ASSERT_OK(
      CreateAccumProc("accum", iota_accum_channel, out_channel, package.get())
          .status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  ChannelQueue& queue = runtime->queue_manager().GetQueue(out_channel);
  XLS_ASSERT_OK_AND_ASSIGN(int64_t tick_count,
                           runtime->TickUntilOutput({{out_channel, 4}}));

  EXPECT_EQ(tick_count, 4);
  EXPECT_EQ(queue.GetSize(), 4);
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(0, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(1, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(3, 32))));
  EXPECT_THAT(queue.Read(), Optional(Value(UBits(6, 32))));
}

TEST_P(ProcRuntimeTestBase, DegenerateProc) {
  // Tests interpreting a proc with no send of receive nodes.
  auto package = CreatePackage();
  ProcBuilder pb(TestName(), /*token_name=*/"tok", package.get());
  XLS_ASSERT_OK(pb.Build(pb.GetTokenParam(), {}));

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  // Ticking the proc has no observable effect, but it should not hang or crash.
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
}

TEST_P(ProcRuntimeTestBase, WrappedProc) {
  // Create a proc which receives a value, sends it the accumulator proc, and
  // sends the result.
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * in_channel,
      package->CreateStreamingChannel("input", ChannelOps::kReceiveOnly,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * in_accum_channel,
      package->CreateStreamingChannel("accum_in", ChannelOps::kSendReceive,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out_accum_channel,
      package->CreateStreamingChannel("accum_out", ChannelOps::kSendReceive,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out_channel,
      package->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));

  ProcBuilder pb(TestName(), /*token_name=*/"tok", package.get());
  BValue recv_input = pb.Receive(in_channel, pb.GetTokenParam());
  BValue send_to_accum =
      pb.Send(in_accum_channel, /*token=*/pb.TupleIndex(recv_input, 0),
              /*data_operands=*/{pb.TupleIndex(recv_input, 1)});
  BValue recv_from_accum = pb.Receive(out_accum_channel, send_to_accum);
  BValue send_output =
      pb.Send(out_channel, /*token=*/pb.TupleIndex(recv_from_accum, 0),
              /*data_operands=*/{pb.TupleIndex(recv_from_accum, 1)});
  XLS_ASSERT_OK(pb.Build(send_output, {}));

  XLS_ASSERT_OK(CreateAccumProc("accum", /*in_channel=*/in_accum_channel,
                                /*out_channel=*/out_accum_channel,
                                package.get())
                    .status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  XLS_ASSERT_OK(runtime->queue_manager()
                    .GetQueue(in_channel)
                    .AttachGenerator(FixedValueGenerator(
                        {Value(UBits(10, 32)), Value(UBits(20, 32)),
                         Value(UBits(30, 32))})));

  XLS_ASSERT_OK_AND_ASSIGN(int64_t tick_count,
                           runtime->TickUntilOutput({{out_channel, 3}}));
  EXPECT_EQ(tick_count, 3);

  ChannelQueue& output_queue = runtime->queue_manager().GetQueue(out_channel);
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(10, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(30, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(60, 32))));
}

TEST_P(ProcRuntimeTestBase, DeadlockedProc) {
  // Test a trivial deadlocked proc network. A single proc with a feedback edge
  // from its send operation to its receive.
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * channel,
      package->CreateStreamingChannel("my_channel", ChannelOps::kSendReceive,
                                      package->GetBitsType(32)));
  XLS_ASSERT_OK(CreatePassThroughProc("feedback", /*in_channel=*/channel,
                                      /*out_channel=*/channel, package.get())
                    .status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  // The interpreter can tick once without deadlocking because some instructions
  // can actually execute initially (e.g., the parameters). A subsequent call to
  // Tick() will detect the deadlock.
  XLS_ASSERT_OK(runtime->Tick());
  EXPECT_THAT(
      runtime->Tick(),
      StatusIs(
          absl::StatusCode::kInternal,
          HasSubstr(
              "Proc network is deadlocked. Blocked channels: my_channel")));
}

TEST_P(ProcRuntimeTestBase, RunLengthDecoding) {
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * input_channel,
      package->CreateStreamingChannel(
          "in", ChannelOps::kReceiveOnly,
          package->GetTupleType(
              {package->GetBitsType(32), package->GetBitsType(8)})));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * output_channel,
      package->CreateStreamingChannel("output", ChannelOps::kSendOnly,
                                      package->GetBitsType(8)));

  XLS_ASSERT_OK(CreateRunLengthDecoderProc("decoder", input_channel,
                                           output_channel, package.get())
                    .status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());
  std::vector<Value> inputs = {
      Value::Tuple({Value(UBits(1, 32)), Value(UBits(42, 8))}),
      Value::Tuple({Value(UBits(3, 32)), Value(UBits(123, 8))}),
      Value::Tuple({Value(UBits(0, 32)), Value(UBits(55, 8))}),
      Value::Tuple({Value(UBits(0, 32)), Value(UBits(66, 8))}),
      Value::Tuple({Value(UBits(2, 32)), Value(UBits(20, 8))})};
  XLS_ASSERT_OK(runtime->queue_manager()
                    .GetQueue(input_channel)
                    .AttachGenerator(FixedValueGenerator(inputs)));

  XLS_ASSERT_OK(runtime->TickUntilBlocked().status());

  ChannelQueue& output_queue =
      runtime->queue_manager().GetQueue(output_channel);
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(42, 8))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(123, 8))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(123, 8))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(123, 8))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(20, 8))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(20, 8))));
}

TEST_P(ProcRuntimeTestBase, RunLengthDecodingFilter) {
  // Connect a run-length decoding proc to a proc which only passes through even
  // values.
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * input_channel,
      package->CreateStreamingChannel(
          "in", ChannelOps::kReceiveOnly,
          package->GetTupleType(
              {package->GetBitsType(32), package->GetBitsType(8)})));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * decoded_channel,
      package->CreateStreamingChannel("decoded", ChannelOps::kSendReceive,
                                      package->GetBitsType(8)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * output_channel,
      package->CreateStreamingChannel("output", ChannelOps::kSendOnly,
                                      package->GetBitsType(8)));

  XLS_ASSERT_OK(CreateRunLengthDecoderProc("decoder", input_channel,
                                           decoded_channel, package.get())
                    .status());
  ProcBuilder pb("filter", /*token_name=*/"tok", package.get());
  BValue receive = pb.Receive(decoded_channel, pb.GetTokenParam());
  BValue rx_token = pb.TupleIndex(receive, 0);
  BValue rx_value = pb.TupleIndex(receive, 1);
  BValue rx_value_even =
      pb.Not(pb.BitSlice(rx_value, /*start=*/0, /*width=*/1));
  BValue send_if = pb.SendIf(output_channel, rx_token, rx_value_even, rx_value);
  XLS_ASSERT_OK(pb.Build(send_if, {}));

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  std::vector<Value> inputs = {
      Value::Tuple({Value(UBits(1, 32)), Value(UBits(42, 8))}),
      Value::Tuple({Value(UBits(3, 32)), Value(UBits(123, 8))}),
      Value::Tuple({Value(UBits(0, 32)), Value(UBits(55, 8))}),
      Value::Tuple({Value(UBits(0, 32)), Value(UBits(66, 8))}),
      Value::Tuple({Value(UBits(2, 32)), Value(UBits(20, 8))})};
  XLS_ASSERT_OK(runtime->queue_manager()
                    .GetQueue(input_channel)
                    .AttachGenerator(FixedValueGenerator(inputs)));

  XLS_ASSERT_OK(runtime->TickUntilBlocked().status());

  ChannelQueue& output_queue =
      runtime->queue_manager().GetQueue(output_channel);

  // Only even values should make it through the filter.
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(42, 8))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(20, 8))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(20, 8))));
}

TEST_P(ProcRuntimeTestBase, IotaWithChannelBackedge) {
  // Create an iota proc which uses a channel to convey the state rather than
  // using the explicit proc state. The state channel has an initial value, just
  // like a proc's state.
  auto package = CreatePackage();
  ProcBuilder pb(TestName(), /*token_name=*/"tok", package.get());
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * state_channel,
      package->CreateStreamingChannel(
          "state", ChannelOps::kSendReceive, package->GetBitsType(32),
          /*initial_values=*/{Value(UBits(42, 32))}));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * output_channel,
      package->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));

  BValue state_receive = pb.Receive(state_channel, pb.GetTokenParam());
  BValue receive_token = pb.TupleIndex(state_receive, /*idx=*/0);
  BValue state = pb.TupleIndex(state_receive, /*idx=*/1);
  BValue next_state = pb.Add(state, pb.Literal(UBits(1, 32)));
  BValue out_send = pb.Send(output_channel, pb.GetTokenParam(), state);
  BValue state_send = pb.Send(state_channel, receive_token, next_state);
  XLS_ASSERT_OK(pb.Build(pb.AfterAll({out_send, state_send}), {}).status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  XLS_ASSERT_OK_AND_ASSIGN(int64_t tick_count,
                           runtime->TickUntilOutput({{output_channel, 3}}));
  EXPECT_EQ(tick_count, 3);

  ChannelQueue& output_queue =
      runtime->queue_manager().GetQueue(output_channel);
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(42, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(43, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(44, 32))));
}

TEST_P(ProcRuntimeTestBase, IotaWithChannelBackedgeAndTwoInitialValues) {
  auto package = CreatePackage();
  // Create an iota proc which uses a channel to convey the state rather than
  // using the explicit proc state. However, the state channel has multiple
  // initial values which results in interleaving of difference sequences of
  // iota values.
  ProcBuilder pb(TestName(), /*token_name=*/"tok", package.get());
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * state_channel,
      package->CreateStreamingChannel(
          "state", ChannelOps::kSendReceive, package->GetBitsType(32),
          // Initial value of iotas are 42, 55, 100. Three sequences of
          // interleaved numbers will be generated starting at these
          // values.
          {Value(UBits(42, 32)), Value(UBits(55, 32)), Value(UBits(100, 32))}));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * output_channel,
      package->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));

  BValue state_receive = pb.Receive(state_channel, pb.GetTokenParam());
  BValue receive_token = pb.TupleIndex(state_receive, /*idx=*/0);
  BValue state = pb.TupleIndex(state_receive, /*idx=*/1);
  BValue next_state = pb.Add(state, pb.Literal(UBits(1, 32)));
  BValue out_send = pb.Send(output_channel, pb.GetTokenParam(), state);
  BValue state_send = pb.Send(state_channel, receive_token, next_state);
  XLS_ASSERT_OK(pb.Build(pb.AfterAll({out_send, state_send}), {}).status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  XLS_ASSERT_OK_AND_ASSIGN(int64_t tick_count,
                           runtime->TickUntilOutput({{output_channel, 9}}));
  EXPECT_EQ(tick_count, 9);

  ChannelQueue& output_queue =
      runtime->queue_manager().GetQueue(output_channel);
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(42, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(55, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(100, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(43, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(56, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(101, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(44, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(57, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(102, 32))));
}

// Test verifies that an "X"-shaped network can be modeled correctly, i.e.,
// a network that looks like:
//  A   B
//   \ /
//    C
//   / \
//  D   E
//
// Where A and B receive inputs from "outside", and D and E produce outputs.
TEST_P(ProcRuntimeTestBase, XNetwork) {
  constexpr int kNumCycles = 32;
  const std::string kIrText = R"(
package p

chan i_a(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=none, metadata="")
chan i_b(bits[32], id=1, kind=streaming, ops=receive_only, flow_control=none, metadata="")
chan a_c(bits[32], id=2, kind=streaming, ops=send_receive, flow_control=none, metadata="")
chan b_c(bits[32], id=3, kind=streaming, ops=send_receive, flow_control=none, metadata="")
chan c_d(bits[32], id=4, kind=streaming, ops=send_receive, flow_control=none, metadata="")
chan c_e(bits[32], id=5, kind=streaming, ops=send_receive, flow_control=none, metadata="")
chan d_o(bits[32], id=6, kind=streaming, ops=send_only, flow_control=none, metadata="")
chan e_o(bits[32], id=7, kind=streaming, ops=send_only, flow_control=none, metadata="")

proc a(my_token: token, state: (), init={()}) {
  literal.1: bits[32] = literal(value=1)
  receive.2: (token, bits[32]) = receive(my_token, channel=i_a)
  tuple_index.3: token = tuple_index(receive.2, index=0)
  tuple_index.4: bits[32] = tuple_index(receive.2, index=1)
  umul.5: bits[32] = umul(literal.1, tuple_index.4)
  send.6: token = send(tuple_index.3, umul.5, channel=a_c)
  next (send.6, state)
}

proc b(my_token: token, state: (), init={()}) {
  literal.101: bits[32] = literal(value=2)
  receive.102: (token, bits[32]) = receive(my_token, channel=i_b)
  tuple_index.103: token = tuple_index(receive.102, index=0)
  tuple_index.104: bits[32] = tuple_index(receive.102, index=1)
  umul.105: bits[32] = umul(literal.101, tuple_index.104)
  send.106: token = send(tuple_index.103, umul.105, channel=b_c)
  next (send.106, state)
}

proc c(my_token: token, state: (), init={()}) {
  literal.201: bits[32] = literal(value=3)
  receive.202: (token, bits[32]) = receive(my_token, channel=a_c)
  tuple_index.203: token = tuple_index(receive.202, index=0)
  tuple_index.204: bits[32] = tuple_index(receive.202, index=1)
  receive.205: (token, bits[32]) = receive(tuple_index.203, channel=b_c)
  tuple_index.206: token = tuple_index(receive.205, index=0)
  tuple_index.207: bits[32] = tuple_index(receive.205, index=1)
  umul.208: bits[32] = umul(literal.201, tuple_index.204)
  umul.209: bits[32] = umul(literal.201, tuple_index.207)
  send.210: token = send(tuple_index.206, umul.208, channel=c_d)
  send.211: token = send(send.210, umul.209, channel=c_e)
  next (send.211, state)
}

proc d(my_token: token, state: (), init={()}) {
  literal.301: bits[32] = literal(value=4)
  receive.302: (token, bits[32]) = receive(my_token, channel=c_d)
  tuple_index.303: token = tuple_index(receive.302, index=0)
  tuple_index.304: bits[32] = tuple_index(receive.302, index=1)
  umul.305: bits[32] = umul(literal.301, tuple_index.304)
  send.306: token = send(tuple_index.303, umul.305, channel=d_o)
  next (send.306, state)
}

proc e(my_token: token, state: (), init={()}) {
  literal.401: bits[32] = literal(value=5)
  receive.402: (token, bits[32]) = receive(my_token, channel=c_e)
  tuple_index.403: token = tuple_index(receive.402, index=0)
  tuple_index.404: bits[32] = tuple_index(receive.402, index=1)
  umul.405: bits[32] = umul(literal.401, tuple_index.404)
  send.406: token = send(tuple_index.403, umul.405, channel=e_o)
  next (send.406, state)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(auto p, Parser::ParsePackage(kIrText));
  std::unique_ptr<ProcRuntime> runtime = GetParam().CreateRuntime(p.get());
  XLS_ASSERT_OK_AND_ASSIGN(auto i_a, runtime->queue_manager().GetQueueById(0));
  XLS_ASSERT_OK_AND_ASSIGN(auto i_b, runtime->queue_manager().GetQueueById(1));
  XLS_ASSERT_OK_AND_ASSIGN(auto d_o, runtime->queue_manager().GetQueueById(6));
  XLS_ASSERT_OK_AND_ASSIGN(auto e_o, runtime->queue_manager().GetQueueById(7));

  for (int i = 0; i < kNumCycles; i++) {
    XLS_ASSERT_OK(i_a->Write(Value(UBits(i, 32))));
    XLS_ASSERT_OK(i_b->Write(Value(UBits(i + 10, 32))));
  }

  for (int i = 0; i < kNumCycles; i++) {
    XLS_ASSERT_OK(runtime->Tick());
  }

  // Now, cut out the garbage data from the output queues, and then verify their
  // contents.
  for (int i = 0; i < kNumCycles; i++) {
    EXPECT_THAT(d_o->Read(), Optional(Value(UBits(i * 1 * 3 * 4, 32))));
    EXPECT_THAT(e_o->Read(), Optional(Value(UBits((i + 10) * 2 * 3 * 5, 32))));
  }
}

TEST_P(ProcRuntimeTestBase, ChannelInitValues) {
  auto package = CreatePackage();

  // Create an iota proc which uses a channel to convey the state rather than
  // using the explicit proc state. However, the state channel has multiple
  // initial values which results in interleaving of difference sequences of
  // iota values.
  ProcBuilder pb("backedge_proc", /*token_name=*/"tok", package.get());
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * state_channel,
      package->CreateStreamingChannel(
          "state", ChannelOps::kSendReceive, package->GetBitsType(32),
          // Initial value of iotas are 42, 55, 100. Three sequences of
          // interleaved numbers will be generated starting at these
          // values.
          {Value(UBits(42, 32)), Value(UBits(55, 32)), Value(UBits(100, 32))}));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * output_channel,
      package->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                      package->GetBitsType(32)));

  BValue state_receive = pb.Receive(state_channel, pb.GetTokenParam());
  BValue receive_token = pb.TupleIndex(state_receive, /*idx=*/0);
  BValue state = pb.TupleIndex(state_receive, /*idx=*/1);
  BValue next_state = pb.Add(state, pb.Literal(UBits(1, 32)));
  BValue out_send = pb.Send(output_channel, pb.GetTokenParam(), state);
  BValue state_send = pb.Send(state_channel, receive_token, next_state);
  XLS_ASSERT_OK(pb.Build(pb.AfterAll({out_send, state_send}), {}).status());

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());

  for (int64_t i = 0; i < 9; ++i) {
    XLS_ASSERT_OK(runtime->Tick());
  }

  ChannelQueue& output_queue =
      runtime->queue_manager().GetQueue(output_channel);
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(42, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(55, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(100, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(43, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(56, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(101, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(44, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(57, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(102, 32))));
}

TEST_P(ProcRuntimeTestBase, StateReset) {
  auto package = CreatePackage();

  Type* u32 = package->GetBitsType(32);
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * ch_out,
      package->CreateStreamingChannel("out", ChannelOps::kSendOnly, u32));

  ProcBuilder pb("state_reset", /*token_name=*/"tkn", package.get());
  BValue st = pb.StateElement("st", Value(UBits(11, 32)));
  BValue send_token = pb.Send(ch_out, pb.GetTokenParam(), st);
  BValue add_lit = pb.Literal(SBits(3, 32));
  BValue next_int = pb.Add(st, add_lit);
  XLS_ASSERT_OK(pb.Build(send_token, {next_int}));

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());
  ChannelQueue& output_queue = runtime->queue_manager().GetQueue(ch_out);

  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  EXPECT_THAT(output_queue.Read(), Optional(Value(SBits(11, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(SBits(14, 32))));

  runtime->ResetState();
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());

  EXPECT_THAT(output_queue.Read(), Optional(Value(SBits(11, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(SBits(14, 32))));
}

TEST_P(ProcRuntimeTestBase, NonBlockingReceivesProc) {
  auto package = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Channel * in0, package->CreateStreamingChannel(
                                              "in0", ChannelOps::kReceiveOnly,
                                              package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel * in1, package->CreateStreamingChannel(
                                              "in1", ChannelOps::kReceiveOnly,
                                              package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel * in2, package->CreateSingleValueChannel(
                                              "in2", ChannelOps::kReceiveOnly,
                                              package->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel * out0, package->CreateStreamingChannel(
                                               "out0", ChannelOps::kSendOnly,
                                               package->GetBitsType(32)));

  ProcBuilder pb("nb_recv", /*token_name=*/"tok", package.get());

  BValue in0_data_and_valid = pb.ReceiveNonBlocking(in0, pb.GetTokenParam());
  BValue in1_data_and_valid = pb.ReceiveNonBlocking(in1, pb.GetTokenParam());
  BValue in2_data_and_valid = pb.ReceiveNonBlocking(in2, pb.GetTokenParam());

  BValue sum = pb.Literal(UBits(0, 32));

  BValue in0_tok = pb.TupleIndex(in0_data_and_valid, 0);
  BValue in0_data = pb.TupleIndex(in0_data_and_valid, 1);
  BValue in0_valid = pb.TupleIndex(in0_data_and_valid, 2);
  BValue add_sum_in0 = pb.Add(sum, in0_data);
  BValue sum0 = pb.Select(in0_valid, {sum, add_sum_in0});

  BValue in1_tok = pb.TupleIndex(in1_data_and_valid, 0);
  BValue in1_data = pb.TupleIndex(in1_data_and_valid, 1);
  BValue in1_valid = pb.TupleIndex(in1_data_and_valid, 2);
  BValue add_sum0_in1 = pb.Add(sum0, in1_data);
  BValue sum1 = pb.Select(in1_valid, {sum0, add_sum0_in1});

  BValue in2_tok = pb.TupleIndex(in2_data_and_valid, 0);
  BValue in2_data = pb.TupleIndex(in2_data_and_valid, 1);
  BValue in2_valid = pb.TupleIndex(in2_data_and_valid, 2);
  BValue add_sum1_in2 = pb.Add(sum1, in2_data);
  BValue sum2 = pb.Select(in2_valid, {sum1, add_sum1_in2});

  BValue after_in_tok = pb.AfterAll({in0_tok, in1_tok, in2_tok});
  BValue tok_fin = pb.Send(out0, after_in_tok, sum2);

  XLS_ASSERT_OK(pb.Build(tok_fin, {}));

  std::unique_ptr<ProcRuntime> runtime =
      GetParam().CreateRuntime(package.get());
  ChannelQueue& output_queue = runtime->queue_manager().GetQueue(out0);
  ChannelQueue& in0_queue = runtime->queue_manager().GetQueue(in0);
  ChannelQueue& in1_queue = runtime->queue_manager().GetQueue(in1);
  ChannelQueue& in2_queue = runtime->queue_manager().GetQueue(in2);

  // Initialize the single value queue.
  XLS_ASSERT_OK(in2_queue.Write(Value(UBits(10, 32))));

  // All other channels are non-blocking, so run even if the queues are empty.
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(10, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(10, 32))));

  // Run with only in1 (and in2) having data, followed by only in2 with data.
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(5, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(7, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(3, 32))));

  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());

  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(15, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(17, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(13, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(10, 32))));

  // Run with only in0 (and in2) having data followed by only in2 with data.
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(7, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(100, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(13, 32))));

  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());

  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(17, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(110, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(23, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(10, 32))));

  // Run with all channels having data.
  XLS_ASSERT_OK(in0_queue.Write(Value(UBits(11, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(22, 32))));

  XLS_ASSERT_OK(runtime->Tick());

  EXPECT_THAT(output_queue.Read(), Optional(Value(SBits(43, 32))));

  // Try large numnbers in the channels.
  XLS_ASSERT_OK(in0_queue.Write(Value(UBits(0xffffffff, 32))));
  XLS_ASSERT_OK(in0_queue.Write(Value(UBits(0x0faabbcc, 32))));
  XLS_ASSERT_OK(in0_queue.Write(Value(UBits(0xfffffff2, 32))));
  XLS_ASSERT_OK(in0_queue.Write(Value(UBits(0xfffffff2, 32))));

  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(0, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(0xf0000000, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(0x0000000e, 32))));
  XLS_ASSERT_OK(in1_queue.Write(Value(UBits(0x0000000f, 32))));

  XLS_ASSERT_OK(in2_queue.Write(Value(UBits(0, 32))));

  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());
  XLS_ASSERT_OK(runtime->Tick());

  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(0xffffffff, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(0xffaabbcc, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(0, 32))));
  EXPECT_THAT(output_queue.Read(), Optional(Value(UBits(1, 32))));
}

}  // namespace
}  // namespace xls
