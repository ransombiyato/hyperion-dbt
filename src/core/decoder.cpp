// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/decoder.hpp"

#include <array>
#include <cstdio>

namespace hyperion {

namespace {

constexpr std::size_t kMaxInstructionLength = 15;

class Reader {
public:
    Reader(const std::uint8_t* code, std::size_t size) : code_(code), size_(size) {}

    bool has(std::size_t n = 1) const { return pos_ + n <= size_; }

    std::uint8_t u8() {
        if (!has()) {
            truncated_ = true;
            return 0;
        }
        return code_[pos_++];
    }

    std::int64_t disp8() {
        const std::uint8_t raw = u8();
        return (raw & 0x80u) != 0 ? static_cast<std::int64_t>(raw) - 256
                                  : static_cast<std::int64_t>(raw);
    }

    std::uint16_t u16() {
        std::uint16_t v = 0;
        for (int i = 0; i < 2; ++i) {
            v = static_cast<std::uint16_t>(
                v | static_cast<std::uint16_t>(static_cast<unsigned>(u8()) << (8 * i)));
        }
        return v;
    }

    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }

    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::size_t position() const { return pos_; }
    bool truncated() const { return truncated_; }

private:
    const std::uint8_t* code_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool truncated_ = false;
};

struct Rex {
    bool present = false;
    bool w = false;
    bool r = false;
    bool x = false;
    bool b = false;
};

struct Prefixes {
    bool op66 = false;
    bool addr32 = false;
    bool lock = false;
    bool fs = false;
    std::uint8_t rep = 0; // 0, 0xF2, or 0xF3 (last repeat prefix wins per Intel SDM)
};

struct Modrm {
    std::uint8_t mod = 0;
    std::uint8_t reg = 0;
    std::uint8_t rm = 0;
};

Modrm read_modrm(Reader& r) {
    const std::uint8_t b = r.u8();
    return Modrm{
        static_cast<std::uint8_t>(b >> 6),
        static_cast<std::uint8_t>((b >> 3) & 0x7),
        static_cast<std::uint8_t>(b & 0x7),
    };
}

Gpr gpr_from(std::uint8_t index) {
    return static_cast<Gpr>(index & 0xF);
}

Operand make_gpr_operand(std::uint8_t raw_idx, const Rex& rex, OperandSize size) {
    Operand op;
    op.kind = OperandKind::kRegister;
    op.size = size;
    if (size == OperandSize::k8 && !rex.present && raw_idx >= 4 && raw_idx <= 7) {
        op.reg = gpr_from(static_cast<std::uint8_t>(raw_idx - 4));
        op.high_byte = true;
    } else {
        op.reg = gpr_from(raw_idx);
        op.high_byte = false;
    }
    return op;
}

Operand reg_operand(const Modrm& m, const Rex& rex, OperandSize size) {
    const auto idx = static_cast<std::uint8_t>(m.reg | (rex.r ? 8 : 0));
    return make_gpr_operand(idx, rex, size);
}

Operand xmm_reg_operand(const Modrm& m, const Rex& rex, OperandSize size = OperandSize::k128) {
    Operand op;
    op.kind = OperandKind::kXmm;
    op.size = size;
    op.xmm = static_cast<std::uint8_t>((m.reg | (rex.r ? 8 : 0)) & 0xF);
    return op;
}

bool decode_rm_operand(Reader& r, const Modrm& m, const Rex& rex, const Prefixes& pfx,
                       OperandSize size, Operand& out) {
    if (m.mod == 3) {
        const auto idx = static_cast<std::uint8_t>(m.rm | (rex.b ? 8 : 0));
        out = make_gpr_operand(idx, rex, size);
        return true;
    }

    MemoryOperand mem;
    mem.address_size = pfx.addr32 ? OperandSize::k32 : OperandSize::k64;
    mem.fs_relative = pfx.fs;
    bool sib_disp32 = false;

    if (m.rm == 4) {
        const std::uint8_t sib = r.u8();
        if (r.truncated()) {
            return false;
        }
        const auto scale_bits = static_cast<std::uint8_t>(sib >> 6);
        const auto index_bits = static_cast<std::uint8_t>((sib >> 3) & 0x7);
        const auto base_bits = static_cast<std::uint8_t>(sib & 0x7);

        static constexpr std::array<std::uint8_t, 4> kScale = {1, 2, 4, 8};
        mem.scale = kScale[scale_bits];

        const bool index_present = !(index_bits == 4 && !rex.x);
        if (index_present) {
            mem.has_index = true;
            mem.index = gpr_from(static_cast<std::uint8_t>(index_bits | (rex.x ? 8 : 0)));
        }

        if (base_bits == 5 && m.mod == 0) {
            sib_disp32 = true;
        } else {
            mem.has_base = true;
            mem.base = gpr_from(static_cast<std::uint8_t>(base_bits | (rex.b ? 8 : 0)));
        }
    } else if (m.rm == 5 && m.mod == 0) {
        mem.rip_relative = true;
    } else {
        mem.has_base = true;
        mem.base = gpr_from(static_cast<std::uint8_t>(m.rm | (rex.b ? 8 : 0)));
    }

    switch (m.mod) {
    case 0:
        if (sib_disp32 || mem.rip_relative) {
            mem.disp = r.i32();
        }
        break;
    case 1:
        mem.disp = r.disp8();
        break;
    case 2:
        mem.disp = r.i32();
        break;
    default:
        return false;
    }

    if (r.truncated()) {
        return false;
    }
    out.kind = OperandKind::kMemory;
    out.size = size;
    out.mem = mem;
    return true;
}

bool decode_xmm_rm_operand(Reader& r, const Modrm& m, const Rex& rex, const Prefixes& pfx,
                           OperandSize mem_size, Operand& out) {
    if (m.mod == 3) {
        out.kind = OperandKind::kXmm;
        out.size = OperandSize::k128;
        out.xmm = static_cast<std::uint8_t>((m.rm | (rex.b ? 8 : 0)) & 0xF);
        return true;
    }
    return decode_rm_operand(r, m, rex, pfx, mem_size, out);
}

bool read_sized_immediate(Reader& r, OperandSize size, Operand& out) {
    out.kind = OperandKind::kImmediate;
    out.size = size;
    switch (size) {
    case OperandSize::k8:
        out.imm = r.u8();
        break;
    case OperandSize::k16:
        out.imm = r.u16();
        break;
    case OperandSize::k32:
        out.imm = r.u32();
        break;
    case OperandSize::k64: {
        const std::int32_t v = r.i32();
        out.imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
        break;
    }
    case OperandSize::k128:
        return false;
    }
    return !r.truncated();
}

Opcode alu_group_opcode(std::uint8_t reg_field) {
    switch (reg_field & 0x7) {
    case 0:
        return Opcode::kAdd;
    case 1:
        return Opcode::kOr;
    case 2:
        return Opcode::kAdc;
    case 3:
        return Opcode::kSbb;
    case 4:
        return Opcode::kAnd;
    case 5:
        return Opcode::kSub;
    case 6:
        return Opcode::kXor;
    case 7:
        return Opcode::kCmp;
    default:
        return Opcode::kAdd;
    }
}

bool is_lockable_alu(Opcode op) {
    return op == Opcode::kAdd || op == Opcode::kAdc || op == Opcode::kSub || op == Opcode::kSbb ||
           op == Opcode::kAnd || op == Opcode::kOr || op == Opcode::kXor;
}

bool decode_shift_opcode(std::uint8_t ext, Opcode& out) {
    switch (ext & 0x7) {
    case 0:
        out = Opcode::kRol;
        return true;
    case 1:
        out = Opcode::kRor;
        return true;
    case 4:
    case 6: // SAL (4) and undocumented alias SHL (6) are identical on x86-64
        out = Opcode::kShl;
        return true;
    case 5:
        out = Opcode::kShr;
        return true;
    case 7:
        out = Opcode::kSar;
        return true;
    default:
        return false; // RCL/RCR (/2, /3)
    }
}

} // namespace

