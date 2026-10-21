// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Differential test: the reference interpreter vs. real x86-64 hardware.
//
// This is the Phase 2 acceptance test. The interpreter is only trustworthy if
// an independent implementation agrees with it, so for each operation we run
// the SAME semantics BOTH ways and compare architectural state:
//
//   1. natively, by letting this host CPU execute the instruction (inline asm);
//   2. in the interpreter, over a FlatMemory image holding the encoded bytes.
//
// The hardware is the reference. The expected value is never re-derived in C++
// (that would just re-implement the interpreter and prove nothing) — it is
// whatever the CPU produced, captured alongside the interpreter's result.
//
// On a non-x86-64 host the native half cannot run. The test then reports that
// it did not compare, rather than reporting success for work it did not do.
// That is what happens on the AArch64 cross-test path under qemu-user.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/decoder.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"

namespace {

using hyperion::Flags;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::StepStatus;

#if defined(__x86_64__)
inline constexpr bool kHostIsX86_64 = true;
#else
inline constexpr bool kHostIsX86_64 = false;
#endif

constexpr std::uint64_t kBase = 0x400000;
constexpr std::uint64_t kRflagsBit1 = 0x2;

// Mask a captured RFLAGS down to the six flags this project models, so a
// comparison is not defeated by bits the model deliberately ignores.
std::uint64_t model_flags(std::uint64_t rflags) {
    return hyperion::pack_flags(hyperion::unpack_flags(rflags));
}

// Run the interpreter over `code` at kBase with rax = a, rbx = b, flags = f.
// Returns the final state. Requires the sequence to end in HLT.
GuestState interp(const std::vector<std::uint8_t>& code, std::uint64_t a, std::uint64_t b,
                  std::uint64_t f) {
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = a;
    state.reg(Gpr::kRbx) = b;
    state.flags = hyperion::unpack_flags(f);
    state.rip = kBase;
    for (int i = 0; i < 16; ++i) {
        const auto r = hyperion::step(state, memory);
        if (r.status == StepStatus::kHalted) {
            break;
        }
        HYPERION_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(StepStatus::kOk));
        break;
    }
    return state;
}

#if defined(__x86_64__)
// Native execution: load a -> rax, b -> rbx, flags -> EFLAGS, run `mnemonic`,
// then capture rax and the resulting flags. Memory constraints are used for
// every value so the compiler cannot allocate rax/rbx for something else.
//
// `mnemonic` must be a string literal in extended-asm syntax, i.e. register
// names written with `%%` (e.g. "addq %%rbx, %%rax").
struct NativeResult {
    std::uint64_t value = 0;
    std::uint64_t flags = 0;
};

#define HYPERION_NATIVE_ALU(mnemonic)                                                              \
    [](std::uint64_t a, std::uint64_t b, std::uint64_t flags_in) -> NativeResult {                 \
        std::uint64_t out = 0;                                                                     \
        std::uint64_t fo = 0;                                                                      \
        asm volatile("movq %[a], %%rax\n\t"                                                        \
                     "movq %[b], %%rbx\n\t"                                                        \
                     "pushq %[fl]\n\t"                                                             \
                     "popfq\n\t" mnemonic "\n\t"                                                   \
                     "movq %%rax, %[o]\n\t"                                                        \
                     "pushfq\n\t"                                                                  \
                     "popq %[fo]\n\t"                                                              \
                     : [o] "=m"(out), [fo] "=m"(fo)                                                \
                     : [a] "m"(a), [b] "m"(b), [fl] "m"(flags_in)                                  \
                     : "rax", "rbx", "cc", "memory");                                              \
        return NativeResult{out, fo};                                                              \
    }
#endif

