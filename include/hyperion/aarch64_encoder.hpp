// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// AArch64 instruction encoder for the Phase 3 code generator.
//
// Design (see DECISIONS.md D-015):
//   - This is an *encoder*, not a disassembler: it turns a small, explicit set
//     of AArch64 operations into their 32-bit encodings. There is no decoding
//     path and no "close enough" output.
//   - Encodings are verified against GNU as / objdump, not written from
//     memory. tests/unit/test_aarch64_encoder.cpp holds the reference bytes.
//   - Branches use labels with a fixup pass, so a caller never computes a
//     byte offset by hand. Hand-computed offsets are a classic source of
//     "works in the test, wrong in the field" bugs.
//   - Only the instructions the Phase 3 translator actually emits are here.
//     Adding an instruction requires adding a reference-verified test.
#ifndef HYPERION_AARCH64_ENCODER_HPP
#define HYPERION_AARCH64_ENCODER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hyperion::a64 {

// AArch64 general-purpose register numbers, as used in encodings. X0..X30 are
// the GPRs; 31 means XZR (or SP in some contexts, which this encoder avoids by
// never treating 31 as SP).
using Reg = unsigned;
constexpr Reg kXzr = 31;
constexpr Reg kLr = 30;
constexpr Reg kSp = 31;

// Condition codes (the low nibble of B.cond).
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

// Operand width selector for the data-processing instructions here.
enum class Width : std::uint8_t { k32, k64 };

// Shift selector for the shifted-register forms of the logical instructions.
enum class Shift : std::uint8_t { kLsl = 0, kLsr = 1, kAsr = 2, kRor = 3 };

using Label = std::size_t;

