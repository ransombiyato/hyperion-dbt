// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 -> AArch64 translation of the Phase 2 subset (see translator.hpp for
// the exact scope). The generated block has the signature
// `BlockResult block(GuestState*)` and updates the guest state in place.
//
// Design notes:
//   - Guest registers are allocated to callee-saved host registers within a
//     block (Phase 5); see the RegAlloc comment below. GuestState remains
//     authoritative at block boundaries.
//   - x86 flags are *computed*, not delegated: AArch64's ADDS/SUBS set N/Z/C/V,
//     which map onto x86 SF/ZF/CF/OF directly; AF and PF are derived with two
//     ALU identities documented below. This is what makes the translation
//     independently checkable against the interpreter rather than a re-entry
//     into it.
//   - The x8 register holds the GuestState pointer. It is caller-saved in the
//     AAPCS64, so the block needs no prologue/epilogue unless it allocates.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "hyperion/translator.hpp"

#include "hyperion/runtime_helpers.hpp"

namespace hyperion {

namespace {

// AArch64 registers used by a translated block. x8 is the state pointer; the
// rest are scratch. All are caller-saved, so no save/restore is required.
constexpr a64::Reg kState = 8; // GuestState*
constexpr a64::Reg kA = 9;     // operand A (destination value)
constexpr a64::Reg kB = 10;    // operand B (source value)
constexpr a64::Reg kR = 11;    // result
constexpr a64::Reg kT0 = 12;   // scratch
constexpr a64::Reg kT1 = 13;   // scratch
constexpr a64::Reg kT2 = 14;   // scratch
constexpr a64::Reg kT3 = 15;   // scratch

// Guest-register allocation (Phase 5, BENCHMARKS.md §4.2.6).
//
// Within a block, the most-used guest registers are held in callee-saved host
// registers (x19..x28) rather than GuestState memory. Each is loaded once at
// block entry, kept in sync by every instruction that touches it, and spilled
// once at block exit. This lets `add rbx, rax` become a single `adds x_h,
// x_h, x_h` instead of load-load-add-store.
//
// The mapping is a pure fast path: GuestState remains the authoritative home of
// every register at block entry and exit, so the block signature and the
// authoritative-rip rule are unchanged, and a block with no allocation (or a
// guest register beyond the cap) keeps the old memory lowering. Because the
// host registers are callee-saved, they are saved/restored around the block.
constexpr std::size_t kMaxAllocated = 10; // x19..x28
constexpr a64::Reg kAllocBase = 19;

struct RegAlloc {
    std::int8_t host_of_guest[kGprCount];
    RegAlloc() {
        for (auto& h : host_of_guest) {
            h = -1;
        }
    }
    bool mapped(Gpr r) const { return host_of_guest[static_cast<unsigned>(r)] >= 0; }
    a64::Reg host(Gpr r) const {
        return static_cast<a64::Reg>(host_of_guest[static_cast<unsigned>(r)]);
    }
    // The host registers actually in use, in allocation order.
    std::vector<a64::Reg> allocated;
};

// Offsets into GuestState. Generated code hard-codes these, so they are tied
// to the struct with offsetof/static_assert: a layout change becomes a compile
// error here instead of silently corrupting translated code.
constexpr unsigned kRipOffset = offsetof(GuestState, rip);
constexpr unsigned kCfOffset = offsetof(GuestState, flags) + offsetof(Flags, cf);
constexpr unsigned kPfOffset = offsetof(GuestState, flags) + offsetof(Flags, pf);
constexpr unsigned kAfOffset = offsetof(GuestState, flags) + offsetof(Flags, af);
constexpr unsigned kZfOffset = offsetof(GuestState, flags) + offsetof(Flags, zf);
constexpr unsigned kSfOffset = offsetof(GuestState, flags) + offsetof(Flags, sf);
constexpr unsigned kOfOffset = offsetof(GuestState, flags) + offsetof(Flags, of);

static_assert(std::is_trivially_copyable_v<GuestState>,
              "generated code relies on GuestState being a plain memory image");

constexpr unsigned gpr_offset(Gpr r) {
    return 8u * static_cast<unsigned>(r); // 0..120
}

// gpr[] must be first in GuestState for the gpr_offset() arithmetic to hold.
static_assert(offsetof(GuestState, gpr) == 0, "gpr[] must be first in GuestState");
static_assert(offsetof(GuestState, gpr) + 8 * kGprCount == offsetof(GuestState, rip),
              "gpr[] must be immediately followed by rip");

void load_gpr(a64::Encoder& e, Gpr r, a64::Reg dst) {
    e.ldr(a64::Width::k64, dst, kState, gpr_offset(r));
}

void store_gpr(a64::Encoder& e, a64::Reg src, Gpr r) {
    e.str(a64::Width::k64, src, kState, gpr_offset(r));
}

// Register-allocation-aware load/store. If guest register `r` is allocated to a
// host register, the value is copied to/from that register; otherwise it goes
// through GuestState as before.
void load_gpr_rb(a64::Encoder& e, const RegAlloc* ra, Gpr r, a64::Reg dst) {
    if (ra != nullptr && ra->mapped(r)) {
        e.mov_reg(a64::Width::k64, dst, ra->host(r));
    } else {
        load_gpr(e, r, dst);
    }
}

void store_gpr_rb(a64::Encoder& e, const RegAlloc* ra, a64::Reg src, Gpr r) {
    if (ra != nullptr && ra->mapped(r)) {
        e.mov_reg(a64::Width::k64, ra->host(r), src);
    } else {
        store_gpr(e, src, r);
    }
}

// The guest register held in host register `h`, or XZR (31) if none. Used to
// spill an allocated register back to its GuestState slot.
Gpr guest_of_host(const RegAlloc& ra, a64::Reg h) {
    for (unsigned r = 0; r < kGprCount; ++r) {
        if (ra.host_of_guest[r] == static_cast<std::int8_t>(h)) {
            return static_cast<Gpr>(r);
        }
    }
    return static_cast<Gpr>(31);
}

// Parity of the low byte of `src`, into `dst` (0 = odd, 1 = even).
//
// x86 PF is set when the number of set bits in the low byte is even. XOR-folding
// the eight bits gives the odd/even bit; inverting it gives PF. Three shifted
// XORs fold 8 -> 4 -> 2 -> 1 bits, then a UBFX isolates the bit and an XOR with
// 1 inverts it. This replaces a 16-instruction per-bit fold plus its setup (19
// instructions in total) with 6.
void emit_parity(a64::Encoder& e, a64::Reg dst, a64::Reg src) {
    e.eor_shifted(a64::Width::k32, dst, src, src, a64::Shift::kLsr, 4); // b7..b4 ^ b3..b0
    e.eor_shifted(a64::Width::k32, dst, dst, dst, a64::Shift::kLsr, 2);
    e.eor_shifted(a64::Width::k32, dst, dst, dst, a64::Shift::kLsr, 1);
    e.ubfx(a64::Width::k32, dst, dst, 0, 1); // low bit = XOR of the eight bits
    e.movz(a64::Width::k32, kT3, 1);
    e.eor(a64::Width::k32, dst, dst, kT3); // PF = 1 - parity_bit
}

// --- Flag liveness ----------------------------------------------------------
//
// Which x86 flags an instruction reads and writes. Modelling this lets the
// translator skip computing a flag whose value is overwritten before anything
// reads it, which is most of the emitted code for an ALU instruction.
//
// The subtlety that a naive "every ALU op writes all six flags" model gets
// wrong is INC/DEC: they are ADD/SUB of 1 *with CF held*, so they write
// PF/AF/ZF/SF/OF but leave CF unchanged. flags_written() encodes that, and the
// emission never stores CF for INC/DEC, so CF is preserved for free.
enum FlagBit : std::uint8_t {
    kFbCf = 1u << 0,
    kFbPf = 1u << 1,
    kFbAf = 1u << 2,
    kFbZf = 1u << 3,
    kFbSf = 1u << 4,
    kFbOf = 1u << 5,
};
constexpr std::uint8_t kFbAll = 0x3F;

// Flags an instruction writes. Everything outside the flag-setting subset
// (MOV, NOP, JMP, HLT, ...) writes none.
std::uint8_t flags_written(Opcode op) {
    switch (op) {
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
        return kFbAll;
    case Opcode::kInc:
    case Opcode::kDec:
        return static_cast<std::uint8_t>(kFbAll & ~kFbCf);
    default:
        return 0;
    }
}

// Flags a condition reads. Must agree with alu::evaluate()/Condition, which the
// differential tests check directly.
std::uint8_t condition_flags(Condition c) {
    switch (c) {
    case Condition::kO:
    case Condition::kNo:
        return kFbOf;
    case Condition::kB:
    case Condition::kAe:
        return kFbCf;
    case Condition::kE:
    case Condition::kNe:
        return kFbZf;
    case Condition::kS:
    case Condition::kNs:
        return kFbSf;
    case Condition::kP:
    case Condition::kNp:
        return kFbPf;
    case Condition::kBe:
    case Condition::kA:
        return static_cast<std::uint8_t>(kFbCf | kFbZf);
    case Condition::kL:
    case Condition::kGe:
        return static_cast<std::uint8_t>(kFbSf | kFbOf);
    case Condition::kLe:
    case Condition::kG:
        return static_cast<std::uint8_t>(kFbSf | kFbOf | kFbZf);
    }
    return 0;
}

// Guest registers an instruction reads, and those it writes. Used both to pick
// which registers to allocate and to price that allocation. Conservative: it
// names every register the instruction could read or write, so the allocation is
// a pure fast path that never changes which registers exist.
void regs_read_write(const Instruction& insn, bool reads[kGprCount], bool writes[kGprCount]) {
    // A memory operand reads its base and index registers (the effective
    // address), whatever the instruction does with the value. Doing this first
    // keeps the address registers live for allocation.
    const Operand* mem_ops[2] = {&insn.dst, insn.has_src ? &insn.src : nullptr};
    for (const Operand* op : mem_ops) {
        if (op != nullptr && op->kind == OperandKind::kMemory) {
            if (op->mem.has_base) {
                reads[static_cast<unsigned>(op->mem.base)] = true;
            }
            if (op->mem.has_index) {
                reads[static_cast<unsigned>(op->mem.index)] = true;
            }
        }
    }
    switch (insn.opcode) {
    case Opcode::kMov:
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        // A memory destination writes memory, not a register.
        if (insn.dst.kind == OperandKind::kRegister) {
            writes[static_cast<unsigned>(insn.dst.reg)] = true;
        }
        break;
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
        // A memory destination is read and written in memory, not in a register;
        // only a register destination names a guest register here.
        if (insn.dst.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.dst.reg)] = true;
            writes[static_cast<unsigned>(insn.dst.reg)] = true;
        }
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        break;
    case Opcode::kCmp:
        // Reads both operands, writes no register (only flags).
        if (insn.dst.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.dst.reg)] = true;
        }
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        break;
    case Opcode::kInc:
    case Opcode::kDec:
        if (insn.dst.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.dst.reg)] = true;
            writes[static_cast<unsigned>(insn.dst.reg)] = true;
        }
        break;
    default:
        // Control flow, syscall, hlt, nop: no guest register operand.
        break;
    }
}

