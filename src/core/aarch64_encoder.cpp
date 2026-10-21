// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// AArch64 encoder. Every encoding here is checked against GNU as/objdump in
// tests/unit/test_aarch64_encoder.cpp; see the reference dump recorded there.
// Base opcodes carry a short comment naming the encoding class so a reader can
// cross-check against the ARM ARM without reverse-engineering the constants.
#include "hyperion/aarch64_encoder.hpp"

#include <cassert>
#include <stdexcept>

namespace hyperion::a64 {

namespace {

constexpr std::size_t kUnbound = static_cast<std::size_t>(-1);

// sf bit (bit 31) and a few shared bases for data-processing instructions.
constexpr std::uint32_t sf_of(Width w) {
    return w == Width::k64 ? 0x80000000u : 0u;
}

// Add/subtract (shifted register): sf op S 01011 shift(2) 0 Rm imm6 Rn Rd.
// LSL by 0 means shift=0, imm6=0.
constexpr std::uint32_t kAddSubRegBase = 0x0B000000u;
// Add/subtract (immediate): sf op S 10001 sh(1) imm12 Rn Rd.
constexpr std::uint32_t kAddSubImmBase = 0x11000000u;
// Logical (shifted register): sf opc(2) 01010 shift(2) N(1) Rm imm6 Rn Rd.
constexpr std::uint32_t kLogicalRegBase = 0x0A000000u;

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

void Encoder::add(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (rm << 16) | (rn << 5) | rd);
}

void Encoder::sub(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | (rm << 16) | (rn << 5) | rd);
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
    // Logical (shifted register): bits 23-22 select the shift type and bits
    // 15-10 hold the shift amount (5 bits for 32-bit, 6 for 64-bit operands).
    // Bit 21 is N and must be 0 here: N=1 selects the *inverted* (AND-family)
    // form, and binutils encodes `eor Xd, Xn, Xm, LSR #n` with N=0.
    const unsigned max_amount = (w == Width::k64) ? 63u : 31u;
    if (amount > max_amount) {
        throw std::invalid_argument("a64::Encoder: shift amount out of range");
    }
    const std::uint32_t shift_field = static_cast<std::uint32_t>(shift) << 22;
    emit(sf_of(w) | kLogicalRegBase | (2u << 29) | shift_field |
         (static_cast<std::uint32_t>(rm) << 16) | (amount << 10) | (rn << 5) | rd);
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
    // SUBS with Rd = XZR.
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
    // S = 1 (bit 29) makes this ADDS, setting N/Z/C/V.
    emit(sf_of(w) | kAddSubRegBase | (1u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::subs(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | (1u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::adds_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 29) | (imm12 << 10) | (rn << 5) | rd);
}

void Encoder::subs_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 30) | (1u << 29) | (imm12 << 10) | (rn << 5) | rd);
}

void Encoder::cset(Width w, Reg rd, Cond c) {
    validate_reg(rd);
    // CSET Rd, cond is CSINC Rd, XZR, XZR, invert(cond).
    // CSINC: sf 0 0 11010100 Rm cond 0 1 Rn Rd.
    const std::uint32_t inverted = (static_cast<std::uint32_t>(c) ^ 1u) & 0xFu;
    emit(sf_of(w) | 0x1A800400u | (inverted << 12) | (kXzr << 16) | (kXzr << 5) | rd);
}

void Encoder::lsr_imm(Width w, Reg rd, Reg rn, unsigned shift) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned width = (w == Width::k64) ? 64u : 32u;
    if (shift >= width) {
        throw std::invalid_argument("a64::Encoder: lsr shift out of range");
    }
    // LSR is UBFM with immr = shift and imms = width-1. UBFM: sf 10 100110 N immr imms Rn Rd.
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    emit(sf_of(w) | 0x53000000u | (n << 22) | ((shift & 0x3Fu) << 16) |
         (((width - 1) & 0x3Fu) << 10) | (rn << 5) | rd);
}

