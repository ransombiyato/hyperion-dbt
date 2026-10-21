// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// AArch64 encoder tests. The expected words below are the exact output of
// GNU as / objdump (binutils 2.44), captured from this source:
//
//   movz x0, #0x1234
//   movz x1, #0xabcd, lsl #16
//   movk x0, #0xbeef, lsl #32
//   movz w2, #0x5678
//   add x3, x4, x5
//   sub x6, x7, x8
//   and x9, x10, x11
//   orr x12, x13, x14
//   eor x15, x16, x17
//   add w18, w19, w20
//   sub w21, w22, w23
//   add x0, x1, #0x1ff
//   sub x2, x3, #0x7
//   cmp x4, x5
//   cmp x6, #42
//   b 4 ; b.eq 8 ; b.ne -4
//   ret
//
// This is deliberate: an encoder checked only against itself proves nothing.
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/aarch64_encoder.hpp"

using hyperion::a64::Cond;
using hyperion::a64::Encoder;
using hyperion::a64::Label;
using hyperion::a64::Shift;
using hyperion::a64::Width;

HYPERION_TEST("aarch64_encoder", "data_processing_register_matches_binutils") {
    Encoder e;
    e.add(Width::k64, 3, 4, 5);
    e.sub(Width::k64, 6, 7, 8);
    e.and_(Width::k64, 9, 10, 11);
    e.orr(Width::k64, 12, 13, 14);
    e.eor(Width::k64, 15, 16, 17);
    e.add(Width::k32, 18, 19, 20);
    e.sub(Width::k32, 21, 22, 23);

    const std::vector<std::uint32_t> expected = {
        0x8B050083u, // add x3, x4, x5
        0xCB0800E6u, // sub x6, x7, x8
        0x8A0B0149u, // and x9, x10, x11
        0xAA0E01ACu, // orr x12, x13, x14
        0xCA11020Fu, // eor x15, x16, x17
        0x0B140272u, // add w18, w19, w20
        0x4B1702D5u, // sub w21, w22, w23
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "store_pair_matches_binutils") {
    // Expected words are the exact output of GNU as / objdump (binutils 2.44):
    //   stp x19, x20, [sp, #-32]!
    //   stp x21, x22, [sp, #16]
    //   ldp x21, x22, [sp, #16]
    //   ldp x19, x20, [sp], #32
    Encoder e;
    e.stp_pre(19, 20, 31, -32);
    e.stp_off(21, 22, 31, 16);
    e.ldp_off(21, 22, 31, 16);
    e.ldp_post(19, 20, 31, 32);

    const std::vector<std::uint32_t> expected = {
        0xA9BE53F3u, // stp x19, x20, [sp, #-32]!
        0xA9015BF5u, // stp x21, x22, [sp, #16]
        0xA9415BF5u, // ldp x21, x22, [sp, #16]
        0xA8C253F3u, // ldp x19, x20, [sp], #32
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "moves_match_binutils") {
    Encoder e;
    e.movz(Width::k64, 0, 0x1234);
    e.movz(Width::k64, 1, 0xABCD, 16);
    e.movk(Width::k64, 0, 0xBEEF, 32);
    e.movz(Width::k32, 2, 0x5678);

    const std::vector<std::uint32_t> expected = {
        0xD2824680u, // movz x0, #0x1234
        0xD2B579A1u, // movz x1, #0xabcd, lsl #16
        0xF2D7DDE0u, // movk x0, #0xbeef, lsl #32
        0x528ACF02u, // movz w2, #0x5678
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "immediate_arith_and_cmp_match_binutils") {
    Encoder e;
    e.add_imm(Width::k64, 0, 1, 0x1FF);
    e.sub_imm(Width::k64, 2, 3, 7);
    e.cmp(Width::k64, 4, 5);
    e.cmp_imm(Width::k64, 6, 42);

    const std::vector<std::uint32_t> expected = {
        0x9107FC20u, // add x0, x1, #0x1ff
        0xD1001C62u, // sub x2, x3, #0x7
        0xEB05009Fu, // cmp x4, x5
        0xF100A8DFu, // cmp x6, #0x2a
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "ret_and_nop") {
    Encoder e;
    e.ret();
    e.nop();
    HYPERION_CHECK_EQ(e.code()[0], 0xD65F03C0u); // ret
    HYPERION_CHECK_EQ(e.code()[1], 0xD503201Fu); // nop
}

HYPERION_TEST("aarch64_encoder", "br_and_blr_register_indirect") {
    // Encodings verified against aarch64-linux-gnu-as: `br x9` = 0xd61f0120,
    // `blr x10` = 0xd63f0140. blr writes the return address into X30, so
    // generated code can call a runtime helper and RET back.
    Encoder e;
    e.br(9);
    e.blr(10);
    e.blr(8);
    HYPERION_CHECK_EQ(e.code()[0], 0xD61F0120u); // br x9
    HYPERION_CHECK_EQ(e.code()[1], 0xD63F0140u); // blr x10
    HYPERION_CHECK_EQ(e.code()[2], 0xD63F0100u); // blr x8
}

HYPERION_TEST("aarch64_encoder", "add_sub_shifted_register") {
    // Encodings verified against aarch64-linux-gnu-as: add x0,x1,x2,lsl #3 =
    // 0x8b020c20, sub x3,x4,x5,lsl #2 = 0xcb050883. Used to form a memory
    // effective address (index*scale) in one instruction.
    Encoder e;
    e.add_shifted(Width::k64, 0, 1, 2, Shift::kLsl, 3);
    e.sub_shifted(Width::k64, 3, 4, 5, Shift::kLsl, 2);
    HYPERION_CHECK_EQ(e.code()[0], 0x8B020C20u);
    HYPERION_CHECK_EQ(e.code()[1], 0xCB050883u);
}

HYPERION_TEST("aarch64_encoder", "cbnz_forward_and_backward") {
    // cbnz x1, <8> ; nop ; <8>: cbnz x0, <10> ; nop ; <10>:
    // Verified against aarch64-linux-gnu-as: 0xb5000041, 0xd503201f,
    // 0xb5000040, 0xd503201f. Used to test a helper's fault flag.
    Encoder e;
    const Label skip = e.new_label();
    e.cbnz(1, skip);
    e.nop();
    e.bind(skip);
    const Label done = e.new_label();
    e.cbnz(0, done);
    e.nop();
    e.bind(done);
    HYPERION_CHECK_EQ(e.code()[0], 0xB5000041u);
    HYPERION_CHECK_EQ(e.code()[1], 0xD503201Fu);
    HYPERION_CHECK_EQ(e.code()[2], 0xB5000040u);
    HYPERION_CHECK_EQ(e.code()[3], 0xD503201Fu);
}

HYPERION_TEST("aarch64_encoder", "forward_branch_patch") {
    // b <skip> ; nop ; skip: nop ; nop
    // skip is at instruction index 2, so the branch spans 2 instructions.
    // binutils: `b 8` (forward 8 bytes) = 0x14000002.
    Encoder e;
    const auto skip = e.new_label();
    e.b(skip);
    e.nop();
    e.bind(skip);
    e.nop();
    HYPERION_CHECK_EQ(e.code()[0], 0x14000002u);
}

HYPERION_TEST("aarch64_encoder", "backward_conditional_branch_patch") {
    // loop: nop ; b.ne loop
    // b.ne to an address 4 bytes back => imm19 = -1 => 0x54FFFFE1.
    // This is exactly the encoding binutils produced for `b.ne -4`.
    Encoder e;
    const auto loop = e.new_label();
    e.bind(loop);
    e.nop();
    e.b_cond(Cond::kNe, loop);
    HYPERION_CHECK_EQ(e.code()[1], 0x54FFFFE1u);
}

HYPERION_TEST("aarch64_encoder", "forward_conditional_branch_patch") {
    // b.eq <skip> ; nop ; skip: nop
    // skip is at instruction index 2 => imm19 = 2 => 0x54000040, which is
    // exactly the encoding binutils produced for `b.eq 14` from address 0xc
    // (a forward span of two instructions).
    Encoder e;
    const auto skip = e.new_label();
    e.b_cond(Cond::kEq, skip);
    e.nop();
    e.bind(skip);
    e.nop();
    HYPERION_CHECK_EQ(e.code()[0], 0x54000040u);
}

HYPERION_TEST("aarch64_encoder", "mov_imm64_uses_minimal_sequence") {
    Encoder e;
    e.mov_imm64(0, 0x1234);
    HYPERION_CHECK_EQ(e.count(), 1ULL); // just movz
    HYPERION_CHECK_EQ(e.code()[0], 0xD2824680u);

    Encoder e2;
    e2.mov_imm64(0, 0xABCD00001234ULL);
    // bits 0..15 = 0x1234, bits 16..31 = 0, bits 32..47 = 0xabcd.
    // => movz x0, #0x1234 ; movk x0, #0xabcd, lsl #32
    // binutils: `movk x0, #0xabcd, lsl #32` = 0xF2D579A0.
    HYPERION_CHECK_EQ(e2.count(), 2ULL);
    HYPERION_CHECK_EQ(e2.code()[0], 0xD2824680u);
    HYPERION_CHECK_EQ(e2.code()[1], 0xF2D579A0u);
}

HYPERION_TEST("aarch64_encoder", "bytes_are_little_endian") {
    Encoder e;
    e.ret(); // 0xD65F03C0
    const auto b = e.bytes();
    HYPERION_CHECK_EQ(b.size(), 4ULL);
    HYPERION_CHECK_EQ(b[0], 0xC0u);
    HYPERION_CHECK_EQ(b[1], 0x03u);
    HYPERION_CHECK_EQ(b[2], 0x5Fu);
    HYPERION_CHECK_EQ(b[3], 0xD6u);
}

HYPERION_TEST("aarch64_encoder", "unbound_label_is_rejected") {
    Encoder e;
    const auto never = e.new_label();
    e.b(never);
    bool threw = false;
    try {
        (void)e.bytes();
    } catch (const std::logic_error&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("aarch64_encoder", "double_bind_is_rejected") {
    Encoder e;
    const auto l = e.new_label();
    e.bind(l);
    bool threw = false;
    try {
        e.bind(l);
    } catch (const std::logic_error&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("aarch64_encoder", "imm12_overflow_is_rejected") {
    Encoder e;
    bool threw = false;
    try {
        e.add_imm(Width::k64, 0, 1, 0x1000); // one past the 12-bit range
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

// --- Phase 3 additions: flag-setting, cset, bitfield, load/store ------------
//
// Reference dump for the words below (binutils 2.44, aarch64-linux-gnu-as):
//
//   adds x0, x1, x2         subs x6, x7, x8        adds x12, x13, #1
//   adds w3, w4, w5         subs w9, w10, w11      subs x14, x15, #1
//   cset w16, eq            cset w17, ne           cset w18, cs
//   cset w19, cc            cset w20, mi           cset w21, vs
//   lsr w22, w23, #4        lsr x24, x25, #4       ubfx w26, w27, #4, #1
//   ldr x1, [x2, #8]        ldr w3, [x4, #4]       str x5, [x6, #16]
//   str w7, [x8, #12]       ldrb w9, [x10, #1]     strb w11, [x12, #2]
//   ldr x13, [x14]          str x15, [x16]

HYPERION_TEST("aarch64_encoder", "flag_setting_arith_matches_binutils") {
    Encoder e;
    e.adds(Width::k64, 0, 1, 2);
    e.adds(Width::k32, 3, 4, 5);
    e.subs(Width::k64, 6, 7, 8);
    e.subs(Width::k32, 9, 10, 11);
    e.adds_imm(Width::k64, 12, 13, 1);
    e.subs_imm(Width::k64, 14, 15, 1);

    const std::vector<std::uint32_t> expected = {
        0xAB020020u, // adds x0, x1, x2
        0x2B050083u, // adds w3, w4, w5
        0xEB0800E6u, // subs x6, x7, x8
        0x6B0B0149u, // subs w9, w10, w11
        0xB10005ACu, // adds x12, x13, #0x1
        0xF10005EEu, // subs x14, x15, #0x1
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "cset_matches_binutils") {
    Encoder e;
    e.cset(Width::k32, 16, Cond::kEq);
    e.cset(Width::k32, 17, Cond::kNe);
    e.cset(Width::k32, 18, Cond::kCs);
    e.cset(Width::k32, 19, Cond::kCc);
    e.cset(Width::k32, 20, Cond::kMi);
    e.cset(Width::k32, 21, Cond::kVs);

    const std::vector<std::uint32_t> expected = {
        0x1A9F17F0u, // cset w16, eq
        0x1A9F07F1u, // cset w17, ne
        0x1A9F37F2u, // cset w18, cs
        0x1A9F27F3u, // cset w19, cc
        0x1A9F57F4u, // cset w20, mi
        0x1A9F77F5u, // cset w21, vs
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "bitfield_matches_binutils") {
    Encoder e;
    e.lsr_imm(Width::k32, 22, 23, 4);
    e.lsr_imm(Width::k64, 24, 25, 4);
    e.ubfx(Width::k32, 26, 27, 4, 1);

    const std::vector<std::uint32_t> expected = {
        0x53047EF6u, // lsr w22, w23, #4
        0xD344FF38u, // lsr x24, x25, #4
        0x5304137Au, // ubfx w26, w27, #4, #1
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "eor_shifted_matches_binutils") {
    Encoder e;
    e.eor_shifted(Width::k64, 11, 9, 9, Shift::kLsr, 4);
    e.eor_shifted(Width::k64, 11, 11, 11, Shift::kLsr, 2);
    e.eor_shifted(Width::k64, 11, 11, 11, Shift::kLsr, 1);
    e.eor_shifted(Width::k32, 11, 9, 9, Shift::kLsr, 4);
    e.eor_shifted(Width::k32, 11, 11, 11, Shift::kLsr, 2);
    e.eor_shifted(Width::k32, 11, 11, 11, Shift::kLsr, 1);
    e.eor_shifted(Width::k64, 3, 1, 2, Shift::kLsl, 0); // plain eor x3, x1, x2
    // All four shift types, so a wrong shift-field encoding cannot slip through.
    e.eor_shifted(Width::k64, 5, 6, 7, Shift::kLsl, 3);
    e.eor_shifted(Width::k64, 5, 6, 7, Shift::kAsr, 3);
    e.eor_shifted(Width::k64, 5, 6, 7, Shift::kRor, 3);
    e.eor_shifted(Width::k32, 5, 6, 7, Shift::kRor, 3);

    const std::vector<std::uint32_t> expected = {
        0xCA49112Bu, // eor x11, x9, x9, lsr #4
        0xCA4B096Bu, // eor x11, x11, x11, lsr #2
        0xCA4B056Bu, // eor x11, x11, x11, lsr #1
        0x4A49112Bu, // eor w11, w9, w9, lsr #4
        0x4A4B096Bu, // eor w11, w11, w11, lsr #2
        0x4A4B056Bu, // eor w11, w11, w11, lsr #1
        0xCA020023u, // eor x3, x1, x2
        0xCA070CC5u, // eor x5, x6, x7, lsl #3
        0xCA870CC5u, // eor x5, x6, x7, asr #3
        0xCAC70CC5u, // eor x5, x6, x7, ror #3
        0x4AC70CC5u, // eor w5, w6, w7, ror #3
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "eor_shifted_amount_overflow_is_rejected") {
    Encoder e;
    bool threw = false;
    try {
        e.eor_shifted(Width::k32, 1, 2, 3, Shift::kLsr, 32); // 32 > 31 for 32-bit
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    HYPERION_CHECK(threw);

    threw = false;
    try {
        e.eor_shifted(Width::k64, 1, 2, 3, Shift::kLsr, 64); // 64 > 63 for 64-bit
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("aarch64_encoder", "load_store_matches_binutils") {
    Encoder e;
    e.ldr(Width::k64, 1, 2, 8);
    e.ldr(Width::k32, 3, 4, 4);
    e.str(Width::k64, 5, 6, 16);
    e.str(Width::k32, 7, 8, 12);
    e.ldrb(9, 10, 1);
    e.strb(11, 12, 2);
    e.ldr(Width::k64, 13, 14, 0);
    e.str(Width::k64, 15, 16, 0);

    const std::vector<std::uint32_t> expected = {
        0xF9400441u, // ldr x1, [x2, #8]
        0xB9400483u, // ldr w3, [x4, #4]
        0xF90008C5u, // str x5, [x6, #16]
        0xB9000D07u, // str w7, [x8, #12]
        0x39400549u, // ldrb w9, [x10, #1]
        0x3900098Bu, // strb w11, [x12, #2]
        0xF94001CDu, // ldr x13, [x14]
        0xF900020Fu, // str x15, [x16]
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "unaligned_load_offset_is_rejected") {
    Encoder e;
    bool threw = false;
    try {
        e.ldr(Width::k64, 1, 2, 4); // 4 is not a multiple of the 8-byte access size
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("aarch64_encoder", "ubfx_field_overflow_is_rejected") {
    Encoder e;
    bool threw = false;
    try {
        e.ubfx(Width::k32, 1, 2, 31, 2); // 31 + 2 > 32
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("aarch64_encoder", "shifted_imm12_mvn_and_tbnz_match_binutils") {
    // Verified word-for-word against GNU binutils 2.44 (aarch64-linux-gnu-as /
    // aarch64-linux-gnu-objdump -d):
    //   add  x0, x1, #0x1, lsl #12    -> 0x91400420
    //   sub  w2, w3, #0xabc, lsl #12  -> 0x516af062
    //   adds x12, x13, #0x1, lsl #12  -> 0xb14005ac
    //   subs x14, x15, #0x2, lsl #12  -> 0xf14009ee
    //   cmp  x6, #0x3, lsl #12        -> 0xf1400cdf
    //   mvn  w13, w13                 -> 0x2a2d03ed
    //   mvn  x14, x15                 -> 0xaa2f03ee
    //   tbnz x7, #63, -4              -> 0xb7ffffe7
    //   tbz  x7, #63, -8              -> 0xb6ffffc7
    Encoder e;
    e.add_imm(Width::k64, 0, 1, 1, true);
    e.sub_imm(Width::k32, 2, 3, 0xABC, true);
    e.adds_imm(Width::k64, 12, 13, 1, true);
    e.subs_imm(Width::k64, 14, 15, 2, true);
    e.cmp_imm(Width::k64, 6, 3, true);
    e.mvn(Width::k32, 13, 13);
    e.mvn(Width::k64, 14, 15);
    const auto back = e.new_label();
    e.bind(back);
    e.nop();
    e.tbnz(7, 63, back);
    e.tbz(7, 63, back);

    const std::vector<std::uint32_t> expected = {
        0x91400420u, // add  x0, x1, #0x1, lsl #12
        0x516AF062u, // sub  w2, w3, #0xabc, lsl #12
        0xB14005ACu, // adds x12, x13, #0x1, lsl #12
        0xF14009EEu, // subs x14, x15, #0x2, lsl #12
        0xF1400CDFu, // cmp  x6, #0x3, lsl #12
        0x2A2D03EDu, // mvn  w13, w13
        0xAA2F03EEu, // mvn  x14, x15
        0xD503201Fu, // nop
        0xB7FFFFE7u, // tbnz x7, #63, back (-4)
        0xB6FFFFC7u, // tbz  x7, #63, back (-8)
    };
    HYPERION_CHECK(e.code() == expected);
}
