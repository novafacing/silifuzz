// Copyright 2022 The SiliFuzz Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./runner/snap_maker.h"

#include <sys/user.h>

#include <optional>
#include <string>
#include <utility>

#include "absl/functional/bind_front.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "./common/memory_bytes_set.h"
#include "./common/memory_perms.h"
#include "./common/snapshot.h"
#include "./common/snapshot_enums.h"
#include "./common/snapshot_printer.h"
#include "./player/trace_options.h"
#include "./runner/driver/runner_driver.h"
#include "./snap/gen/reserved_memory_mappings.h"
#include "./snap/gen/snap_generator.h"
#include "./util/checks.h"
#include "./util/itoa.h"
#include "./util/line_printer.h"
#include "./util/page_util.h"
#include "./util/platform.h"

#if defined(__x86_64__)
#include "./runner/disassembling_snap_tracer.h"
#endif

namespace silifuzz {

using snapshot_types::Endpoint;
using snapshot_types::MakerStopReason;
using snapshot_types::PlaybackOutcome;
using snapshot_types::SigCause;
using snapshot_types::SigNum;

SnapMaker::SnapMaker(const Options& opts) : opts_(opts) {
  CHECK_STATUS(opts_.Validate());
}

absl::StatusOr<Snapshot> SnapMaker::Make(const Snapshot& snapshot) {
  CHECK(!snapshot.expected_end_states().empty());
  Snapshot copy = snapshot.Copy();
  snapshot_types::Address orig_endpoint_address;
  const Snapshot::EndState& es = copy.expected_end_states()[0];
  if (es.endpoint().type() == Endpoint::kInstruction) {
    orig_endpoint_address = es.endpoint().instruction_address();
  } else {
    orig_endpoint_address = es.endpoint().sig_instruction_address();
  }
  // Replace any end state with a single undefined end state located at either
  // the instruction address or the signal address. It won't always be possible
  // to repair the latter cases.
  Snapshot::EndState undef_end_state =
      Snapshot::EndState(Endpoint(orig_endpoint_address));
  copy.set_expected_end_states({});
  copy.set_negative_memory_mappings({});
  RETURN_IF_NOT_OK_PLUS(copy.can_add_expected_end_state(undef_end_state),
                        "Cannot add an undef endstate:");
  copy.add_expected_end_state(undef_end_state);

  MakerStopReason stop_reason;
  SnapifyOptions snapify_opts =
      SnapifyOptions::V2InputMakeOpts(copy.architecture_id());

  ASSIGN_OR_RETURN_IF_NOT_OK(copy, Snapify(copy, snapify_opts));
  ASSIGN_OR_RETURN_IF_NOT_OK(RunnerDriver runner_driver,
                             RunnerDriverFromSnapshot(copy, opts_.runner_path));
  ASSIGN_OR_RETURN_IF_NOT_OK(
      RunnerDriver::RunResult make_result,
      runner_driver.MakeOne(copy.id(), opts_.max_pages_to_add));
  if (make_result.success()) {
    // In practice this can happen if the snapshot hits just the right
    // sequence of instructions to call _exit(0) either by jumping into
    // a library function or directly invoking the corresponding syscall.
    return absl::InternalError(
        absl::StrCat("Unlikely: snapshot ", copy.id(),
                     " had an undefined end state yet ran successfully"));
  }
  const Snapshot::Endpoint& actual_endpoint =
      make_result.player_result().actual_end_state->endpoint();
  switch (make_result.player_result().outcome) {
    case PlaybackOutcome::kAsExpected:
      return absl::InternalError(
          absl::StrCat("Impossible: snapshot ", copy.id(),
                       " did not run successfully but ended as expected"));
    case PlaybackOutcome::kMemoryMismatch:
    case PlaybackOutcome::kRegisterStateMismatch:
      VLOG_INFO(1, "Reached a fixable outcome at ",
                HexStr(actual_endpoint.instruction_address()));
      stop_reason = MakerStopReason::kEndpoint;
      break;
    case PlaybackOutcome::kExecutionMisbehave: {
      // Default stop reason unless overridden below.
      stop_reason = MakerStopReason::kSignal;
      if (actual_endpoint.sig_num() == SigNum::kSigTrap) {
        VLOG_INFO(1, "Stopping due to SigTrap");
        stop_reason = MakerStopReason::kSigTrap;
      } else if (actual_endpoint.sig_num() == SigNum::kSigSegv) {
        switch (actual_endpoint.sig_cause()) {
          case SigCause::kSegvCantRead:
          case SigCause::kSegvCantWrite:
            // The runner attempts to fix up missing pages but adding them to
            // the snapshot. We reach here if the runner failed to add new
            // memory to fix up a R/W SEGV fault.
            stop_reason = MakerStopReason::kCannotAddMemory;
            break;
          case SigCause::kSegvGeneralProtection:
            stop_reason = MakerStopReason::kGeneralProtectionSigSegv;
            break;
          case SigCause::kSegvCantExec:
          case SigCause::kSegvOverflow:
          case SigCause::kGenericSigCause:
            stop_reason = MakerStopReason::kHardSigSegv;
            break;
        }
      }
      break;
    }
    case PlaybackOutcome::kExecutionRunaway:
      stop_reason = MakerStopReason::kTimeBudget;
      break;
    case PlaybackOutcome::kEndpointMismatch:
    case PlaybackOutcome::kPlatformMismatch:
      return absl::InternalError(
          absl::StrCat("Unsupported outcome ",
                       EnumStr(make_result.player_result().outcome)));
  }

  if (stop_reason != MakerStopReason::kEndpoint) {
    std::string msg =
        absl::StrCat(EnumStr(stop_reason), " isn't Snap-compatible.");
    if (actual_endpoint.type() == Endpoint::kSignal) {
      absl::StrAppend(&msg, " Endpoint = {", EnumStr(actual_endpoint.sig_num()),
                      "/", EnumStr(actual_endpoint.sig_cause()), "}");
    }
    return absl::InternalError(msg);
  }
  RETURN_IF_NOT_OK(AddWritableMemoryForEndState(
      &copy, make_result.player_result().actual_end_state.value()));

  Snapshot::EndState repaired_end_state = Snapshot::EndState(actual_endpoint);

  copy.set_expected_end_states({});
  RETURN_IF_NOT_OK(copy.can_add_expected_end_state(repaired_end_state));
  copy.add_expected_end_state(repaired_end_state);
  return copy;
}

absl::StatusOr<Snapshot> SnapMaker::RecordEndState(const Snapshot& snapshot) {
  SnapifyOptions snapify_opts =
      SnapifyOptions::V2InputMakeOpts(snapshot.architecture_id());
  ASSIGN_OR_RETURN_IF_NOT_OK(Snapshot snapified,
                             Snapify(snapshot, snapify_opts));
  ASSIGN_OR_RETURN_IF_NOT_OK(
      RunnerDriver recorder,
      RunnerDriverFromSnapshot(snapified, opts_.runner_path));
  ASSIGN_OR_RETURN_IF_NOT_OK(
      RunnerDriver::RunResult record_result,
      recorder.MakeOne(snapified.id(), /* max_pages_to_add=*/0));
  if (record_result.success()) {
    RETURN_IF_NOT_OK(snapified.IsComplete());
    return snapified;
  }
  if (!record_result.player_result().actual_end_state.has_value()) {
    return absl::InternalError("The runner didn't report actual_end_state");
  }
  Snapshot::EndState& actual_end_state =
      *record_result.player_result().actual_end_state;
  actual_end_state.set_platforms({CurrentPlatformId()});
  snapified.set_expected_end_states({});
  // TODO(ksteuck): [as-needed] The runner machinery already supports signal
  // handling. We'd need to extend the generator to support negative memory
  // mappings and a struct to record signal state to fully support
  // sig-causing snaps.
  //
  // Any snapshot with negative memory mappings will be discarded later on. We
  // don't need to make end state recording code any less generic here.
  RETURN_IF_NOT_OK(snapified.AddNegativeMemoryMappingsFor(actual_end_state));
  RETURN_IF_NOT_OK(snapified.can_add_expected_end_state(actual_end_state));
  snapified.add_expected_end_state(actual_end_state);
  RETURN_IF_NOT_OK(snapified.IsComplete());
  return snapified;
}

absl::StatusOr<Snapshot> SnapMaker::CheckTrace(
    const Snapshot& snapshot, const TraceOptions& trace_options) const {
  Snapshot copy = snapshot.Copy();
  // TODO(ncbray): instruction filtering on aarch64. This will likely involve
  // static decompilation rather than dynamic tracing.
#if defined(__x86_64__)
  ASSIGN_OR_RETURN_IF_NOT_OK(
      RunnerDriver driver,
      RunnerDriverFromSnapshot(snapshot, opts_.runner_path));

  DisassemblingSnapTracer tracer(snapshot, trace_options);
  absl::StatusOr<RunnerDriver::RunResult> trace_result_or = driver.TraceOne(
      snapshot.id(), absl::bind_front(&DisassemblingSnapTracer::Step, &tracer));
  DisassemblingSnapTracer::TraceResult trace_result = tracer.trace_result();

  if (!trace_result_or.status().ok() || !trace_result_or->success()) {
    return absl::InternalError(absl::StrCat(
        "Tracing failed: ", trace_result.early_termination_reason));
  }
  Snapshot::TraceData trace_data(trace_result.instructions_executed,
                                 absl::StrJoin(trace_result.disassembly, "\n"));
  trace_data.add_platform(CurrentPlatformId());
  copy.set_trace_data({trace_data});
#endif
  return copy;
}

absl::Status SnapMaker::VerifyPlaysDeterministically(
    const Snapshot& snapshot) const {
  SnapifyOptions snapify_opts =
      SnapifyOptions::V2InputRunOpts(snapshot.architecture_id());
  ASSIGN_OR_RETURN_IF_NOT_OK(Snapshot snapified,
                             Snapify(snapshot, snapify_opts));
  ASSIGN_OR_RETURN_IF_NOT_OK(
      RunnerDriver driver,
      RunnerDriverFromSnapshot(snapified, opts_.runner_path));

  // TODO(ksteuck): [as-needed] Consider VerifyDisjointly()-like functionality
  // to ensure that the snapshot does not touch any runner memory regions.
  // Current code plays the snapshot several times with ASLR enabled which
  // takes care of vDSO mappings and stack but the runner code itself is
  // always placed at the fixed address (--image-base linker arg).
  ASSIGN_OR_RETURN_IF_NOT_OK(
      RunnerDriver::RunResult verify_result,
      driver.VerifyOneRepeatedly(snapified.id(), opts_.num_verify_attempts));
  if (!verify_result.success()) {
    if (VLOG_IS_ON(1)) {
      LinePrinter lp(LinePrinter::StdErrPrinter);
      SnapshotPrinter printer(&lp);
      printer.PrintActualEndState(
          snapified, *verify_result.player_result().actual_end_state);
    }
    return absl::InternalError("Verify() failed, non-deterministic snapshot?");
  }
  return absl::OkStatus();
}

absl::Status SnapMaker::AddWritableMemoryForEndState(
    Snapshot* snapshot, const Snapshot::EndState& end_state) {
  // Compute the memory mapping added in the making process by subtracting
  // existing memory mappings from the snapshot from all the memory bytes
  // in the end state.
  MemoryBytesSet memory_bytes_set;
  for (const auto& memory_bytes : end_state.memory_bytes()) {
    memory_bytes_set.Add(memory_bytes.start_address(),
                         memory_bytes.limit_address());
  }

  for (const auto& mapping : snapshot->memory_mappings()) {
    memory_bytes_set.Remove(mapping.start_address(), mapping.limit_address());
  }

  // Helper to add a new zero-initialized data memory mapping to snapshot.
  auto add_mapping = [](Snapshot* snapshot,
                        const Snapshot::Address& start_address,
                        const Snapshot::Address& limit_address) {
    // Add a new memory RW mapping at [start_address, limit_address) for
    // the new memory bytes.
    VLOG_INFO(1, "Adding data mapping for [", HexStr(start_address), ", ",
              HexStr(limit_address), ")");

    if (!IsPageAligned(start_address) || !IsPageAligned(limit_address)) {
      return absl::InternalError(absl::StrCat(
          "New memory bytes not page-aligned: [", absl::Hex(start_address),
          ", ", absl::Hex(limit_address), ")"));
    }

    // Check that new mapping does not conflict with reserved memory
    // mappings.
    if (ReservedMemoryMappings().Overlaps(start_address, limit_address)) {
      return absl::InternalError(absl::StrCat(
          "New memory mapping overlaps with reserved memory mappings: [",
          absl::Hex(start_address), ", ", absl::Hex(limit_address), ")"));
    }

    RETURN_IF_NOT_OK(
        Snapshot::MemoryMapping::CanMakeRanged(start_address, limit_address));
    auto m = Snapshot::MemoryMapping::MakeRanged(start_address, limit_address,
                                                 MemoryPerms::RW());
    RETURN_IF_NOT_OK(snapshot->can_add_memory_mapping(m));

    snapshot->add_memory_mapping(m);
    Snapshot::MemoryBytes zero_memory_bytes(
        start_address, std::string(limit_address - start_address, '\0'));
    RETURN_IF_NOT_OK(snapshot->can_add_memory_bytes(zero_memory_bytes));
    snapshot->add_memory_bytes(std::move(zero_memory_bytes));
    return absl::OkStatus();
  };

  absl::Status status;
  memory_bytes_set.Iterate([snapshot, &status, &add_mapping](
                               const Snapshot::Address& start_address,
                               const Snapshot::Address& limit_address) {
    status.Update(add_mapping(snapshot, start_address, limit_address));
  });
  return status;
}

}  // namespace silifuzz
