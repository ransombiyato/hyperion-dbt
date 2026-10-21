// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// See runtime_helpers.hpp for the design. These functions are deliberately
// small and branch-free beyond the fault check: they sit on the hot path of
// every translated memory operand.
#include "hyperion/runtime_helpers.hpp"

#include <cstdint>

#include "hyperion/interpreter.hpp"

namespace hyperion {

namespace {
thread_local Memory* tls_memory = nullptr;
} // namespace

Memory* current_memory() {
    return tls_memory;
}

Memory* set_current_memory(Memory* memory) {
    Memory* previous = tls_memory;
    tls_memory = memory;
    return previous;
}

} // namespace hyperion

extern "C" LoadResult hyperion_load(std::uint64_t address, std::uint64_t count) {
    LoadResult result;
    hyperion::Memory* memory = hyperion::tls_memory;
    if (memory == nullptr) {
        result.fault = 1;
        return result;
    }
    std::uint8_t buffer[8] = {};
    if (!memory->read(address, buffer, static_cast<std::size_t>(count))) {
        result.fault = 1;
        return result;
    }
    for (std::size_t i = 0; i < count; ++i) {
        result.value |= static_cast<std::uint64_t>(buffer[i]) << (8 * i);
    }
    return result;
}

extern "C" std::uint64_t hyperion_store(std::uint64_t address, std::uint64_t value,
                                        std::uint64_t count) {
    hyperion::Memory* memory = hyperion::tls_memory;
    if (memory == nullptr) {
        return 1;
    }
    std::uint8_t buffer[8] = {};
    for (std::size_t i = 0; i < count; ++i) {
        buffer[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
    }
    return memory->write(address, buffer, static_cast<std::size_t>(count)) ? 0 : 1;
}
