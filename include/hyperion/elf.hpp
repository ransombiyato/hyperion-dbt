// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// ELF64 program-image loader.
//
// Until now `hyperion run` accepted only a raw code blob copied at a fixed base
// address. A real program is an ELF executable, so this file parses and
// validates one, then maps its PT_LOAD segments into the guest address space.
//
// Design rules (see DECISIONS.md D-033 and SECURITY.md):
//   - The file is untrusted input. Every offset, size and count is bounds
//     checked against the actual file length before it is used; nothing is
//     trusted because it "looks like" an ELF header.
//   - Parsing and mapping are separate steps. parse_elf64() touches no memory
//     and cannot fail half-way through a mapping, which makes it directly
//     unit-testable on hostile input.
//   - Only static executables are supported. A dynamically linked image needs a
//     PT_INTERP (a userspace dynamic loader) that this project does not have, so
//     it is rejected with a distinct error rather than loaded and then failing
//     somewhere confusing.
//   - This is not a sandbox and not a validator of the *program's* intent: it
//     only guarantees the image is internally consistent and maps inside the
//     guest address space. See SECURITY.md.
#ifndef HYPERION_ELF_HPP
#define HYPERION_ELF_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hyperion/address_space.hpp"

namespace hyperion {

// Why an image could not be parsed or mapped. Each value is a distinct,
// testable failure, so a caller can report the exact reason instead of "bad
// ELF".
enum class ElfError {
    kNone = 0,
    kTooSmall,          // shorter than an ELF64 header
    kBadMagic,          // not \x7fELF
    kNotElf64,          // EI_CLASS != ELFCLASS64 (32-bit image)
    kNotLittleEndian,   // EI_DATA != ELFDATA2LSB
    kBadVersion,        // EI_VERSION != EV_CURRENT
    kNotX86_64,         // e_machine != EM_X86_64
    kBadType,           // e_type not ET_EXEC or ET_DYN
    kNoProgramHeaders,  // e_phnum == 0, or e_phentsize too small
    kBadProgramHeader,  // a header extends past EOF
    kBadSegment,        // p_filesz > p_memsz, or p_offset+p_filesz past EOF
    kMisalignedSegment, // p_vaddr and p_offset are incongruent mod p_align
    kBadInterpreter,    // PT_INTERP is malformed
    kNeedsInterpreter,  // dynamically linked: no loader in this project yet
    kLoadFailed,        // the address space refused a segment (overlap, OOM)
};

const char* to_string(ElfError error);

// One PT_LOAD segment, described but not yet copied.
struct ElfSegment {
    std::uint64_t vaddr = 0;
    std::uint64_t memsz = 0;  // size in memory (>= filesz); the rest is zero (BSS)
    std::uint64_t filesz = 0; // bytes present in the file
    std::uint64_t offset = 0; // file offset of those bytes
    std::uint32_t flags = 0;  // PF_R / PF_W / PF_X

    bool readable() const { return (flags & 4U) != 0; }
    bool writable() const { return (flags & 2U) != 0; }
    bool executable() const { return (flags & 1U) != 0; }
};

// A parsed, validated image. No memory has been touched yet.
struct ElfImage {
    std::uint64_t entry = 0;
    std::vector<ElfSegment> segments; // PT_LOAD, in file order
    std::string interpreter;          // PT_INTERP path; empty when static

    bool is_static() const { return interpreter.empty(); }
    // Highest address the image occupies (exclusive); 0 when there are none.
    std::uint64_t image_end() const;
};

// Parse and validate `data[0..size)`. Returns nullopt and sets `error` on any
// malformed input. Never reads outside [data, data + size).
std::optional<ElfImage> parse_elf64(const std::uint8_t* data, std::size_t size, ElfError& error);

// Map a parsed image's PT_LOAD segments into `space`, then allocate an initial
// guest stack and write the startup argument block (`argc`, `argv`, `envp`
// terminator and a small `auxv`) onto it. On success, returns the image's entry
// point and sets `state.rip` / `state.gpr[rsp]` in `state`; on failure returns
// nullopt and sets `error`.
//
// `argv` is the guest argument vector (argv[0] is conventionally the program
// name). An empty vector is treated as a single empty argument, so the guest
// always sees a valid `argc >= 1` stack, exactly as the kernel guarantees.
std::optional<std::uint64_t> map_elf_image(const ElfImage& image, const std::uint8_t* data,
                                           std::size_t size, GuestAddressSpace& space,
                                           GuestState& state, const std::vector<std::string>& argv,
                                           ElfError& error);

// Convenience: parse then map. On success `state` is ready to execute at
// `state.rip`.
std::optional<ElfImage> load_elf64(const std::vector<std::uint8_t>& file, GuestAddressSpace& space,
                                   GuestState& state, const std::vector<std::string>& argv,
                                   ElfError& error);

// True if `data` begins with the ELF magic. Used by the CLI to decide between
// the ELF path and the raw-blob path without a full parse.
bool looks_like_elf(const std::uint8_t* data, std::size_t size);

} // namespace hyperion

#endif // HYPERION_ELF_HPP
