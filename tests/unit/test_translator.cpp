// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Translator tests.
//
// Two layers, deliberately separated:
//   - Scope tests run everywhere: they assert that the translator accepts the
//     instructions it claims and *refuses* the ones it does not, so an
//     unsupported instruction can never be silently miscompiled.
//   - Differential tests run only on AArch64: they execute the generated block
//     and compare the resulting GuestState against the reference interpreter
//     instruction by instruction. On a non-AArch64 host they SKIP rather than
//     pretend the generated code ran.
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/decoder.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/translation_cache.hpp"
#include "hyperion/translator.hpp"

using hyperion::BlockResult;
using hyperion::DecodeResult;
using hyperion::ExecMem;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::StepStatus;
using hyperion::TranslateError;
using hyperion::Translation;

namespace {

constexpr std::uint64_t kBase = 0x400000;

Translation translate_bytes(const std::vector<std::uint8_t>& bytes) {
    const DecodeResult decoded = hyperion::decode(bytes.data(), bytes.size(), kBase);
    HYPERION_CHECK(decoded.ok());
    return hyperion::translate_instruction(decoded.instruction);
}

} // namespace

// --- Scope: what the translator accepts and what it refuses -----------------

HYPERION_TEST("translator", "accepts_register_and_immediate_alu") {
    // mov rax, imm64 ; add rax, rbx ; and rcx, rdx ; xor r8, r9 ; inc r10
    for (const std::vector<std::uint8_t>& bytes : std::vector<std::vector<std::uint8_t>>{
             {0x48, 0xB8, 0x34, 0x12, 0, 0, 0, 0, 0, 0}, // mov rax, 0x1234
             {0x48, 0x01, 0xD8},                         // add rax, rbx
             {0x48, 0x21, 0xD1},                         // and rcx, rdx
             {0x4D, 0x31, 0xC8},                         // xor r8, r9
             {0x49, 0xFF, 0xC2},                         // inc r10
             {0x48, 0x83, 0xC0, 0x05},                   // add rax, 5
             {0x90},                                     // nop
             {0xF4},                                     // hlt
         }) {
        const Translation t = translate_bytes(bytes);
        HYPERION_CHECK(t.ok());
        HYPERION_CHECK(!t.code.empty());
    }
}

HYPERION_TEST("translator", "refuses_control_flow_and_stack") {
    // jmp rel32, je rel8, push rax, pop rax, ret -- Phase 4 work.
    for (const std::vector<std::uint8_t>& bytes : std::vector<std::vector<std::uint8_t>>{
             {0xE9, 0x00, 0x00, 0x00, 0x00}, // jmp +0
             {0x74, 0x00},                   // je +0
             {0x50},                         // push rax
             {0x58},                         // pop rax
             {0xC3},                         // ret
         }) {
        const Translation t = translate_bytes(bytes);
        HYPERION_CHECK_EQ(static_cast<int>(t.error),
                          static_cast<int>(TranslateError::kUnsupportedOpcode));
        HYPERION_CHECK(t.code.empty());
    }
}

HYPERION_TEST("translator", "translates_mov_memory_forms") {
    // mov rbx, [rax] ; mov [rax], rbx -- lowered to a runtime-helper call
    // (soft MMU), because guest addresses are not host addresses.
    for (const std::vector<std::uint8_t>& bytes : std::vector<std::vector<std::uint8_t>>{
             {0x48, 0x8B, 0x18},             // mov rbx, [rax]
             {0x48, 0x89, 0x18},             // mov [rax], rbx
             {0x8B, 0x18},                   // mov ebx, [rax] (32-bit load, zero-extends)
             {0x89, 0x18},                   // mov [rax], ebx
             {0x48, 0x8B, 0x5C, 0x8B, 0x10}, // mov rbx, [rax + rcx*4 + 0x10]
         }) {
        const Translation t = translate_bytes(bytes);
        HYPERION_CHECK(t.ok());
        HYPERION_CHECK(!t.code.empty());
    }
}

HYPERION_TEST("translator", "translates_alu_memory_forms") {
    // ALU read-modify-write and compare-through-memory are lowered to a
    // soft-MMU helper load, the register operation, and a helper store.
    for (const std::vector<std::uint8_t>& bytes : std::vector<std::vector<std::uint8_t>>{
             {0x48, 0x03, 0x03},             // add rax, [rbx]
             {0x48, 0x01, 0x03},             // add [rbx], rax
             {0x48, 0x2B, 0x03},             // sub rax, [rbx]
             {0x48, 0x29, 0x03},             // sub [rbx], rax
             {0x48, 0x21, 0x03},             // and [rbx], rax
             {0x48, 0x09, 0x03},             // or  [rbx], rax
             {0x48, 0x31, 0x03},             // xor [rbx], rax
             {0x48, 0x39, 0x03},             // cmp [rbx], rax
             {0x48, 0x3B, 0x03},             // cmp rax, [rbx]
             {0x48, 0x83, 0x03, 0x05},       // add qword [rbx], 5
             {0x83, 0x03, 0x05},             // add dword [rbx], 5
             {0xFF, 0x03},                   // inc dword [rbx]
             {0x48, 0xFF, 0x03},             // inc qword [rbx]
             {0x48, 0xFF, 0x0B},             // dec qword [rbx]
             {0x48, 0x01, 0x5C, 0x8B, 0x10}, // add [rbx + rcx*4 + 0x10], rbx
         }) {
        const Translation t = translate_bytes(bytes);
        HYPERION_CHECK(t.ok());
        HYPERION_CHECK(!t.code.empty());
    }
}

