// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// ELF64 loader. See include/hyperion/elf.hpp for the design rules.
#include "hyperion/elf.hpp"

#include <algorithm>
#include <cstring>

namespace hyperion {

namespace {

// --- On-disk structures ----------------------------------------------------
//
// Read field by field with explicit little-endian decoding rather than
// reinterpreting the input as a packed struct. The input is untrusted, the
// structs would need __attribute__((packed)) to be correct, and unaligned or
// truncated input must not be able to read past the buffer.

std::uint16_t read_u16(const std::uint8_t* p) {
    const std::uint32_t value =
        static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
    return static_cast<std::uint16_t>(value);
}
std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t read_u64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(read_u32(p)) |
           (static_cast<std::uint64_t>(read_u32(p + 4)) << 32);
}

constexpr std::size_t kEhdrSize = 64;
constexpr std::size_t kPhdr64Size = 56;

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtInterp = 3;

constexpr std::uint16_t kEtExec = 2;
constexpr std::uint16_t kEtDyn = 3;
constexpr std::uint16_t kEmX86_64 = 62;

// The guest stack is placed at the top of a dedicated region, well clear of
// typical image bases and of the default mmap area (0x7f0000000000). It grows
// down from `kStackTop`.
constexpr std::uint64_t kStackSize = 8ULL << 20; // 8 MiB
constexpr std::uint64_t kStackTop = 0x7ff000000000ULL;

// A hard ceiling on how much of a file may be mapped, so a hostile image cannot
// drive an enormous host allocation. Matches the address space's per-mapping
// bound.
constexpr std::uint64_t kMaxImageBytes = 1ULL << 30; // 1 GiB

// Write a NUL-terminated string at `addr` in the guest stack buffer, returning
// the address, or nullopt if it would not fit.
std::optional<std::uint64_t> put_string(std::uint8_t* stack, std::uint64_t stack_base,
                                        std::uint64_t& cursor, const std::string& text) {
    const std::uint64_t need = text.size() + 1;
    if (cursor < stack_base || cursor - stack_base < need) {
        return std::nullopt; // would underflow below the stack base
    }
    cursor -= need;
    std::memcpy(stack + (cursor - stack_base), text.data(), text.size());
    stack[cursor - stack_base + text.size()] = 0;
    return cursor;
}

} // namespace

const char* to_string(ElfError error) {
    switch (error) {
    case ElfError::kNone:
        return "ok";
    case ElfError::kTooSmall:
        return "file is shorter than an ELF64 header";
    case ElfError::kBadMagic:
        return "not an ELF file (bad magic)";
    case ElfError::kNotElf64:
        return "not a 64-bit ELF image";
    case ElfError::kNotLittleEndian:
        return "not a little-endian image";
    case ElfError::kBadVersion:
        return "unsupported ELF version";
    case ElfError::kNotX86_64:
        return "not an x86-64 image";
    case ElfError::kBadType:
        return "not an executable (e_type)";
    case ElfError::kNoProgramHeaders:
        return "no usable program headers";
    case ElfError::kBadProgramHeader:
        return "program header table runs past end of file";
    case ElfError::kBadSegment:
        return "segment sizes are inconsistent with the file";
    case ElfError::kMisalignedSegment:
        return "segment is not page-aligned";
    case ElfError::kBadInterpreter:
        return "malformed PT_INTERP";
    case ElfError::kNeedsInterpreter:
        return "dynamically linked image (no loader yet)";
    case ElfError::kLoadFailed:
        return "the address space refused a segment";
    }
    return "unknown ELF error";
}

std::uint64_t ElfImage::image_end() const {
    std::uint64_t end = 0;
    for (const ElfSegment& s : segments) {
        const std::uint64_t seg_end = s.vaddr + s.memsz;
        if (seg_end > end) {
            end = seg_end;
        }
    }
    return end;
}

bool looks_like_elf(const std::uint8_t* data, std::size_t size) {
    return data != nullptr && size >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' &&
           data[3] == 'F';
}