// Instructions an allocation of `n` registers adds to the block: the
// save/restore of the borrowed callee-saved host registers (one stp/ldp per
// pair, with an odd register stored as a pair with XZR) plus one ldr in the
// prologue and one str in the epilogue per allocated guest register.
std::size_t alloc_cost(std::size_t n) {
    return 2 * n + 2 * ((n + 1) / 2);
}

// Store the masked flags from the AArch64 condition flags, which must have been
// set by a preceding ADDS (is_sub=false) or SUBS (is_sub=true). `t2` must hold
// (a ^ b ^ result) and is read only when AF is requested; `result` is used for
// parity. Flags outside `mask` are not written, so their previous value stands.
void emit_flags_from_nzcv(a64::Encoder& e, bool is_sub, std::uint8_t mask, a64::Reg t2,
                          a64::Reg result) {
    if (mask & kFbZf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kEq); // ZF = Z
        e.strb(kT1, kState, kZfOffset);
    }
    if (mask & kFbSf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kMi); // SF = N
        e.strb(kT1, kState, kSfOffset);
    }
    if (mask & kFbCf) {
        // x86 CF is the carry-out: for add it is "carry set" (CS); for subtract
        // it is "borrow" (CC, i.e. no borrow), the standard ADDS/SUBS mapping.
        e.cset(a64::Width::k32, kT1, is_sub ? a64::Cond::kCc : a64::Cond::kCs);
        e.strb(kT1, kState, kCfOffset);
    }
    if (mask & kFbOf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kVs); // OF = V
        e.strb(kT1, kState, kOfOffset);
    }
    if (mask & kFbAf) {
        // AF: carry out of bit 3. For both ADD and SUB, AF == bit 4 of
        // (a ^ b ^ r).
        e.ubfx(a64::Width::k32, kT1, t2, 4, 1);
        e.strb(kT1, kState, kAfOffset);
    }
    if (mask & kFbPf) {
        // Parity is computed last: it clobbers the condition flags, which the
        // csets above need.
        emit_parity(e, kT1, result);
        e.strb(kT1, kState, kPfOffset);
    }
}

