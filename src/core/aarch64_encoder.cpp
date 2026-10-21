// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// AArch64 encoder. Every encoding here is checked against GNU as/objdump in
// tests/unit/test_aarch64_encoder.cpp.
#include "hyperion/aarch64_encoder.hpp"

#include <cassert>
#include <stdexcept>

namespace hyperion::a64 {

namespace {

constexpr std::size_t kUnbound = static_cast<std::size_t>(-1);

constexpr std::uint32_t sf_of(Width w) {
    return w == Width::k64 ? 0x80000000u : 0u;
}

constexpr std::uint32_t kAddSubRegBase = 0x0B000000u;
constexpr std::uint32_t kAddSubImmBase = 0x11000000u;
constexpr std::uint32_t kLogicalRegBase = 0x0A000000u;
constexpr std::uint32_t kLoadStoreBase = 0x39000000u;
constexpr std::uint32_t kLoadStoreRegBase = 0x38206800u; // option=011 (LSL), S=0
constexpr std::uint32_t kLoadStorePairBase = 0x28000000u;

void validate_reg(Reg r) {
    if (r > 31) {
        throw std::invalid_argument("a64::Encoder: register number out of range");
    }
}

void validate_imm12(std::uint32_t v) {
    if (v > 0xFFFu) {
        throw std::invalid_argument("a64::Encoder: immediate does not fit in 12 bits");
    }
}

unsigned size_log2_of_bytes(unsigned bytes) {
    switch (bytes) {
    case 1:
        return 0;
    case 2:
        return 1;
    case 4:
        return 2;
    case 8:
        return 3;
    default:
        throw std::invalid_argument("a64::Encoder: invalid byte access size");
    }
}

} // namespace

const char* to_string(Cond c) {
    switch (c) {
    case Cond::kEq:
        return "eq";
    case Cond::kNe:
        return "ne";
    case Cond::kCs:
        return "cs";
    case Cond::kCc:
        return "cc";
    case Cond::kMi:
        return "mi";
    case Cond::kPl:
        return "pl";
    case Cond::kVs:
        return "vs";
    case Cond::kVc:
        return "vc";
    case Cond::kHi:
        return "hi";
    case Cond::kLs:
        return "ls";
    case Cond::kGe:
        return "ge";
    case Cond::kLt:
        return "lt";
    case Cond::kGt:
        return "gt";
    case Cond::kLe:
        return "le";
    case Cond::kAl:
        return "al";
    case Cond::kNv:
        return "nv";
    }
    return "?";
}

void Encoder::emit(std::uint32_t word) {
    code_.push_back(word);
}

void Encoder::emit_raw_u32(std::uint32_t word) {
    code_.push_back(word);
}

void Encoder::add(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (rm << 16) | (rn << 5) | rd);
}

void Encoder::add_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    const unsigned max_shift = (w == Width::k64) ? 63u : 31u;
    if (amount > max_shift || shift == Shift::kRor) {
        throw std::invalid_argument("Encoder::add_shifted: invalid shift");
    }
    emit(sf_of(w) | kAddSubRegBase | (static_cast<std::uint32_t>(shift) << 22) | (rm << 16) |
         (amount << 10) | (rn << 5) | rd);
}

void Encoder::sub(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::sub_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    if (shift == Shift::kRor) {
        throw std::invalid_argument("a64::Encoder: ROR is invalid on shifted ADD/SUB");
    }
    const unsigned max_amount = (w == Width::k64) ? 63u : 31u;
    if (amount > max_amount) {
        throw std::invalid_argument("a64::Encoder: shift amount out of range");
    }
    const std::uint32_t shift_field = static_cast<std::uint32_t>(shift) << 22;
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | shift_field |
         (static_cast<std::uint32_t>(rm) << 16) | (amount << 10) | (rn << 5) | rd);
}

void Encoder::and_(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (rm << 16) | (rn << 5) | rd);
}

