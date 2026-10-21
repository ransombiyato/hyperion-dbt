// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Shared result types for running a guest program to completion.
//
// Both execution tiers report through these types: the Tier 0 reference
// interpreter (interpreter.hpp) and the Tier 1 AArch64 translator
// (translation_cache.hpp). Sharing them means the differential test can compare
// two tiers by their status alone, and callers do not learn a new vocabulary
// per tier.
#ifndef HYPERION_RUN_HPP
#define HYPERION_RUN_HPP

#include <cstdint>
#include <string>

#include "hyperion/guest_state.hpp"

namespace hyperion {

// Result of running a program image to completion.
enum class RunStatus : std::uint8_t {
    kOk,          // program halted (HLT/RET) normally
    kStepLimit,   // max_steps reached without halting
    kFault,       // memory fault during execution
    kUnsupported, // hit an instruction outside the supported subset
};

const char* to_string(RunStatus status);

struct RunResult {
    RunStatus status = RunStatus::kOk;
    std::uint64_t steps = 0; // instructions retired
    std::string message;     // human-readable reason on non-kOk
    // Number of guest syscalls whose handler returned -ENOSYS. Non-zero means
    // the program asked for something Hyperion does not implement yet; it is
    // recorded so an unimplemented call is never invisible.
    std::uint64_t unimplemented_syscalls = 0;
    bool exited_via_syscall = false;
    std::int64_t exit_code = 0;
    // The final architectural state, valid when status == kOk or kStepLimit.
    GuestState state;
};

} // namespace hyperion

#endif // HYPERION_RUN_HPP