// Logical ops (AND/OR/XOR) clear CF, OF and AF, and set SF/ZF/PF from the
// result. There is no NZCV involvement.
void emit_flags_logical(a64::Encoder& e, a64::Width w, std::uint8_t mask, a64::Reg result) {
    if (mask & (kFbZf | kFbSf)) {
        e.cmp_imm(w, result, 0); // sets N (sign) and Z from the result
    }
    if (mask & kFbZf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kEq);
        e.strb(kT1, kState, kZfOffset);
    }
    if (mask & kFbSf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kMi);
        e.strb(kT1, kState, kSfOffset);
    }
    if (mask & (kFbCf | kFbOf | kFbAf)) {
        e.movz(a64::Width::k32, kT1, 0);
        if (mask & kFbCf) {
            e.strb(kT1, kState, kCfOffset);
        }
        if (mask & kFbOf) {
            e.strb(kT1, kState, kOfOffset);
        }
        if (mask & kFbAf) {
            e.strb(kT1, kState, kAfOffset);
        }
    }
    if (mask & kFbPf) {
        emit_parity(e, kT1, result);
        e.strb(kT1, kState, kPfOffset);
    }
}

// Whether `value` fits the AArch64 ADD/SUB 12-bit immediate form (imm12,
// optionally shifted left by 12). If so, fills `imm12`/`shift12`.
bool encode_imm12(std::uint64_t value, std::uint32_t& imm12, bool& shift12) {
    if (value <= 0xFFFu) {
        imm12 = static_cast<std::uint32_t>(value);
        shift12 = false;
        return true;
    }
    if ((value & 0xFFFu) == 0 && (value >> 12) <= 0xFFFu) {
        imm12 = static_cast<std::uint32_t>(value >> 12);
        shift12 = true;
        return true;
    }
    return false;
}

// Load a source operand into `dst`. Only registers and immediates are in scope
// for this increment.
TranslateError load_source(a64::Encoder& e, const Operand& src, a64::Reg dst, int bits,
                           const RegAlloc* ra) {
    switch (src.kind) {
    case OperandKind::kRegister:
        load_gpr_rb(e, ra, src.reg, dst);
        if (bits == 32) {
            // A 32-bit operand use sees only the low 32 bits, and a 32-bit MOV
            // must zero-extend into the destination (the x86-64 rule).
            e.mov_reg(a64::Width::k32, dst, dst);
        }
        return TranslateError::kNone;
    case OperandKind::kImmediate: {
        const std::uint64_t value = (bits == 32) ? (src.imm & 0xFFFFFFFFULL) : src.imm;
        e.mov_imm64(dst, value);
        return TranslateError::kNone;
    }
    case OperandKind::kMemory:
        return TranslateError::kUnsupportedOperand;
    }
    return TranslateError::kInternal;
}

// Compute an x86 effective address (base + index*scale + disp, all mod 2^64)
// into `dst`, reading guest registers through the allocator fast path. Uses
// kT0/kT1 as scratch, which must not alias `dst` or each other's live values.
void emit_effective_address(a64::Encoder& e, const MemoryOperand& mem, const RegAlloc* ra,
                            a64::Reg dst) {
    if (mem.has_base) {
        load_gpr_rb(e, ra, mem.base, dst);
    } else {
        e.movz(a64::Width::k64, dst, 0);
    }
    if (mem.has_index) {
        load_gpr_rb(e, ra, mem.index, kT0);
        unsigned shift = 0;
        switch (mem.scale) {
        case 2:
            shift = 1;
            break;
        case 4:
            shift = 2;
            break;
        case 8:
            shift = 3;
            break;
        default:
            shift = 0;
            break;
        }
        if (shift == 0) {
            e.add(a64::Width::k64, dst, dst, kT0);
        } else {
            e.add_shifted(a64::Width::k64, dst, dst, kT0, a64::Shift::kLsl, shift);
        }
    }
    if (mem.disp != 0) {
        const std::uint64_t magnitude = (mem.disp < 0)
                                            ? (0ULL - static_cast<std::uint64_t>(mem.disp))
                                            : static_cast<std::uint64_t>(mem.disp);
        std::uint32_t imm12 = 0;
        bool shift12 = false;
        if (encode_imm12(magnitude, imm12, shift12)) {
            if (mem.disp < 0) {
                e.sub_imm(a64::Width::k64, dst, dst, imm12, shift12);
            } else {
                e.add_imm(a64::Width::k64, dst, dst, imm12, shift12);
            }
        } else {
            e.mov_imm64(kT1, static_cast<std::uint64_t>(mem.disp));
            e.add(a64::Width::k64, dst, dst, kT1);
        }
    }
}

// Call a runtime helper whose address has been materialised into `target`. The
// helper is an ordinary C function, so it clobbers every caller-saved register,
// including x8 (GuestState*) and x30 (this block's return address); both are
// saved to the stack and restored. `fault_reg` holds the helper's fault flag
// (x1 for the load's struct return, x0 for the store's scalar return) and a
// non-zero value branches to `fault`.
void emit_helper_call(a64::Encoder& e, a64::Reg target, a64::Reg fault_reg, a64::Label fault) {
    e.stp_pre(kState, 30, 31, -16); // save GuestState* and LR
    e.blr(target);
    e.ldp_post(kState, 30, 31, 16); // restore them
    e.cbnz(fault_reg, fault);
}

