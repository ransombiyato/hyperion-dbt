// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Decoder unit tests. These do not need real hardware: they check that a byte
// sequence decodes to the expected instruction shape, and — just as important —
// that everything outside the documented subset is REJECTED with a specific
// reason. A decoder that silently accepts an unknown opcode is worse than one
// that refuses it.
#include <cstdint>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/decoder.hpp"

using hyperion::Condition;
using hyperion::DecodeError;
using hyperion::Gpr;
using hyperion::Opcode;
using hyperion::OperandKind;

namespace {

hyperion::DecodeResult dec(const std::vector<std::uint8_t>& bytes) {
    return hyperion::decode(bytes.data(), bytes.size(), 0x1000);
}

} // namespace

HYPERION_TEST("decoder", "mov_r64_rm64") {
    // 48 8B C3 = mov rbx -> rax  (REX.W, 0x8B, modrm reg=rax rm=rbx)
    const auto r = dec({0x48, 0x8B, 0xC3});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kMov);
    HYPERION_CHECK(r.instruction.length == 3);
    HYPERION_CHECK(r.instruction.rex_w);
    HYPERION_CHECK(r.instruction.dst.kind == OperandKind::kRegister);
    HYPERION_CHECK(r.instruction.dst.reg == Gpr::kRax);
    HYPERION_CHECK(r.instruction.dst.size == hyperion::OperandSize::k64);
    HYPERION_CHECK(r.instruction.src.kind == OperandKind::kRegister);
    HYPERION_CHECK(r.instruction.src.reg == Gpr::kRbx);
}

HYPERION_TEST("decoder", "mov_32bit_is_not_rex_w") {
    // 89 D8 = mov eax, ebx (no REX.W -> 32-bit operand size)
    const auto r = dec({0x89, 0xD8});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kMov);
    HYPERION_CHECK(!r.instruction.rex_w);
    HYPERION_CHECK(r.instruction.dst.size == hyperion::OperandSize::k32);
}

HYPERION_TEST("decoder", "rex_b_extends_register") {
    // 49 8B C3 = mov r11 -> rax (REX.W|REX.B, rm=rbx+8=r11)
    const auto r = dec({0x49, 0x8B, 0xC3});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.src.reg == Gpr::kR11);
}

HYPERION_TEST("decoder", "mov_imm32") {
    // B8 78 56 34 12 = mov eax, 0x12345678
    const auto r = dec({0xB8, 0x78, 0x56, 0x34, 0x12});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kMov);
    HYPERION_CHECK(r.instruction.dst.reg == Gpr::kRax);
    HYPERION_CHECK(r.instruction.src.kind == OperandKind::kImmediate);
    HYPERION_CHECK(r.instruction.src.imm == 0x12345678ULL);
    HYPERION_CHECK(r.instruction.length == 5);
}

HYPERION_TEST("decoder", "mov_imm64") {
    // 48 B8 <8 bytes> = mov rax, imm64
    const auto r = dec({0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.src.imm == 0x0123456789ABCDEFULL);
    HYPERION_CHECK(r.instruction.length == 10);
}

HYPERION_TEST("decoder", "memory_operand_base_disp") {
    // 48 8B 43 08 = mov rax, [rbx+8]  (mod=01, reg=rax, rm=rbx, disp8)
    const auto r = dec({0x48, 0x8B, 0x43, 0x08});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.src.kind == OperandKind::kMemory);
    HYPERION_CHECK(r.instruction.src.mem.has_base);
    HYPERION_CHECK(r.instruction.src.mem.base == Gpr::kRbx);
    HYPERION_CHECK(!r.instruction.src.mem.has_index);
    HYPERION_CHECK(r.instruction.src.mem.disp == 8);
}