// --- Scope: basic-block translation -----------------------------------------

HYPERION_TEST("translator", "block_ends_at_terminators") {
    // A block runs up to and including its terminator, then stops.
    {
        // add eax, ecx ; dec ecx ; jnz -6 ; hlt
        const std::vector<std::uint8_t> program = {0x01, 0xC8, 0xFF, 0xC9, 0x75, 0xFA, 0xF4};
        const DecodeResult first = hyperion::decode(program.data(), program.size(), kBase);
        HYPERION_CHECK(first.ok());
        // Translating a block from the first instruction: it is a body
        // instruction, so the block continues to the Jcc and stops there.
        const hyperion::BlockTranslation b =
            hyperion::translate_block(program.data(), program.size(), first.instruction);
        HYPERION_CHECK(b.ok());
        HYPERION_CHECK(b.exit == hyperion::BlockExit::kJcc);
        // add + dec + jcc = 3 guest instructions.
        HYPERION_CHECK_EQ(b.guest_instruction_count, std::size_t{3});
        // Fall-through hint is the address just past the Jcc.
        HYPERION_CHECK_EQ(b.target, kBase + 6);
        HYPERION_CHECK(!b.code.empty());
    }
    {
        // hlt on its own: a one-instruction block that halts.
        const std::vector<std::uint8_t> program = {0xF4};
        const DecodeResult first = hyperion::decode(program.data(), program.size(), kBase);
        HYPERION_CHECK(first.ok());
        const hyperion::BlockTranslation b =
            hyperion::translate_block(program.data(), program.size(), first.instruction);
        HYPERION_CHECK(b.ok());
        HYPERION_CHECK(b.exit == hyperion::BlockExit::kHalted);
        HYPERION_CHECK_EQ(b.guest_instruction_count, std::size_t{1});
    }
    {
        // jmp +0 : a one-instruction block that transfers control.
        const std::vector<std::uint8_t> program = {0xE9, 0x00, 0x00, 0x00, 0x00};
        const DecodeResult first = hyperion::decode(program.data(), program.size(), kBase);
        HYPERION_CHECK(first.ok());
        const hyperion::BlockTranslation b =
            hyperion::translate_block(program.data(), program.size(), first.instruction);
        HYPERION_CHECK(b.ok());
        HYPERION_CHECK(b.exit == hyperion::BlockExit::kJmp);
        HYPERION_CHECK_EQ(b.target, kBase + 5); // rel is from the end of the jmp
    }
}

HYPERION_TEST("translator", "block_cuts_before_unsupported") {
    // mov eax, 1 ; push rax ; hlt
    // The block covers only `mov` and cuts *before* `push`, so the dispatcher
    // sees the push PC and stops rather than miscompiling it.
    const std::vector<std::uint8_t> program = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x50, 0xF4};
    const DecodeResult first = hyperion::decode(program.data(), program.size(), kBase);
    HYPERION_CHECK(first.ok());
    const hyperion::BlockTranslation b =
        hyperion::translate_block(program.data(), program.size(), first.instruction);
    HYPERION_CHECK(b.ok());
    HYPERION_CHECK(b.exit == hyperion::BlockExit::kFallThrough);
    HYPERION_CHECK_EQ(b.guest_instruction_count, std::size_t{1});
    HYPERION_CHECK_EQ(b.target, kBase + 5); // the push, not the hlt
}

HYPERION_TEST("translator", "block_cuts_at_buffer_end") {
    // Two adds with no terminator: the block runs off the end of the buffer and
    // falls through past the last instruction.
    const std::vector<std::uint8_t> program = {0x01, 0xC8, 0x01, 0xC8};
    const DecodeResult first = hyperion::decode(program.data(), program.size(), kBase);
    HYPERION_CHECK(first.ok());
    const hyperion::BlockTranslation b =
        hyperion::translate_block(program.data(), program.size(), first.instruction);
    HYPERION_CHECK(b.ok());
    HYPERION_CHECK(b.exit == hyperion::BlockExit::kFallThrough);
    HYPERION_CHECK_EQ(b.guest_instruction_count, std::size_t{2});
    HYPERION_CHECK_EQ(b.target, kBase + 4);
}

// --- Differential: generated code vs. the reference interpreter -------------

#if defined(__aarch64__)

namespace {

// Run one instruction through the interpreter, returning the resulting state.
GuestState interpret_one(const std::vector<std::uint8_t>& bytes, GuestState initial) {
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), bytes.data(), bytes.size());
    const auto r = hyperion::step(initial, memory);
    HYPERION_CHECK(r.status == StepStatus::kOk || r.status == StepStatus::kHalted);
    return initial;
}