void Encoder::ubfx(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width) {
    validate_reg(rd);
    validate_reg(rn);
    const unsigned reg_width = (w == Width::k64) ? 64u : 32u;
    if (width == 0 || width > reg_width || lsb + width > reg_width) {
        throw std::invalid_argument("a64::Encoder: ubfx field out of range");
    }
    // UBFX is UBFM with immr = lsb and imms = lsb + width - 1.
    const unsigned n = (w == Width::k64) ? 1u : 0u;
    const unsigned immr = lsb;
    const unsigned imms = lsb + width - 1;
    emit(sf_of(w) | 0x53000000u | (n << 22) | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10) |
         (rn << 5) | rd);
}

namespace {
// LDR/STR (unsigned offset, scaled): size(2) 111 0 01 opc(2) imm12 Rn Rt.
// The immediate is the byte offset divided by the access size.
constexpr std::uint32_t kLoadStoreBase = 0x39000000u;
constexpr std::uint32_t kLoadStorePairBase = 0x28000000u;
} // namespace

void Encoder::ldr(Width w, Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    const unsigned size = (w == Width::k64) ? 3u : 2u;
    const unsigned scale = (w == Width::k64) ? 8u : 4u;
    if (byte_offset % scale != 0 || byte_offset / scale > 0xFFFu) {
        throw std::invalid_argument(
            "a64::Encoder: ldr offset must be a scaled multiple <= 4095*size");
    }
    // opc = 01 (zero-extending load).
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
    // opc = 00 for STR (store).
    emit((size << 30) | kLoadStoreBase | ((byte_offset / scale) << 10) | (rn << 5) | rt);
}