void Encoder::orr(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (1u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::eor(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (2u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::eor_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    const unsigned max_amount = (w == Width::k64) ? 63u : 31u;
    if (amount > max_amount) {
        throw std::invalid_argument("a64::Encoder: shift amount out of range");
    }
    const std::uint32_t shift_field = static_cast<std::uint32_t>(shift) << 22;
    emit(sf_of(w) | kLogicalRegBase | (2u << 29) | shift_field |
         (static_cast<std::uint32_t>(rm) << 16) | (amount << 10) | (rn << 5) | rd);
}

void Encoder::lsl_reg(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | 0x1AC02000u | (rm << 16) | (rn << 5) | rd);
}

void Encoder::lsr_reg(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | 0x1AC02400u | (rm << 16) | (rn << 5) | rd);
}

void Encoder::asr_reg(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | 0x1AC02800u | (rm << 16) | (rn << 5) | rd);
}

void Encoder::ror_reg(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | 0x1AC02C00u | (rm << 16) | (rn << 5) | rd);
}

void Encoder::mul(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | 0x1B007C00u | (rm << 16) | (rn << 5) | rd);
}

void Encoder::smulh(Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(0x9B407C00u | (rm << 16) | (rn << 5) | rd);
}

void Encoder::umulh(Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(0x9BC07C00u | (rm << 16) | (rn << 5) | rd);
}

void Encoder::add_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (shift12 ? (1u << 22) : 0u) | (imm12 << 10) | (rn << 5) | rd);
}

void Encoder::sub_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 30) | (shift12 ? (1u << 22) : 0u) | (imm12 << 10) |
         (rn << 5) | rd);
}

void Encoder::cmp(Width w, Reg rn, Reg rm) {
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | (1u << 29) | (rm << 16) | (rn << 5) | kXzr);
}

void Encoder::cmp_imm(Width w, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 30) | (1u << 29) | (shift12 ? (1u << 22) : 0u) |
         (imm12 << 10) | (rn << 5) | kXzr);
}

void Encoder::adds(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (1u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::subs(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | (1u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::adds_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 29) | (shift12 ? (1u << 22) : 0u) | (imm12 << 10) |
         (rn << 5) | rd);
}

void Encoder::subs_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 30) | (1u << 29) | (shift12 ? (1u << 22) : 0u) |
         (imm12 << 10) | (rn << 5) | rd);
}

void Encoder::mvn(Width w, Reg rd, Reg rm) {
    validate_reg(rd);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (1u << 29) | (1u << 21) | (rm << 16) | (kXzr << 5) | rd);
}

void Encoder::rev(Width w, Reg rd, Reg rn) {
    validate_reg(rd);
    validate_reg(rn);
    emit((w == Width::k64 ? 0xDAC00C00u : 0x5AC00800u) | (rn << 5) | rd);
}

void Encoder::rbit(Width w, Reg rd, Reg rn) {
    validate_reg(rd);
    validate_reg(rn);
    emit((w == Width::k64 ? 0xDAC00000u : 0x5AC00000u) | (rn << 5) | rd);
}

void Encoder::clz(Width w, Reg rd, Reg rn) {
    validate_reg(rd);
    validate_reg(rn);
    emit((w == Width::k64 ? 0xDAC01000u : 0x5AC01000u) | (rn << 5) | rd);
}

void Encoder::cset(Width w, Reg rd, Cond c) {
    validate_reg(rd);
    const std::uint32_t inverted = (static_cast<std::uint32_t>(c) ^ 1u) & 0xFu;
    emit(sf_of(w) | 0x1A800400u | (inverted << 12) | (kXzr << 16) | (kXzr << 5) | rd);
}

void Encoder::csel(Width w, Reg rd, Reg rn, Reg rm, Cond c) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | 0x1A800000u | (rm << 16) | (static_cast<std::uint32_t>(c) << 12) | (rn << 5) |
         rd);
}