// Decode, translate and execute one instruction; return the resulting state.
GuestState translate_and_run(const std::vector<std::uint8_t>& bytes, GuestState initial) {
    const DecodeResult decoded = hyperion::decode(bytes.data(), bytes.size(), kBase);
    HYPERION_CHECK(decoded.ok());
    const Translation t = hyperion::translate_instruction(decoded.instruction);
    HYPERION_CHECK(t.ok());

    ExecMem mem(t.code.size());
    mem.write(0, t.code.data(), t.code.size());
    mem.finalize();

    using BlockFn = BlockResult (*)(GuestState*);
    auto fn = reinterpret_cast<BlockFn>(mem.entry());
    const BlockResult result = fn(&initial);

    // The interpreter and the translator must agree on whether the program
    // stopped: HLT maps to kHalted, everything else to kOk.
    const bool halted = decoded.instruction.opcode == hyperion::Opcode::kHlt;
    HYPERION_CHECK_EQ(static_cast<std::uint64_t>(result),
                      static_cast<std::uint64_t>(halted ? BlockResult::kHalted : BlockResult::kOk));
    return initial;
}

void check_matches_interpreter(const std::vector<std::uint8_t>& bytes, GuestState initial) {
    const GuestState expected = interpret_one(bytes, initial);
    const GuestState actual = translate_and_run(bytes, initial);
    for (std::size_t i = 0; i < hyperion::kGprCount; ++i) {
        HYPERION_CHECK_EQ(actual.gpr[i], expected.gpr[i]);
    }
    HYPERION_CHECK_EQ(actual.rip, expected.rip);
    HYPERION_CHECK(actual.flags == expected.flags);
}

} // namespace

HYPERION_TEST("translator", "differential_mov_and_alu") {
    GuestState base;
    base.rip = kBase;
    base.reg(Gpr::kRax) = 0x1122334455667788ULL;
    base.reg(Gpr::kRbx) = 0x0102030405060708ULL;
    base.reg(Gpr::kRcx) = 0xFFFFFFFF00000001ULL;
    base.reg(Gpr::kRdx) = 0x8000000000000001ULL;
    base.reg(Gpr::kR10) = 0x00000000FFFFFFFEULL;
    base.flags.cf = true;
    base.flags.zf = false;

    const std::vector<std::vector<std::uint8_t>> cases = {
        {0x48, 0xB8, 0x34, 0x12, 0, 0, 0, 0, 0, 0}, // mov rax, 0x1234
        {0xB8, 0x78, 0x56, 0x34, 0x12},             // mov eax, 0x12345678 (zero-extends)
        {0x48, 0x01, 0xD8},                         // add rax, rbx
        {0x48, 0x29, 0xD8},                         // sub rax, rbx
        {0x48, 0x21, 0xD8},                         // and rax, rbx
        {0x48, 0x09, 0xD8},                         // or  rax, rbx
        {0x48, 0x31, 0xD8},                         // xor rax, rbx
        {0x48, 0x39, 0xD8},                         // cmp rax, rbx
        {0x01, 0xD8},                               // add eax, ebx (32-bit flags)
        {0x29, 0xD8},                               // sub eax, ebx
        {0x48, 0x83, 0xC0, 0x7F},                   // add rax, 127
        {0x48, 0xFF, 0xC0},                         // inc rax
        {0x48, 0xFF, 0xC8},                         // dec rax
        {0x49, 0xFF, 0xC2},                         // inc r10
        {0x90},                                     // nop
        {0xF4},                                     // hlt
    };
    for (const auto& bytes : cases) {
        check_matches_interpreter(bytes, base);
    }
}

HYPERION_TEST("translator", "differential_flags_are_exact") {
    // Sweep operand values that stress carry/overflow/parity boundaries, and
    // require every one of the six modelled flags to match the interpreter.
    const std::uint64_t values[] = {
        0x0000000000000000ULL, 0x0000000000000001ULL, 0x000000000000000FULL, 0x0000000000000010ULL,
        0x00000000000000FFULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
        0x00000000FFFFFFFFULL, 0x0000000100000000ULL, 0xDEADBEEFCAFEF00DULL,
    };
    const std::vector<std::vector<std::uint8_t>> ops = {
        {0x48, 0x01, 0xD8}, // add rax, rbx
        {0x48, 0x29, 0xD8}, // sub rax, rbx
        {0x48, 0x31, 0xD8}, // xor rax, rbx
        {0x48, 0x21, 0xD8}, // and rax, rbx
        {0x48, 0x39, 0xD8}, // cmp rax, rbx
        {0x01, 0xD8},       // add eax, ebx
        {0x29, 0xD8},       // sub eax, ebx
    };
    for (const std::uint64_t a : values) {
        for (const std::uint64_t b : values) {
            for (const auto& bytes : ops) {
                GuestState initial;
                initial.rip = kBase;
                initial.reg(Gpr::kRax) = a;
                initial.reg(Gpr::kRbx) = b;
                check_matches_interpreter(bytes, initial);
            }
        }
    }
}

// --- Differential: whole programs through the translation cache --------------

