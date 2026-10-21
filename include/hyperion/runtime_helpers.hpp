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
// small. The dispatcher installs it around a run and restores the previous
// value, so nested runs (e.g. a differential test that drives a cache from
// inside a test) do not clobber each other.
//
// ABI: these are called from generated code, so the C ABI is part of the
// contract. Under AAPCS64 a two-`uint64_t` struct is returned in x0 (first
// member) and x1 (second), which is why `LoadResult` returns the loaded value
// and the fault flag together rather than through an out-pointer. `count` is
// 1, 2, 4 or 8; both helpers use the interpreter's little-endian,
// value-in-low-bits convention (interpreter.cpp read_operand/write_operand), so
// a translated access matches the oracle bit-for-bit.
#ifndef HYPERION_RUNTIME_HELPERS_HPP
#define HYPERION_RUNTIME_HELPERS_HPP

#include <cstdint>

namespace hyperion {

class Memory;

// The Memory the helpers resolve against for the current thread. A null value
// makes every helper fault (fault = 1), so a block invoked without a dispatcher
// faults rather than dereferencing a stale pointer.
Memory* current_memory();
Memory* set_current_memory(Memory* memory); // returns the previous value

} // namespace hyperion

// A load returns both the value and a fault flag. `value` is the little-endian
// assembly of `count` bytes at `address` (0 on fault); `fault` is 0 on success
// and 1 on an unmapped address or a missing Memory.
//
// LoadResult is declared at global scope, and the helpers below as plain
// `extern "C"`, because generated code calls them through the C ABI. The type
// is a C-compatible aggregate: two `uint64_t` members in order, which AAPCS64
// returns in x0 and x1. Clang warns that returning a user-defined type from an
// `extern "C"` function is "incompatible with C"; the layout is exactly the two
// registers generated code reads, so the warning is suppressed here rather than
// working around it with an out-pointer (which would add a store on every
// translated load).
struct LoadResult {
    std::uint64_t value = 0;
    std::uint64_t fault = 0;
};

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
extern "C" LoadResult hyperion_load(std::uint64_t address, std::uint64_t count);
// Stores the low `count` bytes of `value` at `address`. Returns 0 on success,
// 1 on a fault.
extern "C" std::uint64_t hyperion_store(std::uint64_t address, std::uint64_t value,
                                        std::uint64_t count);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif // HYPERION_RUNTIME_HELPERS_HPP
