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

#ifndef THIRD_PARTY_SILIFUZZ_RUNNER_SNAP_MAKER_H_
#define THIRD_PARTY_SILIFUZZ_RUNNER_SNAP_MAKER_H_

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "./common/snapshot.h"
#include "./player/trace_options.h"

namespace silifuzz {

// SnapMaker is a helper class for making, recording and verifying Snapshot-s.
// The generic pipeline to transform a sequence of instructions into a Snapshot
// looks like this:
//
//   <bytes>   ->   SnapMaker::Make()  ->   SnapMaper::RecordEndState()
//             ->   SnapMaker::VerifyPlaysDeterministically()
//             ->   SnapMaker::CheckTrace()
//
// Refer to individual function documentation for details.
//
// This class is thread-compatible.
class SnapMaker {
 public:
  struct Options {
    // Location of the runner binary.
    std::string runner_path = "";

    // How many rw memory pages Make() can add to repair any occurring
    // page faults
    int max_pages_to_add = 5;

    // How many times Verify() will play each snapshot. Higher values provide
    // more confidence that the snapshot is indeed deterministic. The default
    // value is somewhat arbitrary but it should normally be > 1.
    int num_verify_attempts = 5;

    absl::Status Validate() const {
      if (runner_path.empty()) {
        return absl::InvalidArgumentError("runner_path must be non-empty");
      }
      if (max_pages_to_add < 0) {
        return absl::InvalidArgumentError("max_pages_to_add < 0");
      }
      if (num_verify_attempts <= 0) {
        return absl::InvalidArgumentError("num_verify_attempts <= 0");
      }

      return absl::OkStatus();
    }
  };

  explicit SnapMaker(const Options& opts);

  // Not movable or copyable (not needed).
  SnapMaker(const SnapMaker&) = delete;
  SnapMaker(SnapMaker&&) = delete;
  SnapMaker& operator=(const SnapMaker&) = delete;
  SnapMaker& operator=(SnapMaker&&) = delete;

  ~SnapMaker() = default;

  // Make is a function that converts a Snapshot into _potentially_
  // Snap-compatible Snapshot.
  //
  // Its main use is to repair and grow the given Snapshot by adding necessary
  // data mappings.
  //
  // Make() always creates exactly one undefined (i.e. endpoint-only) end state
  // or returns an error.
  //
  // One might want to apply both of these to the snapshot (re)made by Make():
  // RecordEndState() and Snapshot::NormalizeAll().
  absl::StatusOr<Snapshot> Make(const Snapshot& snapshot);

  // Records an expected end state for the input snapshot.
  // RETURNS: A snapshot with exactly one expected end state that satisfies
  // EndState::IsComplete() or an error.
  absl::StatusOr<Snapshot> RecordEndState(const Snapshot& snapshot);

  // Verifies the snapshot plays deterministically i.e. reaches the same
  // expected end state when played multiple times.
  // RETURNS: OkStatus() if the snapshot was successfully verified.
  absl::Status VerifyPlaysDeterministically(const Snapshot& snapshot) const;

  // Single-steps the input snapshot and checks the conditions described below.
  //
  // Returns a Status if the snapshot does one of the following: a) executes
  // a non-deterministic instruction  b) executes an instruction that causes a
  // split lock or c) executes more than X instructions. See trace_options.h for
  // the default value of X.
  // Returns the input snapshot if it passed all the filters.
  // REQUIRES: `snapshot` must be Snapify()-ed.
  absl::StatusOr<Snapshot> CheckTrace(
      const Snapshot& snapshot,
      const TraceOptions& trace_options = TraceOptions::Default()) const;

 private:
  // Adds writable memory pages from `end_state` to `snapshot`. This only
  // adds pages that are not already included in `snapshot`. Pages are always
  // added with RW but not X permissions and must be in an allowable memory
  // range.
  absl::Status AddWritableMemoryForEndState(
      Snapshot* snapshot, const Snapshot::EndState& end_state);

  // C-tor args.
  Options opts_;
};

}  // namespace silifuzz

#endif  // THIRD_PARTY_SILIFUZZ_RUNNER_SNAP_MAKER_H_
