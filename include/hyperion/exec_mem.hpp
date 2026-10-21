// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Executable memory for the Phase 3 code generator.
//
// Design (see DECISIONS.md D-016 and SECURITY.md):
//   - Memory is allocated read-write, filled, then made read-execute. It is
//     never simultaneously writable and executable, so the translator does not
//     need to disable W^X to work.
//   - After making the region executable the instruction cache is cleared.
//     On AArch64 a missing I-cache maintenance step produces stale code
//     execution, which is a classic and very confusing bug.
//   - The region is owned and freed on destruction; nothing is leaked and no
//     address is ever handed out that outlives the object.
#ifndef HYPERION_EXEC_MEM_HPP
#define HYPERION_EXEC_MEM_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace hyperion {

// A page-aligned region that can be written and then executed.
class ExecMem {
public:
    explicit ExecMem(std::size_t bytes);
    ~ExecMem();
    ExecMem(const ExecMem&) = delete;
    ExecMem& operator=(const ExecMem&) = delete;
    ExecMem(ExecMem&& other) noexcept;
    ExecMem& operator=(ExecMem&& other) noexcept;

    // Writable view. Valid only before finalize().
    std::uint8_t* writable();
    std::size_t size() const { return size_; }

    // Copy `bytes` into the region at `offset`. Throws if it would not fit or
    // if the region has already been finalized.
    void write(std::size_t offset, const std::uint8_t* bytes, std::size_t count);

    // Make the region read-execute and flush the instruction cache. Idempotent.
    void finalize();

    // Address of the code, suitable for calling. Requires finalize() first.
    void* entry() const;

    // Whether this platform can execute memory the program generated. False on
    // a host whose ISA differs from the target, and false if W^X is enforced
    // without a way to obtain an executable mapping.
    static bool executable_supported();

private:
    void release() noexcept;

    void* base_ = nullptr;
    std::size_t size_ = 0;
    bool finalized_ = false;
};

// Reusable W^X arena for translated basic blocks. Allocates multi-page slabs
// once instead of calling mmap/mprotect per 64-byte basic block, switching the
// active slab RW only during `allocate()` and immediately restoring RX +
// I-cache coherency over the newly written slice before returning.
class ExecArena {
public:
    explicit ExecArena(std::size_t slab_bytes = 64ULL * 1024ULL);
    ~ExecArena();
    ExecArena(const ExecArena&) = delete;
    ExecArena& operator=(const ExecArena&) = delete;

    // Copy `size` bytes (4-byte aligned) into RX memory and return entry ptr.
    void* allocate(const std::uint8_t* code, std::size_t size);

    // Drop all live allocations and reuse the first slab on subsequent runs.
    void reset() noexcept;

private:
    struct Slab {
        std::uint8_t* base = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    std::size_t slab_bytes_ = 0;
    Slab* slabs_ = nullptr;
    std::size_t slab_count_ = 0;
    std::size_t slab_cap_ = 0;
};

} // namespace hyperion

#endif // HYPERION_EXEC_MEM_HPP