void Encoder::ldrb(Reg rt, Reg rn, unsigned byte_offset) {
    validate_reg(rt);
    validate_reg(rn);
    if (byte_offset > 0xFFFu) {
        throw std::invalid_argument("a64::Encoder: ldrb offset out of range");
    }
    // size = 00, opc = 01 (zero-extending load) -> LDRB.
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

// STP/LDP use the signed-offset load/store-pair encoding. The 7-bit signed
// immediate is scaled by 8 for 64-bit pairs.
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
    // opc = 10 (64-bit), L = 0 (store), mode = 10 (signed offset).
    emit(kLoadStorePairBase | (2u << 30) | (2u << 23) | (scaled_pair_imm(offset) << 15) |
         (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::ldp_off(Reg rt, Reg rt2, Reg rn, int offset) {
    validate_reg(rt);
    validate_reg(rt2);
    validate_reg(rn);
    // opc = 10 (64-bit), L = 1 (load), mode = 10 (signed offset).
    emit(kLoadStorePairBase | (2u << 30) | (1u << 22) | (2u << 23) |
         (scaled_pair_imm(offset) << 15) | (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::stp_pre(Reg rt, Reg rt2, Reg rn, int offset) {
    validate_reg(rt);
    validate_reg(rt2);
    validate_reg(rn);
    // opc = 10 (64-bit), L = 0 (store), mode = 11 (pre-index).
    emit(kLoadStorePairBase | (2u << 30) | (3u << 23) | (scaled_pair_imm(offset) << 15) |
         (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::ldp_post(Reg rt, Reg rt2, Reg rn, int offset) {
    validate_reg(rt);
    validate_reg(rt2);
    validate_reg(rn);
    // opc = 10 (64-bit), L = 1 (load), mode = 01 (post-index).
    emit(kLoadStorePairBase | (2u << 30) | (1u << 22) | (1u << 23) |
         (scaled_pair_imm(offset) << 15) | (rt2 << 10) | (rn << 5) | rt);
}

void Encoder::mov_reg(Width w, Reg rd, Reg rm) {
    validate_reg(rd);
    validate_reg(rm);
    // ORR Rd, XZR, Rm  -- the canonical MOV alias. A 32-bit form zero-extends.
    emit(sf_of(w) | kLogicalRegBase | (1u << 29) | (rm << 16) | (kXzr << 5) | rd);
}

void Encoder::movz(Width w, Reg rd, std::uint16_t imm16, unsigned shift) {
    validate_reg(rd);
    if (shift > 48 || (shift % 16) != 0) {
        throw std::invalid_argument("a64::Encoder: movz shift must be 0/16/32/48");
    }
    const std::uint32_t hw = shift / 16;
    // MOVZ: sf 10 100101 hw(2) imm16 Rd.
    const std::uint32_t base = (w == Width::k64) ? 0xD2800000u : 0x52800000u;
    emit(base | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) | rd);
}

void Encoder::movk(Width w, Reg rd, std::uint16_t imm16, unsigned shift) {
    validate_reg(rd);
    if (shift > 48 || (shift % 16) != 0) {
        throw std::invalid_argument("a64::Encoder: movk shift must be 0/16/32/48");
    }
    const std::uint32_t hw = shift / 16;
    // MOVK: sf 11 100101 hw(2) imm16 Rd.
    const std::uint32_t base = (w == Width::k64) ? 0xF2800000u : 0x72800000u;
    emit(base | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) | rd);
}

void Encoder::mov_imm64(Reg rd, std::uint64_t value) {
    // First instruction is always MOVZ so the register is fully defined; higher
    // halfwords are added only when non-zero.
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

void Encoder::b(Label target) {
    check_label(target);
    const std::size_t at = code_.size();
    emit(0x14000000u); // B, imm26 placeholder
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], false);
    } else {
        fixups_.push_back({at, target, false});
    }
}

void Encoder::b_cond(Cond c, Label target) {
    check_label(target);
    const std::size_t at = code_.size();
    emit(0x54000000u | static_cast<std::uint32_t>(c)); // B.cond, imm19 placeholder
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], true);
    } else {
        fixups_.push_back({at, target, true});
    }
}

void Encoder::ret(Reg rn) {
    validate_reg(rn);
    emit(0xD65F0000u | (rn << 5)); // RET Xn
}

void Encoder::br(Reg rn) {
    validate_reg(rn);
    emit(0xD61F0000u | (rn << 5)); // BR Xn
}

void Encoder::blr(Reg rn) {
    validate_reg(rn);
    emit(0xD63F0000u | (rn << 5)); // BLR Xn (writes X30)
}

void Encoder::nop() {
    emit(0xD503201Fu);
}

Label Encoder::new_label() {
    label_pos_.push_back(kUnbound);
    return label_pos_.size() - 1;
}

void Encoder::bind(Label label) {
    check_label(label);
    if (label_pos_[label] != kUnbound) {
        throw std::logic_error("a64::Encoder: label bound twice");
    }
    label_pos_[label] = code_.size();
    // Patch any forward branches that were waiting on this label.
    for (auto& f : fixups_) {
        if (f.target == label && f.at != kUnbound) {
            resolve_patch(f.at, code_.size(), f.conditional);
            f.at = kUnbound; // mark done
        }
    }
}

void Encoder::resolve_patch(std::size_t at, std::size_t target, bool conditional) {
    const auto delta_bytes =
        static_cast<std::int64_t>(target) * 4 - static_cast<std::int64_t>(at) * 4;
    if (conditional) {
        const std::int64_t imm19 = delta_bytes / 4;
        if (imm19 < -(1 << 18) || imm19 > (1 << 18) - 1) {
            throw std::out_of_range("a64::Encoder: B.cond target out of range");
        }
        code_[at] |= (static_cast<std::uint32_t>(imm19) & 0x7FFFFu) << 5;
    } else {
        const std::int64_t imm26 = delta_bytes / 4;
        if (imm26 < -(1 << 25) || imm26 > (1 << 25) - 1) {
            throw std::out_of_range("a64::Encoder: B target out of range");
        }
        code_[at] |= static_cast<std::uint32_t>(imm26) & 0x03FFFFFFu;
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