void emit_return(a64::Encoder& e, BlockResult result, const RegAlloc* ra = nullptr) {
    if (ra != nullptr) {
        // Spill every allocated guest register back to its GuestState slot.
        // Spills are individual stores: two allocated host registers are not
        // necessarily consecutive in GuestState, so a pair store would write the
        // wrong slot.
        for (a64::Reg h : ra->allocated) {
            e.str(a64::Width::k64, h, kState, gpr_offset(guest_of_host(*ra, h)));
        }
        // Restore the callee-saved host registers the block borrowed, in the
        // exact reverse of the prologue. An odd count was stored as a pair with
        // XZR, so it is restored first and its second element is discarded.
        const std::size_t npairs = ra->allocated.size() / 2;
        if (ra->allocated.size() % 2 == 1) {
            e.ldp_post(ra->allocated.back(), 31, 31, 16);
        }
        for (std::size_t i = npairs; i > 0; --i) {
            e.ldp_post(ra->allocated[2 * i - 2], ra->allocated[2 * i - 1], 31, 16);
        }
    }
    e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(result));
    e.ret();
}

// Save the callee-saved host registers a block borrows. Pairs are stored with
// a pre-index writeback; an odd count stores the last alongside XZR.
void emit_prologue(a64::Encoder& e, const RegAlloc& ra) {
    for (std::size_t i = 0; i + 1 < ra.allocated.size(); i += 2) {
        e.stp_pre(ra.allocated[i], ra.allocated[i + 1], 31, -16);
    }
    if (ra.allocated.size() % 2 == 1) {
        e.stp_pre(ra.allocated.back(), 31, 31, -16);
    }
    // Load the allocated guest registers from GuestState into their host
    // registers. This must run after the saves above, which use the stack.
    for (a64::Reg h : ra.allocated) {
        e.ldr(a64::Width::k64, h, kState, gpr_offset(guest_of_host(ra, h)));
    }
}

// Emit `rip = value` for a constant guest address.
void emit_set_rip(a64::Encoder& e, std::uint64_t value) {
    e.mov_imm64(kT0, value);
    e.str(a64::Width::k64, kT0, kState, kRipOffset);
}

// Emit `rip = instruction.address + instruction.length`.
void emit_advance_rip(a64::Encoder& e, const Instruction& insn) {
    emit_set_rip(e, insn.address + insn.length);
}

// Branch to `taken` if the x86 condition `c` holds for the modelled flags in
// GuestState, otherwise fall through.
//
// x86 flags live in memory, not in AArch64's NZCV, and several x86 conditions
// (PF; the SF/OF combinations L/GE/LE/G) have no single AArch64 equivalent. So
// rather than map conditions onto NZCV, each condition loads exactly the flag
// bytes it reads, materialises a boolean, and branches on zero/non-zero. This
// is a few more instructions than a clever mapping but is straightforward to
// check against alu::evaluate(), which is the oracle.
void emit_cond_branch(a64::Encoder& e, Condition c, a64::Label taken) {
    auto load = [&](a64::Reg dst, unsigned off) { e.ldrb(dst, kState, off); };
    auto branch_if = [&](a64::Reg src, bool want_nonzero) {
        e.cmp_imm(a64::Width::k32, src, 0);
        e.b_cond(want_nonzero ? a64::Cond::kNe : a64::Cond::kEq, taken);
    };

    switch (c) {
    case Condition::kO:
    case Condition::kNo:
        load(kT0, kOfOffset);
        branch_if(kT0, c == Condition::kO);
        return;
    case Condition::kB:
    case Condition::kAe:
        load(kT0, kCfOffset);
        branch_if(kT0, c == Condition::kB);
        return;
    case Condition::kE:
    case Condition::kNe:
        load(kT0, kZfOffset);
        branch_if(kT0, c == Condition::kE);
        return;
    case Condition::kS:
    case Condition::kNs:
        load(kT0, kSfOffset);
        branch_if(kT0, c == Condition::kS);
        return;
    case Condition::kP:
    case Condition::kNp:
        load(kT0, kPfOffset);
        branch_if(kT0, c == Condition::kP);
        return;
    case Condition::kBe:
    case Condition::kA: {
        load(kT0, kCfOffset);
        load(kT1, kZfOffset);
        e.orr(a64::Width::k32, kT2, kT0, kT1); // kT2 = CF | ZF
        branch_if(kT2, c == Condition::kBe);
        return;
    }
    case Condition::kL:
    case Condition::kGe: {
        load(kT0, kSfOffset);
        load(kT1, kOfOffset);
        e.eor(a64::Width::k32, kT2, kT0, kT1); // kT2 = SF ^ OF
        branch_if(kT2, c == Condition::kL);
        return;
    }
    case Condition::kLe:
    case Condition::kG: {
        load(kT0, kSfOffset);
        load(kT1, kOfOffset);
        load(kT2, kZfOffset);
        e.eor(a64::Width::k32, kT3, kT0, kT1); // kT3 = SF ^ OF
        e.orr(a64::Width::k32, kT3, kT3, kT2); // kT3 = (SF ^ OF) | ZF
        branch_if(kT3, c == Condition::kLe);
        return;
    }
    }
}

} // namespace

const char* to_string(BlockExit exit) {
    switch (exit) {
    case BlockExit::kFallThrough:
        return "fall-through";
    case BlockExit::kJmp:
        return "jmp";
    case BlockExit::kJcc:
        return "jcc";
    case BlockExit::kHalted:
        return "halted";
    case BlockExit::kSyscall:
        return "syscall";
    }
    return "?";
}