namespace {

// Run a whole program through both tiers and require identical final state.
// This is the strongest check in the suite: the generated blocks are executed
// by real hardware (under qemu-user on CI) while the interpreter independently
// models the same semantics.
void check_program_matches(const std::vector<std::uint8_t>& program, GuestState initial,
                           std::uint64_t max_steps = 1'000'000) {
    FlatMemory interp_mem(kBase, 4096);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial, max_steps);

    FlatMemory tier1_mem(kBase, 4096);
    std::memcpy(tier1_mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    const hyperion::RunResult actual = cache.run(tier1_mem, initial, max_steps);

    HYPERION_CHECK_EQ(static_cast<int>(actual.status), static_cast<int>(expected.status));
    if (expected.status == hyperion::RunStatus::kOk) {
        // A halted program must agree exactly, including the instruction count.
        HYPERION_CHECK_EQ(actual.steps, expected.steps);
        for (std::size_t i = 0; i < hyperion::kGprCount; ++i) {
            HYPERION_CHECK_EQ(actual.state.gpr[i], expected.state.gpr[i]);
        }
        HYPERION_CHECK_EQ(actual.state.rip, expected.state.rip);
        HYPERION_CHECK(actual.state.flags == expected.state.flags);
    } else if (expected.status == hyperion::RunStatus::kStepLimit) {
        // Blocks are indivisible, so tier 1 may stop a few instructions short of
        // the limit, but it must never exceed it and must never run past it.
        HYPERION_CHECK(actual.steps <= expected.steps);
        HYPERION_CHECK(actual.steps <= max_steps);
    }
}

} // namespace

HYPERION_TEST("translator", "differential_program_loops") {
    // Count down from rcx, accumulating rbx += rax each iteration:
    //   xor ebx, ebx            ; 31 DB
    // loop:
    //   add rbx, rax            ; 48 01 C3
    //   dec rcx                 ; 48 FF C9
    //   jnz loop                ; 75 F8
    //   hlt                     ; F4
    const std::vector<std::uint8_t> program = {
        0x31, 0xDB, 0x48, 0x01, 0xC3, 0x48, 0xFF, 0xC9, 0x75, 0xF8, 0xF4,
    };
    for (std::uint64_t count :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{5}, std::uint64_t{1000}}) {
        GuestState initial;
        initial.rip = kBase;
        initial.reg(Gpr::kRax) = 7;
        initial.reg(Gpr::kRcx) = count;
        check_program_matches(program, initial);
    }
}

HYPERION_TEST("translator", "differential_branch_directions") {
    // A conditional branch must take the correct path for many flag states.
    //   cmp rax, rbx            ; 48 39 D8
    //   je  taken               ; 74 05
    //   mov ecx, 0x1111         ; B9 11 11 00 00
    //   jmp done                ; EB 03
    // taken:
    //   mov ecx, 0x2222         ; B9 22 22 00 00
    // done:
    //   hlt                     ; F4
    const std::vector<std::uint8_t> program = {
        0x48, 0x39, 0xD8, 0x74, 0x05, 0xB9, 0x11, 0x11, 0x00,
        0x00, 0xEB, 0x03, 0xB9, 0x22, 0x22, 0x00, 0x00, 0xF4,
    };
    for (std::uint64_t a :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xFFFFFFFFFFFFFFFFULL}}) {
        for (std::uint64_t b :
             {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xFFFFFFFFFFFFFFFFULL}}) {
            GuestState initial;
            initial.rip = kBase;
            initial.reg(Gpr::kRax) = a;
            initial.reg(Gpr::kRbx) = b;
            check_program_matches(program, initial);
        }
    }
}

HYPERION_TEST("translator", "differential_nested_loops") {
    // Nested counted loops, both branching backwards, so the block cache is
    // re-entered at two distinct PCs. Bytes verified with `as`/`objdump`.
    //   xor edx, edx            ; 31 D2
    // outer:
    //   mov ecx, 3              ; B9 03 00 00 00
    // inner:
    //   add rdx, 1              ; 48 83 C2 01
    //   dec rcx                 ; 48 FF C9
    //   jnz inner               ; 75 F7
    //   dec rsi                 ; 48 FF CE
    //   jnz outer               ; 75 ED
    //   hlt                     ; F4
    const std::vector<std::uint8_t> program = {
        0x31, 0xD2, 0xB9, 0x03, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC2, 0x01,
        0x48, 0xFF, 0xC9, 0x75, 0xF7, 0x48, 0xFF, 0xCE, 0x75, 0xED, 0xF4,
    };
    for (std::uint64_t outer : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{7}}) {
        GuestState initial;
        initial.rip = kBase;
        initial.reg(Gpr::kRsi) = outer;
        // outer == 0 is an infinite loop: `dec rsi` wraps to all-ones and never
        // reaches zero. A small step limit keeps that case fast while still
        // proving both tiers report a step limit (not a hang or a wrong halt).
        check_program_matches(program, initial, 100'000);
    }
    // Independent expected value: 3*outer additions of 1 into rdx.
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRsi) = 7;
    const hyperion::RunResult r = cache.run(mem, initial);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRdx), std::uint64_t{21});
}

