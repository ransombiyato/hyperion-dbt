// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/decoder.hpp"

#include <array>
#include <cstdio>

namespace hyperion {

namespace {

// Maximum architectural instruction length. The CPU raises #GP beyond this;
// we reject rather than read further.
constexpr std::size_t kMaxInstructionLength = 15;

// Bounds-checked byte reader. Every access goes through this so a truncated
// buffer can never be read out of bounds.
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

    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }

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

// ModRM byte, decomposed.
struct Modrm {
    std::uint8_t mod = 0;
    std::uint8_t reg = 0;
    std::uint8_t rm = 0;
};

Gpr gpr_from(std::uint8_t index) {
    return static_cast<Gpr>(index & 0xF);
}

// Resolve the register encoded in ModRM.reg, extended by REX.R.
Gpr reg_field(const Modrm& m, const Rex& rex) {
    return gpr_from(static_cast<std::uint8_t>(m.reg | (rex.r ? 8 : 0)));
}

// Resolve the register encoded in ModRM.rm when mod == 3, extended by REX.B.
Gpr rm_register(const Modrm& m, const Rex& rex) {
    return gpr_from(static_cast<std::uint8_t>(m.rm | (rex.b ? 8 : 0)));
}

// Decode a ModRM byte plus any SIB/displacement into a register or memory
// operand. Returns false if the form is outside the supported subset.
bool decode_rm_operand(Reader& r, const Modrm& m, const Rex& rex, OperandSize size, Operand& out) {
    if (m.mod == 3) {
        out.kind = OperandKind::kRegister;
        out.size = size;
        out.reg = rm_register(m, rex);
        return true;
    }

    MemoryOperand mem;
    mem.address_size = OperandSize::k64;

    if (m.rm == 4) {
        // SIB byte follows. Read it through the bounds-checked reader so a
        // missing byte sets the truncation flag; a bare has() check would let
        // the caller misclassify truncation as an unsupported ModRM form.
        const std::uint8_t sib = r.u8();
        if (r.truncated()) {
            return false;
        }
        const std::uint8_t scale_bits = static_cast<std::uint8_t>(sib >> 6);
        const std::uint8_t index_bits = static_cast<std::uint8_t>((sib >> 3) & 0x7);
        const std::uint8_t base_bits = static_cast<std::uint8_t>(sib & 0x7);

        static constexpr std::array<std::uint8_t, 4> kScale = {1, 2, 4, 8};
        mem.scale = kScale[scale_bits];

        // index == 4 (rsp) means "no index" unless REX.X extends it.
        const bool index_present = !(index_bits == 4 && !rex.x);
        if (index_present) {
            mem.has_index = true;
            mem.index = gpr_from(static_cast<std::uint8_t>(index_bits | (rex.x ? 8 : 0)));
        }

        // base == 5 with mod == 0 is "no base, disp32" (RIP-relative in 64-bit
        // mode when there is no SIB base). Rejected: not in the subset.
        if (base_bits == 5 && m.mod == 0) {
            return false;
        }
        mem.has_base = true;
        mem.base = gpr_from(static_cast<std::uint8_t>(base_bits | (rex.b ? 8 : 0)));
    } else if (m.rm == 5 && m.mod == 0) {
        // RIP-relative addressing (mod=00, rm=101). Common in real code, but
        // not modelled yet; reject explicitly rather than mis-compute.
        return false;
    } else {
        mem.has_base = true;
        mem.base = gpr_from(static_cast<std::uint8_t>(m.rm | (rex.b ? 8 : 0)));
    }

    switch (m.mod) {
    case 0:
        break; // no displacement (the rm==5 / SIB-base==5 cases were rejected)
    case 1:
        mem.disp = r.i8();
        break;
    case 2:
        mem.disp = r.i32();
        break;
    default:
        return false;
    }

    out.kind = OperandKind::kMemory;
    out.size = size;
    out.mem = mem;
    return true;
}

// Immediate operand size for the ALU group (0x81 -> imm32 sign-extended for
// 64-bit operands, 0x83 -> imm8 sign-extended).
bool read_alu_immediate(Reader& r, OperandSize size, Operand& out) {
    out.kind = OperandKind::kImmediate;
    out.size = size;
    if (size == OperandSize::k64) {
        const std::int32_t v = r.i32();
        out.imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
    } else {
        out.imm = r.u32();
    }
    return !r.truncated();
}

// The ALU opcode table for the 0x00..0x3F primary map. Group 1 opcodes
// (0x80/0x81/0x83) select from this via ModRM.reg.
Opcode alu_group_opcode(std::uint8_t reg_field) {
    switch (reg_field & 0x7) {
    case 0:
        return Opcode::kAdd;
    case 1:
        return Opcode::kOr;
    case 4:
        return Opcode::kAnd;
    case 5:
        return Opcode::kSub;
    case 6:
        return Opcode::kXor;
    case 7:
        return Opcode::kCmp;
    default:
        return static_cast<Opcode>(0xFF); // unsupported (ADC/SBB)
    }
}

bool is_supported_alu_group(std::uint8_t reg_field) {
    switch (reg_field & 0x7) {
    case 0:
    case 1:
    case 4:
    case 5:
    case 6:
    case 7:
        return true;
    default:
        return false;
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
    case Opcode::kAdd:
        return "add";
    case Opcode::kSub:
        return "sub";
    case Opcode::kCmp:
        return "cmp";
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
    case Opcode::kNop:
        return "nop";
    case Opcode::kRet:
        return "ret";
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
    case OperandKind::kImmediate:
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(op.imm));
        return buf;
    case OperandKind::kMemory: {
        std::string s = "[";
        bool need_plus = false;
        if (op.mem.has_base) {
            s += to_string(op.mem.base);
            need_plus = true;
        }
        if (op.mem.has_index) {
            if (need_plus)
                s += "+";
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
    } else {
        out = to_string(instruction.opcode);
    }

    if (instruction.opcode == Opcode::kNop || instruction.opcode == Opcode::kRet ||
        instruction.opcode == Opcode::kHlt || instruction.opcode == Opcode::kSyscall) {
        return out;
    }

    out += " ";
    out += operand_to_string(instruction.dst);
    if (instruction.has_src) {
        out += ", ";
        out += operand_to_string(instruction.src);
    }
    return out;
}

DecodeResult decode(const std::uint8_t* code, std::size_t size, std::uint64_t address) {
    DecodeResult result;
    Instruction& insn = result.instruction;
    insn.address = address;

    Reader r(code, size);
    Rex rex;

    // --- Legacy prefixes ---------------------------------------------------
    // Only REX (0x40..0x4F) is modelled. Everything else that changes meaning
    // is rejected rather than ignored: ignoring 0x66 would decode a 16-bit
    // instruction as 32-bit, which is silently wrong.
    bool saw_rex = false;
    for (;;) {
        if (!r.has()) {
            result.error = DecodeError::kTruncated;
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
            saw_rex = true;
            // REX must be the last prefix; only one is meaningful.
            break;
        }
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x66 || b == 0x67 || b == 0x2E ||
            b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
            result.error = DecodeError::kUnsupportedPrefix;
            return result;
        }
        break;
    }
    (void)saw_rex;

    const std::uint8_t opcode = r.u8();
    if (r.truncated()) {
        result.error = DecodeError::kTruncated;
        return result;
    }

    // Operand size from REX.W. Without REX.W, the default operand size in
    // 64-bit mode is 32 bits.
    const OperandSize op_size = rex.w ? OperandSize::k64 : OperandSize::k32;
    insn.rex_w = rex.w;

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

    // --- 0x88..0x8B: MOV r/m, r and MOV r, r/m -----------------------------
    if (opcode == 0x88 || opcode == 0x89 || opcode == 0x8A || opcode == 0x8B) {
        const bool byte_form = (opcode == 0x88 || opcode == 0x8A);
        if (byte_form) {
            result.error = DecodeError::kUnsupportedOperand; // 8-bit not modelled
            return result;
        }
        Modrm m;
        const std::uint8_t modrm = r.u8();
        m.mod = static_cast<std::uint8_t>(modrm >> 6);
        m.reg = static_cast<std::uint8_t>((modrm >> 3) & 0x7);
        m.rm = static_cast<std::uint8_t>(modrm & 0x7);

        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, op_size, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        Operand reg_op;
        reg_op.kind = OperandKind::kRegister;
        reg_op.size = op_size;
        reg_op.reg = reg_field(m, rex);

        if (opcode == 0x89) { // MOV r/m, r  -> dst = rm, src = reg
            insn.dst = rm_op;
            insn.src = reg_op;
        } else { // 0x8B: MOV r, r/m -> dst = reg, src = rm
            insn.dst = reg_op;
            insn.src = rm_op;
        }
        insn.has_src = true;
        finish(Opcode::kMov);
        return result;
    }

    // --- 0x01/0x03 and 0x29/0x2B, 0x21/0x23, 0x09/0x0B, 0x31/0x33: ALU -----
    // Primary-map ALU pattern: (base + 0x00) = r/m, r ; (base + 0x02) = r, r/m
    // base: ADD 0x00, OR 0x08, AND 0x20, SUB 0x28, XOR 0x30, CMP 0x38
    {
        Opcode alu = static_cast<Opcode>(0xFF);
        bool is_alu = false;
        switch (opcode) {
        case 0x01:
            alu = Opcode::kAdd;
            is_alu = true;
            break;
        case 0x03:
            alu = Opcode::kAdd;
            is_alu = true;
            break;
        case 0x09:
            alu = Opcode::kOr;
            is_alu = true;
            break;
        case 0x0B:
            alu = Opcode::kOr;
            is_alu = true;
            break;
        case 0x21:
            alu = Opcode::kAnd;
            is_alu = true;
            break;
        case 0x23:
            alu = Opcode::kAnd;
            is_alu = true;
            break;
        case 0x29:
            alu = Opcode::kSub;
            is_alu = true;
            break;
        case 0x2B:
            alu = Opcode::kSub;
            is_alu = true;
            break;
        case 0x31:
            alu = Opcode::kXor;
            is_alu = true;
            break;
        case 0x33:
            alu = Opcode::kXor;
            is_alu = true;
            break;
        case 0x39:
            alu = Opcode::kCmp;
            is_alu = true;
            break;
        case 0x3B:
            alu = Opcode::kCmp;
            is_alu = true;
            break;
        default:
            break;
        }
        if (is_alu) {
            Modrm m;
            const std::uint8_t modrm = r.u8();
            m.mod = static_cast<std::uint8_t>(modrm >> 6);
            m.reg = static_cast<std::uint8_t>((modrm >> 3) & 0x7);
            m.rm = static_cast<std::uint8_t>(modrm & 0x7);

            Operand rm_op;
            if (!decode_rm_operand(r, m, rex, op_size, rm_op)) {
                result.error =
                    r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
                return result;
            }
            Operand reg_op;
            reg_op.kind = OperandKind::kRegister;
            reg_op.size = op_size;
            reg_op.reg = reg_field(m, rex);

            const bool rm_is_dst = (opcode & 0x02) == 0; // 0x01,0x09,... = r/m,r
            if (rm_is_dst) {
                insn.dst = rm_op;
                insn.src = reg_op;
            } else {
                insn.dst = reg_op;
                insn.src = rm_op;
            }
            insn.has_src = true;
            finish(alu);
            return result;
        }
    }

    // --- 0x81/0x83: ALU group 1, r/m, imm ----------------------------------
    if (opcode == 0x81 || opcode == 0x83) {
        Modrm m;
        const std::uint8_t modrm = r.u8();
        m.mod = static_cast<std::uint8_t>(modrm >> 6);
        m.reg = static_cast<std::uint8_t>((modrm >> 3) & 0x7);
        m.rm = static_cast<std::uint8_t>(modrm & 0x7);

        if (!is_supported_alu_group(m.reg)) {
            result.error = DecodeError::kUnknownOpcode; // ADC/SBB not modelled
            return result;
        }

        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, op_size, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }

        Operand imm;
        imm.kind = OperandKind::kImmediate;
        imm.size = op_size;
        if (opcode == 0x83) {
            // imm8, sign-extended to the operand size.
            const std::int8_t v = r.i8();
            imm.imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
        } else if (!read_alu_immediate(r, op_size, imm)) {
            result.error = DecodeError::kTruncated;
            return result;
        }

        insn.dst = rm_op;
        insn.src = imm;
        insn.has_src = true;
        finish(alu_group_opcode(m.reg));
        return result;
    }

