// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// ELF64 loader tests.
//
// Two kinds of test here. The first builds ELF images byte by byte in memory:
// that lets each validation rule be triggered in isolation (bad magic, 32-bit,
// truncated program-header table, p_filesz > p_memsz, unaligned vaddr, a
// PT_INTERP that runs off the end) without needing a cross compiler. The second
// loads a *real* `gcc -static` executable and checks the mapped image and the
// startup stack the way the kernel would: argc/argv on the stack, rsp pointing
// at argc, the entry point page mapped executable.
//
// The builder tests are deliberately hostile: every input is something an
// attacker could hand the loader.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/address_space.hpp"
#include "hyperion/elf.hpp"
#include "hyperion/guest_state.hpp"

using hyperion::ElfError;
using hyperion::ElfImage;
using hyperion::GuestAddressSpace;
using hyperion::GuestState;
using hyperion::MemPerm;

namespace {

constexpr std::uint64_t kPage = GuestAddressSpace::kPageSize;

void put_u16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v & 0xff);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put_u32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
    }
}
void put_u64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
    }
}

std::uint64_t get_u64(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return v;
}

// A minimal but valid static ET_EXEC image with one PT_LOAD segment. The
// segment covers file offset 0 with vaddr 0x400000 (so the ABI's
// `p_vaddr ≡ p_offset (mod p_align)` holds), and the single `hlt` (0xF4) sits at
// file offset 0x1000, i.e. guest address 0x401000 — the entry point.
struct ElfBuilder {
    std::vector<std::uint8_t> file;
    std::size_t ph = 64; // program header table offset
    std::uint64_t base = 0x400000;
    std::uint64_t entry = 0x401000;

    ElfBuilder() {
        file.assign(0x1001, 0);
        file[0] = 0x7f;
        file[1] = 'E';
        file[2] = 'L';
        file[3] = 'F';
        file[4] = 2;           // ELFCLASS64
        file[5] = 1;           // ELFDATA2LSB
        file[6] = 1;           // EV_CURRENT
        put_u16(file, 16, 2);  // ET_EXEC
        put_u16(file, 18, 62); // EM_X86_64
        put_u32(file, 20, 1);  // e_version
        put_u64(file, 24, entry);
        put_u64(file, 32, ph); // e_phoff
        put_u16(file, 52, 64); // e_ehsize
        put_u16(file, 54, 56); // e_phentsize
        put_u16(file, 56, 1);  // e_phnum
        set_load(base, /*file_offset=*/0, /*filesz=*/0x1001, /*memsz=*/0x1001, /*flags=*/5);
        file[0x1000] = 0xF4; // hlt at the entry point
    }

    void set_load(std::uint64_t vaddr, std::uint64_t off, std::uint64_t filesz, std::uint64_t memsz,
                  std::uint32_t flags) {
        put_u32(file, ph + 0, 1); // PT_LOAD
        put_u32(file, ph + 4, flags);
        put_u64(file, ph + 8, off);
        put_u64(file, ph + 16, vaddr);
        put_u64(file, ph + 32, filesz);
        put_u64(file, ph + 40, memsz);
        put_u64(file, ph + 48, kPage); // p_align
    }
};

} // namespace

HYPERION_TEST("elf", "parses_a_minimal_static_image") {
    ElfBuilder b;
    ElfError err = ElfError::kNone;
    auto img = hyperion::parse_elf64(b.file.data(), b.file.size(), err);
    HYPERION_CHECK(img.has_value());
    HYPERION_CHECK(err == ElfError::kNone);
    HYPERION_CHECK_EQ(img->entry, 0x401000u);
    HYPERION_CHECK_EQ(img->segments.size(), 1u);
    HYPERION_CHECK(img->is_static());
    HYPERION_CHECK(img->segments[0].executable());
    HYPERION_CHECK(img->segments[0].readable());
    HYPERION_CHECK(!img->segments[0].writable());
    HYPERION_CHECK_EQ(img->image_end(), 0x401001u);
}

HYPERION_TEST("elf", "rejects_bad_magic") {
    ElfBuilder b;
    b.file[1] = 'X';
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kBadMagic);
}

HYPERION_TEST("elf", "rejects_32_bit_image") {
    ElfBuilder b;
    b.file[4] = 1; // ELFCLASS32
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kNotElf64);
}

HYPERION_TEST("elf", "rejects_big_endian_image") {
    ElfBuilder b;
    b.file[5] = 2; // ELFDATA2MSB
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kNotLittleEndian);
}