// One comparison: same inputs, hardware vs interpreter. `code` must be the
// encoding of `mnemonic` operating on rax (destination) and rbx (source).
void compare_alu(const char* name, const std::vector<std::uint8_t>& code, std::uint64_t a,
                 std::uint64_t b, std::uint64_t flags_in
#if defined(__x86_64__)
                 ,
                 NativeResult (*native)(std::uint64_t, std::uint64_t, std::uint64_t)
#endif
) {
#if defined(__x86_64__)
    const NativeResult expected = native(a, b, flags_in);
    const GuestState actual = interp(code, a, b, flags_in);

    if (actual.reg(Gpr::kRax) != expected.value) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: rax hardware=0x%llx interpreter=0x%llx", name,
                      static_cast<unsigned long long>(expected.value),
                      static_cast<unsigned long long>(actual.reg(Gpr::kRax)));
        HYPERION_CHECK_EQ(std::string(buf), std::string());
    }
    if (hyperion::pack_flags(actual.flags) != model_flags(expected.flags)) {
        char buf[256];
        std::snprintf(
            buf, sizeof(buf), "%s: flags hardware=%s interpreter=%s", name,
            hyperion::to_string(hyperion::unpack_flags(model_flags(expected.flags))).c_str(),
            hyperion::to_string(actual.flags).c_str());
        HYPERION_CHECK_EQ(std::string(buf), std::string());
    }
#else
    (void)name;
    (void)code;
    (void)a;
    (void)b;
    (void)flags_in;
#endif
}

// Encode `op r/m64, r64` (0x01-style) with reg=rbx, rm=rax, i.e. dst=rax,
// src=rbx. REX.W = 0x48, ModRM = 11 011 000 = 0xD8.
std::vector<std::uint8_t> enc64_rm_r(std::uint8_t opcode) {
    return {0x48, opcode, 0xD8, 0xF4}; // ... ; hlt
}
// Encode `op r64, r/m64` (0x03-style): reg=rax, rm=rbx -> ModRM 0xC3.
std::vector<std::uint8_t> enc64_r_rm(std::uint8_t opcode) {
    return {0x48, opcode, 0xC3, 0xF4};
}
// Encode `op r/m64, imm8` (0x83 group): ModRM = 11 <ext> 000.
std::vector<std::uint8_t> enc64_rm_imm8(std::uint8_t ext, std::uint8_t imm) {
    return {0x48, 0x83, static_cast<std::uint8_t>(0xC0 | (ext << 3)), imm, 0xF4};
}
// Encode `mov r/m64, imm32` (0xC7 /0) into rax: ModRM = 11 000 000 = 0xC0.
std::vector<std::uint8_t> enc64_mov_rax_imm32(std::uint32_t imm) {
    return {0x48,
            0xC7,
            0xC0,
            static_cast<std::uint8_t>(imm & 0xFF),
            static_cast<std::uint8_t>((imm >> 8) & 0xFF),
            static_cast<std::uint8_t>((imm >> 16) & 0xFF),
            static_cast<std::uint8_t>((imm >> 24) & 0xFF),
            0xF4};
}

// Flag inputs chosen to exercise carry/overflow/zero/sign/parity in different
// combinations. Bit 1 is always set, as on real hardware.
constexpr std::uint64_t kFlagInputs[] = {
    kRflagsBit1,
    kRflagsBit1 | 0x1,  // CF set
    kRflagsBit1 | 0x40, // ZF set
    kRflagsBit1 | 0x1 | 0x40,
};

} // namespace

HYPERION_TEST("differential", "native_vs_interpreter_is_available_on_x86_64") {
    // Guards the suite: on x86-64 the comparisons below run against real
    // hardware. On any other host each hardware case SKIPs (reported
    // separately from a pass), so this test states which situation applies
    // instead of letting an unexecuted comparison look like coverage.
    if (!kHostIsX86_64) {
        HYPERION_SKIP("hardware differential requires an x86-64 host");
    }
    HYPERION_CHECK(kHostIsX86_64);
}