HYPERION_TEST("decoder", "memory_operand_sib") {
    // 48 8B 04 4B = mov rax, [rbx + rcx*2]
    const auto r = dec({0x48, 0x8B, 0x04, 0x4B});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.src.kind == OperandKind::kMemory);
    HYPERION_CHECK(r.instruction.src.mem.base == Gpr::kRbx);
    HYPERION_CHECK(r.instruction.src.mem.has_index);
    HYPERION_CHECK(r.instruction.src.mem.index == Gpr::kRcx);
    HYPERION_CHECK(r.instruction.src.mem.scale == 2);
}

HYPERION_TEST("decoder", "alu_imm8_sign_extended") {
    // 48 83 C0 FF = add rax, -1  (imm8 sign-extended to 64 bits)
    const auto r = dec({0x48, 0x83, 0xC0, 0xFF});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kAdd);
    HYPERION_CHECK(r.instruction.src.kind == OperandKind::kImmediate);
    HYPERION_CHECK(r.instruction.src.imm == 0xFFFFFFFFFFFFFFFFULL);
}

HYPERION_TEST("decoder", "jmp_relative_target") {
    // E9 00 00 00 00 = jmp +0 -> target = address after the instruction
    const auto r = dec({0xE9, 0x00, 0x00, 0x00, 0x00});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kJmp);
    HYPERION_CHECK(r.instruction.dst.imm == 0x1000 + 5);
}

HYPERION_TEST("decoder", "jmp_short_rel8") {
    // EB FE = jmp -2 (short, rel8). Target = address after + (-2) = the jmp
    // itself. This is the classic infinite-loop encoding.
    const auto r = dec({0xEB, 0xFE});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kJmp);
    HYPERION_CHECK(r.instruction.length == 2);
    HYPERION_CHECK(r.instruction.dst.imm == 0x1000 + 2 - 2);
}

HYPERION_TEST("decoder", "jcc_condition_decoded") {
    // 74 05 = je +5  (short jump, rel8). Target = address_after + 5.
    const auto r = dec({0x74, 0x05});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kJcc);
    HYPERION_CHECK(r.instruction.condition == Condition::kE);
    HYPERION_CHECK(r.instruction.length == 2);
    HYPERION_CHECK(r.instruction.dst.imm == 0x1000 + 2 + 5);
}

HYPERION_TEST("decoder", "jcc_negative_rel8_is_signed") {
    // 75 F6 = jne -10. The displacement is signed, so this must wrap backwards.
    const auto r = dec({0x75, 0xF6});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.condition == Condition::kNe);
    HYPERION_CHECK(r.instruction.dst.imm == 0x1000 + 2 - 10);
}

HYPERION_TEST("decoder", "rejects_unsupported_prefix") {
    // 0x66 is the operand-size prefix. Ignoring it would decode a 16-bit
    // instruction as 32-bit, which is silently wrong.
    const auto r = dec({0x66, 0x89, 0xD8});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kUnsupportedPrefix);
}

HYPERION_TEST("decoder", "rejects_lock_prefix") {
    const auto r = dec({0xF0, 0x01, 0xD8});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kUnsupportedPrefix);
}

HYPERION_TEST("decoder", "rejects_rip_relative") {
    // 48 8B 05 <disp32> = mov rax, [rip+disp] — common, but not modelled yet.
    const auto r = dec({0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kUnsupportedModrm);
}

HYPERION_TEST("decoder", "rejects_unknown_opcode") {
    const auto r = dec({0x0F, 0x0B}); // UD2, not in the subset
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kUnknownOpcode);
}

HYPERION_TEST("decoder", "rejects_truncated") {
    // 48 8B declares a ModRM byte that is not present.
    const auto r = dec({0x48, 0x8B});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kTruncated);
}

HYPERION_TEST("decoder", "rejects_truncated_sib") {
    // 48 8B 04 declares a SIB byte that is not present. This must be reported
    // as truncation, not as an unsupported ModRM form: the difference matters
    // to the caller (a truncated window may be re-fetched larger, an
    // unsupported form never becomes decodable).
    const auto r = dec({0x48, 0x8B, 0x04});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kTruncated);
}