HYPERION_TEST("elf", "rejects_a_non_x86_64_machine") {
    ElfBuilder b;
    put_u16(b.file, 18, 183); // EM_AARCH64
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kNotX86_64);
}

HYPERION_TEST("elf", "rejects_a_relocatable_object") {
    ElfBuilder b;
    put_u16(b.file, 16, 1); // ET_REL, not an executable
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kBadType);
}

HYPERION_TEST("elf", "rejects_a_truncated_file") {
    ElfBuilder b;
    b.file.resize(32); // shorter than a header
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kTooSmall);
}

HYPERION_TEST("elf", "rejects_a_program_header_table_past_eof") {
    ElfBuilder b;
    put_u16(b.file, 56, 100); // 100 headers, but the file holds one
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kBadProgramHeader);
}

HYPERION_TEST("elf", "rejects_filesz_greater_than_memsz") {
    ElfBuilder b;
    b.set_load(b.base, /*offset=*/0, /*filesz=*/8, /*memsz=*/1, /*flags=*/5);
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kBadSegment);
}

HYPERION_TEST("elf", "rejects_segment_bytes_past_eof") {
    ElfBuilder b;
    b.set_load(b.base, /*offset=*/0, /*filesz=*/0x2000, /*memsz=*/0x2000, /*flags=*/5);
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kBadSegment);
}

HYPERION_TEST("elf", "rejects_an_unaligned_segment") {
    ElfBuilder b;
    // vaddr and file offset are incongruent mod p_align: the ABI violation.
    b.set_load(b.base + 1, /*offset=*/0, 1, 1, 5);
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kMisalignedSegment);
}

HYPERION_TEST("elf", "rejects_an_image_without_load_segments") {
    ElfBuilder b;
    put_u32(b.file, b.ph + 0, 4); // PT_NOTE, not PT_LOAD
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(b.file.data(), b.file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kNoProgramHeaders);
}

HYPERION_TEST("elf", "detects_a_dynamic_interpreter") {
    // Two program headers: a PT_INTERP with a path, and the PT_LOAD.
    std::vector<std::uint8_t> file(64 + 2 * 56 + 64, 0);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    put_u16(file, 16, 2);
    put_u16(file, 18, 62);
    put_u64(file, 24, 0x400000);
    put_u64(file, 32, 64);
    put_u16(file, 54, 56);
    put_u16(file, 56, 2);
    // PT_INTERP at ph=64, string at the very end of the file.
    const std::size_t str_off = 64 + 2 * 56;
    const char* path = "/lib64/ld-linux-x86-64.so.2";
    std::memcpy(file.data() + str_off, path, std::strlen(path) + 1);
    put_u32(file, 64 + 0, 3); // PT_INTERP
    put_u64(file, 64 + 8, str_off);
    put_u64(file, 64 + 32, std::strlen(path) + 1);
    // PT_LOAD at ph=64+56.
    put_u32(file, 120 + 0, 1);
    put_u32(file, 120 + 4, 5);
    put_u64(file, 120 + 8, 0); // offset into the header area; size 0 so no read
    put_u64(file, 120 + 16, 0x400000);
    put_u64(file, 120 + 32, 0);
    put_u64(file, 120 + 40, 16);
    put_u64(file, 120 + 48, kPage);

    ElfError err = ElfError::kNone;
    auto img = hyperion::parse_elf64(file.data(), file.size(), err);
    HYPERION_CHECK(img.has_value());
    HYPERION_CHECK(!img->is_static());
    HYPERION_CHECK_EQ(img->interpreter, std::string(path));

    // Mapping must refuse it with a distinct reason, not load it.
    GuestAddressSpace space;
    GuestState state;
    HYPERION_CHECK(
        !hyperion::map_elf_image(*img, file.data(), file.size(), space, state, {"prog"}, err)
             .has_value());
    HYPERION_CHECK(err == ElfError::kNeedsInterpreter);
}

HYPERION_TEST("elf", "rejects_a_pt_interp_that_is_not_terminated") {
    std::vector<std::uint8_t> file(64 + 56 + 4, 0x41); // 'A' repeated, no NUL
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    put_u16(file, 16, 2);
    put_u16(file, 18, 62);
    put_u64(file, 32, 64);
    put_u16(file, 54, 56);
    put_u16(file, 56, 1);
    put_u32(file, 64 + 0, 3); // PT_INTERP
    put_u64(file, 64 + 8, 64 + 56);
    put_u64(file, 64 + 32, 4);
    ElfError err = ElfError::kNone;
    HYPERION_CHECK(!hyperion::parse_elf64(file.data(), file.size(), err).has_value());
    HYPERION_CHECK(err == ElfError::kBadInterpreter);
}