HYPERION_TEST("differential", "add_matches_hardware") {
#if defined(__x86_64__)
    const std::uint64_t pairs[][2] = {
        {1, 1},
        {0, 0},
        {0xFFFFFFFFFFFFFFFFULL, 1},
        {0x7FFFFFFFFFFFFFFFULL, 1},
        {0x8000000000000000ULL, 1},
        {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
        {0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL},
        {0x8000000000000000ULL, 0x8000000000000000ULL},
    };
    for (const auto& p : pairs) {
        for (const std::uint64_t f : kFlagInputs) {
            compare_alu("add r/m64,r64", enc64_rm_r(0x01), p[0], p[1], f,
                        HYPERION_NATIVE_ALU("addq %%rbx, %%rax"));
            compare_alu("add r64,r/m64", enc64_r_rm(0x03), p[0], p[1], f,
                        HYPERION_NATIVE_ALU("addq %%rbx, %%rax"));
        }
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}

HYPERION_TEST("differential", "sub_matches_hardware") {
#if defined(__x86_64__)
    const std::uint64_t pairs[][2] = {
        {0, 0},
        {0, 1},
        {1, 0},
        {0x8000000000000000ULL, 1},
        {0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
        {0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL},
        {5, 5},
        {0, 0x8000000000000000ULL},
    };
    for (const auto& p : pairs) {
        for (const std::uint64_t f : kFlagInputs) {
            compare_alu("sub r/m64,r64", enc64_rm_r(0x29), p[0], p[1], f,
                        HYPERION_NATIVE_ALU("subq %%rbx, %%rax"));
            compare_alu("sub r64,r/m64", enc64_r_rm(0x2B), p[0], p[1], f,
                        HYPERION_NATIVE_ALU("subq %%rbx, %%rax"));
        }
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}

HYPERION_TEST("differential", "logical_ops_match_hardware") {
#if defined(__x86_64__)
    const std::uint64_t pairs[][2] = {
        {0, 0},
        {0xF0F0F0F0F0F0F0F0ULL, 0x0F0F0F0F0F0F0F0FULL},
        {0xFFFFFFFFFFFFFFFFULL, 1},
        {0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL},
        {1, 1},
    };
    struct Op {
        const char* name;
        std::uint8_t rm_r;
        std::uint8_t r_rm;
        NativeResult (*native)(std::uint64_t, std::uint64_t, std::uint64_t);
    };
    const Op ops[] = {
        {"and", 0x21, 0x23, HYPERION_NATIVE_ALU("andq %%rbx, %%rax")},
        {"or", 0x09, 0x0B, HYPERION_NATIVE_ALU("orq %%rbx, %%rax")},
        {"xor", 0x31, 0x33, HYPERION_NATIVE_ALU("xorq %%rbx, %%rax")},
    };
    for (const auto& op : ops) {
        for (const auto& p : pairs) {
            for (const std::uint64_t f : kFlagInputs) {
                compare_alu(op.name, enc64_rm_r(op.rm_r), p[0], p[1], f, op.native);
                compare_alu(op.name, enc64_r_rm(op.r_rm), p[0], p[1], f, op.native);
            }
        }
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}

// CMP produces flags exactly like SUB but writes nothing. Hardware is the
// reference: run `cmpq`, capture flags, and compare against the interpreter's
// CMP (which must leave rax untouched).
HYPERION_TEST("differential", "cmp_matches_hardware_and_writes_nothing") {
#if defined(__x86_64__)
    const std::uint64_t pairs[][2] = {
        {0, 0},
        {1, 0},
        {0, 1},
        {0x8000000000000000ULL, 1},
        {0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
    };
    for (const auto& p : pairs) {
        for (const std::uint64_t f : kFlagInputs) {
            const NativeResult expected = HYPERION_NATIVE_ALU("cmpq %%rbx, %%rax")(p[0], p[1], f);
            const GuestState actual = interp(enc64_rm_r(0x39), p[0], p[1], f);
            // CMP must not modify rax.
            HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), p[0]);
            HYPERION_CHECK_EQ(hyperion::pack_flags(actual.flags), model_flags(expected.flags));
        }
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}

// INC and DEC are the classic flags trap: they change every flag EXCEPT CF.
HYPERION_TEST("differential", "inc_dec_preserve_carry") {
#if defined(__x86_64__)
    const std::uint64_t values[] = {
        0, 1, 0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0x0F};
    for (const std::uint64_t v : values) {
        for (const std::uint64_t f : kFlagInputs) {
            {
                const NativeResult expected = HYPERION_NATIVE_ALU("incq %%rax")(v, 0, f);
                const GuestState actual = interp({0x48, 0xFF, 0xC0, 0xF4}, v, 0, f); // inc rax; hlt
                HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), expected.value);
                HYPERION_CHECK_EQ(hyperion::pack_flags(actual.flags), model_flags(expected.flags));
            }
            {
                const NativeResult expected = HYPERION_NATIVE_ALU("decq %%rax")(v, 0, f);
                const GuestState actual = interp({0x48, 0xFF, 0xC8, 0xF4}, v, 0, f); // dec rax; hlt
                HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), expected.value);
                HYPERION_CHECK_EQ(hyperion::pack_flags(actual.flags), model_flags(expected.flags));
            }
        }
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}

// 32-bit operations zero-extend into the 64-bit register. This is the other
// classic trap, and it is exactly what the AArch64 backend must reproduce.
HYPERION_TEST("differential", "thirty_two_bit_ops_zero_extend") {
#if defined(__x86_64__)
    const std::uint64_t pairs[][2] = {
        {0xFFFFFFFFFFFFFFFFULL, 1},
        {0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL},
        {0, 0},
    };
    for (const auto& p : pairs) {
        for (const std::uint64_t f : kFlagInputs) {
            // addl %ebx, %eax  (no REX.W)
            const NativeResult expected = HYPERION_NATIVE_ALU("addl %%ebx, %%eax")(p[0], p[1], f);
            const GuestState actual = interp({0x01, 0xD8, 0xF4}, p[0], p[1], f);
            // The 64-bit register must equal the 32-bit result zero-extended.
            HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), expected.value);
            HYPERION_CHECK_EQ(actual.reg(Gpr::kRax) >> 32, 0ULL);
            HYPERION_CHECK_EQ(hyperion::pack_flags(actual.flags), model_flags(expected.flags));
        }
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}

// The 0xC7 /0 form takes an imm32 sign-extended to the operand size.
HYPERION_TEST("differential", "mov_imm32_sign_extends_in_64bit") {
#if defined(__x86_64__)
    // 48 C7 C0 <imm32> = mov rax, imm32. The 64-bit form sign-extends, so a
    // value with bit 31 set must land in rax as 0xFFFFFFFF________. A decoder
    // that zero-extends instead is a classic bug and this catches it.
    for (const std::uint32_t imm :
         {0x00000001u, 0x0000002Au, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
        const GuestState actual = interp(enc64_mov_rax_imm32(imm), 0, 0, kRflagsBit1);
        const std::uint64_t expected =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(imm)));
        HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), expected);
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}