std::optional<ElfImage> parse_elf64(const std::uint8_t* data, std::size_t size, ElfError& error) {
    error = ElfError::kNone;
    if (data == nullptr || size < kEhdrSize) {
        error = ElfError::kTooSmall;
        return std::nullopt;
    }
    if (!looks_like_elf(data, size)) {
        error = ElfError::kBadMagic;
        return std::nullopt;
    }
    if (data[4] != 2) { // EI_CLASS
        error = ElfError::kNotElf64;
        return std::nullopt;
    }
    if (data[5] != 1) { // EI_DATA: ELFDATA2LSB
        error = ElfError::kNotLittleEndian;
        return std::nullopt;
    }
    if (data[6] != 1) { // EI_VERSION: EV_CURRENT
        error = ElfError::kBadVersion;
        return std::nullopt;
    }

    const std::uint16_t e_type = read_u16(data + 16);
    const std::uint16_t e_machine = read_u16(data + 18);
    if (e_machine != kEmX86_64) {
        error = ElfError::kNotX86_64;
        return std::nullopt;
    }
    if (e_type != kEtExec && e_type != kEtDyn) {
        error = ElfError::kBadType;
        return std::nullopt;
    }

    ElfImage image;
    image.entry = read_u64(data + 24);

    const std::uint64_t e_phoff = read_u64(data + 32);
    const std::uint16_t e_phentsize = read_u16(data + 54);
    const std::uint16_t e_phnum = read_u16(data + 56);

    if (e_phnum == 0 || e_phentsize < kPhdr64Size) {
        error = ElfError::kNoProgramHeaders;
        return std::nullopt;
    }
    // The whole table must lie inside the file. Checked with division-free
    // arithmetic so a huge e_phnum cannot wrap.
    const std::uint64_t table_bytes = static_cast<std::uint64_t>(e_phnum) * e_phentsize;
    if (e_phoff > size || table_bytes > size - e_phoff) {
        error = ElfError::kBadProgramHeader;
        return std::nullopt;
    }

    for (std::uint16_t i = 0; i < e_phnum; ++i) {
        const std::uint8_t* ph = data + e_phoff + static_cast<std::uint64_t>(i) * e_phentsize;
        const std::uint32_t p_type = read_u32(ph + 0);
        if (p_type == kPtInterp) {
            const std::uint64_t off = read_u64(ph + 8);
            const std::uint64_t fsz = read_u64(ph + 32);
            if (fsz == 0 || off > size || fsz > size - off) {
                error = ElfError::kBadInterpreter;
                return std::nullopt;
            }
            // The interpreter path must be NUL-terminated within the segment.
            const char* begin = reinterpret_cast<const char*>(data + off);
            const std::size_t len = strnlen(begin, static_cast<std::size_t>(fsz));
            if (len == fsz) {
                error = ElfError::kBadInterpreter;
                return std::nullopt;
            }
            image.interpreter.assign(begin, len);
            continue;
        }
        if (p_type != kPtLoad) {
            continue;
        }

        ElfSegment seg;
        seg.flags = read_u32(ph + 4);
        seg.offset = read_u64(ph + 8);
        seg.vaddr = read_u64(ph + 16);
        seg.filesz = read_u64(ph + 32);
        seg.memsz = read_u64(ph + 40);

        if (seg.filesz > seg.memsz) {
            error = ElfError::kBadSegment;
            return std::nullopt;
        }
        // filesz bytes must be present in the file.
        if (seg.offset > size || seg.filesz > size - seg.offset) {
            error = ElfError::kBadSegment;
            return std::nullopt;
        }
        // The ABI requires p_vaddr ≡ p_offset (mod p_align). With p_align a page
        // this means the offset within a page must match, which is what lets the
        // loader place the bytes at the right place in the mapped page.
        const std::uint64_t p_align = read_u64(ph + 48);
        if (p_align > 1) {
            if ((seg.vaddr % p_align) != (seg.offset % p_align)) {
                error = ElfError::kMisalignedSegment;
                return std::nullopt;
            }
        }
        if (seg.memsz > kMaxImageBytes) {
            error = ElfError::kBadSegment;
            return std::nullopt;
        }
        if (seg.memsz > 0) {
            image.segments.push_back(seg);
        }
    }

    if (image.segments.empty()) {
        error = ElfError::kNoProgramHeaders;
        return std::nullopt;
    }
    return image;
}