HYPERION_TEST("elf", "maps_a_static_image_and_sets_up_the_stack") {
    ElfBuilder b;
    ElfError err = ElfError::kNone;
    auto img = hyperion::parse_elf64(b.file.data(), b.file.size(), err);
    HYPERION_CHECK(img.has_value());

    GuestAddressSpace space;
    GuestState state;
    const std::vector<std::string> argv = {"/prog", "arg1"};
    auto entry =
        hyperion::map_elf_image(*img, b.file.data(), b.file.size(), space, state, argv, err);
    HYPERION_CHECK(entry.has_value());
    HYPERION_CHECK_EQ(*entry, b.entry);
    HYPERION_CHECK_EQ(state.rip, b.entry);

    // The entry point must be mapped executable and hold the `hlt` byte.
    std::uint8_t op = 0;
    HYPERION_CHECK(space.read(b.entry, &op, 1));
    HYPERION_CHECK_EQ(op, 0xF4u);
    const auto perm = space.perm_at(b.entry);
    HYPERION_CHECK(perm.has_value());
    HYPERION_CHECK(hyperion::has_perm(*perm, MemPerm::kExec));

    // rsp must point at argc, 16-byte aligned, with argc == 2 and the argv
    // pointers naming the strings.
    const std::uint64_t rsp = state.reg(hyperion::Gpr::kRsp);
    HYPERION_CHECK_EQ(rsp % 16, 0u);
    std::vector<std::uint8_t> word(8);
    HYPERION_CHECK(space.read(rsp, word.data(), 8));
    HYPERION_CHECK_EQ(get_u64(word, 0), 2u);

    HYPERION_CHECK(space.read(rsp + 8, word.data(), 8));
    const std::uint64_t a0 = get_u64(word, 0);
    HYPERION_CHECK(space.read(rsp + 16, word.data(), 8));
    const std::uint64_t a1 = get_u64(word, 0);
    HYPERION_CHECK(space.read(rsp + 24, word.data(), 8)); // argv terminator
    HYPERION_CHECK_EQ(get_u64(word, 0), 0u);

    // Read exactly the strings' lengths: the strings sit at the very top of the
    // stack region, so reading past the NUL would leave the mapping (correctly).
    std::vector<std::uint8_t> s0(6), s1(5);
    HYPERION_CHECK(space.read(a0, s0.data(), s0.size()));
    HYPERION_CHECK(space.read(a1, s1.data(), s1.size()));
    HYPERION_CHECK_EQ(std::string(reinterpret_cast<const char*>(s0.data())), std::string("/prog"));
    HYPERION_CHECK_EQ(std::string(reinterpret_cast<const char*>(s1.data())), std::string("arg1"));
}

HYPERION_TEST("elf", "map_refuses_an_empty_argv") {
    ElfBuilder b;
    ElfError err = ElfError::kNone;
    auto img = hyperion::parse_elf64(b.file.data(), b.file.size(), err);
    HYPERION_CHECK(img.has_value());
    GuestAddressSpace space;
    GuestState state;
    HYPERION_CHECK(
        !hyperion::map_elf_image(*img, b.file.data(), b.file.size(), space, state, {}, err)
             .has_value());
}

HYPERION_TEST("elf", "looks_like_elf_only_for_the_magic") {
    ElfBuilder b;
    HYPERION_CHECK(hyperion::looks_like_elf(b.file.data(), b.file.size()));
    const std::uint8_t not_elf[4] = {'E', 'L', 'F', 0};
    HYPERION_CHECK(!hyperion::looks_like_elf(not_elf, sizeof(not_elf)));
    HYPERION_CHECK(!hyperion::looks_like_elf(not_elf, 3));
}

HYPERION_TEST("address_space", "map_segment_zero_fills_bss_beyond_filesz") {
    GuestAddressSpace as;
    const std::uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    // memsz 16 > filesz 4: bytes 4..15 must read back as zero.
    HYPERION_CHECK(as.map_segment(0x400000, data, 4, 16, MemPerm::kRead | MemPerm::kWrite));
    std::uint8_t out[16] = {};
    HYPERION_CHECK(as.read(0x400000, out, sizeof(out)));
    HYPERION_CHECK(out[0] == 0xAA && out[3] == 0xDD);
    for (std::size_t i = 4; i < 16; ++i) {
        HYPERION_CHECK(out[i] == 0);
    }
}