HYPERION_TEST("differential", "alu_imm8_sign_extends") {
#if defined(__x86_64__)
    // 0x01 -> +1, 0xFF -> -1 (must sign-extend to 0xFFFFFFFFFFFFFFFF, not
    // 0x00000000000000FF). Getting this wrong is a classic decoder bug.
    const std::uint64_t base = 0x1000;
    for (const std::uint64_t f : kFlagInputs) {
        // addq $1, %rax
        {
            const GuestState actual = interp(enc64_rm_imm8(0, 0x01), base, 0, f);
            const NativeResult expected = HYPERION_NATIVE_ALU("addq $1, %%rax")(base, 0, f);
            HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), expected.value);
            HYPERION_CHECK_EQ(hyperion::pack_flags(actual.flags), model_flags(expected.flags));
        }
        // addq $-1, %rax
        {
            const GuestState actual = interp(enc64_rm_imm8(0, 0xFF), base, 0, f);
            const NativeResult expected = HYPERION_NATIVE_ALU("addq $-1, %%rax")(base, 0, f);
            HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), expected.value);
            HYPERION_CHECK_EQ(hyperion::pack_flags(actual.flags), model_flags(expected.flags));
        }
        // subq $-1, %rax  (extension 5)
        {
            const GuestState actual = interp(enc64_rm_imm8(5, 0xFF), base, 0, f);
            const NativeResult expected = HYPERION_NATIVE_ALU("subq $-1, %%rax")(base, 0, f);
            HYPERION_CHECK_EQ(actual.reg(Gpr::kRax), expected.value);
            HYPERION_CHECK_EQ(hyperion::pack_flags(actual.flags), model_flags(expected.flags));
        }
    }
#else
    HYPERION_SKIP("requires an x86-64 host to execute the reference instructions");
#endif
}