HYPERION_TEST("translator", "differential_branch_inside_loop") {
    // A conditional *inside* a loop: the taken and not-taken exits reconverge at
    // a shared continuation, and the branch alternates direction every
    // iteration. Bytes verified with `as`/`objdump`.
    //   xor edx, edx            ; 31 D2
    //   mov ecx, 8              ; B9 08 00 00 00
    // loop:
    //   mov rbx, rcx            ; 48 89 CB
    //   and rbx, 1              ; 48 83 E3 01
    //   jz  skip                ; 74 04
    //   add rdx, 10             ; 48 83 C2 0A
    // skip:
    //   add rdx, 1              ; 48 83 C2 01
    //   dec rcx                 ; 48 FF C9
    //   jnz loop                ; 75 EA
    //   hlt                     ; F4
    const std::vector<std::uint8_t> program = {
        0x31, 0xD2, 0xB9, 0x08, 0x00, 0x00, 0x00, 0x48, 0x89, 0xCB, 0x48, 0x83, 0xE3, 0x01, 0x74,
        0x04, 0x48, 0x83, 0xC2, 0x0A, 0x48, 0x83, 0xC2, 0x01, 0x48, 0xFF, 0xC9, 0x75, 0xEA, 0xF4,
    };
    GuestState initial;
    initial.rip = kBase;
    check_program_matches(program, initial);
    // Independent expected value: 8 iterations, +10 on the 4 odd counters.
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(mem, initial);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRdx), std::uint64_t{48});
}

HYPERION_TEST("translator", "differential_register_allocation_many_registers") {
    // Register allocation (BENCHMARKS.md 4.2.6) must not change behaviour when a
    // block touches more guest registers than there are host registers to
    // allocate (kMaxAllocated), nor when the allocation count is odd. This
    // program touches 11 distinct guest registers (rax..r11), so the allocator
    // must spill and reuse, and the odd save/restore prologue path is exercised.
    // Bytes verified with `as`/`objdump`:
    //   mov eax..r11d, 1..10    ; distinct values so a misallocated register is
    //                           ; caught (all-zero would not discriminate)
    //   add rax, rbx..r11       ; accumulate the 2..10
    //   hlt
    // Expected rax = 55 (1 + 2 + 3 + ... + 10).
    const std::vector<std::uint8_t> program = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x02, 0x00, 0x00, 0x00, 0xB9, 0x03, 0x00, 0x00,
        0x00, 0xBA, 0x04, 0x00, 0x00, 0x00, 0xBE, 0x05, 0x00, 0x00, 0x00, 0xBF, 0x06, 0x00,
        0x00, 0x00, 0x41, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x41, 0xB9, 0x08, 0x00, 0x00, 0x00,
        0x41, 0xBA, 0x09, 0x00, 0x00, 0x00, 0x41, 0xBB, 0x0A, 0x00, 0x00, 0x00, 0x48, 0x01,
        0xD8, 0x48, 0x01, 0xC8, 0x48, 0x01, 0xD0, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xF8, 0x4C,
        0x01, 0xC0, 0x4C, 0x01, 0xC8, 0x4C, 0x01, 0xD0, 0x4C, 0x01, 0xD8, 0xF4,
    };
    GuestState initial;
    initial.rip = kBase;
    check_program_matches(program, initial);
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(mem, initial);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), std::uint64_t{55});
}

HYPERION_TEST("translator", "differential_register_allocation_immediates") {
    // A mov-immediate whose destination is allocated must land in the host
    // register (a fast path in emit_one), and a later add must see it. The
    // values are chosen so the 32-bit zero-extension of `mov eax, imm32` matters.
    //   mov eax, 1              ; B8 01 00 00 00
    //   mov ebx, 2              ; BB 02 00 00 00
    //   mov ecx, 3              ; B9 03 00 00 00
    //   add eax, ebx            ; 01 D8
    //   add eax, ecx            ; 01 C8
    //   hlt                     ; F4
    // Expected eax = 6.
    const std::vector<std::uint8_t> program = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x02, 0x00, 0x00, 0x00,
        0xB9, 0x03, 0x00, 0x00, 0x00, 0x01, 0xD8, 0x01, 0xC8, 0xF4,
    };
    GuestState initial;
    initial.rip = kBase;
    check_program_matches(program, initial);
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(mem, initial);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), std::uint64_t{6});
}

HYPERION_TEST("translator", "dispatcher_stops_at_unsupported") {
    // mov eax, 1 ; push rax ; hlt. The dispatcher translates `mov`, then stops
    // at `push` with kUnsupported and leaves rip on the push, never running the
    // untranslatable instruction.
    const std::vector<std::uint8_t> program = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x50, 0xF4};
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), program.data(), program.size());
    GuestState initial;
    initial.rip = kBase;
    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(mem, initial);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kUnsupported);
    HYPERION_CHECK_EQ(r.state.rip, kBase + 5); // the push
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), std::uint64_t{1});
}

HYPERION_TEST("translator", "dispatcher_step_limit") {
    // An infinite loop must be reported as a step limit, not hang.
    // loop: jmp loop  ->  EB FE
    const std::vector<std::uint8_t> program = {0xEB, 0xFE};
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), program.data(), program.size());
    GuestState initial;
    initial.rip = kBase;
    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(mem, initial, 1000);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kStepLimit);
    HYPERION_CHECK_EQ(r.steps, std::uint64_t{1000});
}