HYPERION_TEST("decoder", "rejects_truncated_displacement") {
    // 48 8B 44 24 declares a mod=01 disp8 that is not present.
    const auto r = dec({0x48, 0x8B, 0x44, 0x24});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kTruncated);
}

HYPERION_TEST("decoder", "rejects_empty") {
    const auto r = hyperion::decode(nullptr, 0, 0x1000);
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kTruncated);
}

HYPERION_TEST("decoder", "rejects_adc_sbb_group") {
    // 48 81 D0 <imm32> = adc rax, imm — group 1 extension 2, not modelled.
    const auto r = dec({0x48, 0x81, 0xD0, 0x01, 0x00, 0x00, 0x00});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kUnknownOpcode);
}

HYPERION_TEST("decoder", "mov_rm_imm32_form") {
    // 48 C7 C7 25 17 40 00 = mov rdi, 0x401725 (imm32 sign-extended).
    // This is the instruction glibc's `_start` uses to load `main`.
    const auto r = dec({0x48, 0xC7, 0xC7, 0x25, 0x17, 0x40, 0x00});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kMov);
    HYPERION_CHECK(r.instruction.dst.kind == OperandKind::kRegister);
    HYPERION_CHECK(r.instruction.dst.reg == Gpr::kRdi);
    HYPERION_CHECK(r.instruction.dst.size == hyperion::OperandSize::k64);
    HYPERION_CHECK(r.instruction.src.kind == OperandKind::kImmediate);
    HYPERION_CHECK(r.instruction.src.imm == 0x401725ULL);
    HYPERION_CHECK(r.instruction.length == 7);
}

HYPERION_TEST("decoder", "mov_rm_imm32_sign_extends_in_64bit") {
    // 48 C7 C0 F8 FF FF FF = mov rax, -8 (imm32 sign-extended to 64 bits).
    const auto r = dec({0x48, 0xC7, 0xC0, 0xF8, 0xFF, 0xFF, 0xFF});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.src.imm == 0xFFFFFFFFFFFFFFF8ULL);
}

HYPERION_TEST("decoder", "mov_rm_imm32_is_not_sign_extended_in_32bit") {
    // C7 C0 F8 FF FF FF = mov eax, 0xFFFFFFF8 (no REX.W -> plain imm32).
    const auto r = dec({0xC7, 0xC0, 0xF8, 0xFF, 0xFF, 0xFF});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.dst.size == hyperion::OperandSize::k32);
    HYPERION_CHECK(r.instruction.src.imm == 0x00000000FFFFFFF8ULL);
}

HYPERION_TEST("decoder", "mov_rm_imm32_to_memory") {
    // 48 C7 03 2A 00 00 00 = mov qword ptr [rbx], 42 (mod=00 rm=011).
    const auto r = dec({0x48, 0xC7, 0x03, 0x2A, 0x00, 0x00, 0x00});
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kMov);
    HYPERION_CHECK(r.instruction.dst.kind == OperandKind::kMemory);
    HYPERION_CHECK(r.instruction.src.kind == OperandKind::kImmediate);
    HYPERION_CHECK(r.instruction.src.imm == 42ULL);
}

HYPERION_TEST("decoder", "rejects_mov_imm32_non_zero_reg_field") {
    // 48 C7 C8 <imm32> = group 11 with reg=1, not MOV; rejected explicitly.
    const auto r = dec({0x48, 0xC7, 0xC8, 0x01, 0x00, 0x00, 0x00});
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == DecodeError::kUnknownOpcode);
}

HYPERION_TEST("decoder", "to_string_is_readable") {
    const auto r = dec({0x48, 0x01, 0xD8}); // add rax, rbx
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK_EQ(hyperion::to_string(r.instruction), std::string("add rax, rbx"));
}
