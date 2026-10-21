// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Interpreter unit tests that do not depend on the host being x86-64: memory
// addressing, control flow, stack operations, and the parts of the flag model
// that can be checked from first principles. The hardware differential lives
// in tests/differential/.
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"

using hyperion::Flags;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::StepStatus;

namespace {

constexpr std::uint64_t kBase = 0x400000;

GuestState run(const std::vector<std::uint8_t>& code, std::uint64_t a = 0, std::uint64_t b = 0,
               int max_steps = 32) {
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = a;
    state.reg(Gpr::kRbx) = b;
    state.reg(Gpr::kRsp) = kBase + 2048;
    state.rip = kBase;
    for (int i = 0; i < max_steps; ++i) {
        const auto r = hyperion::step(state, memory);
        if (r.status == StepStatus::kHalted) {
            break;
        }
        HYPERION_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(StepStatus::kOk));
        if (r.status != StepStatus::kOk) {
            break;
        }
    }
    return state;
}

} // namespace

HYPERION_TEST("interpreter", "mov_immediate_then_halt") {
    // mov rax, 0x1234 ; hlt
    const GuestState s = run({0x48, 0xB8, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4});
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), 0x1234ULL);
}

HYPERION_TEST("interpreter", "rip_advances_by_instruction_length") {
    // nop ; hlt
    const GuestState s = run({0x90, 0xF4});
    HYPERION_CHECK_EQ(s.rip, kBase + 1);
}

HYPERION_TEST("interpreter", "store_and_load_through_memory") {
    // mov rbx, [rax] ; mov [rax+8], rbx ; hlt   with memory seeded below.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x8B, 0x18,       // mov rbx, [rax]
                                            0x48, 0x89, 0x58, 0x08, // mov [rax+8], rbx
                                            0xF4};
    std::memcpy(memory.data(), code.data(), code.size());
    const std::uint64_t data = kBase + 0x100;
    const std::uint64_t value = 0xDEADBEEFCAFEF00DULL;
    memory.write(data, reinterpret_cast<const std::uint8_t*>(&value), 8);

    GuestState state;
    state.reg(Gpr::kRax) = data;
    state.rip = kBase;
    for (int i = 0; i < 8; ++i) {
        const auto r = hyperion::step(state, memory);
        if (r.status == StepStatus::kHalted) {
            break;
        }
        HYPERION_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(StepStatus::kOk));
    }
    HYPERION_CHECK_EQ(state.reg(Gpr::kRbx), value);
    std::uint64_t round_trip = 0;
    memory.read(data + 8, reinterpret_cast<std::uint8_t*>(&round_trip), 8);
    HYPERION_CHECK_EQ(round_trip, value);
}

HYPERION_TEST("interpreter", "sib_addressing") {
    // mov rax, [rbx + rcx*4 + 0x10] ; hlt
    // Encoding: 48 8B 44 8B 10
    //   44 = mod=01 (disp8), reg=000 (rax), rm=100 (SIB follows)
    //   8B = scale=4, index=001 (rcx), base=011 (rbx)
    //   10 = disp8 = 0x10
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x8B, 0x44, 0x8B, 0x10, 0xF4};
    std::memcpy(memory.data(), code.data(), code.size());
    const std::uint64_t target = kBase + 0x200;
    const std::uint64_t value = 0x1122334455667788ULL;
    memory.write(target, reinterpret_cast<const std::uint8_t*>(&value), 8);

    GuestState state;
    state.reg(Gpr::kRbx) = kBase + 0x100;
    state.reg(Gpr::kRcx) = 0x3C; // 0x100 + 0x3C*4 + 0x10 = 0x200 -> target
    state.rip = kBase;
    for (int i = 0; i < 4; ++i) {
        const auto r = hyperion::step(state, memory);
        if (r.status == StepStatus::kHalted) {
            break;
        }
        HYPERION_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(StepStatus::kOk));
    }
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), value);
}