void Encoder::lsl_imm(Width w, Reg rd, Reg rn, unsigned shift) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned width = (w == Width::k64) ? 64u : 32u;
    if (shift >= width) {
        throw std::invalid_argument("a64::Encoder: lsl shift out of range");
    }
    if (shift == 0) {
        mov_reg(w, rd, rn);
        return;
    }
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    const unsigned immr = width - shift;
    const unsigned imms = width - 1u - shift;
    emit(sf_of(w) | 0x53000000u | (n << 22) | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10) |
         (rn << 5) | rd);
}

void Encoder::lsr_imm(Width w, Reg rd, Reg rn, unsigned shift) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned width = (w == Width::k64) ? 64u : 32u;
    if (shift >= width) {
        throw std::invalid_argument("a64::Encoder: lsr shift out of range");
    }
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    emit(sf_of(w) | 0x53000000u | (n << 22) | ((shift & 0x3Fu) << 16) |
         (((width - 1) & 0x3Fu) << 10) | (rn << 5) | rd);
}

void Encoder::asr_imm(Width w, Reg rd, Reg rn, unsigned shift) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned width = (w == Width::k64) ? 64u : 32u;
    if (shift >= width) {
        throw std::invalid_argument("a64::Encoder: asr shift out of range");
    }
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    emit(sf_of(w) | 0x13000000u | (n << 22) | ((shift & 0x3Fu) << 16) |
         (((width - 1) & 0x3Fu) << 10) | (rn << 5) | rd);
}

void Encoder::ror_imm(Width w, Reg rd, Reg rn, unsigned shift) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned width = (w == Width::k64) ? 64u : 32u;
    if (shift >= width) {
        throw std::invalid_argument("a64::Encoder: ror shift out of range");
    }
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    emit(sf_of(w) | 0x13800000u | (n << 22) | (rn << 16) | ((shift & 0x3Fu) << 10) | (rn << 5) |
         rd);
}

void Encoder::ubfx(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned reg_width = (w == Width::k64) ? 64u : 32u;
    if (width == 0 || width > reg_width || lsb + width > reg_width) {
        throw std::invalid_argument("a64::Encoder: ubfx field out of range");
    }
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    const unsigned immr = lsb;
    const unsigned imms = lsb + width - 1;
    emit(sf_of(w) | 0x53000000u | (n << 22) | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10) |
         (rn << 5) | rd);
}

void Encoder::sbfx(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned reg_width = (w == Width::k64) ? 64u : 32u;
    if (width == 0 || width > reg_width || lsb + width > reg_width) {
        throw std::invalid_argument("a64::Encoder: sbfx field out of range");
    }
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    const unsigned immr = lsb;
    const unsigned imms = lsb + width - 1;
    emit(sf_of(w) | 0x13000000u | (n << 22) | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10) |
         (rn << 5) | rd);
}

void Encoder::bfi(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned reg_width = (w == Width::k64) ? 64u : 32u;
    if (width == 0 || width > reg_width || lsb + width > reg_width) {
        throw std::invalid_argument("a64::Encoder: bfi field out of range");
    }
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    const unsigned immr = (reg_width - lsb) % reg_width;
    const unsigned imms = width - 1u;
    emit(sf_of(w) | 0x33000000u | (n << 22) | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10) |
         (rn << 5) | rd);
}

void Encoder::ldr(Width w, Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    const unsigned size = (w == Width::k64) ? 3u : 2u;
    const unsigned scale = (w == Width::k64) ? 8u : 4u;
    if (byte_offset % scale != 0 || byte_offset / scale > 0xFFFu) {
        throw std::invalid_argument(
            "a64::Encoder: ldr offset must be a scaled multiple <= 4095*size");
    }
    emit((size << 30) | kLoadStoreBase | (1u << 22) | ((byte_offset / scale) << 10) | (rn << 5) |
         rt);
}

void Encoder::str(Width w, Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    const unsigned size = (w == Width::k64) ? 3u : 2u;
    const unsigned scale = (w == Width::k64) ? 8u : 4u;
    if (byte_offset % scale != 0 || byte_offset / scale > 0xFFFu) {
        throw std::invalid_argument(
            "a64::Encoder: str offset must be a scaled multiple <= 4095*size");
    }
    emit((size << 30) | kLoadStoreBase | ((byte_offset / scale) << 10) | (rn << 5) | rt);
}

