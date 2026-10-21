// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Runtime-helper unit tests. The helpers are the only way generated code
// reaches the region-based guest address space, so their fault behaviour and
// their byte/endianness convention are pinned down here, independent of any
// AArch64 host: they run natively on x86-64 as ordinary C++.
#include <cstdint>

#include "harness/test_harness.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/runtime_helpers.hpp"

using hyperion::FlatMemory;
using hyperion::hyperion_load;
using hyperion::hyperion_store;
using hyperion::Memory;
using hyperion::set_current_memory;

HYPERION_TEST("runtime_helpers", "no_memory_installed_faults") {
    // With no Memory installed a block must fault, not dereference a stale
    // pointer. The dispatcher installs one around a run.
    Memory* previous = set_current_memory(nullptr);
    std::uint64_t value = 0xDEADBEEFULL;
    HYPERION_CHECK_EQ(hyperion_load(0x1000, &value, 8), 1u);
    HYPERION_CHECK_EQ(hyperion_store(0x1000, 0x1234, 4), 1u);
    HYPERION_CHECK_EQ(value, 0xDEADBEEFULL); // untouched on fault
    set_current_memory(previous);
}

HYPERION_TEST("runtime_helpers", "load_store_roundtrip_all_widths") {
    FlatMemory memory(0x1000, 0x100);
    Memory* previous = set_current_memory(&memory);

    // Little-endian, value-in-low-bits, exactly the interpreter's convention.
    HYPERION_CHECK_EQ(hyperion_store(0x1010, 0x1122334455667788ULL, 8), 0u);
    std::uint64_t out = 0;
    HYPERION_CHECK_EQ(hyperion_load(0x1010, &out, 8), 0u);
    HYPERION_CHECK_EQ(out, 0x1122334455667788ULL);

    // A 4-byte store writes only the low four bytes and leaves the rest.
    HYPERION_CHECK_EQ(hyperion_store(0x1020, 0xAABBCCDDULL, 4), 0u);
    HYPERION_CHECK_EQ(hyperion_load(0x1020, &out, 8), 0u);
    HYPERION_CHECK_EQ(out, 0x00000000AABBCCDDULL);

    // 1- and 2-byte widths truncate the value to the low bits.
    HYPERION_CHECK_EQ(hyperion_store(0x1030, 0xFFFF12ULL, 1), 0u);
    HYPERION_CHECK_EQ(hyperion_load(0x1030, &out, 1), 0u);
    HYPERION_CHECK_EQ(out, 0x12ULL);
    HYPERION_CHECK_EQ(hyperion_store(0x1032, 0xFFFF1234ULL, 2), 0u);
    HYPERION_CHECK_EQ(hyperion_load(0x1032, &out, 2), 0u);
    HYPERION_CHECK_EQ(out, 0x1234ULL);

    set_current_memory(previous);
}

HYPERION_TEST("runtime_helpers", "out_of_range_access_faults") {
    FlatMemory memory(0x1000, 0x100);
    Memory* previous = set_current_memory(&memory);

    // 8 bytes starting one past the last byte cross the end of the region.
    std::uint64_t out = 0;
    HYPERION_CHECK_EQ(hyperion_load(0x1100, &out, 8), 1u);
    HYPERION_CHECK_EQ(hyperion_load(0x0FFF, &out, 1), 1u); // just below the base
    HYPERION_CHECK_EQ(hyperion_store(0x1100, 1, 1), 1u);

    set_current_memory(previous);
}

HYPERION_TEST("runtime_helpers", "set_current_memory_returns_previous") {
    // The dispatcher relies on this to save/restore around a nested run.
    FlatMemory a(0x1000, 0x10);
    FlatMemory b(0x2000, 0x10);
    Memory* original = set_current_memory(&a);
    HYPERION_CHECK(hyperion::current_memory() == &a);
    Memory* prev = set_current_memory(&b);
    HYPERION_CHECK(prev == &a);
    HYPERION_CHECK(hyperion::current_memory() == &b);
    set_current_memory(original);
    HYPERION_CHECK(hyperion::current_memory() == original);
}