HYPERION_TEST("interpreter", "conditional_branch_taken_and_not_taken") {
    // cmp rax, rbx ; je +5 ; mov ebx, 1 ; hlt
    // The 0xC7 (mov r/m, imm32) encoding is not in the subset, so the body
    // uses the supported MOV r32, imm32 form (0xBB = B8+rbx).
    const std::vector<std::uint8_t> code = {0x48, 0x39, 0xD8,             // cmp rax, rbx
                                            0x74, 0x05,                   // je +5
                                            0xBB, 0x01, 0x00, 0x00, 0x00, // mov ebx, 1
                                            0xF4};                        // hlt

    const GuestState equal = run(code, 5, 5);
    HYPERION_CHECK_EQ(equal.reg(Gpr::kRbx), 5ULL); // jump taken, not overwritten
    HYPERION_CHECK(equal.flags.zf);

    const GuestState unequal = run(code, 5, 6);
    HYPERION_CHECK_EQ(unequal.reg(Gpr::kRbx), 1ULL); // jump not taken, mov ran
    HYPERION_CHECK(!unequal.flags.zf);
}

HYPERION_TEST("interpreter", "push_and_pop") {
    // mov eax, 0x42 ; push rax ; pop rbx ; hlt
    const GuestState s = run({0xB8, 0x42, 0x00, 0x00, 0x00, 0x50, 0x5B, 0xF4});
    HYPERION_CHECK_EQ(s.reg(Gpr::kRbx), 0x42ULL);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRsp), kBase + 2048); // balanced
}

HYPERION_TEST("interpreter", "unsupported_instruction_leaves_state_unchanged") {
    // A UD2 (0x0F 0x0B) is not in the subset. The interpreter must stop with
    // kUnsupported and must NOT advance rip or modify anything.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x0F, 0x0B};
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = 0xABCD;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kUnsupported);
    HYPERION_CHECK_EQ(state.rip, kBase);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), 0xABCDULL);
}

HYPERION_TEST("interpreter", "memory_fault_is_reported") {
    // mov rax, [rbx] with rbx pointing outside the flat region must fault, not
    // read zeros.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x8B, 0x03};
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRbx) = 0xDEAD0000ULL;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kFault);
}

HYPERION_TEST("interpreter", "inc_preserves_carry_flag") {
    // inc rax with CF set must leave CF set but update ZF.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0xFF, 0xC0};
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = 0xFFFFFFFFFFFFFFFFULL; // +1 -> 0, ZF set
    state.flags.cf = true;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kOk);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), 0ULL);
    HYPERION_CHECK(state.flags.zf);
    HYPERION_CHECK(state.flags.cf); // preserved
}

HYPERION_TEST("interpreter", "logical_op_clears_carry_and_overflow") {
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x31, 0xD8}; // xor rax, rbx
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = 0xF0F0;
    state.reg(Gpr::kRbx) = 0xF0F0; // xor -> 0
    state.flags.cf = true;
    state.flags.of = true;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kOk);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), 0ULL);
    HYPERION_CHECK(state.flags.zf);
    HYPERION_CHECK(!state.flags.cf);
    HYPERION_CHECK(!state.flags.of);
}

HYPERION_TEST("interpreter", "flags_pack_unpack_round_trip") {
    Flags f;
    f.cf = true;
    f.pf = true;
    f.zf = true;
    const std::uint64_t packed = hyperion::pack_flags(f);
    const Flags back = hyperion::unpack_flags(packed);
    HYPERION_CHECK(back == f);
    // Bit 1 is architecturally set on real hardware.
    HYPERION_CHECK((packed & 0x2ULL) != 0);
}

