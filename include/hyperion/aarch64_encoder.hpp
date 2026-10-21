// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// AArch64 instruction encoder for the Phase 3–7 code generator.
//
// Design (see DECISIONS.md D-015):
//   - This is an *encoder*, not a disassembler: it turns an explicit set of
//     AArch64 operations into their exact 32-bit encodings.
//   - Every encoding is verified bit-for-bit against GNU as / objdump in
//     tests/unit/test_aarch64_encoder.cpp.
//   - Branches and PC-relative `ADR` use labels with a fixup pass, so a caller
//     never computes a byte offset by hand.
#ifndef HYPERION_AARCH64_ENCODER_HPP
#define HYPERION_AARCH64_ENCODER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hyperion::a64 {

using Reg = unsigned;
constexpr Reg kXzr = 31;
constexpr Reg kLr = 30;
constexpr Reg kSp = 31;

enum class Cond : std::uint8_t {
    kEq = 0x0,
    kNe = 0x1,
    kCs = 0x2,
    kCc = 0x3,
    kMi = 0x4,
    kPl = 0x5,
    kVs = 0x6,
    kVc = 0x7,
    kHi = 0x8,
    kLs = 0x9,
    kGe = 0xA,
    kLt = 0xB,
    kGt = 0xC,
    kLe = 0xD,
    kAl = 0xE,
    kNv = 0xF,
};

const char* to_string(Cond c);

enum class Width : std::uint8_t { k32, k64 };

enum class Shift : std::uint8_t { kLsl = 0, kLsr = 1, kAsr = 2, kRor = 3 };

using Label = std::size_t;