void Encoder::ldrh(Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    if (byte_offset % 2 != 0 || byte_offset / 2 > 0xFFFu) {
        throw std::invalid_argument("a64::Encoder: ldrh offset out of range");
    }
    emit((1u << 30) | kLoadStoreBase | (1u << 22) | ((byte_offset / 2) << 10) | (rn << 5) | rt);
}

void Encoder::strh(Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    if (byte_offset % 2 != 0 || byte_offset / 2 > 0xFFFu) {
        throw std::invalid_argument("a64::Encoder: strh offset out of range");
    }
    emit((1u << 30) | kLoadStoreBase | ((byte_offset / 2) << 10) | (rn << 5) | rt);
}

void Encoder::ldrb(Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    if (byte_offset > 0xFFFu) {
        throw std::invalid_argument("a64::Encoder: ldrb offset out of range");
    }
    emit(kLoadStoreBase | (1u << 22) | (byte_offset << 10) | (rn << 5) | rt);
}

void Encoder::strb(Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    if (byte_offset > 0xFFFu) {
        throw std::invalid_argument("a64::Encoder: strb offset out of range");
    }
    emit(kLoadStoreBase | (byte_offset << 10) | (rn << 5) | rt);
}

void Encoder::ldr_reg_sized(unsigned bytes, Reg rt, Reg rn, Reg rm) {
    validate_reg(rt);
    validate_reg(rn);
    validate_reg(rm);
    const unsigned sz = size_log2_of_bytes(bytes);
    emit((sz << 30) | kLoadStoreRegBase | (1u << 22) | (rm << 16) | (rn << 5) | rt);
}

void Encoder::str_reg_sized(unsigned bytes, Reg rt, Reg rn, Reg rm) {
    validate_reg(rt);
    validate_reg(rn);
    validate_reg(rm);
    const unsigned sz = size_log2_of_bytes(bytes);
    emit((sz << 30) | kLoadStoreRegBase | (rm << 16) | (rn << 5) | rt);
}

namespace {
std::uint32_t scaled_pair_imm(int offset) {
    if (offset % 8 != 0 || offset < -512 || offset > 504) {
        throw std::invalid_argument(
            "a64::Encoder: stp/ldp offset must be a multiple of 8 in [-512, 504]");
    }
    return static_cast<std::uint32_t>((offset >> 3) & 0x7F);
}
} // namespace