// Emit the AArch64 for one already-validated instruction into `e`. `e` must
// already have x8 = GuestState*. Returns a translate error, or kNone on
// success. Control-flow instructions are handled by the block translator, not
// here, so they are rejected as unsupported by this helper.
namespace {

TranslateError emit_one(a64::Encoder& e, const Instruction& insn, bool write_rip,
                        std::uint8_t live_flags, const RegAlloc* ra = nullptr) {
    const bool is32 = insn.dst.size == OperandSize::k32;
    const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
    const int bits = is32 ? 32 : 64;

    switch (insn.opcode) {
    case Opcode::kNop:
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;

    case Opcode::kHlt:
        emit_return(e, BlockResult::kHalted);
        return TranslateError::kNone;

    case Opcode::kSyscall:
        // A syscall ends the block: the dispatcher must run the handler (which
        // needs the Memory object, unavailable in generated code) and then
        // resume. rip is advanced here so the handler, and any resume, sees the
        // instruction after `syscall`, matching a real CPU.
        emit_set_rip(e, insn.address + insn.length);
        emit_return(e, BlockResult::kSyscall);
        return TranslateError::kNone;

    case Opcode::kMov: {
        // Memory forms: `mov r64, [mem]` (0x8B) and `mov [mem], r64` (0x89).
        // The guest address space is host-backed regions at arbitrary guest
        // addresses, so the access goes through a runtime helper (soft MMU).
        const bool dst_mem = insn.dst.kind == OperandKind::kMemory;
        const bool src_mem = insn.has_src && insn.src.kind == OperandKind::kMemory;
        if (dst_mem || src_mem) {
            // 32- and 64-bit forms both reach here (0x89/0x8B with and without
            // REX.W); the access width follows the operand size, and a 32-bit
            // load zero-extends because the helper assembles only `bytes`.
            const std::size_t bytes = static_cast<std::size_t>(bits) / 8;
            const a64::Label fault = e.new_label();
            const a64::Label done = e.new_label();

            if (src_mem) {                                      // load: mov r64, [mem]
                emit_effective_address(e, insn.src.mem, ra, 0); // x0 = address
                e.mov_imm64(1, bytes);                          // x1 = count
                e.mov_imm64(16, reinterpret_cast<std::uint64_t>(&hyperion_load));
                emit_helper_call(e, 16, 1, fault); // value -> x0, fault -> x1
                store_gpr_rb(e, ra, 0, insn.dst.reg);
            } else {                                            // store: mov [mem], r64/imm32
                emit_effective_address(e, insn.dst.mem, ra, 0); // x0 = address
                if (insn.src.kind == OperandKind::kImmediate) {
                    // 0xC7 /0: the value is an imm32 (sign-extended for the
                    // 64-bit form by the decoder). Materialise it directly.
                    e.mov_imm64(1, insn.src.imm);
                } else {
                    load_gpr_rb(e, ra, insn.src.reg, 1); // x1 = value
                }
                e.mov_imm64(2, bytes); // x2 = count
                e.mov_imm64(16, reinterpret_cast<std::uint64_t>(&hyperion_store));
                emit_helper_call(e, 16, 0, fault); // return -> x0 (fault flag)
            }

            e.b(done);
            e.bind(fault);
            emit_set_rip(e, insn.address); // stop at the faulting instruction
            emit_return(e, BlockResult::kMemoryFault, ra);
            e.bind(done);

            if (write_rip) {
                emit_advance_rip(e, insn);
            }
            return TranslateError::kNone;
        }

        // If both operands are allocated, the move is a single host-register
        // move (the 32-bit form zero-extends, matching x86-64). If the source is
        // an immediate and the destination is allocated, materialise straight
        // into the host register. Otherwise fall back to the memory path.
        if (ra != nullptr && ra->mapped(insn.dst.reg) && insn.src.kind == OperandKind::kRegister &&
            ra->mapped(insn.src.reg)) {
            e.mov_reg(w, ra->host(insn.dst.reg), ra->host(insn.src.reg));
        } else if (ra != nullptr && ra->mapped(insn.dst.reg) &&
                   insn.src.kind == OperandKind::kImmediate) {
            const std::uint64_t value =
                (bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm;
            e.mov_imm64(ra->host(insn.dst.reg), value);
        } else {
            const TranslateError err = load_source(e, insn.src, kA, bits, ra);
            if (err != TranslateError::kNone) {
                return err;
            }
            store_gpr_rb(e, ra, kA, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kCmp: {
        // A memory operand makes this a read-modify-write (or a compare against
        // memory). The guest address space is host-backed regions at arbitrary
        // guest addresses, so the access goes through the soft-MMU runtime
        // helpers exactly as the MOV forms do (D-032): load the memory value,
        // run the operation on registers, store the result back. A fault on
        // either access exits the block as kMemoryFault rather than continuing.
        const bool dst_mem = insn.dst.kind == OperandKind::kMemory;
        const bool src_mem = insn.has_src && insn.src.kind == OperandKind::kMemory;
        if (dst_mem || src_mem) {
            const std::size_t bytes = static_cast<std::size_t>(bits) / 8;
            const a64::Label fault = e.new_label();
            const a64::Label done = e.new_label();
            const MemoryOperand& mem = dst_mem ? insn.dst.mem : insn.src.mem;

            // A holds the destination value, B the source value, matching the
            // register path below. Whichever side is memory arrives via the
            // helper; the other side is a register or an immediate.
            emit_effective_address(e, mem, ra, 0);
            e.mov_imm64(1, bytes);
            e.mov_imm64(16, reinterpret_cast<std::uint64_t>(&hyperion_load));
            emit_helper_call(e, 16, 1, fault); // value -> x0, fault -> x1
            // A 32-bit load leaves the upper half of x0 zero, which is the
            // zero-extension a 32-bit x86 operand needs.
            e.mov_reg(a64::Width::k64, dst_mem ? kA : kB, 0);

            if (dst_mem) {
                if (insn.src.kind == OperandKind::kRegister) {
                    load_gpr_rb(e, ra, insn.src.reg, kB);
                } else {
                    const std::uint64_t value =
                        (bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm;
                    e.mov_imm64(kB, value);
                }
            } else {
                load_gpr_rb(e, ra, insn.dst.reg, kA);
            }

            const bool is_logical = insn.opcode == Opcode::kAnd || insn.opcode == Opcode::kOr ||
                                    insn.opcode == Opcode::kXor;
            const bool is_sub = insn.opcode == Opcode::kSub || insn.opcode == Opcode::kCmp;
            const std::uint8_t mask =
                static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);

            if (is_logical) {
                switch (insn.opcode) {
                case Opcode::kAnd:
                    e.and_(w, kR, kA, kB);
                    break;
                case Opcode::kOr:
                    e.orr(w, kR, kA, kB);
                    break;
                default:
                    e.eor(w, kR, kA, kB);
                    break;
                }
                if (mask != 0) {
                    emit_flags_logical(e, w, mask, kR);
                }
            } else {
                if (mask & kFbAf) {
                    e.eor(a64::Width::k64, kT2, kA, kB);
                }
                if (is_sub) {
                    e.subs(w, kR, kA, kB);
                } else {
                    e.adds(w, kR, kA, kB);
                }
                if (mask != 0) {
                    if (mask & kFbAf) {
                        e.eor(a64::Width::k64, kT2, kT2, kR);
                    }
                    emit_flags_from_nzcv(e, is_sub, mask, kT2, kR);
                }
            }

            // CMP writes nothing back; every other op stores the result. The
            // effective address is recomputed because the load helper call
            // clobbered the scratch registers that held it.
            if (insn.opcode != Opcode::kCmp) {
                emit_effective_address(e, mem, ra, 0);
                e.mov_reg(a64::Width::k64, 1, kR);
                e.mov_imm64(2, bytes);
                e.mov_imm64(16, reinterpret_cast<std::uint64_t>(&hyperion_store));
                emit_helper_call(e, 16, 0, fault);
            }

            e.b(done);
            e.bind(fault);
            emit_set_rip(e, insn.address); // stop at the faulting instruction
            emit_return(e, BlockResult::kMemoryFault, ra);
            e.bind(done);

            if (write_rip) {
                emit_advance_rip(e, insn);
            }
            return TranslateError::kNone;
        }

        // Operate directly on the allocated host registers when both operands
        // are allocated; kA/kB/kR are used only as fallbacks. This is what turns
        // `add rdx, rax` into one `adds x, x, x` instead of load/load/add/store.
        const a64::Reg A =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kA;
        if (A == kA) {
            load_gpr(e, insn.dst.reg, kA);
        }
        const a64::Reg B = (ra != nullptr && insn.has_src &&
                            insn.src.kind == OperandKind::kRegister && ra->mapped(insn.src.reg))
                               ? ra->host(insn.src.reg)
                               : kB;
        // CMP is not a write, so its "result" is only needed for the flags.
        const bool writes_dst = insn.opcode != Opcode::kCmp;
        const a64::Reg R =
            (ra != nullptr && writes_dst && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;

        const bool is_logical = insn.opcode == Opcode::kAnd || insn.opcode == Opcode::kOr ||
                                insn.opcode == Opcode::kXor;
        const bool is_sub = insn.opcode == Opcode::kSub || insn.opcode == Opcode::kCmp;

        // Only flags that are written *and* read before being overwritten are
        // worth computing. An empty mask means no flag is live, so the whole
        // flag prologue disappears.
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);

        // An immediate source that fits the AArch64 ADD/SUB immediate form is
        // folded into the operation instead of being materialised into kB first
        // (mov_imm64 is 1-4 instructions) - but only when AF is not live. AF is
        // derived from (a ^ b ^ r), which needs b in a register, so when AF is
        // needed we keep the register form and stay bit-exact rather than
        // trading correctness for size.
        std::uint32_t imm12 = 0;
        bool shift12 = false;
        const bool imm_foldable =
            insn.has_src && insn.src.kind == OperandKind::kImmediate &&
            (insn.opcode == Opcode::kAdd || insn.opcode == Opcode::kSub ||
             insn.opcode == Opcode::kCmp) &&
            encode_imm12((bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm, imm12,
                         shift12);
        const bool imm_folded = imm_foldable && (mask & kFbAf) == 0;

        if (!imm_folded && B == kB) {
            const TranslateError err = load_source(e, insn.src, kB, bits, ra);
            if (err != TranslateError::kNone) {
                return err;
            }
        }

        if (is_logical) {
            switch (insn.opcode) {
            case Opcode::kAnd:
                e.and_(w, R, A, B);
                break;
            case Opcode::kOr:
                e.orr(w, R, A, B);
                break;
            default:
                e.eor(w, R, A, B);
                break;
            }
            if (mask != 0) {
                emit_flags_logical(e, w, mask, R);
            }
        } else {
            if (mask & kFbAf) {
                // t2 = a ^ b, completed to (a ^ b ^ r) after the op for AF.
                e.eor(a64::Width::k64, kT2, A, B);
            }
            if (imm_folded) {
                if (is_sub) {
                    e.subs_imm(w, R, A, imm12);
                } else {
                    e.adds_imm(w, R, A, imm12);
                }
            } else if (is_sub) {
                e.subs(w, R, A, B);
            } else {
                e.adds(w, R, A, B);
            }
            if (mask != 0) {
                if (mask & kFbAf) {
                    e.eor(a64::Width::k64, kT2, kT2, R);
                }
                emit_flags_from_nzcv(e, is_sub, mask, kT2, R);
            }
        }

        if (writes_dst && R == kR) {
            // The result landed in the scratch register, so it must be written
            // back. When R is the destination's own host register it is already
            // in place.
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kInc:
    case Opcode::kDec: {
        // A memory operand makes INC/DEC a read-modify-write through the
        // soft-MMU helper, exactly as the ALU memory forms above.
        if (insn.dst.kind == OperandKind::kMemory) {
            const std::size_t bytes = static_cast<std::size_t>(bits) / 8;
            const a64::Label fault = e.new_label();
            const a64::Label done = e.new_label();
            const bool is_sub_mem = insn.opcode == Opcode::kDec;
            const std::uint8_t mask_mem =
                static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);

            emit_effective_address(e, insn.dst.mem, ra, 0);
            e.mov_imm64(1, bytes);
            e.mov_imm64(16, reinterpret_cast<std::uint64_t>(&hyperion_load));
            emit_helper_call(e, 16, 1, fault); // value -> x0, fault -> x1
            e.mov_reg(a64::Width::k64, kA, 0);
            e.movz(a64::Width::k32, kB, 1);
            if (mask_mem & kFbAf) {
                e.eor(a64::Width::k64, kT2, kA, kB);
            }
            if (is_sub_mem) {
                e.subs(w, kR, kA, kB);
            } else {
                e.adds(w, kR, kA, kB);
            }
            if (mask_mem != 0) {
                if (mask_mem & kFbAf) {
                    e.eor(a64::Width::k64, kT2, kT2, kR);
                }
                emit_flags_from_nzcv(e, is_sub_mem, mask_mem, kT2, kR);
            }

            emit_effective_address(e, insn.dst.mem, ra, 0);
            e.mov_reg(a64::Width::k64, 1, kR);
            e.mov_imm64(2, bytes);
            e.mov_imm64(16, reinterpret_cast<std::uint64_t>(&hyperion_store));
            emit_helper_call(e, 16, 0, fault);

            e.b(done);
            e.bind(fault);
            emit_set_rip(e, insn.address); // stop at the faulting instruction
            emit_return(e, BlockResult::kMemoryFault, ra);
            e.bind(done);

            if (write_rip) {
                emit_advance_rip(e, insn);
            }
            return TranslateError::kNone;
        }

        // INC/DEC are ADD/SUB of 1, except that CF is preserved. flags_written()
        // excludes CF for these, so CF is never stored and its old value stands;
        // no explicit save/restore is needed. As with the ALU ops, operate
        // directly on the allocated host register when there is one.
        const a64::Reg A =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kA;
        if (A == kA) {
            load_gpr(e, insn.dst.reg, kA);
        }
        e.movz(a64::Width::k32, kB, 1);
        const bool is_sub = insn.opcode == Opcode::kDec;
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);
        const a64::Reg R =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;
        if (mask & kFbAf) {
            e.eor(a64::Width::k64, kT2, A, kB);
        }
        if (is_sub) {
            e.subs(w, R, A, kB);
        } else {
            e.adds(w, R, A, kB);
        }
        if (mask != 0) {
            if (mask & kFbAf) {
                e.eor(a64::Width::k64, kT2, kT2, R);
            }
            emit_flags_from_nzcv(e, is_sub, mask, kT2, R);
        }
        if (R == kR) {
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kJmp:
    case Opcode::kJcc:
    case Opcode::kPush:
    case Opcode::kPop:
    case Opcode::kRet:
        return TranslateError::kUnsupportedOpcode;
    }
    return TranslateError::kInternal;
}

// Whether the block translator may include this instruction. Control flow is
// handled by the block driver; PUSH/POP need a guest stack (Phase 6) and are
// cut, so the dispatcher stops at them rather than miscompiling.
bool is_block_body_opcode(Opcode op) {
    switch (op) {
    case Opcode::kMov:
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kInc:
    case Opcode::kDec:
    case Opcode::kNop:
        return true;
    default:
        return false;
    }
}

// Validate the operand shape of a body instruction, matching translate_instruction.
TranslateError validate_body(const Instruction& insn) {
    if (insn.dst.size != OperandSize::k32 && insn.dst.size != OperandSize::k64) {
        return TranslateError::kUnsupportedSize;
    }
    if (insn.dst.kind != OperandKind::kRegister && insn.dst.kind != OperandKind::kMemory) {
        return TranslateError::kInternal;
    }
    // A memory operand is accepted for every body opcode: MOV already lowered to
    // the soft-MMU helpers, and the ALU read-modify-write and INC/DEC forms now
    // do too (load, operate, store back; D-032). The decoder never produces a
    // form with two memory operands, and the emit paths above handle each shape
    // that reaches them; anything else is still refused rather than
    // mistranslated as if the memory operand were a register.
    if (insn.has_src && insn.src.kind != OperandKind::kRegister &&
        insn.src.kind != OperandKind::kImmediate && insn.src.kind != OperandKind::kMemory) {
        return TranslateError::kInternal;
    }
    if (insn.dst.kind == OperandKind::kMemory && insn.has_src &&
        insn.src.kind == OperandKind::kMemory) {
        return TranslateError::kUnsupportedOperand; // two memory operands
    }
    return TranslateError::kNone;
}

} // namespace

const char* to_string(TranslateError error) {
    switch (error) {
    case TranslateError::kNone:
        return "none";
    case TranslateError::kUnsupportedOpcode:
        return "unsupported opcode";
    case TranslateError::kUnsupportedOperand:
        return "unsupported operand";
    case TranslateError::kUnsupportedSize:
        return "unsupported operand size";
    case TranslateError::kInternal:
        return "internal error";
    }
    return "?";
}

Translation translate_instruction(const Instruction& insn) {
    Translation out;
    a64::Encoder e;
    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*

    // Control flow is not meaningful for a lone instruction: a standalone block
    // cannot branch anywhere. Refuse it here; translate_block() handles it.
    if (insn.opcode == Opcode::kJmp || insn.opcode == Opcode::kJcc || insn.opcode == Opcode::kRet ||
        insn.opcode == Opcode::kPush || insn.opcode == Opcode::kPop) {
        out.error = TranslateError::kUnsupportedOpcode;
        return out;
    }

    if (insn.opcode != Opcode::kNop && insn.opcode != Opcode::kHlt) {
        const TranslateError err = validate_body(insn);
        if (err != TranslateError::kNone) {
            out.error = err;
            return out;
        }
    }

    const TranslateError err = emit_one(e, insn, /*write_rip=*/true, kFbAll);
    if (err != TranslateError::kNone) {
        out.error = err;
        return out;
    }

    if (insn.opcode != Opcode::kHlt) {
        emit_return(e, BlockResult::kOk);
    }

    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

BlockTranslation translate_block(const std::uint8_t* code, std::size_t size,
                                 const Instruction& first) {
    BlockTranslation out;

    // Pass 1: decode the block into a straight-line list of instructions. This
    // has to happen before emission so flag liveness can be computed backward
    // over the whole block (pass 2); the emitter (pass 3) then knows, for each
    // instruction, which flags are actually read before being overwritten.
    struct Step {
        Instruction insn;
        std::uint8_t live_flags = 0;
    };
    std::vector<Step> steps;

    // `code`/`size` start at `first.address`, so an instruction at guest address
    // A sits at byte offset A - first.address.
    Instruction insn = first;
    std::size_t next_offset = first.length;
    BlockExit exit = BlockExit::kFallThrough;
    std::uint64_t target = 0;

    while (true) {
        const Opcode op = insn.opcode;

        if (op == Opcode::kHlt || op == Opcode::kRet) {
            // Both stop the program. RET is modelled by the interpreter as a
            // top-level return (there is no guest stack yet, Phase 6).
            steps.push_back({insn});
            exit = BlockExit::kHalted;
            break;
        }
        if (op == Opcode::kSyscall) {
            // End the block at the syscall so the dispatcher can run the
            // handler. rip is advanced past the instruction, so a resume (or the
            // next block lookup) continues correctly.
            steps.push_back({insn});
            exit = BlockExit::kSyscall;
            target = insn.address + insn.length;
            break;
        }
        if (op == Opcode::kJmp) {
            steps.push_back({insn});
            exit = BlockExit::kJmp;
            target = insn.dst.imm;
            break;
        }
        if (op == Opcode::kJcc) {
            steps.push_back({insn});
            exit = BlockExit::kJcc;
            target = insn.address + insn.length; // hint only; rip is authoritative
            break;
        }

        // Not a terminator. If we can translate it, add it and continue.
        // Otherwise cut the block *before* it: the dispatcher then sees the
        // unsupported PC on the next lookup and stops, instead of running an
        // instruction we did not translate.
        //
        // The cut is only valid once the block covers at least one instruction.
        // If the *first* instruction is unsupported there is nothing to cut
        // back to, and an empty block would fall through to the same PC and loop
        // forever, so we report the error and let the caller stop.
        if (!is_block_body_opcode(op) || validate_body(insn) != TranslateError::kNone) {
            if (steps.empty()) {
                out.error = TranslateError::kUnsupportedOpcode;
                return out;
            }
            exit = BlockExit::kFallThrough;
            target = insn.address;
            break;
        }
        steps.push_back({insn});

        if (next_offset >= size) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        const DecodeResult next =
            decode(code + next_offset, size - next_offset, first.address + next_offset);
        if (!next.ok()) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        insn = next.instruction;
        next_offset += insn.length;
    }

    // Pass 1.5: choose which guest registers to keep in host registers for this
    // block. Without allocation every source reference emits one `ldr` from
    // GuestState and every write emits one `str`; with allocation all of those
    // disappear from the body, at the cost of one `ldr` (prologue) and one `str`
    // (epilogue) per allocated register plus the save/restore of the borrowed
    // host registers. A register is worth a slot only when its body references
    // outnumber that overhead; see alloc_cost().
    std::size_t refs[kGprCount] = {};
    for (const Step& step : steps) {
        bool r[kGprCount] = {};
        bool w[kGprCount] = {};
        regs_read_write(step.insn, r, w);
        for (unsigned i = 0; i < kGprCount; ++i) {
            refs[i] += static_cast<std::size_t>((r[i] ? 1 : 0) + (w[i] ? 1 : 0));
        }
    }

    RegAlloc ra;
    // Take the registers with the largest saving first and stop as soon as the
    // marginal register no longer covers the step increase in overhead, so a
    // block is allocated only when it nets positive.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < kGprCount; ++i) {
        if (refs[i] > 0) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return refs[a] != refs[b] ? refs[a] > refs[b] : a < b;
    });
    std::size_t total_saving = 0;
    std::size_t nalloc = 0;
    while (nalloc < order.size() && nalloc < kMaxAllocated) {
        const std::size_t next = nalloc + 1;
        const std::size_t gain = refs[order[nalloc]];
        if (total_saving + gain < alloc_cost(next)) {
            break;
        }
        total_saving += gain;
        ++nalloc;
    }
    for (std::size_t i = 0; i < nalloc; ++i) {
        const a64::Reg h = static_cast<a64::Reg>(kAllocBase + i);
        ra.host_of_guest[order[i]] = static_cast<std::int8_t>(h);
        ra.allocated.push_back(h);
    }

    // Pass 2: backward flag liveness. Flags live in GuestState and persist
    // across blocks, so at a block exit every flag is conservatively live: a
    // successor block may read any of them, and there is no cross-block
    // analysis. (This is sound because the invariant "all flags are correct at
    // block entry" then holds by induction from program start, where both tiers
    // begin with the same zeroed flags.)
    //
    // live_out = flags that must be correct just after this instruction.
    // live_in  = reads | (live_out & ~writes) = flags that must be correct on
    //            entry. A flag the instruction writes is *computed* only if it
    //            is also live_out; a flag it does not write is merely preserved
    //            (never stored), which is exactly how INC/DEC keep CF.
    std::uint8_t live_out = kFbAll;
    for (std::size_t i = steps.size(); i-- > 0;) {
        const Instruction& in = steps[i].insn;
        const std::uint8_t reads =
            (in.opcode == Opcode::kJcc) ? condition_flags(in.condition) : std::uint8_t{0};
        const std::uint8_t writes = flags_written(in.opcode);
        steps[i].live_flags = static_cast<std::uint8_t>(writes & live_out);
        live_out = static_cast<std::uint8_t>(reads | (live_out & ~writes));
    }

    // Pass 3: emit. Body instructions do not write rip: nothing inside a block
    // reads it, so the per-instruction store is dead. The block writes rip once,
    // at its exit. See BENCHMARKS.md §4.2.2.
    a64::Encoder e;
    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*
    // Save the callee-saved host registers this block borrows and load the
    // allocated guest registers into them. Empty (no code) when nothing is
    // allocated, which keeps the previous lowering byte-for-byte.
    emit_prologue(e, ra);

    for (const Step& step : steps) {
        const Opcode op = step.insn.opcode;
        if (op == Opcode::kHlt || op == Opcode::kRet) {
            // The interpreter leaves rip on the HLT/RET instruction (a real CPU
            // faults on HLT with rip at the instruction). Body instructions do
            // not write rip, so it must be set explicitly here or the block's
            // start address would leak out as the halted rip.
            emit_set_rip(e, step.insn.address);
            emit_return(e, BlockResult::kHalted, &ra);
            break;
        }
        if (op == Opcode::kSyscall) {
            emit_set_rip(e, step.insn.address + step.insn.length);
            emit_return(e, BlockResult::kSyscall, &ra);
            break;
        }
        if (op == Opcode::kJmp) {
            emit_set_rip(e, step.insn.dst.imm);
            emit_return(e, BlockResult::kOk, &ra);
            break;
        }
        if (op == Opcode::kJcc) {
            // Evaluate the x86 condition and write the correct next rip on both
            // paths, so the dispatcher only has to read rip back.
            const std::uint64_t fallthrough = step.insn.address + step.insn.length;
            const a64::Label taken = e.new_label();
            const a64::Label done = e.new_label();
            emit_cond_branch(e, step.insn.condition, taken);
            emit_set_rip(e, fallthrough);
            e.b(done);
            e.bind(taken);
            emit_set_rip(e, step.insn.dst.imm);
            e.bind(done);
            emit_return(e, BlockResult::kOk, &ra);
            break;
        }
        const TranslateError emit_err =
            emit_one(e, step.insn, /*write_rip=*/false, step.live_flags, &ra);
        if (emit_err != TranslateError::kNone) {
            // validate_body() already rejected every operand shape emit_one()
            // cannot handle, so reaching here is an internal inconsistency.
            out.error = emit_err;
            return out;
        }
    }

    if (exit == BlockExit::kFallThrough) {
        emit_set_rip(e, target);
        emit_return(e, BlockResult::kOk, &ra);
    }

    out.exit = exit;
    out.target = target;
    out.guest_instruction_count = steps.size();
    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

} // namespace hyperion