class Encoder {
public:
    // --- data processing (register) -------------------------------------
    void add(Width w, Reg rd, Reg rn, Reg rm);
    void add_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount);
    void sub(Width w, Reg rd, Reg rn, Reg rm);
    void sub_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount);
    void and_(Width w, Reg rd, Reg rn, Reg rm);
    void orr(Width w, Reg rd, Reg rn, Reg rm);
    void eor(Width w, Reg rd, Reg rn, Reg rm);
    void eor_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount);

    // --- variable register shifts & rotates -----------------------------
    void lsl_reg(Width w, Reg rd, Reg rn, Reg rm);
    void lsr_reg(Width w, Reg rd, Reg rn, Reg rm);
    void asr_reg(Width w, Reg rd, Reg rn, Reg rm);
    void ror_reg(Width w, Reg rd, Reg rn, Reg rm);

    // --- multiply (32/64-bit low + 64x64->128 high half) ----------------
    void mul(Width w, Reg rd, Reg rn, Reg rm);
    void smulh(Reg rd, Reg rn, Reg rm);
    void umulh(Reg rd, Reg rn, Reg rm);

    // --- data processing (immediate) ------------------------------------
    void add_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12 = false);
    void sub_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12 = false);

    // --- comparisons (set NZCV) -----------------------------------------
    void cmp(Width w, Reg rn, Reg rm);
    void cmp_imm(Width w, Reg rn, std::uint32_t imm12, bool shift12 = false);

    // --- flag-setting arithmetic (ADDS/SUBS) -----------------------------
    void adds(Width w, Reg rd, Reg rn, Reg rm);
    void subs(Width w, Reg rd, Reg rn, Reg rm);
    void adds_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12 = false);
    void subs_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12 = false);

    // Bitwise NOT / bit-reverse / byte-reverse / count leading zeros.
    void mvn(Width w, Reg rd, Reg rm);
    void rev(Width w, Reg rd, Reg rn);
    void rbit(Width w, Reg rd, Reg rn);
    void clz(Width w, Reg rd, Reg rn);

    // --- conditional set & select ---------------------------------------
    void cset(Width w, Reg rd, Cond c);
    void csel(Width w, Reg rd, Reg rn, Reg rm, Cond c);

    // --- bitfield & immediate shifts/rotates -----------------------------
    void lsl_imm(Width w, Reg rd, Reg rn, unsigned shift);
    void lsr_imm(Width w, Reg rd, Reg rn, unsigned shift);
    void asr_imm(Width w, Reg rd, Reg rn, unsigned shift);
    void ror_imm(Width w, Reg rd, Reg rn, unsigned shift);
    void ubfx(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width);
    void sbfx(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width);
    void bfi(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width);

    // --- loads/stores (unsigned immediate offset, scaled) ----------------
    void ldr(Width w, Reg rt, Reg rn, unsigned byte_offset);
    void str(Width w, Reg rt, Reg rn, unsigned byte_offset);
    void ldrh(Reg rt, Reg rn, unsigned byte_offset);
    void strh(Reg rt, Reg rn, unsigned byte_offset);
    void ldrb(Reg rt, Reg rn, unsigned byte_offset);
    void strb(Reg rt, Reg rn, unsigned byte_offset);

    // --- loads/stores (unscaled 64-bit register offset `[Xn, Xm]`) -------
    // `bytes` must be 1, 2, 4, or 8; loads zero-extend into `rt`.
    void ldr_reg_sized(unsigned bytes, Reg rt, Reg rn, Reg rm);
    void str_reg_sized(unsigned bytes, Reg rt, Reg rn, Reg rm);

    // --- load/store pair (64-bit) ----------------------------------------
    void stp_off(Reg rt, Reg rt2, Reg rn, int offset);
    void ldp_off(Reg rt, Reg rt2, Reg rn, int offset);
    void stp_pre(Reg rt, Reg rt2, Reg rn, int offset);
    void ldp_post(Reg rt, Reg rt2, Reg rn, int offset);

    // --- moves & PC-relative addressing ----------------------------------
    void mov_reg(Width w, Reg rd, Reg rm);
    void movz(Width w, Reg rd, std::uint16_t imm16, unsigned shift = 0);
    void movk(Width w, Reg rd, std::uint16_t imm16, unsigned shift = 0);
    void mov_imm64(Reg rd, std::uint64_t value);
    void adr(Reg rd, Label target);

    // --- branches & calls ------------------------------------------------
    void b(Label target);
    void b_cond(Cond c, Label target);
    void cbz(Width w, Reg rt, Label target);
    void cbnz(Width w, Reg rt, Label target);
    void cbnz(Reg rt, Label target) { cbnz(Width::k64, rt, target); }
    void tbz(Reg rt, unsigned bit, Label target);
    void tbnz(Reg rt, unsigned bit, Label target);
    void blr(Reg rn);
    void br(Reg rn);
    void ret(Reg rn = kLr);
    void nop();

    // Emit raw 32-bit word (used for 8-byte-aligned read-only literal pools).
    void emit_raw_u32(std::uint32_t word);

    static std::uint32_t encode_b(std::int64_t delta_bytes);

    // --- labels ----------------------------------------------------------
    Label new_label();
    void bind(Label label);

    // --- output ----------------------------------------------------------
    std::size_t count() const { return code_.size(); }
    std::size_t byte_size() const { return code_.size() * 4; }
    const std::vector<std::uint32_t>& code() const { return code_; }
    std::vector<std::uint8_t> bytes() const;

private:
    enum class BranchKind : std::uint8_t { kUncond, kCond, kTestBit, kAdr };

    void emit(std::uint32_t word);
    void check_label(Label label) const;
    void resolve_patch(std::size_t at, std::size_t target, BranchKind kind);
    bool has_unresolved_labels() const;

    std::vector<std::uint32_t> code_;
    std::vector<std::size_t> label_pos_;
    struct Fixup {
        std::size_t at;
        Label target;
        BranchKind kind;
    };
    std::vector<Fixup> fixups_;
};

} // namespace hyperion::a64

#endif // HYPERION_AARCH64_ENCODER_HPP
