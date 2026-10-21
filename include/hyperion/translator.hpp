// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 -> AArch64 translation of the Phase 2 instruction subset.
//
// Scope of this first translator (Phase 3, increment 1) - deliberately small
// and stated rather than implied:
//   - Register and immediate operands only. A memory operand is reported as
//     unsupported by the translator, not silently miscompiled; memory needs an
//     address space that does not exist yet.
//   - MOV, ADD, SUB, AND, OR, XOR, CMP, INC, DEC, NOP, HLT.
//   - Control flow (JMP, Jcc, RET) and the stack (PUSH, POP) are Phase 4: they
//     need block exits and a translation cache to be meaningful.
//
// Guest registers live in the GuestState struct in memory; generated code
// loads and stores them. There is no guest-register allocation yet - that is a
// Phase 5 optimisation. Every x86 flag (CF/PF/AF/ZF/SF/OF) is computed in
// generated code from the AArch64 condition flags plus a couple of ALU
// identities, so the result is a genuine, independently-checkable translation
// rather than a call back into the interpreter. The interpreter remains the
// oracle (see DECISIONS.md D-003, D-014).
#ifndef HYPERION_TRANSLATOR_HPP
#define HYPERION_TRANSLATOR_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hyperion/aarch64_encoder.hpp"
#include "hyperion/decoder.hpp"

namespace hyperion {

// Why a decoded instruction could not be translated. Explicit so a caller can
// distinguish "not in the translator's scope yet" from a real internal error.
enum class TranslateError {
    kNone = 0,
    kUnsupportedOpcode,  // control flow, stack: not in this increment
    kUnsupportedOperand, // memory operand, or an operand shape we do not emit
    kUnsupportedSize,    // operand width outside 32/64
    kInternal,           // a bug: the decoder produced something inconsistent
};

const char* to_string(TranslateError error);

struct Translation {
    TranslateError error = TranslateError::kNone;
    // AArch64 code for a block that is called as `uint64_t block(GuestState*)`
    // and returns kBlockOk or kBlockHalted (see below). Empty on error.
    std::vector<std::uint8_t> code;
    // Number of AArch64 instructions emitted (diagnostics/tests).
    std::size_t instruction_count = 0;

    bool ok() const { return error == TranslateError::kNone; }
};

// Value returned in x0 by a translated block.
enum class BlockResult : std::uint64_t {
    kOk = 0,          // block completed; guest state (including rip) is updated
    kHalted = 1,      // HLT/RET: the program has stopped
    kSyscall = 2,     // a `syscall` instruction was reached; the dispatcher must
                      // handle it. rip already points past the instruction.
    kMemoryFault = 3, // a guest load/store faulted (unmapped or no Memory
                      // installed). The block stopped at the faulting
                      // instruction; rip points at it, not past it, so the
                      // dispatcher can report the exact address.
};

// How a translated block left. Purely informational for callers: the
// authoritative next PC is always GuestState::rip, which the block writes.
enum class BlockExit : std::uint8_t {
    kFallThrough, // ran to the end of the decoded sequence (or hit an
                  // instruction the translator does not support)
    kJmp,         // unconditional JMP
    kJcc,         // conditional branch; the runtime decides taken/not-taken
    kHalted,      // HLT or RET: the program has stopped
    kSyscall,     // a `syscall` instruction ended the block
};

const char* to_string(BlockExit exit);

// The result of translating one basic block (a straight-line run of
// instructions ending in a terminator, or in an instruction the translator
// does not support).
struct BlockTranslation {
    TranslateError error = TranslateError::kNone;
    // AArch64 code for `BlockResult block(GuestState*)`. Empty on error.
    std::vector<std::uint8_t> code;
    std::size_t instruction_count = 0;       // AArch64 instructions emitted
    std::size_t guest_instruction_count = 0; // guest instructions covered

    BlockExit exit = BlockExit::kFallThrough;
    // Predicted successor for a fall-through or JMP (correct for both). For
    // kJcc it is the fall-through address and is only a hint; the dispatcher
    // must read the real PC from GuestState::rip. Unused for kHalted.
    std::uint64_t target = 0;

    bool ok() const { return error == TranslateError::kNone; }
};

// Translate a basic block starting at `instruction.address` in `code`, whose
// first instruction has already been decoded. Decoding continues until a
// terminator (JMP/Jcc/HLT/RET), an instruction outside the translated subset
// (the block is cut *before* it), or the end of `size` bytes.
//
// A block that hits an unsupported instruction still returns ok() with a valid
// block: the block covers only the supported prefix and its exit is
// kFallThrough, so the dispatcher can detect the unsupported PC on the next
// lookup and stop cleanly. This is what makes "do not silently execute
// incorrect code" true at the program level.
//
// For kJcc the block evaluates the x86 condition from the modelled flags and
// writes the correct next rip itself, so `BlockResult` alone is enough to know
// whether the program halted.
BlockTranslation translate_block(const std::uint8_t* code, std::size_t size,
                                 const Instruction& first);

// Translate one decoded x86-64 instruction into a self-contained AArch64 block
// with the signature `BlockResult block(GuestState*)`. The block reads and
// writes the guest state in place, updates rip for non-halting instructions,
// and returns kBlockOk or kBlockHalted.
//
// On an unsupported instruction the returned Translation carries an error and
// no code; nothing is silently emitted.
Translation translate_instruction(const Instruction& instruction);

} // namespace hyperion

#endif // HYPERION_TRANSLATOR_HPP