void Encoder::stp_off(Reg rt, Reg rt2, Reg rn, int offset) {
    validate_reg(rt);
    validate_reg(rt2);
    validate_reg(rn);
    emit(kLoadStorePairBase | (2u << 30) | (2u << 23) | (scaled_pair_imm(offset) << 15) |
         (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::ldp_off(Reg rt, Reg rt2, Reg rn, int offset) {
    validate_reg(rt);
    validate_reg(rt2);
    validate_reg(rn);
    emit(kLoadStorePairBase | (2u << 30) | (1u << 22) | (2u << 23) |
         (scaled_pair_imm(offset) << 15) | (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::stp_pre(Reg rt, Reg rt2, Reg rn, int offset) {
    validate_reg(rt);
    validate_reg(rt2);
    validate_reg(rn);
    emit(kLoadStorePairBase | (2u << 30) | (3u << 23) | (scaled_pair_imm(offset) << 15) |
         (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::ldp_post(Reg rt, Reg rt2, Reg rn, int offset) {
    validate_reg(rt);
    validate_reg(rt2);
    validate_reg(rn);
    emit(kLoadStorePairBase | (2u << 30) | (1u << 22) | (1u << 23) |
         (scaled_pair_imm(offset) << 15) | (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::mov_reg(Width w, Reg rd, Reg rm) {
    validate_reg(rd);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (1u << 29) | (rm << 16) | (kXzr << 5) | rd);
}

void Encoder::movz(Width w, Reg rd, std::uint16_t imm16, unsigned shift) {
    validate_reg(rd);
    if (shift > 48 || (shift % 16) != 0) {
        throw std::invalid_argument("a64::Encoder: movz shift must be 0/16/32/48");
    }
    const std::uint32_t hw = shift / 16;
    const std::uint32_t base = (w == Width::k64) ? 0xD2800000u : 0x52800000u;
    emit(base | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) | rd);
}

void Encoder::movk(Width w, Reg rd, std::uint16_t imm16, unsigned shift) {
    validate_reg(rd);
    if (shift > 48 || (shift % 16) != 0) {
        throw std::invalid_argument("a64::Encoder: movk shift must be 0/16/32/48");
    }
    const std::uint32_t hw = shift / 16;
    const std::uint32_t base = (w == Width::k64) ? 0xF2800000u : 0x72800000u;
    emit(base | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) | rd);
}

void Encoder::mov_imm64(Reg rd, std::uint64_t value) {
    bool first = true;
    for (unsigned shift = 0; shift <= 48; shift += 16) {
        const auto half = static_cast<std::uint16_t>((value >> shift) & 0xFFFFu);
        if (first) {
            movz(Width::k64, rd, half, shift);
            first = false;
        } else if (half != 0) {
            movk(Width::k64, rd, half, shift);
        }
    }
}

void Encoder::adr(Reg rd, Label target) {
    validate_reg(rd);
    check_label(target);
    const std::size_t at = code_.size();
    emit(0x10000000u | rd);
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], BranchKind::kAdr);
    } else {
        fixups_.push_back({at, target, BranchKind::kAdr});
    }
}

void Encoder::b(Label target) {
    check_label(target);
    const std::size_t at = code_.size();
    emit(0x14000000u);
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], BranchKind::kUncond);
    } else {
        fixups_.push_back({at, target, BranchKind::kUncond});
    }
}

void Encoder::b_cond(Cond c, Label target) {
    check_label(target);
    const std::size_t at = code_.size();
    emit(0x54000000u | static_cast<std::uint32_t>(c));
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], BranchKind::kCond);
    } else {
        fixups_.push_back({at, target, BranchKind::kCond});
    }
}

void Encoder::cbz(Width w, Reg rt, Label target) {
    validate_reg(rt);
    check_label(target);
    const std::size_t at = code_.size();
    emit(sf_of(w) | 0x34000000u | rt);
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], BranchKind::kCond);
    } else {
        fixups_.push_back({at, target, BranchKind::kCond});
    }
}

void Encoder::cbnz(Width w, Reg rt, Label target) {
    validate_reg(rt);
    check_label(target);
    const std::size_t at = code_.size();
    emit(sf_of(w) | 0x35000000u | rt);
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], BranchKind::kCond);
    } else {
        fixups_.push_back({at, target, BranchKind::kCond});
    }
}

void Encoder::tbz(Reg rt, unsigned bit, Label target) {
    validate_reg(rt);
    check_label(target);
    if (bit >= 64) {
        throw std::invalid_argument("a64::Encoder: tbz bit out of range");
    }
    const std::size_t at = code_.size();
    const std::uint32_t b5 = (bit >> 5) & 1u;
    const std::uint32_t b40 = bit & 0x1Fu;
    emit(0x36000000u | (b5 << 31) | (b40 << 19) | rt);
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], BranchKind::kTestBit);
    } else {
        fixups_.push_back({at, target, BranchKind::kTestBit});
    }
}

void Encoder::tbnz(Reg rt, unsigned bit, Label target) {
    validate_reg(rt);
    check_label(target);
    if (bit >= 64) {
        throw std::invalid_argument("a64::Encoder: tbnz bit out of range");
    }
    const std::size_t at = code_.size();
    const std::uint32_t b5 = (bit >> 5) & 1u;
    const std::uint32_t b40 = bit & 0x1Fu;
    emit(0x37000000u | (b5 << 31) | (b40 << 19) | rt);
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], BranchKind::kTestBit);
    } else {
        fixups_.push_back({at, target, BranchKind::kTestBit});
    }
}