HYPERION_TEST("translator", "translated_code_size_is_bounded") {
    // A deterministic translation-quality guard. Timing under qemu-user is
    // noisy (IQR several percent), so a regression in the emitted code size
    // would be easy to miss. This pins the exact AArch64 word count for a
    // known loop, so a change to the code generator must either keep it or
    // update the number deliberately.
    //
    //   mov ecx, 3      B9 03 00 00 00
    //   xor edx, edx    31 D2
    //   add rdx, 1      48 83 C2 01
    //   dec rcx         48 FF C9
    //   jne -7          75 F7
    //   hlt             F4
    const std::vector<std::uint8_t> program = {0xB9, 0x03, 0x00, 0x00, 0x00, 0x31, 0xD2, 0x48, 0x83,
                                               0xC2, 0x01, 0x48, 0xFF, 0xC9, 0x75, 0xF7, 0xF4};
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), program.data(), program.size());
    GuestState initial;
    initial.rip = kBase;
    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(mem, initial);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRdx), std::uint64_t{3});
    // Three blocks: the entry block at +0 (mov/xor/add/dec/jne), the loop
    // target at +7 (the jne target: add/dec/jne), and the HLT at +16. The
    // target of the backward branch is a separate cache entry even though it
    // lies inside the entry block's byte range.
    HYPERION_CHECK_EQ(cache.block_count(), std::size_t{3});
    const std::size_t words = cache.total_code_bytes() / 4;
    // Exact count at the time of writing is 88 words (352 bytes). Guest-register
    // allocation (4.2.6) takes the halt block's prologue+epilogue but removes
    // enough load/store traffic to net −3 words; the correctness fix that writes
    // rip on a halt (4.2.7) adds 3 words back, landing on the same 88 recorded
    // in 4.2.5 by coincidence. The profitability guard in pass 1.5 is what keeps
    // allocation from regressing small blocks (see BENCHMARKS.md 4.2.6).
    // The guard is an upper bound so a *reduction* is allowed but any growth
    // fails CI; a code-size regression is otherwise easy to miss under qemu-user
    // timing noise. Update the bound deliberately when the count legitimately
    // changes, and record the exact figure in BENCHMARKS.md.
    HYPERION_CHECK_EQ(cache.total_code_bytes() % 4, std::size_t{0});
    HYPERION_CHECK(words > 0 && words <= 88);
}

HYPERION_TEST("translator", "alu_immediate_is_folded") {
    // Instruction selection (BENCHMARKS.md 4.2.5): an immediate that fits the
    // AArch64 ADDS-immediate form is folded into the operation *when AF is
    // dead*. AF is derived from (a ^ b ^ r) and needs b in a register, so a
    // live-AF instruction keeps the register form for bit-exactness. The first
    // `add` below is killed by the second, so its AF is dead and it folds; the
    // second is live and does not.
    //
    //   add rdx, <imm>   48 83 C2 01        (small, foldable)
    //                    48 81 C2 10 32 54 76 (large, not foldable)
    //   add rdx, 1       48 83 C2 01
    //   jnz +0           75 00
    //   hlt              F4
    auto words_of = [](std::vector<std::uint8_t> program) {
        FlatMemory mem(kBase, 4096);
        std::memcpy(mem.data(), program.data(), program.size());
        GuestState st;
        st.rip = kBase;
        hyperion::TranslationCache cache;
        const hyperion::RunResult r = cache.run(mem, st);
        HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
        return cache.total_code_bytes() / 4;
    };

    const std::size_t small =
        words_of({0x48, 0x83, 0xC2, 0x01, 0x48, 0x83, 0xC2, 0x01, 0x75, 0x00, 0xF4});
    const std::size_t large = words_of(
        {0x48, 0x81, 0xC2, 0x10, 0x32, 0x54, 0x76, 0x48, 0x83, 0xC2, 0x01, 0x75, 0x00, 0xF4});

    // Only the first `add` differs. The foldable small immediate needs one
    // ADDS-immediate; the large one must be materialised (movz+movk) first, so
    // the otherwise-identical program is strictly larger.
    HYPERION_CHECK(large > small);
    HYPERION_CHECK(large - small >= 2);
}

HYPERION_TEST("translator", "dead_flags_are_not_computed") {
    // Flag liveness (BENCHMARKS.md 4.2.3): an ALU instruction whose flags are
    // overwritten before any read must not compute them. Compare the emitted
    // cost of an `add` whose flags are live (nothing later overwrites them)
    // against one whose flags are dead (a second `add` follows before any read).
    //
    //   hlt               F4                       -> h
    //   add rax, rbx     48 01 D8 ; hlt            -> one  (add flags live)
    //   add rax, rbx     48 01 D8 ; add rax,rbx; hlt -> two (first add dead)
    auto words_of = [](std::vector<std::uint8_t> program) {
        FlatMemory mem(kBase, 4096);
        std::memcpy(mem.data(), program.data(), program.size());
        GuestState st;
        st.rip = kBase;
        hyperion::TranslationCache cache;
        const hyperion::RunResult r = cache.run(mem, st);
        HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
        return cache.total_code_bytes() / 4;
    };

    const std::size_t h = words_of({0xF4});
    const std::size_t one = words_of({0x48, 0x01, 0xD8, 0xF4});
    const std::size_t two = words_of({0x48, 0x01, 0xD8, 0x48, 0x01, 0xD8, 0xF4});

    const std::size_t live_add = one - h;   // `add` with all six flags computed
    const std::size_t dead_add = two - one; // `add` whose flags a later add kills

    // A live add computes the full flag prologue; a dead add is just the
    // arithmetic plus the destination store. The gap is the whole point.
    HYPERION_CHECK(live_add >= 15);
    HYPERION_CHECK(dead_add <= 4);
    HYPERION_CHECK(dead_add < live_add);
}