// An in-memory sequence of AArch64 instructions with label-based branches.
class Encoder {
public:
    // --- data processing (register) -------------------------------------
    void add(Width w, Reg rd, Reg rn, Reg rm);
    void sub(Width w, Reg rd, Reg rn, Reg rm);
    // ADD/SUB with a shifted register: Rd = Rn +/- (Rm shifted by `amount`).
    // Used to form a memory effective address (index*scale) without a separate
    // shift instruction.
    void add_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount);
    void sub_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount);
    void and_(Width w, Reg rd, Reg rn, Reg rm);
    void orr(Width w, Reg rd, Reg rn, Reg rm);
    void eor(Width w, Reg rd, Reg rn, Reg rm);
    // EOR with a shifted register: Rd = Rn ^ (Rm shifted by `amount`).
    // The shift amount is 0..31 for 32-bit and 0..63 for 64-bit operands.
    void eor_shifted(Width w, Reg rd, Reg rn, Reg rm, Shift shift, unsigned amount);

    // --- data processing (immediate) ------------------------------------
    // ADD/SUB with a 12-bit unsigned immediate, optionally shifted left by 12.
    void add_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12 = false);
    void sub_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12 = false);

    // --- comparisons (set NZCV) -----------------------------------------
    // CMP is SUBS with the destination discarded, so it writes flags and no
    // register. This is how the x86 CMP and the flag-producing x86 ALU ops
    // will be lowered.
    void cmp(Width w, Reg rn, Reg rm);
    void cmp_imm(Width w, Reg rn, std::uint32_t imm12, bool shift12 = false);

    // --- flag-setting arithmetic (ADDS/SUBS) -----------------------------
    // The x86 ALU flags are derived from these: ADDS/SUBS set N,Z,C,V in the
    // AArch64 condition flags, and the translator reads them back with CSET.
    void adds(Width w, Reg rd, Reg rn, Reg rm);
    void subs(Width w, Reg rd, Reg rn, Reg rm);
    void adds_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12);
    void subs_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12);

    // --- conditional set (CSINC alias) -----------------------------------
    // CSET Rd, cond  ->  Rd = (cond ? 1 : 0). Used to materialise x86 flags.
    void cset(Width w, Reg rd, Cond c);

    // --- bitfield (UBFM aliases) -----------------------------------------
    // LSR Rd, Rn, #shift. UBFX Rd, Rn, #lsb, #width (extract a bit field).
    void lsr_imm(Width w, Reg rd, Reg rn, unsigned shift);
    void ubfx(Width w, Reg rd, Reg rn, unsigned lsb, unsigned width);

    // --- loads/stores (unsigned offset, scaled by access size) -----------
    // LDR/STR for 32- and 64-bit access, LDRB/STRB for 8-bit. `byte_offset`
    // must be a non-negative multiple of the access size, at most 4095*size.
    // These are how generated code reads and writes GuestState.
    void ldr(Width w, Reg rt, Reg rn, unsigned byte_offset);
    void str(Width w, Reg rt, Reg rn, unsigned byte_offset);
    void ldrb(Reg rt, Reg rn, unsigned byte_offset);
    void strb(Reg rt, Reg rn, unsigned byte_offset);

    // --- load/store pair (64-bit) ----------------------------------------
    // Used to save and restore callee-saved host registers around a block.
    // `offset` is the signed immediate (a multiple of 8 in [-512, 504]).
    //   *_off:  signed offset, no writeback (e.g. `stp x21, x22, [sp, #16]`)
    //   stp_pre: pre-index writeback        (e.g. `stp x19, x20, [sp, #-32]!`)
    //   ldp_post: post-index writeback      (e.g. `ldp x19, x20, [sp], #32`)
    void stp_off(Reg rt, Reg rt2, Reg rn, int offset);
    void ldp_off(Reg rt, Reg rt2, Reg rn, int offset);
    void stp_pre(Reg rt, Reg rt2, Reg rn, int offset);
    void ldp_post(Reg rt, Reg rt2, Reg rn, int offset);

    // --- moves -----------------------------------------------------------
    // Register move, as an ORR with the zero register (the canonical MOV
    // alias). A 32-bit move zero-extends into the 64-bit register, matching
    // x86-64's 32-bit write rule.
    void mov_reg(Width w, Reg rd, Reg rm);
    void movz(Width w, Reg rd, std::uint16_t imm16, unsigned shift = 0);
    void movk(Width w, Reg rd, std::uint16_t imm16, unsigned shift = 0);
    // Materialise a full 64-bit constant with a movz/movk sequence. Always
    // emits the minimal number of instructions (skipping all-zero halfwords
    // after the first set one).
    void mov_imm64(Reg rd, std::uint64_t value);

    // --- branches --------------------------------------------------------
    // Unconditional B and conditional B.cond to a label.
    void b(Label target);
    void b_cond(Cond c, Label target);
    // Compare-and-branch on a register being non-zero (CBNZ), used to test a
    // runtime helper's fault flag without a separate CMP.
    void cbnz(Reg rt, Label target);
    // Branch to the address in `rn`. `blr` additionally writes the return
    // address into X30 (LR), so generated code can call a runtime helper.
    void br(Reg rn);
    void blr(Reg rn);
    void ret(Reg rn = kLr);
    void nop();

    // --- labels ----------------------------------------------------------
    Label new_label();
    void bind(Label label);

    // --- output ----------------------------------------------------------
    std::size_t count() const { return code_.size(); } // instructions emitted
    std::size_t byte_size() const { return code_.size() * 4; }
    const std::vector<std::uint32_t>& code() const { return code_; }
    // Encoded bytes in little-endian order (AArch64 is little-endian here).
    std::vector<std::uint8_t> bytes() const;

private:
    void emit(std::uint32_t word);
    void check_label(Label label) const;
    void resolve_patch(std::size_t at, std::size_t target, bool conditional);
    bool has_unresolved_labels() const;

    std::vector<std::uint32_t> code_;
    // label id -> instruction index; kUnbound until bind() is called.
    std::vector<std::size_t> label_pos_;
    struct Fixup {
        std::size_t at;   // instruction index of the branch (kUnbound once patched)
        Label target;     // label it points at
        bool conditional; // B vs B.cond (different immediate width)
    };
    std::vector<Fixup> fixups_;
};

} // namespace hyperion::a64

#endif // HYPERION_AARCH64_ENCODER_HPP