const char* to_string(DecodeError error) {
    switch (error) {
    case DecodeError::kNone:
        return "none";
    case DecodeError::kTruncated:
        return "truncated instruction";
    case DecodeError::kUnknownOpcode:
        return "unknown or unsupported opcode";
    case DecodeError::kUnsupportedPrefix:
        return "unsupported prefix";
    case DecodeError::kUnsupportedOperand:
        return "unsupported operand form";
    case DecodeError::kUnsupportedModrm:
        return "unsupported ModRM/SIB form";
    case DecodeError::kTooLong:
        return "instruction exceeds 15 bytes";
    }
    return "unknown decode error";
}

const char* to_string(Opcode opcode) {
    switch (opcode) {
    case Opcode::kMov:
        return "mov";
    case Opcode::kMovzx:
        return "movzx";
    case Opcode::kMovsx:
        return "movsx";
    case Opcode::kLea:
        return "lea";
    case Opcode::kAdd:
        return "add";
    case Opcode::kAdc:
        return "adc";
    case Opcode::kSub:
        return "sub";
    case Opcode::kSbb:
        return "sbb";
    case Opcode::kCmp:
        return "cmp";
    case Opcode::kTest:
        return "test";
    case Opcode::kAnd:
        return "and";
    case Opcode::kOr:
        return "or";
    case Opcode::kXor:
        return "xor";
    case Opcode::kInc:
        return "inc";
    case Opcode::kDec:
        return "dec";
    case Opcode::kNot:
        return "not";
    case Opcode::kNeg:
        return "neg";
    case Opcode::kShl:
        return "shl";
    case Opcode::kShr:
        return "shr";
    case Opcode::kSar:
        return "sar";
    case Opcode::kRol:
        return "rol";
    case Opcode::kRor:
        return "ror";
    case Opcode::kShld:
        return "shld";
    case Opcode::kShrd:
        return "shrd";
    case Opcode::kImul:
        return "imul";
    case Opcode::kMul:
        return "mul";
    case Opcode::kDiv:
        return "div";
    case Opcode::kIdiv:
        return "idiv";
    case Opcode::kCdqe:
        return "cdqe";
    case Opcode::kCqo:
        return "cqo";
    case Opcode::kSetcc:
        return "setcc";
    case Opcode::kCmovcc:
        return "cmovcc";
    case Opcode::kXchg:
        return "xchg";
    case Opcode::kCmpxchg:
        return "cmpxchg";
    case Opcode::kXadd:
        return "xadd";
    case Opcode::kBswap:
        return "bswap";
    case Opcode::kBsf:
        return "bsf";
    case Opcode::kBsr:
        return "bsr";
    case Opcode::kBt:
        return "bt";
    case Opcode::kBts:
        return "bts";
    case Opcode::kBtr:
        return "btr";
    case Opcode::kBtc:
        return "btc";
    case Opcode::kNop:
        return "nop";
    case Opcode::kCall:
        return "call";
    case Opcode::kRet:
        return "ret";
    case Opcode::kLeave:
        return "leave";
    case Opcode::kHlt:
        return "hlt";
    case Opcode::kJmp:
        return "jmp";
    case Opcode::kJcc:
        return "jcc";
    case Opcode::kPush:
        return "push";
    case Opcode::kPop:
        return "pop";
    case Opcode::kStringOp:
        return "string";
    case Opcode::kCld:
        return "cld";
    case Opcode::kCpuid:
        return "cpuid";
    case Opcode::kRdtsc:
        return "rdtsc";
    case Opcode::kSse:
        return "sse2";
    case Opcode::kSyscall:
        return "syscall";
    }
    return "???";
}

const char* to_string(Condition condition) {
    switch (condition) {
    case Condition::kO:
        return "o";
    case Condition::kNo:
        return "no";
    case Condition::kB:
        return "b";
    case Condition::kAe:
        return "ae";
    case Condition::kE:
        return "e";
    case Condition::kNe:
        return "ne";
    case Condition::kBe:
        return "be";
    case Condition::kA:
        return "a";
    case Condition::kS:
        return "s";
    case Condition::kNs:
        return "ns";
    case Condition::kP:
        return "p";
    case Condition::kNp:
        return "np";
    case Condition::kL:
        return "l";
    case Condition::kGe:
        return "ge";
    case Condition::kLe:
        return "le";
    case Condition::kG:
        return "g";
    }
    return "?";
}

namespace {

std::string operand_to_string(const Operand& op) {
    char buf[128];
    switch (op.kind) {
    case OperandKind::kRegister:
        return to_string(op.reg);
    case OperandKind::kXmm:
        std::snprintf(buf, sizeof(buf), "xmm%u", static_cast<unsigned>(op.xmm));
        return buf;
    case OperandKind::kImmediate:
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(op.imm));
        return buf;
    case OperandKind::kMemory: {
        std::string s = op.mem.fs_relative ? "fs:[" : "[";
        bool need_plus = false;
        if (op.mem.rip_relative) {
            s += "rip";
            need_plus = true;
        } else if (op.mem.has_base) {
            s += to_string(op.mem.base);
            need_plus = true;
        }
        if (op.mem.has_index) {
            if (need_plus) {
                s += "+";
            }
            s += to_string(op.mem.index);
            if (op.mem.scale != 1) {
                std::snprintf(buf, sizeof(buf), "*%u", static_cast<unsigned>(op.mem.scale));
                s += buf;
            }
            need_plus = true;
        }
        if (op.mem.disp != 0 || !need_plus) {
            std::snprintf(buf, sizeof(buf), "%s0x%llx", need_plus ? "+" : "",
                          static_cast<unsigned long long>(static_cast<std::uint64_t>(op.mem.disp)));
            s += buf;
        }
        s += "]";
        return s;
    }
    }
    return "?";
}

} // namespace

std::string to_string(const Instruction& instruction) {
    std::string out;
    if (instruction.opcode == Opcode::kJcc) {
        out = "j";
        out += to_string(instruction.condition);
    } else if (instruction.opcode == Opcode::kSetcc) {
        out = "set";
        out += to_string(instruction.condition);
    } else if (instruction.opcode == Opcode::kCmovcc) {
        out = "cmov";
        out += to_string(instruction.condition);
    } else {
        out = to_string(instruction.opcode);
    }

    if (instruction.opcode == Opcode::kNop || instruction.opcode == Opcode::kRet ||
        instruction.opcode == Opcode::kLeave || instruction.opcode == Opcode::kHlt ||
        instruction.opcode == Opcode::kSyscall || instruction.opcode == Opcode::kCpuid ||
        instruction.opcode == Opcode::kRdtsc || instruction.opcode == Opcode::kCdqe ||
        instruction.opcode == Opcode::kCqo || instruction.opcode == Opcode::kCld ||
        instruction.opcode == Opcode::kStringOp) {
        return out;
    }

    out += " ";
    out += operand_to_string(instruction.dst);
    if (instruction.has_src) {
        out += ", ";
        out += operand_to_string(instruction.src);
    }
    if (instruction.has_aux) {
        out += ", ";
        out += operand_to_string(instruction.aux);
    }
    return out;
}

