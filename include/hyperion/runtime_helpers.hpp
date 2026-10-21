// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Runtime helpers called by generated AArch64 code.
//
// The guest address space is a set of host-backed regions at arbitrary guest
// addresses (address_space.hpp), so generated code cannot dereference a guest
// address directly: guest address A is not host address A. A memory operand is
// therefore lowered to a call into one of these helpers, which resolve the
// address through the same Memory interface the interpreter uses and copy the
// bytes. This is the standard soft-MMU approach; it keeps one definition of
// "valid access" for both tiers.
//
// The current Memory* is held in a thread-local so the helper signature stays
// small (address in x0, value in x1). The dispatcher installs it around a run
// and restores the previous value, so nested runs (e.g. a differential test
// that drives a cache from inside a test) do not clobber each other.
#ifndef HYPERION_RUNTIME_HELPERS_HPP
#define HYPERION_RUNTIME_HELPERS_HPP

#include <cstddef>
#include <cstdint>

namespace hyperion {

class Memory;

// The Memory the helpers resolve against for the current thread. A null value
// makes every helper fail (returns 1), so a block that is invoked without a
// dispatcher faults rather than dereferencing a stale pointer.
Memory* current_memory();
Memory* set_current_memory(Memory* memory); // returns the previous value

// Copy `count` bytes between the guest address `address` and the buffer at
// `value`. Return 0 on success, 1 on a fault (unmapped address or no Memory).
// `count` is 1, 2, 4 or 8; the helpers use the same little-endian,
// value-in-low-bits convention the interpreter uses (interpreter.cpp
// read_operand/write_operand), so a load/store matches it bit-for-bit.
extern "C" std::uint64_t hyperion_load(std::uint64_t address, std::uint64_t* value,
                                       std::uint64_t count);
extern "C" std::uint64_t hyperion_store(std::uint64_t address, std::uint64_t value,
                                        std::uint64_t count);

} // namespace hyperion

#endif // HYPERION_RUNTIME_HELPERS_HPP