HYPERION_TEST("address_space", "map_segment_shares_a_page_and_unions_permissions") {
    GuestAddressSpace as;
    const std::uint8_t r[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    // First segment: 8 file bytes but 2 pages of memory, so it spans the page at
    // 0x400000 and the page at 0x401000.
    HYPERION_CHECK(as.map_segment(0x400000, r, 8, 0x1800, MemPerm::kRead));
    // Second segment starts inside the first one's second page. The shared page
    // must keep the first segment's bytes, gain write permission (union), and
    // take the second segment's bytes at 0x401000.
    const std::uint8_t w[4] = {9, 9, 9, 9};
    HYPERION_CHECK(as.map_segment(0x401000, w, 4, 4, MemPerm::kWrite));
    std::uint8_t out[8] = {};
    HYPERION_CHECK(as.read(0x400000, out, 8)); // first segment's bytes survive
    HYPERION_CHECK(out[0] == 1 && out[7] == 8);
    HYPERION_CHECK(as.read(0x401000, out, 4)); // second segment's bytes landed
    HYPERION_CHECK(out[0] == 9 && out[3] == 9);
    const auto perm = as.perm_at(0x401000);
    HYPERION_CHECK(perm.has_value());
    HYPERION_CHECK(hyperion::has_perm(*perm, MemPerm::kRead));
    HYPERION_CHECK(hyperion::has_perm(*perm, MemPerm::kWrite));
}

HYPERION_TEST("address_space", "map_segment_rejects_filesz_over_memsz") {
    GuestAddressSpace as;
    const std::uint8_t data[8] = {};
    HYPERION_CHECK(!as.map_segment(0x400000, data, 8, 4, MemPerm::kRead));
    HYPERION_CHECK(!as.perm_at(0x400000).has_value());
}

// A segment whose vaddr is not page-aligned must have its bytes placed at the
// right in-page offset (the last PT_LOAD of a real binary is usually like this).
HYPERION_TEST("address_space", "map_segment_places_bytes_at_an_unaligned_offset") {
    GuestAddressSpace as;
    const std::uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    // vaddr 0x400010: 0x10 bytes into the page at 0x400000.
    HYPERION_CHECK(as.map_segment(0x400010, data, 4, 4, MemPerm::kRead));
    std::uint8_t out[4] = {};
    HYPERION_CHECK(as.read(0x400010, out, 4));
    HYPERION_CHECK(out[0] == 0xDE && out[3] == 0xEF);
    // The bytes before the segment in the same page must be zero (they were
    // mapped but not written), not the segment's first byte.
    HYPERION_CHECK(as.read(0x400000, out, 4));
    HYPERION_CHECK(out[0] == 0 && out[3] == 0);
}

// The ELF parser handles untrusted input, so it must survive arbitrary bytes
// without crashing or reading out of bounds. This is a bounded, deterministic
// pseudo-fuzz: the same seed always produces the same corpus, so a failure is
// reproducible, and ASan/UBSan builds turn any out-of-bounds access into a test
// failure. A real libFuzzer target can replace it once one exists.
HYPERION_TEST("elf", "survives_random_and_mutated_input") {
    std::uint32_t state = 0x12345678u;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (int iter = 0; iter < 2000; ++iter) {
        // A random length, biased small, with random bytes.
        const std::size_t len = next() % 300;
        std::vector<std::uint8_t> buf(len);
        for (std::uint8_t& b : buf) {
            b = static_cast<std::uint8_t>(next());
        }
        ElfError err = ElfError::kNone;
        (void)hyperion::parse_elf64(buf.data(), buf.size(), err);
    }

    // Now mutate a *valid* image one byte at a time: this reaches the deeper
    // paths (program headers, segment bounds) that random noise rarely does.
    for (std::size_t pos = 0; pos < 0x2000; pos += 7) {
        ElfBuilder b;
        b.file[pos % b.file.size()] ^= static_cast<std::uint8_t>(1 + (next() % 255));
        ElfError err = ElfError::kNone;
        auto img = hyperion::parse_elf64(b.file.data(), b.file.size(), err);
        // If it parsed, mapping must either succeed or refuse cleanly -- never
        // crash or read out of bounds.
        if (img) {
            GuestAddressSpace space;
            GuestState st;
            (void)hyperion::map_elf_image(*img, b.file.data(), b.file.size(), space, st, {"p"},
                                          err);
        }
    }
}
