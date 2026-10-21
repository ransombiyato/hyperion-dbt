// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tier 1 dispatcher: see translation_cache.hpp for the design and the
// correctness rules this file implements.
#include "hyperion/translation_cache.hpp"

#include <string>

#include "hyperion/runtime_helpers.hpp"

namespace hyperion {

namespace {

using BlockFn = BlockResult (*)(GuestState*);

// Guest code is fetched in bounded windows. A block longer than the window is
// simply cut at the window boundary (a fall-through), which is correct: the
// dispatcher re-enters at the next PC and translates the continuation.
constexpr std::size_t kMaxWindow = 4096;

// Try to read a window of guest code starting at `pc`. Returns the number of
// bytes read, shrinking the request until the read succeeds: a Memory
// implementation may fault if the window crosses an unmapped page, and a small
// program image may be shorter than the window. Returns 0 only if not even a
// single byte can be read.
std::size_t read_window(Memory& memory, std::uint64_t pc, std::uint8_t* out, std::size_t capacity) {
    for (std::size_t want = capacity; want >= 1; want /= 2) {
        if (memory.read(pc, out, want)) {
            return want;
        }
        if (want == 1) {
            break;
        }
    }
    return 0;
}

} // namespace

const CachedBlock* TranslationCache::lookup(std::uint64_t pc) {
    const CachedBlock* fast = direct_[pc & kDirectMask];
    if (fast != nullptr && fast->start == pc) {
        ++dispatch_hits_;
        return fast;
    }
    ++dispatch_misses_;
    const auto it = blocks_.find(pc);
    return it == blocks_.end() ? nullptr : it->second.get();
}

void TranslationCache::install(const CachedBlock* block) {
    direct_[block->start & kDirectMask] = block;
}

const CachedBlock* TranslationCache::translate_at(Memory& memory, std::uint64_t pc) {
    const CachedBlock* cached = lookup(pc);
    if (cached != nullptr) {
        return cached;
    }

    if (fetch_.size() < kMaxWindow) {
        fetch_.resize(kMaxWindow);
    }
    const std::size_t available = read_window(memory, pc, fetch_.data(), fetch_.size());
    if (available == 0) {
        return nullptr; // cannot even read one instruction here
    }

    const DecodeResult decoded = decode(fetch_.data(), available, pc);
    if (!decoded.ok()) {
        return nullptr; // not a decodable instruction: report, do not guess
    }

    const BlockTranslation translated =
        translate_block(fetch_.data(), available, decoded.instruction);
    if (!translated.ok() || translated.code.empty()) {
        return nullptr;
    }

    auto block = std::make_unique<CachedBlock>();
    block->memory = std::make_unique<ExecMem>(translated.code.size());
    block->memory->write(0, translated.code.data(), translated.code.size());
    block->memory->finalize();
    block->exit = translated.exit;
    block->start = pc;
    block->target = translated.target;
    block->guest_instruction_count = translated.guest_instruction_count;
    block->code_bytes = translated.code.size();

    const CachedBlock* raw = block.get();
    blocks_.emplace(pc, std::move(block));
    install(raw);
    return raw;
}

std::size_t TranslationCache::total_code_bytes() const {
    std::size_t total = 0;
    for (const auto& entry : blocks_) {
        total += entry.second->code_bytes;
    }
    return total;
}

RunResult TranslationCache::run(Memory& memory, GuestState state, std::uint64_t max_steps,
                                SyscallHandler* syscalls) {
    RunResult out;
    out.state = state;

    if (!ExecMem::executable_supported()) {
        out.status = RunStatus::kUnsupported;
        out.message = "host cannot execute generated AArch64 code";
        return out;
    }

    // Generated code reaches guest memory through the runtime helpers, which
    // resolve against the current thread's Memory. Install it for the duration
    // of the run and restore whatever was there before, so a nested run (a test
    // driving the cache from inside another run) cannot clobber it.
    struct MemoryScope {
        Memory* previous;
        ~MemoryScope() { set_current_memory(previous); }
    } memory_scope{set_current_memory(&memory)};

    std::uint64_t steps = 0;
    while (true) {
        const std::uint64_t pc = out.state.rip;
        const CachedBlock* block = translate_at(memory, pc);
        if (block == nullptr) {
            out.status = RunStatus::kUnsupported;
            out.message = "unsupported instruction at pc=0x" + std::to_string(pc);
            out.steps = steps;
            return out;
        }

        // max_steps is a hard bound on guest instructions executed. A block is
        // indivisible, so a block that cannot fit in the remaining budget is not
        // started at all. This means a step-limited run may stop a little short
        // of the limit and can report fewer steps than the interpreter (which
        // steps one instruction at a time), but it never exceeds the bound.
        if (block->guest_instruction_count > max_steps - steps) {
            out.status = RunStatus::kStepLimit;
            out.message = "step limit reached without halting";
            out.steps = steps;
            return out;
        }

        const BlockFn fn = reinterpret_cast<BlockFn>(block->memory->entry());
        const BlockResult result = fn(&out.state);
        steps += block->guest_instruction_count;

        if (result == BlockResult::kHalted) {
            out.status = RunStatus::kOk;
            out.steps = steps;
            return out;
        }
        if (result == BlockResult::kMemoryFault) {
            // A translated load/store faulted. rip points at the faulting
            // instruction (the block did not advance it), so the report names
            // the exact address rather than a resume point.
            out.status = RunStatus::kFault;
            out.message = "guest memory fault at pc=0x" + std::to_string(out.state.rip);
            out.steps = steps;
            return out;
        }
        if (result == BlockResult::kSyscall) {
            // The block advanced rip past the instruction and stopped. Run the
            // handler here, in the dispatcher, where the Memory object and the
            // handler are available; generated code cannot call back into C++.
            if (syscalls == nullptr) {
                out.status = RunStatus::kUnsupported;
                out.message = "syscall encountered with no handler";
                out.steps = steps;
                return out;
            }
            const SyscallResult sr = syscalls->handle(out.state, memory);
            if (sr.status == SyscallStatus::kExit) {
                out.status = RunStatus::kOk;
                out.message = "guest exited via syscall";
                out.steps = steps;
                return out;
            }
            if (sr.status == SyscallStatus::kUnimplemented) {
                ++out.unimplemented_syscalls;
            }
            continue; // resume at the (already advanced) rip
        }
        // Otherwise the block wrote the next PC into out.state.rip; loop.
    }
}

} // namespace hyperion