std::optional<std::uint64_t> map_elf_image(const ElfImage& image, const std::uint8_t* data,
                                           std::size_t size, GuestAddressSpace& space,
                                           GuestState& state, const std::vector<std::string>& argv,
                                           ElfError& error) {
    error = ElfError::kNone;

    if (!image.is_static()) {
        // A PT_INTERP means the kernel would hand control to a userspace dynamic
        // loader. This project has no such loader, so loading the image would
        // produce a confusing failure much later. Refuse it now, distinctly.
        error = ElfError::kNeedsInterpreter;
        return std::nullopt;
    }

    for (const ElfSegment& seg : image.segments) {
        // Bounds were validated in parse, but map_elf_image can be called with a
        // hand-built image, so re-check before touching the file buffer.
        if (seg.offset > size || seg.filesz > size - seg.offset) {
            error = ElfError::kBadSegment;
            return std::nullopt;
        }
        MemPerm perm = MemPerm::kNone;
        if (seg.readable()) {
            perm = perm | MemPerm::kRead;
        }
        if (seg.writable()) {
            perm = perm | MemPerm::kWrite;
        }
        if (seg.executable()) {
            perm = perm | MemPerm::kExec;
        }
        if (perm == MemPerm::kNone) {
            perm = MemPerm::kRead; // a non-accessible PT_LOAD is meaningless
        }
        const std::uint8_t* seg_data = seg.filesz > 0 ? data + seg.offset : nullptr;
        if (!space.map_segment(seg.vaddr, seg_data, static_cast<std::size_t>(seg.filesz),
                               static_cast<std::size_t>(seg.memsz), perm)) {
            error = ElfError::kLoadFailed;
            return std::nullopt;
        }
    }

    // --- initial stack ------------------------------------------------------
    //
    // The x86-64 process-startup ABI: the stack top holds, in order, argc, the
    // argv pointers, a NULL, the envp pointers, a NULL, then auxv pairs. rsp
    // points at argc. The strings live above the pointer block. We provide the
    // argument strings and an empty environment plus a minimal auxv (the kernel
    // would supply much more; the guest will not get far enough to need it until
    // more of the instruction set exists).
    if (argv.empty()) {
        error = ElfError::kBadInterpreter; // reuse: no argv is a caller error
        return std::nullopt;
    }

    const std::uint64_t stack_base = kStackTop - kStackSize;
    auto stack_opt = space.map_anonymous(static_cast<std::size_t>(kStackSize),
                                         MemPerm::kRead | MemPerm::kWrite, stack_base);
    if (!stack_opt) {
        error = ElfError::kLoadFailed;
        return std::nullopt;
    }
    const std::uint64_t mapped_stack = *stack_opt;

    // Build the whole stack image in a local buffer, then commit it with one
    // write through the address space. (Writing strings at chosen offsets needs
    // a contiguous buffer; a series of small writes would be slower and no
    // clearer.)
    std::vector<std::uint8_t> image_bytes(static_cast<std::size_t>(kStackSize), 0);
    const auto to_host = [&](std::uint64_t guest) -> std::size_t {
        return static_cast<std::size_t>(guest - mapped_stack);
    };

    std::uint64_t cursor = mapped_stack + kStackSize;

    std::vector<std::uint64_t> arg_ptrs;
    arg_ptrs.reserve(argv.size());
    for (const std::string& arg : argv) {
        auto placed = put_string(image_bytes.data(), mapped_stack, cursor, arg);
        if (!placed) {
            error = ElfError::kLoadFailed;
            return std::nullopt;
        }
        arg_ptrs.push_back(*placed);
    }

    // Pointer block: argc, argv[], NULL, envp NULL, auxv terminator.
    // 1 (argc) + argc + 1 (argv NULL) + 1 (envp NULL) + 2 (auxv NULL pair).
    const std::uint64_t words = 1 + arg_ptrs.size() + 1 + 1 + 2;
    const std::uint64_t block_bytes = words * 8;
    // The ABI guarantees rsp is 16-byte aligned at process entry, so the block
    // *starts* on a 16-byte boundary.
    if (cursor < mapped_stack + block_bytes) {
        error = ElfError::kLoadFailed;
        return std::nullopt;
    }
    const std::uint64_t block = (cursor - block_bytes) & ~static_cast<std::uint64_t>(15);

    const auto put_u64 = [&](std::uint64_t at, std::uint64_t value) {
        const std::size_t off = to_host(at);
        for (int i = 0; i < 8; ++i) {
            image_bytes[off + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
        }
    };

    std::uint64_t at = block;
    put_u64(at, arg_ptrs.size());
    at += 8;
    for (std::uint64_t p : arg_ptrs) {
        put_u64(at, p);
        at += 8;
    }
    put_u64(at, 0); // argv terminator
    at += 8;
    put_u64(at, 0); // envp terminator (empty environment)
    at += 8;
    put_u64(at, 0); // auxv: AT_NULL
    at += 8;
    put_u64(at, 0); // auxv: value
    at += 8;

    if (!space.write(mapped_stack, image_bytes.data(), image_bytes.size())) {
        error = ElfError::kLoadFailed;
        return std::nullopt;
    }

    state.rip = image.entry;
    state.reg(Gpr::kRsp) = block;
    return image.entry;
}

std::optional<ElfImage> load_elf64(const std::vector<std::uint8_t>& file, GuestAddressSpace& space,
                                   GuestState& state, const std::vector<std::string>& argv,
                                   ElfError& error) {
    auto image = parse_elf64(file.data(), file.size(), error);
    if (!image) {
        return std::nullopt;
    }
    if (!map_elf_image(*image, file.data(), file.size(), space, state, argv, error)) {
        return std::nullopt;
    }
    return image;
}

} // namespace hyperion