HYPERION_TEST("translator", "differential_memory_operands") {
    // MOV through memory is lowered to a runtime helper (soft MMU). This drives
    // stores and loads through a helper call and checks the whole program
    // against the interpreter, so the generated code and the oracle must agree
    // on the effective address and on the byte order. Bytes verified with
    // `as`/`objdump` (GNU binutils, Intel syntax):
    //   mov [rax], rbx           48 89 18
    //   mov rcx, [rax]           48 8b 08
    //   mov [rax+8], rcx         48 89 48 08
    //   mov rdx, [rax+8]         48 8b 50 08
    //   mov ebx, 0x12345678      bb 78 56 34 12
    //   mov [rax+0x10], ebx      89 58 10   (32-bit store)
    //   mov ecx, [rax+0x10]      8b 48 10   (32-bit load, zero-extends)
    //   hlt                      f4
    const std::vector<std::uint8_t> program = {
        0x48, 0x89, 0x18, 0x48, 0x8B, 0x08, 0x48, 0x89, 0x48, 0x08, 0x48, 0x8B, 0x50,
        0x08, 0xBB, 0x78, 0x56, 0x34, 0x12, 0x89, 0x58, 0x10, 0x8B, 0x48, 0x10, 0xF4,
    };
    const std::uint64_t buffer = kBase + 0x100;
    const std::uint64_t value = 0x1122334455667788ULL;

    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRax) = buffer;
    initial.reg(Gpr::kRbx) = value;

    // Both tiers must reach the same architectural state. Because the two tiers
    // use separate Memory images, the expected memory effects are then checked
    // on the tier-1 image as well (the program leaves the seeded value at
    // [rax], its copy at [rax+8], and 0x12345678 at [rax+0x10]).
    FlatMemory interp_mem(kBase, 4096);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

    FlatMemory tier1_mem(kBase, 4096);
    std::memcpy(tier1_mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    const hyperion::RunResult actual = cache.run(tier1_mem, initial);

    HYPERION_CHECK(actual.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(actual.steps, expected.steps);
    // rdx came from [rax+8], which held a copy of the seeded value loaded from
    // [rax]; so rdx proves store + load + store + load all agree on the address
    // and byte order. rcx ends as the zero-extended 32-bit load, proving the
    // 32-bit store/load path and that a 32-bit load zero-extends.
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRdx), value);
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRbx), std::uint64_t{0x12345678});
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRcx), std::uint64_t{0x12345678});
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRdx), expected.state.reg(Gpr::kRdx));
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRcx), expected.state.reg(Gpr::kRcx));

    std::uint64_t at_base = 0;
    HYPERION_CHECK(tier1_mem.read(buffer, reinterpret_cast<std::uint8_t*>(&at_base), 8));
    HYPERION_CHECK_EQ(at_base, value);
    std::uint64_t stored = 0;
    HYPERION_CHECK(tier1_mem.read(buffer + 8, reinterpret_cast<std::uint8_t*>(&stored), 8));
    HYPERION_CHECK_EQ(stored, value);
    std::uint32_t word = 0;
    HYPERION_CHECK(tier1_mem.read(buffer + 0x10, reinterpret_cast<std::uint8_t*>(&word), 4));
    HYPERION_CHECK_EQ(word, std::uint32_t{0x12345678});
}

HYPERION_TEST("translator", "differential_memory_fault_is_reported") {
    // A translated load from an unmapped address must report a fault and stop
    // with rip on the faulting instruction, matching the interpreter. If the
    // helper's fault flag were ignored, the block would store a bogus value and
    // run on.
    //   mov r9, [rbx-8]   4c 8b 4b f8
    //   hlt               f4
    const std::vector<std::uint8_t> program = {0x4C, 0x8B, 0x4B, 0xF8, 0xF4};

    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRbx) = 0xDEAD0000ULL; // far outside the flat region

    FlatMemory interp_mem(kBase, 4096);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

    FlatMemory tier1_mem(kBase, 4096);
    std::memcpy(tier1_mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    const hyperion::RunResult actual = cache.run(tier1_mem, initial);

    HYPERION_CHECK(expected.status == hyperion::RunStatus::kFault);
    HYPERION_CHECK(actual.status == hyperion::RunStatus::kFault);
    HYPERION_CHECK_EQ(actual.state.rip, kBase);                      // the faulting instruction
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kR9), std::uint64_t{0}); // not written
}