HYPERION_TEST("interpreter", "run_program_executes_a_loop") {
    // A real loop: sum = 1+2+...+10, using dec/jnz.
    //
    //   mov ecx, 10          ; B9 0A 00 00 00
    //   xor eax, eax         ; 31 C0
    // loop:
    //   add eax, ecx         ; 01 C8
    //   dec ecx              ; FF C9
    //   jnz loop             ; 75 F9   (-7 -> back to 'add')
    //   hlt                  ; F4
    //
    // Expected eax = 55. This exercises arithmetic, flags, a backwards short
    // branch, and repeated execution through the whole pipeline.
    const std::vector<std::uint8_t> code = {
        0xB9, 0x0A, 0x00, 0x00, 0x00, // mov ecx, 10
        0x31, 0xC0,                   // xor eax, eax
        0x01, 0xC8,                   // add eax, ecx
        0xFF, 0xC9,                   // dec ecx
        0x75, 0xFA,                   // jnz -6 (to 'add eax, ecx')
        0xF4,                         // hlt
    };
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.rip = kBase;
    const auto r = hyperion::run_program(memory, state);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), 55ULL); // 1+2+...+10
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRcx), 0ULL);
    HYPERION_CHECK_EQ(r.steps, 2ULL + 10ULL * 3ULL + 1ULL); // mov,xor + 10*(add,dec,jnz) + hlt
}

HYPERION_TEST("interpreter", "run_program_reports_step_limit") {
    // An infinite loop must be reported, not hang. jmp -2 jumps to itself.
    const std::vector<std::uint8_t> code = {0xE9, 0xFB, 0xFF, 0xFF,
                                            0xFF}; // jmp rel32 -5 (self-loop)
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.rip = kBase;
    const auto r = hyperion::run_program(memory, state, 1000);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kStepLimit);
    HYPERION_CHECK_EQ(r.steps, 1000ULL);
}

HYPERION_TEST("interpreter", "run_program_stops_at_unsupported_instruction") {
    // A program that reaches an unsupported instruction must stop there and
    // report it, not run past it.
    const std::vector<std::uint8_t> code = {0x90, 0x0F, 0x0B}; // nop; ud2
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.rip = kBase;
    const auto r = hyperion::run_program(memory, state);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kUnsupported);
    HYPERION_CHECK_EQ(r.steps, 1ULL);          // the nop retired
    HYPERION_CHECK_EQ(r.state.rip, kBase + 1); // stopped at the ud2
}

HYPERION_TEST("interpreter", "run_program_matches_hardware_for_a_loop") {
#if defined(__x86_64__)
    // Differential: run the same loop natively and in the interpreter.
    // Sum of 1..10 = 55, computed the same way both times.
    const std::vector<std::uint8_t> code = {
        0xB9, 0x0A, 0x00, 0x00, 0x00, // mov ecx, 10
        0x31, 0xC0,                   // xor eax, eax
        0x01, 0xC8,                   // add eax, ecx
        0xFF, 0xC9,                   // dec ecx
        0x75, 0xFA,                   // jnz -6
        0xF4,                         // hlt (replaced by ret for native)
    };
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.rip = kBase;
    const auto r = hyperion::run_program(memory, state);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);

    // Native: the same loop, ending in ret, called as a function.
    const std::uint32_t native_result = []() -> std::uint32_t {
        std::uint32_t eax = 0;
        std::uint32_t ecx = 10;
        asm volatile("xorl %%eax, %%eax\n\t"
                     "1:\n\t"
                     "addl %%ecx, %%eax\n\t"
                     "decl %%ecx\n\t"
                     "jnz 1b\n\t"
                     : "=a"(eax)
                     : "c"(ecx)
                     : "cc");
        return eax;
    }();
    HYPERION_CHECK_EQ(static_cast<std::uint32_t>(r.state.reg(Gpr::kRax)), native_result);
    HYPERION_CHECK_EQ(static_cast<std::uint32_t>(r.state.reg(Gpr::kRax)), 55U);
#else
    HYPERION_SKIP("requires an x86-64 host for the native reference");
#endif
}