void Encoder::blr(Reg rn) {
    validate_reg(rn);
    emit(0xD63F0000u | (rn << 5));
}

void Encoder::br(Reg rn) {
    validate_reg(rn);
    emit(0xD61F0000u | (rn << 5));
}

void Encoder::ret(Reg rn) {
    validate_reg(rn);
    emit(0xD65F0000u | (rn << 5));
}

void Encoder::nop() {
    emit(0xD503201Fu);
}

std::uint32_t Encoder::encode_b(std::int64_t delta_bytes) {
    if ((delta_bytes & 3) != 0) {
        throw std::invalid_argument("a64::Encoder: branch delta is not 4-byte aligned");
    }
    const std::int64_t imm26 = delta_bytes / 4;
    if (imm26 < -(1LL << 25) || imm26 > (1LL << 25) - 1) {
        throw std::out_of_range("a64::Encoder: B target out of range");
    }
    return 0x14000000u | (static_cast<std::uint32_t>(imm26) & 0x03FFFFFFu);
}

Label Encoder::new_label() {
    const Label id = label_pos_.size();
    label_pos_.push_back(kUnbound);
    return id;
}

void Encoder::bind(Label label) {
    check_label(label);
    if (label_pos_[label] != kUnbound) {
        throw std::logic_error("a64::Encoder: label bound twice");
    }
    label_pos_[label] = code_.size();
    for (auto& f : fixups_) {
        if (f.at != kUnbound && f.target == label) {
            resolve_patch(f.at, code_.size(), f.kind);
            f.at = kUnbound;
        }
    }
}

void Encoder::resolve_patch(std::size_t at, std::size_t target, BranchKind kind) {
    const auto delta_bytes =
        static_cast<std::int64_t>(target) * 4 - static_cast<std::int64_t>(at) * 4;
    switch (kind) {
    case BranchKind::kCond: {
        const std::int64_t imm19 = delta_bytes / 4;
        if (imm19 < -(1 << 18) || imm19 > (1 << 18) - 1) {
            throw std::out_of_range("a64::Encoder: B.cond target out of range");
        }
        code_[at] |= (static_cast<std::uint32_t>(imm19) & 0x7FFFFu) << 5;
        return;
    }
    case BranchKind::kTestBit: {
        const std::int64_t imm14 = delta_bytes / 4;
        if (imm14 < -(1 << 13) || imm14 > (1 << 13) - 1) {
            throw std::out_of_range("a64::Encoder: TBZ/TBNZ target out of range");
        }
        code_[at] |= (static_cast<std::uint32_t>(imm14) & 0x3FFFu) << 5;
        return;
    }
    case BranchKind::kAdr: {
        if (delta_bytes < -(1LL << 20) || delta_bytes > (1LL << 20) - 1) {
            throw std::out_of_range("a64::Encoder: ADR target out of range");
        }
        const auto immlo = static_cast<std::uint32_t>(delta_bytes) & 0x3u;
        const auto immhi = (static_cast<std::uint32_t>(delta_bytes) >> 2) & 0x7FFFFu;
        code_[at] |= (immlo << 29) | (immhi << 5);
        return;
    }
    case BranchKind::kUncond:
        code_[at] = encode_b(delta_bytes);
        return;
    }
}

void Encoder::check_label(Label label) const {
    if (label >= label_pos_.size()) {
        throw std::out_of_range("a64::Encoder: unknown label");
    }
}

bool Encoder::has_unresolved_labels() const {
    for (const auto& f : fixups_) {
        if (f.at != kUnbound) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> Encoder::bytes() const {
    if (has_unresolved_labels()) {
        throw std::logic_error("a64::Encoder: branch to a label that was never bound");
    }
    std::vector<std::uint8_t> out;
    out.reserve(code_.size() * 4);
    for (const std::uint32_t word : code_) {
        out.push_back(static_cast<std::uint8_t>(word & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((word >> 16) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((word >> 24) & 0xFFu));
    }
    return out;
}

} // namespace hyperion::a64