HYPERION_TEST("translator", "differential_alu_memory_operands") {
    // ALU read-modify-write through memory, executed through generated AArch64
    // code and compared against the interpreter (the oracle). Encodings verified
    // with `as`/`objdump` (GNU binutils, Intel syntax):
    //   mov rbx, [rax]            48 8b 18
    //   add rbx, 5                48 83 c3 05
    //   add [rax], rbx            48 01 18
    //   inc qword [rax]           48 ff 00
    //   sub [rax], rbx            48 29 18
    //   dec qword [rax]           48 ff 08
    //   cmp [rax], rbx            48 39 18   (sets flags, writes no memory)
    //   xor [rax], rbx            48 31 18
    //   mov rcx, [rax]            48 8b 08
    //   hlt                       f4
    const std::vector<std::uint8_t> program = {
        0x48, 0x8B, 0x18,       // mov rbx, [rax]
        0x48, 0x83, 0xC3, 0x05, // add rbx, 5
        0x48, 0x01, 0x18,       // add [rax], rbx
        0x48, 0xFF, 0x00,       // inc qword [rax]
        0x48, 0x29, 0x18,       // sub [rax], rbx
        0x48, 0xFF, 0x08,       // dec qword [rax]
        0x48, 0x39, 0x18,       // cmp [rax], rbx
        0x48, 0x31, 0x18,       // xor [rax], rbx
        0x48, 0x8B, 0x08,       // mov rcx, [rax]
        0xF4,                   // hlt
    };
    const std::uint64_t buffer = kBase + 0x100;
    const std::uint64_t seeded = 0x0000000000000040ULL;

    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRax) = buffer;

    FlatMemory interp_mem(kBase, 4096);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    interp_mem.write(buffer, reinterpret_cast<const std::uint8_t*>(&seeded), 8);
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

    FlatMemory tier1_mem(kBase, 4096);
    std::memcpy(tier1_mem.data(), program.data(), program.size());
    tier1_mem.write(buffer, reinterpret_cast<const std::uint8_t*>(&seeded), 8);
    hyperion::TranslationCache cache;
    const hyperion::RunResult actual = cache.run(tier1_mem, initial);

    HYPERION_CHECK(actual.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(actual.steps, expected.steps);
    // Registers must match the oracle exactly, including all six flags from the
    // last flag-writing instruction (the `cmp`).
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRbx), expected.state.reg(Gpr::kRbx));
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRcx), expected.state.reg(Gpr::kRcx));
    HYPERION_CHECK(actual.state.flags == expected.state.flags);
    // And the memory effect must match. Starting from the seed 0x40:
    //   mov rbx,[rax] -> 0x40 ; add rbx,5 -> 0x45
    //   add [rax],rbx -> 0x85 ; inc [rax] -> 0x86 ; sub [rax],rbx -> 0x41
    //   dec [rax] -> 0x40 ; cmp [rax],rbx (flags only) ; xor [rax],rbx -> 0x05
    //   mov rcx,[rax] -> 0x05
    std::uint64_t final_word = 0;
    HYPERION_CHECK(tier1_mem.read(buffer, reinterpret_cast<std::uint8_t*>(&final_word), 8));
    std::uint64_t expected_word = 0;
    HYPERION_CHECK(interp_mem.read(buffer, reinterpret_cast<std::uint8_t*>(&expected_word), 8));
    HYPERION_CHECK_EQ(final_word, expected_word);
    HYPERION_CHECK_EQ(final_word, std::uint64_t{0x05});
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRcx), std::uint64_t{0x05});
}

HYPERION_TEST("translator", "differential_alu_memory_fault_is_reported") {
    // A translated ALU store to an unmapped address must report a fault and stop
    // with rip on the faulting instruction, matching the interpreter, rather
    // than silently continuing with the memory write lost.
    //   add [rbx], rax   48 01 03
    //   hlt              f4
    const std::vector<std::uint8_t> program = {0x48, 0x01, 0x03, 0xF4};

    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRbx) = 0xDEAD0000ULL; // far outside the flat region
    initial.reg(Gpr::kRax) = 1;

    FlatMemory interp_mem(kBase, 4096);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

    FlatMemory tier1_mem(kBase, 4096);
    std::memcpy(tier1_mem.data(), program.data(), program.size());
    hyperion::TranslationCache cache;
    const hyperion::RunResult actual = cache.run(tier1_mem, initial);

    HYPERION_CHECK(expected.status == hyperion::RunStatus::kFault);
    HYPERION_CHECK(actual.status == hyperion::RunStatus::kFault);
    HYPERION_CHECK_EQ(actual.state.rip, kBase); // the faulting instruction
}

#else

HYPERION_TEST("translator", "differential_mov_and_alu") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_alu_memory_operands") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_alu_memory_fault_is_reported") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_flags_are_exact") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_program_loops") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_branch_directions") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_nested_loops") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_branch_inside_loop") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "dispatcher_stops_at_unsupported") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "dispatcher_step_limit") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "translated_code_size_is_bounded") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_memory_operands") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translator", "differential_memory_fault_is_reported") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

#endif