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

#ifndef XLS_CODEGEN_MODULE_TESTBENCH_H_
#define XLS_CODEGEN_MODULE_TESTBENCH_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/codegen/module_signature.h"
#include "xls/codegen/vast.h"
#include "xls/simulation/module_testbench_thread.h"
#include "xls/simulation/testbench_metadata.h"
#include "xls/simulation/testbench_signal_capture.h"
#include "xls/simulation/verilog_simulator.h"
#include "xls/tools/verilog_include.h"

namespace xls {
namespace verilog {

enum class ZeroOrX : int8_t { kZero, kX };

// Test class which does a cycle-by-cycle simulation of a Verilog module.
class ModuleTestbench {
 public:
  // Creates and returns a ModuleTestbench which exercises the given VAST
  // module.
  static absl::StatusOr<std::unique_ptr<ModuleTestbench>> CreateFromVastModule(
      Module* module, const VerilogSimulator* simulator,
      std::optional<std::string_view> clk_name = std::nullopt,
      const std::optional<ResetProto>& reset = std::nullopt,
      absl::Span<const VerilogInclude> includes = {});

  // Creates and returns a ModuleTestbench which exercises the given Verilog
  // text. If `reset_dut` is specified (and the module has a reset signal) then
  // the testbench will automatically handle asserting and reasserting the reset
  // port of the DUT.
  static absl::StatusOr<std::unique_ptr<ModuleTestbench>> CreateFromVerilogText(
      std::string_view verilog_text, FileType file_type,
      const ModuleSignature& signature, const VerilogSimulator* simulator,
      bool reset_dut = true, absl::Span<const VerilogInclude> includes = {});

  // Returns a a newly created thread to execute in the testbench.
  //
  // `dut_inputs` is the set of DUT input ports which this thread is responsible
  // for driving. Each DUT input port may only be driven by one thread. The
  // DutInput also includes the initial value of the input port.
  //
  // If `wait_until_done` is true then the testbench waits for this thread to
  // complete prior to calling $finish to terminate the simulation..
  absl::StatusOr<ModuleTestbenchThread*> CreateThread(
      std::string_view thread_name, absl::Span<const DutInput> dut_inputs,
      bool wait_until_done = true);

  // Convenience method which creates a thread which drives *all* input ports of
  // the DUT. The clock port is always excluded and the reset port is also
  // excluded if `reset_dut` was specified when the testbench was constructed
  // (in this case the reset port is driven internally in the
  // testbench). `initial_value` specifies the initial value
  // to drive on the input ports: either zero or X.
  absl::StatusOr<ModuleTestbenchThread*> CreateThreadDrivingAllInputs(
      std::string_view thread_name, ZeroOrX initial_value,
      bool wait_until_done = true);

  // Generates the Verilog representation of the testbench.
  std::string GenerateVerilog() const;

  // Runs the simulation.
  absl::Status Run();

 private:
  ModuleTestbench(std::string_view verilog_text, FileType file_type,
                  const VerilogSimulator* simulator,
                  const TestbenchMetadata& metadata, bool reset_dut,
                  absl::Span<const VerilogInclude> includes);

  // Create the initial threads such as the watchdog timer and, optionally, the
  // thread driving the reset signal.
  absl::Status CreateInitialThreads();

  absl::StatusOr<ModuleTestbenchThread*> CreateThread(
      std::string_view thread_name, absl::Span<const DutInput> dut_inputs,
      bool wait_until_done, bool wait_for_reset);

  // Checks the stdout of a simulation run against expectations.
  absl::Status CaptureOutputsAndCheckExpectations(
      std::string_view stdout_str) const;

  std::vector<std::string> GatherExpectedTraces() const;

  std::string verilog_text_;
  FileType file_type_;
  const VerilogSimulator* simulator_;
  TestbenchMetadata metadata_;
  bool reset_dut_;
  std::vector<VerilogInclude> includes_;

  SignalCaptureManager capture_manager_;

  // A list of blocks that execute concurrently in the testbench, a.k.a.
  // 'threads'. The xls::verilog::ModuleTestbench::CreateThread function
  // returns a reference to a ModuleTestbenchThread, as a result the threads
  // use std::unique_ptr to avoid bad referencing when vector is resized.
  std::vector<std::unique_ptr<ModuleTestbenchThread>> threads_;

  // The set of DUT input ports which have been claimed by a thread. An input
  // can only be claimed by a single thread (the thread which is responsible for
  // driving the signal). Indexed by thread name.
  absl::flat_hash_map<std::string, ModuleTestbenchThread*> claimed_dut_inputs_;
};

}  // namespace verilog
}  // namespace xls

#endif  // XLS_CODEGEN_MODULE_TESTBENCH_H_