DecodeResult decode(const std::uint8_t* code, std::size_t size, std::uint64_t address) {
    DecodeResult result;
    Instruction& insn = result.instruction;
    insn.address = address;

    Reader r(code, size);
    Rex rex;
    Prefixes pfx;

    // --- Legacy & REX prefixes ---------------------------------------------
    for (;;) {
        if (!r.has()) {
            result.error = DecodeError::kTruncated;
            return result;
        }
        if (r.position() >= kMaxInstructionLength) {
            result.error = DecodeError::kTooLong;
            return result;
        }
        const std::uint8_t b = code[r.position()];
        if (b >= 0x40 && b <= 0x4F) {
            r.u8();
            rex.present = true;
            rex.w = (b & 0x08) != 0;
            rex.r = (b & 0x04) != 0;
            rex.x = (b & 0x02) != 0;
            rex.b = (b & 0x01) != 0;
            continue;
        }
        if (b == 0x66) {
            r.u8();
            rex = {};
            pfx.op66 = true;
            continue;
        }
        if (b == 0x67) {
            r.u8();
            rex = {};
            pfx.addr32 = true;
            continue;
        }
        if (b == 0xF0) {
            r.u8();
            rex = {};
            pfx.lock = true;
            continue;
        }
        if (b == 0xF2 || b == 0xF3) {
            r.u8();
            rex = {};
            pfx.rep = b;
            continue;
        }
        if (b == 0x64) {
            r.u8();
            rex = {};
            pfx.fs = true;
            continue;
        }
        if (b == 0x2E || b == 0x3E) {
            // Architectural branch hint / CET NOTRACK prefix: benign in 64-bit mode.
            r.u8();
            rex = {};
            continue;
        }
        if (b == 0x26 || b == 0x36 || b == 0x65) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        break;
    }

    if (r.position() >= kMaxInstructionLength) {
        result.error = DecodeError::kTooLong;
        return result;
    }
    const std::uint8_t opcode = r.u8();
    if (r.truncated()) {
        result.error = DecodeError::kTruncated;
        return result;
    }

    const OperandSize word_size =
        rex.w ? OperandSize::k64 : (pfx.op66 ? OperandSize::k16 : OperandSize::k32);
    insn.rex_w = rex.w;
    insn.rep = pfx.rep;

    auto finish = [&](Opcode op) {
        insn.opcode = op;
        insn.length = static_cast<std::uint8_t>(r.position());
        if (r.truncated()) {
            result.error = DecodeError::kTruncated;
            return;
        }
        if (insn.length > kMaxInstructionLength) {
            result.error = DecodeError::kTooLong;
            return;
        }
        result.error = DecodeError::kNone;
    };

    auto reject_lock_unless_mem = [&](bool ok_op) -> bool {
        if (!pfx.lock) {
            return true;
        }
        if (!ok_op || insn.dst.kind != OperandKind::kMemory) {
            result.error = DecodeError::kUnsupportedPrefix;
            return false;
        }
        insn.lock = true;
        return true;
    };

    auto branch_target_rel = [&](std::int64_t rel) -> Operand {
        Operand target;
        target.kind = OperandKind::kImmediate;
        target.size = OperandSize::k64;
        const std::uint64_t next = address + static_cast<std::uint64_t>(r.position());
        std::uint64_t dst = static_cast<std::uint64_t>(static_cast<std::int64_t>(next) + rel);
        if (pfx.addr32) {
            dst &= 0xFFFFFFFFULL;
        }
        target.imm = dst;
        return target;
    };

    // --- 0x88..0x8B: MOV r/m, r and MOV r, r/m -----------------------------
    if (opcode >= 0x88 && opcode <= 0x8B) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const OperandSize sz = (opcode & 0x01) == 0 ? OperandSize::k8 : word_size;
        const Modrm m = read_modrm(r);
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        const Operand rg_op = reg_operand(m, rex, sz);
        if ((opcode & 0x02) == 0) {
            insn.dst = rm_op;
            insn.src = rg_op;
        } else {
            insn.dst = rg_op;
            insn.src = rm_op;
        }
        insn.has_src = true;
        finish(Opcode::kMov);
        return result;
    }

    // --- 0x86 / 0x87: XCHG r/m, r ------------------------------------------
    if (opcode == 0x86 || opcode == 0x87) {
        const OperandSize sz = (opcode == 0x86) ? OperandSize::k8 : word_size;
        const Modrm m = read_modrm(r);
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        insn.dst = rm_op;
        insn.src = reg_operand(m, rex, sz);
        insn.has_src = true;
        if (!reject_lock_unless_mem(true)) {
            return result;
        }
        finish(Opcode::kXchg);
        return result;
    }

    // --- 0xC6 /0, 0xC7 /0: MOV r/m, imm ------------------------------------
    if (opcode == 0xC6 || opcode == 0xC7) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const OperandSize sz = (opcode == 0xC6) ? OperandSize::k8 : word_size;
        const Modrm m = read_modrm(r);
        if (m.reg != 0) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnknownOpcode;
            return result;
        }
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        Operand imm;
        if (!read_sized_immediate(r, sz, imm)) {
            result.error = DecodeError::kTruncated;
            return result;
        }
        insn.dst = rm_op;
        insn.src = imm;
        insn.has_src = true;
        finish(Opcode::kMov);
        return result;
    }

    // --- Primary-map ALU + TEST (0x00..0x3D, 0x84, 0x85, 0xA8, 0xA9) -------
    {
        Opcode alu = Opcode::kAdd;
        bool is_modrm = false;
        bool is_acc_imm = false;
        bool is_byte = false;
        bool rm_is_dst = false;

        if (opcode <= 0x3D && (opcode & 0x07) <= 5) {
            const auto group = static_cast<std::uint8_t>((opcode >> 3) & 0x7);
            const auto sub = static_cast<std::uint8_t>(opcode & 0x7);
            if (sub <= 3) {
                alu = alu_group_opcode(group);
                is_modrm = true;
                is_byte = (sub & 1) == 0;
                rm_is_dst = (sub & 2) == 0;
            } else if (sub == 4 || sub == 5) {
                alu = alu_group_opcode(group);
                is_acc_imm = true;
                is_byte = (sub == 4);
            }
        } else if (opcode == 0x84 || opcode == 0x85) {
            alu = Opcode::kTest;
            is_modrm = true;
            is_byte = (opcode == 0x84);
            rm_is_dst = true;
        } else if (opcode == 0xA8 || opcode == 0xA9) {
            alu = Opcode::kTest;
            is_acc_imm = true;
            is_byte = (opcode == 0xA8);
        }

        if (is_acc_imm) {
            if (pfx.lock) {
                result.error = DecodeError::kUnsupportedPrefix;
                return result;
            }
            const OperandSize sz = is_byte ? OperandSize::k8 : word_size;
            insn.dst = make_gpr_operand(0, rex, sz); // AL/AX/EAX/RAX
            Operand imm;
            if (!read_sized_immediate(r, sz, imm)) {
                result.error = DecodeError::kTruncated;
                return result;
            }
            insn.src = imm;
            insn.has_src = true;
            finish(alu);
            return result;
        }

        if (is_modrm) {
            const OperandSize sz = is_byte ? OperandSize::k8 : word_size;
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            const Operand rg_op = reg_operand(m, rex, sz);
            if (rm_is_dst) {
                insn.dst = rm_op;
                insn.src = rg_op;
            } else {
                insn.dst = rg_op;
                insn.src = rm_op;
            }
            insn.has_src = true;
            if (!reject_lock_unless_mem(rm_is_dst && is_lockable_alu(alu))) {
                return result;
            }
            finish(alu);
            return result;
        }
    }

    // --- 0x80 / 0x81 / 0x83: ALU group 1 r/m, imm --------------------------
    if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        const OperandSize sz = (opcode == 0x80) ? OperandSize::k8 : word_size;
        const Modrm m = read_modrm(r);
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        Operand imm;
        imm.kind = OperandKind::kImmediate;
        imm.size = sz;
        if (opcode == 0x83) {
            imm.imm = static_cast<std::uint64_t>(r.disp8());
        } else if (!read_sized_immediate(r, sz, imm)) {
            result.error = DecodeError::kTruncated;
            return result;
        }
        const Opcode alu = alu_group_opcode(m.reg);
        insn.dst = rm_op;
        insn.src = imm;
        insn.has_src = true;
        if (!reject_lock_unless_mem(is_lockable_alu(alu))) {
            return result;
        }
        finish(alu);
        return result;
    }

    // --- 0x69 / 0x6B: 3-operand IMUL r, r/m, imm ---------------------------
    if (opcode == 0x69 || opcode == 0x6B) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const Modrm m = read_modrm(r);
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        Operand imm;
        imm.kind = OperandKind::kImmediate;
        imm.size = word_size;
        if (opcode == 0x6B) {
            imm.imm = static_cast<std::uint64_t>(r.disp8());
        } else if (!read_sized_immediate(r, word_size, imm)) {
            result.error = DecodeError::kTruncated;
            return result;
        }
        insn.dst = reg_operand(m, rex, word_size);
        insn.src = rm_op;
        insn.aux = imm;
        insn.has_src = true;
        insn.has_aux = true;
        finish(Opcode::kImul);
        return result;
    }

    // --- 0x63: MOVSXD r32/r64, r/m32 ---------------------------------------
    if (opcode == 0x63) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const Modrm m = read_modrm(r);
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k32, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        insn.dst = reg_operand(m, rex, word_size);
        insn.src = rm_op;
        insn.has_src = true;
        finish(Opcode::kMovsx);
        return result;
    }

    // --- 0x8D: LEA r16/r32/r64, m ------------------------------------------
    if (opcode == 0x8D) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const Modrm m = read_modrm(r);
        if (m.mod == 3) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        insn.dst = reg_operand(m, rex, word_size);
        insn.src = rm_op;
        insn.has_src = true;
        finish(Opcode::kLea);
        return result;
    }

    // --- 0xB0..0xBF: MOV reg, imm ------------------------------------------
    if (opcode >= 0xB0 && opcode <= 0xB7) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const auto reg_idx = static_cast<std::uint8_t>((opcode - 0xB0) | (rex.b ? 8 : 0));
        insn.dst = make_gpr_operand(reg_idx, rex, OperandSize::k8);
        insn.src.kind = OperandKind::kImmediate;
        insn.src.size = OperandSize::k8;
        insn.src.imm = r.u8();
        insn.has_src = true;
        finish(Opcode::kMov);
        return result;
    }
    if (opcode >= 0xB8 && opcode <= 0xBF) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const auto reg_idx = static_cast<std::uint8_t>((opcode - 0xB8) | (rex.b ? 8 : 0));
        insn.dst = make_gpr_operand(reg_idx, rex, word_size);
        insn.src.kind = OperandKind::kImmediate;
        insn.src.size = word_size;
        if (rex.w) {
            insn.src.imm = r.u64();
        } else if (pfx.op66) {
            insn.src.imm = r.u16();
        } else {
            insn.src.imm = r.u32();
        }
        insn.has_src = true;
        finish(Opcode::kMov);
        return result;
    }

    // --- 0xC0/0xC1, 0xD0..0xD3: Shift / Rotate group 2 ---------------------
    if (opcode == 0xC0 || opcode == 0xC1 || (opcode >= 0xD0 && opcode <= 0xD3)) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const OperandSize sz = (opcode & 0x01) == 0 ? OperandSize::k8 : word_size;
        const Modrm m = read_modrm(r);
        Opcode sh_op = Opcode::kShl;
        if (!decode_shift_opcode(m.reg, sh_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnknownOpcode;
            return result;
        }
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        Operand cnt;
        if (opcode == 0xC0 || opcode == 0xC1) {
            cnt.kind = OperandKind::kImmediate;
            cnt.size = OperandSize::k8;
            cnt.imm = r.u8();
        } else if (opcode == 0xD0 || opcode == 0xD1) {
            cnt.kind = OperandKind::kImmediate;
            cnt.size = OperandSize::k8;
            cnt.imm = 1;
        } else {
            cnt.kind = OperandKind::kRegister;
            cnt.size = OperandSize::k8;
            cnt.reg = Gpr::kRcx;
        }
        insn.dst = rm_op;
        insn.src = cnt;
        insn.has_src = true;
        finish(sh_op);
        return result;
    }

    // --- 0xF6 / 0xF7: Unary ALU group 3 (TEST / NOT / NEG / MUL / IMUL / DIV / IDIV)
    if (opcode == 0xF6 || opcode == 0xF7) {
        const OperandSize sz = (opcode == 0xF6) ? OperandSize::k8 : word_size;
        const Modrm m = read_modrm(r);
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        insn.dst = rm_op;
        switch (m.reg) {
        case 0:
        case 1: {
            if (pfx.lock) {
                result.error = DecodeError::kUnsupportedPrefix;
                return result;
            }
            Operand imm;
            if (!read_sized_immediate(r, sz, imm)) {
                result.error = DecodeError::kTruncated;
                return result;
            }
            insn.src = imm;
            insn.has_src = true;
            finish(Opcode::kTest);
            return result;
        }
        case 2:
            if (!reject_lock_unless_mem(true)) {
                return result;
            }
            finish(Opcode::kNot);
            return result;
        case 3:
            if (!reject_lock_unless_mem(true)) {
                return result;
            }
            finish(Opcode::kNeg);
            return result;
        case 4:
            if (pfx.lock) {
                result.error = DecodeError::kUnsupportedPrefix;
                return result;
            }
            finish(Opcode::kMul);
            return result;
        case 5:
            if (pfx.lock) {
                result.error = DecodeError::kUnsupportedPrefix;
                return result;
            }
            finish(Opcode::kImul);
            return result;
        case 6:
            if (pfx.lock) {
                result.error = DecodeError::kUnsupportedPrefix;
                return result;
            }
            finish(Opcode::kDiv);
            return result;
        case 7:
            if (pfx.lock) {
                result.error = DecodeError::kUnsupportedPrefix;
                return result;
            }
            finish(Opcode::kIdiv);
            return result;
        default:
            break;
        }
    }

    // --- 0xFE / 0xFF: Group 4 & Group 5 (INC / DEC / CALL / JMP / PUSH) ----
    if (opcode == 0xFE || opcode == 0xFF) {
        const OperandSize sz = (opcode == 0xFE) ? OperandSize::k8 : word_size;
        const Modrm m = read_modrm(r);
        if (m.reg == 0 || m.reg == 1) {
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            if (!reject_lock_unless_mem(true)) {
                return result;
            }
            finish(m.reg == 0 ? Opcode::kInc : Opcode::kDec);
            return result;
        }
        if (opcode == 0xFF && (m.reg == 2 || m.reg == 4 || m.reg == 6)) {
            if (pfx.lock) {
                result.error = DecodeError::kUnsupportedPrefix;
                return result;
            }
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k64, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            finish(m.reg == 2 ? Opcode::kCall : (m.reg == 4 ? Opcode::kJmp : Opcode::kPush));
            return result;
        }
        result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnknownOpcode;
        return result;
    }

    // --- 0x8F /0: POP r/m64 ------------------------------------------------
    if (opcode == 0x8F) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        const Modrm m = read_modrm(r);
        if (m.reg != 0) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnknownOpcode;
            return result;
        }
        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k64, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        insn.dst = rm_op;
        finish(Opcode::kPop);
        return result;
    }

    // --- 0x90..0x97: NOP / XCHG r, eax/rax ---------------------------------
    if (opcode >= 0x90 && opcode <= 0x97) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        if (opcode == 0x90 && !rex.b) {
            // Includes PAUSE (`F3 90`), 2-byte `66 90` (`xchg ax, ax`), and `48 90`.
            finish(Opcode::kNop);
            return result;
        }
        const auto reg_idx = static_cast<std::uint8_t>((opcode - 0x90) | (rex.b ? 8 : 0));
        insn.dst = make_gpr_operand(reg_idx, rex, word_size);
        insn.src = make_gpr_operand(0, rex, word_size);
        insn.has_src = true;
        finish(Opcode::kXchg);
        return result;
    }

    // --- 0x98 / 0x99: CBW/CWDE/CDQE and CWD/CDQ/CQO ------------------------
    if (opcode == 0x98) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        insn.dst.size = word_size;
        finish(Opcode::kCdqe);
        return result;
    }
    if (opcode == 0x99) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        insn.dst.size = word_size;
        finish(Opcode::kCqo);
        return result;
    }

    // --- String ops (A4..A7, AA..AF) & CLD/STD (FC/FD) ---------------------
    if (opcode == 0xA4 || opcode == 0xA5 || opcode == 0xA6 || opcode == 0xA7 ||
        (opcode >= 0xAA && opcode <= 0xAF)) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        insn.dst.size = (opcode & 0x01) == 0 ? OperandSize::k8 : word_size;
        insn.subop = opcode;
        finish(Opcode::kStringOp);
        return result;
    }
    if (opcode == 0xFC || opcode == 0xFD) {
        insn.subop = static_cast<std::uint8_t>(opcode == 0xFD ? 1 : 0);
        finish(Opcode::kCld);
        return result;
    }

    // --- CALL rel32, RET, LEAVE, HLT, JMP rel8/rel32, Jcc rel8 -------------
    if (opcode == 0xE8) {
        const std::int32_t rel = r.i32();
        insn.dst = branch_target_rel(rel);
        finish(Opcode::kCall);
        return result;
    }
    if (opcode == 0xC3) {
        insn.dst.kind = OperandKind::kImmediate;
        insn.dst.size = OperandSize::k16;
        insn.dst.imm = 0;
        finish(Opcode::kRet);
        return result;
    }
    if (opcode == 0xC2) {
        insn.dst.kind = OperandKind::kImmediate;
        insn.dst.size = OperandSize::k16;
        insn.dst.imm = r.u16();
        insn.has_src = false;
        finish(Opcode::kRet);
        return result;
    }
    if (opcode == 0xC9) {
        insn.dst.size = OperandSize::k64;
        finish(Opcode::kLeave);
        return result;
    }
    if (opcode == 0xF4) {
        finish(Opcode::kHlt);
        return result;
    }
    if (opcode == 0xEB) {
        const std::int64_t rel = r.disp8();
        insn.dst = branch_target_rel(rel);
        finish(Opcode::kJmp);
        return result;
    }
    if (opcode == 0xE9) {
        const std::int32_t rel = r.i32();
        insn.dst = branch_target_rel(rel);
        finish(Opcode::kJmp);
        return result;
    }
    if (opcode >= 0x70 && opcode <= 0x7F) {
        const std::int64_t rel = r.disp8();
        insn.dst = branch_target_rel(rel);
        insn.condition = static_cast<Condition>(opcode & 0x0F);
        finish(Opcode::kJcc);
        return result;
    }

    // --- Stack PUSH / POP --------------------------------------------------
    if (opcode >= 0x50 && opcode <= 0x57) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        Operand dst;
        dst.kind = OperandKind::kRegister;
        dst.size = OperandSize::k64;
        dst.reg = gpr_from(static_cast<std::uint8_t>((opcode - 0x50) | (rex.b ? 8 : 0)));
        insn.dst = dst;
        finish(Opcode::kPush);
        return result;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        Operand dst;
        dst.kind = OperandKind::kRegister;
        dst.size = OperandSize::k64;
        dst.reg = gpr_from(static_cast<std::uint8_t>((opcode - 0x58) | (rex.b ? 8 : 0)));
        insn.dst = dst;
        finish(Opcode::kPop);
        return result;
    }
    if (opcode == 0x6A || opcode == 0x68) {
        if (pfx.lock) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        Operand imm;
        imm.kind = OperandKind::kImmediate;
        imm.size = OperandSize::k64;
        imm.imm = (opcode == 0x6A) ? static_cast<std::uint64_t>(r.disp8())
                                   : static_cast<std::uint64_t>(static_cast<std::int64_t>(r.i32()));
        insn.dst = imm;
        finish(Opcode::kPush);
        return result;
    }

    if (opcode == 0x9B) {
        finish(Opcode::kNop);
        return result;
    }
    if (opcode >= 0xD8 && opcode <= 0xDF) {
        const std::uint8_t raw_m = r.u8();
        if (r.truncated()) {
            result.error = DecodeError::kTruncated;
            return result;
        }
        const Modrm m{static_cast<std::uint8_t>((raw_m >> 6) & 3u),
                      static_cast<std::uint8_t>((raw_m >> 3) & 7u),
                      static_cast<std::uint8_t>(raw_m & 7u)};
        if (m.mod != 3) {
            Operand mem_op;
            if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k64, mem_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = mem_op;
        }
        insn.aux.kind = OperandKind::kImmediate;
        insn.aux.size = OperandSize::k16;
        insn.aux.imm = (static_cast<std::uint64_t>(opcode) << 8) | raw_m;
        insn.has_aux = true;
        insn.sse_op = SseOp::kX87;
        finish(Opcode::kSse);
        return result;
    }

    // --- Two-byte 0x0F escape map ------------------------------------------
    if (opcode == 0x0F) {
        const std::uint8_t second = r.u8();
        if (r.truncated()) {
            result.error = DecodeError::kTruncated;
            return result;
        }

        if (second == 0x05) {
            finish(Opcode::kSyscall);
            return result;
        }
        if (second == 0x31) {
            finish(Opcode::kRdtsc);
            return result;
        }
        if (second == 0xA2) {
            finish(Opcode::kCpuid);
            return result;
        }
        // XGETBV (0F 01 D0)
        if (second == 0x01) {
            const std::uint8_t modrm_byte = r.u8();
            if (!r.truncated() && modrm_byte == 0xD0) {
                insn.sse_op = SseOp::kXgetbv;
                finish(Opcode::kSse);
                return result;
            }
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnknownOpcode;
            return result;
        }
        // Group 15 (0F AE): FXSAVE/FXRSTOR/LDMXCSR/STMXCSR/XSAVE/XRSTOR +
        // LFENCE/MFENCE/SFENCE/CLFLUSH
        if (second == 0xAE) {
            const Modrm m = read_modrm(r);
            if (m.mod == 3) {
                if (m.reg == 5 || m.reg == 6 || m.reg == 7) {
                    finish(Opcode::kNop);
                    return result;
                }
                result.error = DecodeError::kUnsupportedModrm;
                return result;
            }
            Operand mem_op;
            if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k64, mem_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            if (m.reg == 6 || m.reg == 7) {
                finish(Opcode::kNop);
                return result;
            }
            insn.dst = mem_op;
            switch (m.reg) {
            case 0:
            case 4:
                insn.sse_op = SseOp::kFxsave;
                finish(Opcode::kSse);
                return result;
            case 1:
            case 5:
                insn.sse_op = SseOp::kFxrstor;
                finish(Opcode::kSse);
                return result;
            case 2:
                insn.sse_op = SseOp::kLdmxcsr;
                finish(Opcode::kSse);
                return result;
            case 3:
                insn.sse_op = SseOp::kStmxcsr;
                finish(Opcode::kSse);
                return result;
            default:
                result.error = DecodeError::kUnsupportedModrm;
                return result;
            }
        }
        // Group 9 (0F C7): XSAVEC /4 mem, RDRAND /6 reg, RDSEED /7 reg
        if (second == 0xC7) {
            const Modrm m = read_modrm(r);
            if (m.mod == 3 && (m.reg == 6 || m.reg == 7)) {
                Operand dst;
                if (!decode_rm_operand(r, m, rex, pfx, word_size, dst)) {
                    result.error = DecodeError::kUnsupportedModrm;
                    return result;
                }
                insn.dst = dst;
                insn.sse_op = SseOp::kRdrand;
                finish(Opcode::kSse);
                return result;
            }
            if (m.mod != 3 && m.reg == 4) {
                Operand mem_op;
                if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k64, mem_op)) {
                    result.error =
                        r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                    return result;
                }
                insn.dst = mem_op;
                insn.sse_op = SseOp::kFxsave;
                finish(Opcode::kSse);
                return result;
            }
            result.error = DecodeError::kUnsupportedModrm;
            return result;
        }
        // Multi-byte architectural NOP (`0F 1F /0`) & CET `ENDBR64` (`F3 0F 1E FA` / `0F 1E /r`)
        if (second == 0x1F || second == 0x1E) {
            const Modrm m = read_modrm(r);
            Operand dummy;
            if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k32, dummy)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            finish(Opcode::kNop);
            return result;
        }
        // Near Jcc rel32: 0F 80..8F
        if (second >= 0x80 && second <= 0x8F) {
            const std::int32_t rel = r.i32();
            insn.dst = branch_target_rel(rel);
            insn.condition = static_cast<Condition>(second & 0x0F);
            finish(Opcode::kJcc);
            return result;
        }
        // SETcc r/m8: 0F 90..9F
        if (second >= 0x90 && second <= 0x9F) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, OperandSize::k8, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            insn.condition = static_cast<Condition>(second & 0x0F);
            finish(Opcode::kSetcc);
            return result;
        }
        // CMOVcc r, r/m: 0F 40..4F
        if (second >= 0x40 && second <= 0x4F) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = reg_operand(m, rex, word_size);
            insn.src = rm_op;
            insn.has_src = true;
            insn.condition = static_cast<Condition>(second & 0x0F);
            finish(Opcode::kCmovcc);
            return result;
        }
        // MOVZX (0F B6, 0F B7) & MOVSX (0F BE, 0F BF)
        if (second == 0xB6 || second == 0xB7 || second == 0xBE || second == 0xBF) {
            const OperandSize src_sz =
                (second == 0xB6 || second == 0xBE) ? OperandSize::k8 : OperandSize::k16;
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, src_sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = reg_operand(m, rex, word_size);
            insn.src = rm_op;
            insn.has_src = true;
            finish((second == 0xB6 || second == 0xB7) ? Opcode::kMovzx : Opcode::kMovsx);
            return result;
        }
        // 2-operand IMUL r, r/m (0F AF)
        if (second == 0xAF) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = reg_operand(m, rex, word_size);
            insn.src = rm_op;
            insn.has_src = true;
            finish(Opcode::kImul);
            return result;
        }
        // BSWAP r32/r64: 0F C8..CF
        if (second >= 0xC8 && second <= 0xCF) {
            const auto reg_idx = static_cast<std::uint8_t>((second - 0xC8) | (rex.b ? 8 : 0));
            insn.dst = make_gpr_operand(reg_idx, rex, rex.w ? OperandSize::k64 : OperandSize::k32);
            finish(Opcode::kBswap);
            return result;
        }
        // BSF / TZCNT (0F BC) & BSR / LZCNT (0F BD)
        if (second == 0xBC || second == 0xBD) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = reg_operand(m, rex, word_size);
            insn.src = rm_op;
            insn.has_src = true;
            finish(second == 0xBC ? Opcode::kBsf : Opcode::kBsr);
            return result;
        }
        // CMPXCHG r/m, r (0F B0 / 0F B1) & XADD r/m, r (0F C0 / 0F C1)
        if (second == 0xB0 || second == 0xB1 || second == 0xC0 || second == 0xC1) {
            const OperandSize sz = (second & 0x01) == 0 ? OperandSize::k8 : word_size;
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            insn.src = reg_operand(m, rex, sz);
            insn.has_src = true;
            if (!reject_lock_unless_mem(true)) {
                return result;
            }
            finish((second == 0xB0 || second == 0xB1) ? Opcode::kCmpxchg : Opcode::kXadd);
            return result;
        }
        // SHLD (0F A4 imm8, 0F A5 cl) & SHRD (0F AC imm8, 0F AD cl)
        if (second == 0xA4 || second == 0xA5 || second == 0xAC || second == 0xAD) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            insn.src = reg_operand(m, rex, word_size);
            insn.has_src = true;
            insn.has_aux = true;
            if (second == 0xA4 || second == 0xAC) {
                insn.aux.kind = OperandKind::kImmediate;
                insn.aux.size = OperandSize::k8;
                insn.aux.imm = r.u8();
            } else {
                insn.aux.kind = OperandKind::kRegister;
                insn.aux.size = OperandSize::k8;
                insn.aux.reg = Gpr::kRcx;
            }
            finish((second == 0xA4 || second == 0xA5) ? Opcode::kShld : Opcode::kShrd);
            return result;
        }
        // BT / BTS / BTR / BTC register forms (0F A3, 0F AB, 0F B3, 0F BB) and 0F BA imm8
        if (second == 0xA3 || second == 0xAB || second == 0xB3 || second == 0xBB) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            insn.src = reg_operand(m, rex, word_size);
            insn.has_src = true;
            const Opcode btop = second == 0xA3   ? Opcode::kBt
                                : second == 0xAB ? Opcode::kBts
                                : second == 0xB3 ? Opcode::kBtr
                                                 : Opcode::kBtc;
            if (!reject_lock_unless_mem(btop != Opcode::kBt)) {
                return result;
            }
            finish(btop);
            return result;
        }
        if (second == 0xBA) {
            const Modrm m = read_modrm(r);
            if (m.reg < 4) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnknownOpcode;
                return result;
            }
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, word_size, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            insn.src.kind = OperandKind::kImmediate;
            insn.src.size = OperandSize::k8;
            insn.src.imm = r.u8();
            insn.has_src = true;
            const Opcode btop = m.reg == 4   ? Opcode::kBt
                                : m.reg == 5 ? Opcode::kBts
                                : m.reg == 6 ? Opcode::kBtr
                                             : Opcode::kBtc;
            if (!reject_lock_unless_mem(btop != Opcode::kBt)) {
                return result;
            }
            finish(btop);
            return result;
        }

        // --- Baseline SSE2 instructions ------------------------------------
        auto finish_sse = [&](SseOp sop) {
            insn.sse_op = sop;
            finish(Opcode::kSse);
        };

        // 128-bit XMM loads/stores/moves:
        //   0F 10/11 (MOVUPS/MOVUPD/MOVSS/MOVSD — Note: only full 128b when no F2/F3 or reg/mem)
        //   0F 28/29 (MOVAPS/MOVAPD)
        //   66/F3 0F 6F/7F (MOVDQA/MOVDQU)
        if ((second == 0x10 || second == 0x11) && pfx.rep == 0xF3) {
            // MOVSS (32-bit)
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k32, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            const Operand xreg = xmm_reg_operand(m, rex, OperandSize::k32);
            if (second == 0x10) {
                insn.dst = xreg;
                insn.src = rm_op;
                insn.has_src = true;
                finish_sse(m.mod == 3 ? SseOp::kMovSsReg : SseOp::kMovD);
                return result;
            }
            insn.dst = rm_op;
            insn.src = xreg;
            insn.has_src = true;
            finish_sse(SseOp::kMovD);
            return result;
        }
        if ((second == 0x10 || second == 0x11) && pfx.rep == 0xF2) {
            // MOVSD (64-bit; reg-to-reg preserves bits 127:64, load zeroes bits 127:64)
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k64, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            const Operand xreg = xmm_reg_operand(m, rex, OperandSize::k64);
            if (second == 0x10) {
                insn.dst = xreg;
                insn.src = rm_op;
                insn.has_src = true;
                finish_sse(m.mod == 3 ? SseOp::kMovLpd : SseOp::kMovQ);
                return result;
            }
            insn.dst = rm_op;
            insn.src = xreg;
            insn.has_src = true;
            finish_sse(SseOp::kMovQ);
            return result;
        }
        if (second == 0x10 || second == 0x11 || second == 0x28 || second == 0x29 ||
            ((second == 0x6F || second == 0x7F) && (pfx.op66 || pfx.rep == 0xF3))) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k128, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            const Operand xreg = xmm_reg_operand(m, rex);
            const bool store = (second == 0x11 || second == 0x29 || second == 0x7F);
            if (store) {
                insn.dst = rm_op;
                insn.src = xreg;
            } else {
                insn.dst = xreg;
                insn.src = rm_op;
            }
            insn.has_src = true;
            finish_sse(SseOp::kMovUps);
            return result;
        }

        // MOVLPS/MOVLPD (0F 12/13) and MOVHPS/MOVHPD (0F 16/17)
        if (second == 0x12 || second == 0x13 || second == 0x16 || second == 0x17) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k64, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            const Operand xreg = xmm_reg_operand(m, rex, OperandSize::k64);
            if (second == 0x13 || second == 0x17) {
                if (m.mod == 3) {
                    result.error = DecodeError::kUnsupportedModrm;
                    return result;
                }
                insn.dst = rm_op;
                insn.src = xreg;
                insn.has_src = true;
                finish_sse(second == 0x13 ? SseOp::kMovLpd : SseOp::kMovHpd);
                return result;
            }
            insn.dst = xreg;
            insn.src = rm_op;
            insn.has_src = true;
            if (m.mod == 3) {
                finish_sse(second == 0x12 ? SseOp::kMovHlps : SseOp::kMovLhps);
            } else {
                finish_sse(second == 0x12 ? SseOp::kMovLpd : SseOp::kMovHpd);
            }
            return result;
        }

        // 66 0F 6E (MOVD/MOVQ xmm, r/m32/64) & 66 0F 7E (MOVD/MOVQ r/m32/64, xmm)
        if (pfx.op66 && (second == 0x6E || second == 0x7E)) {
            const OperandSize gp_sz = rex.w ? OperandSize::k64 : OperandSize::k32;
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, gp_sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            const Operand xreg = xmm_reg_operand(m, rex, gp_sz);
            if (second == 0x6E) {
                insn.dst = xreg;
                insn.src = rm_op;
            } else {
                insn.dst = rm_op;
                insn.src = xreg;
            }
            insn.has_src = true;
            finish_sse(rex.w ? SseOp::kMovQ : SseOp::kMovD);
            return result;
        }
        // F3 0F 7E: MOVQ xmm, xmm/m64
        if (pfx.rep == 0xF3 && second == 0x7E) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k64, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = xmm_reg_operand(m, rex, OperandSize::k64);
            insn.src = rm_op;
            insn.has_src = true;
            finish_sse(SseOp::kMovQ);
            return result;
        }
        // 66 0F D6: MOVQ xmm/m64, xmm
        if (pfx.op66 && second == 0xD6) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k64, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = rm_op;
            insn.src = xmm_reg_operand(m, rex, OperandSize::k64);
            insn.has_src = true;
            finish_sse(SseOp::kMovQ);
            return result;
        }

        // PMOVMSKB r32/64, xmm (66 0F D7)
        if (pfx.op66 && second == 0xD7) {
            const Modrm m = read_modrm(r);
            if (m.mod != 3) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = reg_operand(m, rex, OperandSize::k32);
            insn.src.kind = OperandKind::kXmm;
            insn.src.size = OperandSize::k128;
            insn.src.xmm = static_cast<std::uint8_t>((m.rm | (rex.b ? 8 : 0)) & 0xF);
            insn.has_src = true;
            finish_sse(SseOp::kPmovmskb);
            return result;
        }

        // Immediate packed shifts: 66 0F 71 /2,/4,/6 ; 66 0F 72 /2,/4,/6 ; 66 0F 73 /2,/3,/6,/7
        if (pfx.op66 && (second == 0x71 || second == 0x72 || second == 0x73)) {
            const Modrm m = read_modrm(r);
            if (m.mod != 3) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            SseOp sop = SseOp::kNone;
            if (second == 0x71) {
                sop = m.reg == 2   ? SseOp::kPsrlwImm
                      : m.reg == 4 ? SseOp::kPsrawImm
                      : m.reg == 6 ? SseOp::kPsllwImm
                                   : SseOp::kNone;
            } else if (second == 0x72) {
                sop = m.reg == 2   ? SseOp::kPsrldImm
                      : m.reg == 4 ? SseOp::kPsradImm
                      : m.reg == 6 ? SseOp::kPslldImm
                                   : SseOp::kNone;
            } else {
                sop = m.reg == 2   ? SseOp::kPsrlqImm
                      : m.reg == 3 ? SseOp::kPsrldqImm
                      : m.reg == 6 ? SseOp::kPsllqImm
                      : m.reg == 7 ? SseOp::kPslldqImm
                                   : SseOp::kNone;
            }
            if (sop == SseOp::kNone) {
                result.error = DecodeError::kUnknownOpcode;
                return result;
            }
            insn.dst.kind = OperandKind::kXmm;
            insn.dst.size = OperandSize::k128;
            insn.dst.xmm = static_cast<std::uint8_t>((m.rm | (rex.b ? 8 : 0)) & 0xF);
            insn.src.kind = OperandKind::kImmediate;
            insn.src.size = OperandSize::k8;
            insn.src.imm = r.u8();
            insn.has_src = true;
            finish_sse(sop);
            return result;
        }

        // PSHUFD / PSHUFHW / PSHUFLW (66/F3/F2 0F 70 imm8)
        if (second == 0x70 && (pfx.op66 || pfx.rep == 0xF3 || pfx.rep == 0xF2)) {
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k128, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = xmm_reg_operand(m, rex);
            insn.src = rm_op;
            insn.aux.kind = OperandKind::kImmediate;
            insn.aux.size = OperandSize::k8;
            insn.aux.imm = r.u8();
            insn.has_src = true;
            insn.has_aux = true;
            finish_sse(pfx.rep == 0xF3   ? SseOp::kPshufhw
                       : pfx.rep == 0xF2 ? SseOp::kPshuflw
                                         : SseOp::kPshufd);
            return result;
        }

        // 2-operand packed SSE2 ALU / compare / unpack / pack + scalar/packed FP:
        // Scalar/packed IEEE-754 FP conversions & comparisons first:
        if ((second == 0x2A) && (pfx.rep == 0xF3 || pfx.rep == 0xF2)) {
            const OperandSize gp_sz = rex.w ? OperandSize::k64 : OperandSize::k32;
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, pfx, gp_sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = xmm_reg_operand(m, rex);
            insn.src = rm_op;
            insn.has_src = true;
            finish_sse(pfx.rep == 0xF3 ? SseOp::kCvtsi2ss : SseOp::kCvtsi2sd);
            return result;
        }
        if ((second == 0x2C || second == 0x2D) && (pfx.rep == 0xF3 || pfx.rep == 0xF2)) {
            const OperandSize gp_sz = rex.w ? OperandSize::k64 : OperandSize::k32;
            const OperandSize mem_sz = (pfx.rep == 0xF3) ? OperandSize::k32 : OperandSize::k64;
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, mem_sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = reg_operand(m, rex, gp_sz);
            insn.src = rm_op;
            insn.has_src = true;
            if (second == 0x2C) {
                finish_sse(pfx.rep == 0xF3 ? SseOp::kCvttss2si : SseOp::kCvttsd2si);
            } else {
                finish_sse(pfx.rep == 0xF3 ? SseOp::kCvtss2si : SseOp::kCvtsd2si);
            }
            return result;
        }
        if ((second == 0x2E || second == 0x2F) && pfx.rep == 0) {
            const OperandSize mem_sz = pfx.op66 ? OperandSize::k64 : OperandSize::k32;
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, mem_sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = xmm_reg_operand(m, rex);
            insn.src = rm_op;
            insn.has_src = true;
            finish_sse(pfx.op66 ? SseOp::kUcomisd : SseOp::kUcomiss);
            return result;
        }
        if (second == 0x50 && pfx.rep == 0) {
            const Modrm m = read_modrm(r);
            if (m.mod != 3) {
                result.error = DecodeError::kUnsupportedModrm;
                return result;
            }
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k128, rm_op)) {
                result.error = DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = reg_operand(m, rex, rex.w ? OperandSize::k64 : OperandSize::k32);
            insn.src = rm_op;
            insn.has_src = true;
            finish_sse(pfx.op66 ? SseOp::kMovmskPd : SseOp::kMovmskPs);
            return result;
        }
        if (second == 0x51 || second == 0x58 || second == 0x59 || second == 0x5A ||
            second == 0x5B || second == 0x5C || second == 0x5D || second == 0x5E ||
            second == 0x5F || second == 0xE6) {
            SseOp fpop = SseOp::kNone;
            OperandSize mem_sz = OperandSize::k128;
            if (pfx.rep == 0xF3) {
                mem_sz = OperandSize::k32;
                switch (second) {
                case 0x51:
                    fpop = SseOp::kSqrtSs;
                    break;
                case 0x58:
                    fpop = SseOp::kAddSs;
                    break;
                case 0x59:
                    fpop = SseOp::kMulSs;
                    break;
                case 0x5A:
                    fpop = SseOp::kCvtss2sd;
                    break;
                case 0x5B:
                    fpop = SseOp::kCvttps2dq;
                    mem_sz = OperandSize::k128;
                    break;
                case 0x5C:
                    fpop = SseOp::kSubSs;
                    break;
                case 0x5D:
                    fpop = SseOp::kMinSs;
                    break;
                case 0x5E:
                    fpop = SseOp::kDivSs;
                    break;
                case 0x5F:
                    fpop = SseOp::kMaxSs;
                    break;
                case 0xE6:
                    fpop = SseOp::kCvtdq2pd;
                    mem_sz = OperandSize::k64;
                    break;
                default:
                    break;
                }
            } else if (pfx.rep == 0xF2) {
                mem_sz = OperandSize::k64;
                switch (second) {
                case 0x51:
                    fpop = SseOp::kSqrtSd;
                    break;
                case 0x58:
                    fpop = SseOp::kAddSd;
                    break;
                case 0x59:
                    fpop = SseOp::kMulSd;
                    break;
                case 0x5A:
                    fpop = SseOp::kCvtsd2ss;
                    break;
                case 0x5C:
                    fpop = SseOp::kSubSd;
                    break;
                case 0x5D:
                    fpop = SseOp::kMinSd;
                    break;
                case 0x5E:
                    fpop = SseOp::kDivSd;
                    break;
                case 0x5F:
                    fpop = SseOp::kMaxSd;
                    break;
                case 0xE6:
                    fpop = SseOp::kCvtpd2dq;
                    mem_sz = OperandSize::k128;
                    break;
                default:
                    break;
                }
            } else if (pfx.op66) {
                switch (second) {
                case 0x51:
                    fpop = SseOp::kSqrtPd;
                    break;
                case 0x58:
                    fpop = SseOp::kAddPd;
                    break;
                case 0x59:
                    fpop = SseOp::kMulPd;
                    break;
                case 0x5A:
                    fpop = SseOp::kCvtpd2ps;
                    break;
                case 0x5B:
                    fpop = SseOp::kCvtps2dq;
                    break;
                case 0x5C:
                    fpop = SseOp::kSubPd;
                    break;
                case 0x5D:
                    fpop = SseOp::kMinPd;
                    break;
                case 0x5E:
                    fpop = SseOp::kDivPd;
                    break;
                case 0x5F:
                    fpop = SseOp::kMaxPd;
                    break;
                case 0xE6:
                    fpop = SseOp::kCvttpd2dq;
                    break;
                default:
                    break;
                }
            } else {
                switch (second) {
                case 0x51:
                    fpop = SseOp::kSqrtPs;
                    break;
                case 0x58:
                    fpop = SseOp::kAddPs;
                    break;
                case 0x59:
                    fpop = SseOp::kMulPs;
                    break;
                case 0x5A:
                    fpop = SseOp::kCvtps2pd;
                    mem_sz = OperandSize::k64;
                    break;
                case 0x5B:
                    fpop = SseOp::kCvtdq2ps;
                    break;
                case 0x5C:
                    fpop = SseOp::kSubPs;
                    break;
                case 0x5D:
                    fpop = SseOp::kMinPs;
                    break;
                case 0x5E:
                    fpop = SseOp::kDivPs;
                    break;
                case 0x5F:
                    fpop = SseOp::kMaxPs;
                    break;
                default:
                    break;
                }
            }
            if (fpop != SseOp::kNone) {
                const Modrm m = read_modrm(r);
                Operand rm_op;
                if (!decode_xmm_rm_operand(r, m, rex, pfx, mem_sz, rm_op)) {
                    result.error =
                        r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                    return result;
                }
                insn.dst = xmm_reg_operand(m, rex, mem_sz);
                insn.src = rm_op;
                insn.has_src = true;
                finish_sse(fpop);
                return result;
            }
        }
        if (second == 0xC2 || (second == 0xC6 && pfx.rep == 0)) {
            OperandSize mem_sz = OperandSize::k128;
            SseOp fpop = SseOp::kNone;
            if (second == 0xC2) {
                if (pfx.rep == 0xF3) {
                    fpop = SseOp::kCmpSs;
                    mem_sz = OperandSize::k32;
                } else if (pfx.rep == 0xF2) {
                    fpop = SseOp::kCmpSd;
                    mem_sz = OperandSize::k64;
                } else if (pfx.op66) {
                    fpop = SseOp::kCmpPd;
                } else {
                    fpop = SseOp::kCmpPs;
                }
            } else {
                fpop = pfx.op66 ? SseOp::kShufPd : SseOp::kShufPs;
            }
            const Modrm m = read_modrm(r);
            Operand rm_op;
            if (!decode_xmm_rm_operand(r, m, rex, pfx, mem_sz, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            insn.dst = xmm_reg_operand(m, rex, mem_sz);
            insn.src = rm_op;
            insn.aux.kind = OperandKind::kImmediate;
            insn.aux.size = OperandSize::k8;
            insn.aux.imm = r.u8();
            insn.has_src = true;
            insn.has_aux = true;
            finish_sse(fpop);
            return result;
        }
        {
            SseOp sop = SseOp::kNone;
            if (second == 0x54 || (pfx.op66 && second == 0xDB)) {
                sop = SseOp::kPand;
            } else if (second == 0x55 || (pfx.op66 && second == 0xDF)) {
                sop = SseOp::kPandn;
            } else if (second == 0x56 || (pfx.op66 && second == 0xEB)) {
                sop = SseOp::kPor;
            } else if (second == 0x57 || (pfx.op66 && second == 0xEF)) {
                sop = SseOp::kPxor;
            } else if (!pfx.op66 && pfx.rep == 0 && (second == 0x14 || second == 0x15)) {
                sop = (second == 0x14) ? SseOp::kPunpckldq : SseOp::kPunpckhdq;
            } else if (pfx.op66) {
                switch (second) {
                case 0x14:
                    sop = SseOp::kPunpcklqdq;
                    break;
                case 0x15:
                    sop = SseOp::kPunpckhqdq;
                    break;
                case 0x60:
                    sop = SseOp::kPunpcklbw;
                    break;
                case 0x61:
                    sop = SseOp::kPunpcklwd;
                    break;
                case 0x62:
                    sop = SseOp::kPunpckldq;
                    break;
                case 0x63:
                    sop = SseOp::kPacksswb;
                    break;
                case 0x64:
                    sop = SseOp::kPcmpgtb;
                    break;
                case 0x65:
                    sop = SseOp::kPcmpgtw;
                    break;
                case 0x66:
                    sop = SseOp::kPcmpgtd;
                    break;
                case 0x67:
                    sop = SseOp::kPackuswb;
                    break;
                case 0x68:
                    sop = SseOp::kPunpckhbw;
                    break;
                case 0x69:
                    sop = SseOp::kPunpckhwd;
                    break;
                case 0x6A:
                    sop = SseOp::kPunpckhdq;
                    break;
                case 0x6B:
                    sop = SseOp::kPackssdw;
                    break;
                case 0x6C:
                    sop = SseOp::kPunpcklqdq;
                    break;
                case 0x6D:
                    sop = SseOp::kPunpckhqdq;
                    break;
                case 0x74:
                    sop = SseOp::kPcmpeqb;
                    break;
                case 0x75:
                    sop = SseOp::kPcmpeqw;
                    break;
                case 0x76:
                    sop = SseOp::kPcmpeqd;
                    break;
                case 0xD4:
                    sop = SseOp::kPaddq;
                    break;
                case 0xDA:
                    sop = SseOp::kPminub;
                    break;
                case 0xDE:
                    sop = SseOp::kPmaxub;
                    break;
                case 0xF8:
                    sop = SseOp::kPsubb;
                    break;
                case 0xF9:
                    sop = SseOp::kPsubw;
                    break;
                case 0xFA:
                    sop = SseOp::kPsubd;
                    break;
                case 0xFB:
                    sop = SseOp::kPsubq;
                    break;
                case 0xFC:
                    sop = SseOp::kPaddb;
                    break;
                case 0xFD:
                    sop = SseOp::kPaddw;
                    break;
                case 0xFE:
                    sop = SseOp::kPaddd;
                    break;
                default:
                    break;
                }
            }
            if (sop != SseOp::kNone) {
                const Modrm m = read_modrm(r);
                Operand rm_op;
                if (!decode_xmm_rm_operand(r, m, rex, pfx, OperandSize::k128, rm_op)) {
                    result.error =
                        r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                    return result;
                }
                insn.dst = xmm_reg_operand(m, rex);
                insn.src = rm_op;
                insn.has_src = true;
                finish_sse(sop);
                return result;
            }
        }

        result.error = DecodeError::kUnknownOpcode;
        return result;
    }

    result.error = DecodeError::kUnknownOpcode;
    return result;
}

} // namespace hyperion