    // --- 0xC7 /0: MOV r/m32/r/m64, imm32 ----------------------------------
    // The imm32 form of a move to a register or memory operand. glibc's
    // `_start` uses it (`mov $main, %rdi`), so it is a prerequisite for
    // running anything real. Only ModRM.reg == 0 is MOV; the other reg values
    // are XBEGIN and other instructions this project does not model.
    if (opcode == 0xC7) {
        Modrm m;
        const std::uint8_t modrm = r.u8();
        m.mod = static_cast<std::uint8_t>(modrm >> 6);
        m.reg = static_cast<std::uint8_t>((modrm >> 3) & 0x7);
        m.rm = static_cast<std::uint8_t>(modrm & 0x7);

        if (m.reg != 0) {
            result.error = DecodeError::kUnknownOpcode; // XBEGIN and friends
            return result;
        }

        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, op_size, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }

        Operand imm;
        imm.kind = OperandKind::kImmediate;
        imm.size = op_size;
        if (op_size == OperandSize::k64) {
            // imm32 sign-extended to 64 bits, exactly as the ALU group does.
            const std::int32_t v = r.i32();
            imm.imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
        } else {
            imm.imm = r.u32();
        }

        insn.dst = rm_op;
        insn.src = imm;
        insn.has_src = true;
        finish(Opcode::kMov);
        return result;
    }

    // --- 0xB8..0xBF: MOV r32/r64, imm32/imm64 ------------------------------
    if (opcode >= 0xB8 && opcode <= 0xBF) {
        Operand dst;
        dst.kind = OperandKind::kRegister;
        dst.size = op_size;
        dst.reg = gpr_from(static_cast<std::uint8_t>((opcode - 0xB8) | (rex.b ? 8 : 0)));

        Operand imm;
        imm.kind = OperandKind::kImmediate;
        imm.size = op_size;
        if (rex.w) {
            // 64-bit immediate; in practice only 0xB8..0xBF with REX.W. We do
            // not implement MOVABS optimisations, so this is the imm64 form.
            imm.imm = r.u64();
        } else {
            imm.imm = r.u32();
        }

        insn.dst = dst;
        insn.src = imm;
        insn.has_src = true;
        finish(Opcode::kMov);
        return result;
    }

    // --- 0xFF: INC/DEC r/m (group 5, reg 0 and 1 only) ---------------------
    if (opcode == 0xFF) {
        Modrm m;
        const std::uint8_t modrm = r.u8();
        m.mod = static_cast<std::uint8_t>(modrm >> 6);
        m.reg = static_cast<std::uint8_t>((modrm >> 3) & 0x7);
        m.rm = static_cast<std::uint8_t>(modrm & 0x7);

        Opcode op;
        if (m.reg == 0) {
            op = Opcode::kInc;
        } else if (m.reg == 1) {
            op = Opcode::kDec;
        } else {
            result.error = DecodeError::kUnknownOpcode; // call/jmp/push group
            return result;
        }

        Operand rm_op;
        if (!decode_rm_operand(r, m, rex, op_size, rm_op)) {
            result.error = r.truncated() ? DecodeError::kTruncated : DecodeError::kUnsupportedModrm;
            return result;
        }
        insn.dst = rm_op;
        finish(op);
        return result;
    }

    // --- 0x90 NOP ----------------------------------------------------------
    if (opcode == 0x90) {
        // 0x90 is XCHG eax,eax; with REX.B it is XCHG r8,rax. Only plain NOP
        // is in the subset.
        if (rex.present) {
            result.error = DecodeError::kUnknownOpcode;
            return result;
        }
        finish(Opcode::kNop);
        return result;
    }

    // --- 0xC3 RET ----------------------------------------------------------
    if (opcode == 0xC3) {
        finish(Opcode::kRet);
        return result;
    }

    // --- 0xF4 HLT ----------------------------------------------------------
    if (opcode == 0xF4) {
        finish(Opcode::kHlt);
        return result;
    }

    // --- 0x0F 0x05 SYSCALL -------------------------------------------------
    // The only two-byte opcode in the subset. No operands: the number is in
    // rax and the arguments in rdi/rsi/rdx/r10/r8/r9 (see syscall.hpp).
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
        // 0x0F 0x80..0x8F (Jcc rel32) is not in the subset; anything else under
        // the 0x0F escape is likewise outside it.
        result.error = DecodeError::kUnknownOpcode;
        return result;
    }

    // --- 0xEB JMP rel8 (short unconditional jump) -------------------------
    // The rel32 form is 0xE9; the rel8 form is very common in real code and is
    // the natural companion to the 0x70..0x7F short Jcc below.
    if (opcode == 0xEB) {
        const std::int8_t rel = r.i8();
        Operand target;
        target.kind = OperandKind::kImmediate;
        target.size = OperandSize::k64;
        const std::uint64_t next = address + static_cast<std::uint64_t>(r.position());
        target.imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(next) + rel);
        insn.dst = target;
        finish(Opcode::kJmp);
        return result;
    }

    // --- 0xE9 JMP rel32 ----------------------------------------------------
    if (opcode == 0xE9) {
        const std::int32_t rel = r.i32();
        Operand target;
        target.kind = OperandKind::kImmediate;
        target.size = OperandSize::k64;
        // Relative to the end of the instruction. Length is known only after
        // finish(); compute it explicitly here.
        const std::uint64_t next = address + static_cast<std::uint64_t>(r.position());
        target.imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(next) + rel);
        insn.dst = target;
        finish(Opcode::kJmp);
        return result;
    }

    // --- 0x70..0x7F Jcc rel8 (short conditional jump) ---------------------
    // Note: these are SHORT jumps, 1-byte displacement. The rel32 forms are
    // 0x0F 0x80..0x8F and are not in the subset.
    if (opcode >= 0x70 && opcode <= 0x7F) {
        const std::int8_t rel = r.i8();
        Operand target;
        target.kind = OperandKind::kImmediate;
        target.size = OperandSize::k64;
        const std::uint64_t next = address + static_cast<std::uint64_t>(r.position());
        target.imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(next) + rel);
        insn.dst = target;
        insn.condition = static_cast<Condition>(opcode & 0x0F);
        finish(Opcode::kJcc);
        return result;
    }

    // --- 0x50..0x57 PUSH r64, 0x58..0x5F POP r64 --------------------------
    if (opcode >= 0x50 && opcode <= 0x57) {
        Operand dst;
        dst.kind = OperandKind::kRegister;
        dst.size = OperandSize::k64;
        dst.reg = gpr_from(static_cast<std::uint8_t>((opcode - 0x50) | (rex.b ? 8 : 0)));
        insn.dst = dst;
        finish(Opcode::kPush);
        return result;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        Operand dst;
        dst.kind = OperandKind::kRegister;
        dst.size = OperandSize::k64;
        dst.reg = gpr_from(static_cast<std::uint8_t>((opcode - 0x58) | (rex.b ? 8 : 0)));
        insn.dst = dst;
        finish(Opcode::kPop);
        return result;
    }

    result.error = DecodeError::kUnknownOpcode;
    return result;
}

} // namespace hyperion