HYPERION_TEST("interpreter", "condition_evaluation_table") {
    Flags f;
    f.zf = true;
    // With ZF set: E/NE, BE/A, LE/G must all behave.
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kE, f));
    HYPERION_CHECK(!hyperion::alu::evaluate(hyperion::Condition::kNe, f));
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kBe, f));
    HYPERION_CHECK(!hyperion::alu::evaluate(hyperion::Condition::kA, f));
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kLe, f));
    HYPERION_CHECK(!hyperion::alu::evaluate(hyperion::Condition::kG, f));

    // Signed vs unsigned: SF != OF means "less than" for signed comparisons.
    Flags g;
    g.sf = true;
    g.of = false;
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kL, g));
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kB, g) == false);
}

HYPERION_TEST("interpreter", "truncated_instruction_at_region_end_is_reported_not_zero_padded") {
    // Regression: a 2-byte opcode placed at the very last byte of a mapped
    // region. The full 15-byte fetch window fails, but the decoder must be told
    // the true number of readable bytes so it reports a *truncated* instruction
    // rather than decoding zero padding (which could look like a valid
    // instruction and silently mis-execute).
    FlatMemory memory(kBase, 1);
    memory.data()[0] = 0x0F; // the first byte of a 2-byte opcode, nothing after
    GuestState state;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kUnsupported);
    HYPERION_CHECK(r.decode_error == hyperion::DecodeError::kTruncated);
    HYPERION_CHECK_EQ(state.rip, kBase); // nothing executed
}

HYPERION_TEST("interpreter", "multi_byte_instruction_spanning_region_end_is_reported") {
    // mov rax, imm64 (10 bytes) with only 3 bytes mapped: must be reported as
    // truncated, not decoded from zero padding as "mov rax, 0".
    FlatMemory memory(kBase, 3);
    const std::uint8_t code[3] = {0x48, 0xB8, 0x34};
    std::memcpy(memory.data(), code, sizeof(code));
    GuestState state;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kUnsupported);
    HYPERION_CHECK(r.decode_error == hyperion::DecodeError::kTruncated);
}

HYPERION_TEST("interpreter", "last_byte_of_a_region_can_hold_a_one_byte_instruction") {
    // The complement of the truncation case: a complete 1-byte instruction as
    // the final mapped byte must still execute (we must not fault merely because
    // a full 15-byte window is unavailable).
    FlatMemory memory(kBase, 1);
    memory.data()[0] = 0xF4; // hlt
    GuestState state;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kHalted);
}

HYPERION_TEST("interpreter", "ret_at_top_level_halts_without_touching_the_stack") {
    // RET has no CALL to return from: Hyperion models it as "stop here". The
    // stack pointer and memory must be untouched (an earlier comment claimed RET
    // pops the return address; it does not, and this pins the real behaviour).
    FlatMemory memory(kBase, 4096);
    memory.data()[0] = 0xC3; // ret
    GuestState state;
    state.rip = kBase;
    state.reg(Gpr::kRsp) = kBase + 0x800;
    const std::uint64_t rsp_before = state.reg(Gpr::kRsp);
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kHalted);
    HYPERION_CHECK_EQ(state.rip, kBase); // halted at the ret, not advanced
    HYPERION_CHECK_EQ(state.reg(Gpr::kRsp), rsp_before);
}

HYPERION_TEST("interpreter", "pop_rsp_sets_rsp_to_popped_value_not_old_rsp_plus_8") {
    // Regression: on x86-64 hardware, `pop %rsp` (0x5C) reads [old_rsp],
    // increments rsp by 8, and THEN writes the loaded value into rsp — so rsp
    // becomes [old_rsp], never old_rsp + 8.
    FlatMemory memory(kBase, 4096);
    memory.data()[0] = 0x5C; // pop rsp
    memory.data()[1] = 0xF4; // hlt
    const std::uint64_t old_rsp = kBase + 0x800;
    const std::uint64_t target_rsp = 0x1122334455667788ULL;
    std::memcpy(memory.data() + 0x800, &target_rsp, sizeof(target_rsp));

    GuestState state;
    state.rip = kBase;
    state.reg(Gpr::kRsp) = old_rsp;
    const auto r = hyperion::run_program(memory, state, 16);
    HYPERION_CHECK(r.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRsp), target_rsp);
}
