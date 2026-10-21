// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/elf_loader.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/translation_cache.hpp"

namespace {

void w16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void w32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

void w64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

std::uint64_t r64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

// Build a valid minimal two-segment x86-64 static ELF64 image:
//   - LOAD[0] RX at 0x401000 containing `code`
//   - LOAD[1] RW at 0x402000 containing `data` + `.bss` zero-fill (`bss_extra`)
std::vector<std::uint8_t> make_test_elf64(const std::vector<std::uint8_t>& code,
                                          const std::vector<std::uint8_t>& data,
                                          std::size_t bss_extra = 256,
                                          std::uint16_t e_type = 2 /* ET_EXEC */,
                                          bool add_pt_interp = false) {
    const std::uint16_t phnum = add_pt_interp ? 3 : 2;
    const std::size_t hdr_bytes = 64 + static_cast<std::size_t>(phnum) * 56;
    const std::size_t code_off = 0x1000;
    const std::size_t data_off = 0x2000;
    const std::size_t total = data_off + data.size();
    std::vector<std::uint8_t> img(total, 0);

    // Elf64_Ehdr
    img[0] = 0x7F;
    img[1] = 'E';
    img[2] = 'L';
    img[3] = 'F';
    img[4] = 2; // ELFCLASS64
    img[5] = 1; // ELFDATA2LSB
    img[6] = 1; // EV_CURRENT
    w16(img.data() + 16, e_type);
    w16(img.data() + 18, 62); // EM_X86_64
    w32(img.data() + 20, 1);
    const std::uint64_t base = (e_type == 3) ? 0ULL : 0x400000ULL;
    w64(img.data() + 24, base + 0x1000ULL); // e_entry
    w64(img.data() + 32, 64ULL);            // e_phoff
    w16(img.data() + 52, 64);               // e_ehsize
    w16(img.data() + 54, 56);               // e_phentsize
    w16(img.data() + 56, phnum);

    // LOAD[0]: RX code segment at base + 0x1000
    std::uint8_t* ph0 = img.data() + 64;
    w32(ph0 + 0, 1);         // PT_LOAD
    w32(ph0 + 4, 0x4 | 0x1); // PF_R | PF_X
    w64(ph0 + 8, code_off);
    w64(ph0 + 16, base + 0x1000ULL);
    w64(ph0 + 24, base + 0x1000ULL);
    w64(ph0 + 32, code.size());
    w64(ph0 + 40, code.size());
    w64(ph0 + 48, 0x1000ULL);

    // LOAD[1]: RW data + .bss segment at base + 0x2000
    std::uint8_t* ph1 = img.data() + 64 + 56;
    w32(ph1 + 0, 1);         // PT_LOAD
    w32(ph1 + 4, 0x4 | 0x2); // PF_R | PF_W
    w64(ph1 + 8, data_off);
    w64(ph1 + 16, base + 0x2000ULL);
    w64(ph1 + 24, base + 0x2000ULL);
    w64(ph1 + 32, data.size());
    w64(ph1 + 40, data.size() + bss_extra);
    w64(ph1 + 48, 0x1000ULL);

    if (add_pt_interp) {
        const char interp[] = "/lib64/ld-linux-x86-64.so.2";
        std::memcpy(img.data() + hdr_bytes, interp, sizeof(interp));
        std::uint8_t* ph2 = img.data() + 64 + 112;
        w32(ph2 + 0, 3); // PT_INTERP
        w32(ph2 + 4, 0x4);
        w64(ph2 + 8, hdr_bytes);
        w64(ph2 + 32, sizeof(interp));
        w64(ph2 + 40, sizeof(interp));
    }

    if (!code.empty()) {
        std::memcpy(img.data() + code_off, code.data(), code.size());
    }
    if (!data.empty()) {
        std::memcpy(img.data() + data_off, data.data(), data.size());
    }
    return img;
}

} // namespace

HYPERION_TEST("elf_loader", "rejects_non_elf_truncated_wrong_arch_and_dynamic_interp") {
    const std::uint8_t raw[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xF4};
    HYPERION_CHECK(!hyperion::has_elf_magic(raw, sizeof(raw)));
    HYPERION_CHECK(hyperion::inspect_elf(raw, sizeof(raw)).error ==
                   hyperion::ElfLoadError::kNotElf);

    const std::uint8_t tiny_elf[] = {0x7F, 'E', 'L', 'F', 2, 1};
    HYPERION_CHECK(hyperion::has_elf_magic(tiny_elf, sizeof(tiny_elf)));
    HYPERION_CHECK(hyperion::inspect_elf(tiny_elf, sizeof(tiny_elf)).error ==
                   hyperion::ElfLoadError::kTruncated);

    // Wrong architecture (AArch64 EM_AARCH64 = 183)
    auto arm_elf = make_test_elf64({0xF4}, {});
    w16(arm_elf.data() + 18, 183);
    HYPERION_CHECK(hyperion::inspect_elf(arm_elf.data(), arm_elf.size()).error ==
                   hyperion::ElfLoadError::kUnsupportedMachine);

    // Dynamic binary with PT_INTERP is inspectable (`info.has_interp`) and
    // cleanly rejected with `kNeedsInterpreter` by static-only `load_elf_image`.
    const auto dyn_elf = make_test_elf64({0xF4}, {}, 0, 3 /* ET_DYN */, /*add_pt_interp=*/true);
    const auto dyn_info = hyperion::inspect_elf(dyn_elf.data(), dyn_elf.size());
    HYPERION_CHECK(dyn_info.ok());
    HYPERION_CHECK(dyn_info.has_interp);
    HYPERION_CHECK_EQ(dyn_info.interpreter, std::string("/lib64/ld-linux-x86-64.so.2"));

    hyperion::GuestAddressSpace as;
    const auto dyn_load = hyperion::load_elf_image(dyn_elf.data(), dyn_elf.size(), as);
    HYPERION_CHECK(!dyn_load.ok());
    HYPERION_CHECK(dyn_load.error == hyperion::ElfLoadError::kNeedsInterpreter);
}

HYPERION_TEST("elf_loader", "executes_static_elf64_with_sysv_stack_and_nx_segments") {
    // Program at 0x401000:
    //   pop rbx                  ; rbx = argc (== 3)
    //   mov eax, 1               ; sys_write
    //   mov edi, 1               ; stdout
    //   mov esi, 0x402000        ; "ELF64\n" in RW data segment
    //   mov edx, 6
    //   syscall
    //   mov eax, 231             ; sys_exit_group
    //   mov rdi, rbx             ; exit status = argc (3)
    //   syscall
    const std::vector<std::uint8_t> code = {
        0x5B,                         // pop rbx
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1
        0xBE, 0x00, 0x20, 0x40, 0x00, // mov esi, 0x402000
        0xBA, 0x06, 0x00, 0x00, 0x00, // mov edx, 6
        0x0F, 0x05,                   // syscall (write)
        0xB8, 0xE7, 0x00, 0x00, 0x00, // mov eax, 231 (exit_group)
        0x48, 0x89, 0xDF,             // mov rdi, rbx
        0x0F, 0x05,                   // syscall (exit_group)
    };
    const std::vector<std::uint8_t> data = {'E', 'L', 'F', '6', '4', '\n'};
    const auto elf = make_test_elf64(code, data, /*bss_extra=*/512);

    hyperion::GuestAddressSpace as(0x20000000ULL, 0x40000000ULL);
    const auto loaded = hyperion::load_elf_image(elf.data(), elf.size(), as);
    HYPERION_CHECK(loaded.ok());
    HYPERION_CHECK_EQ(loaded.entry, 0x401000ULL);

    // Code segment is RX (not writable); data segment is RW (not executable) with zeroed .bss.
    std::uint8_t probe = 0xCC;
    HYPERION_CHECK(!as.write(0x401000ULL, &probe, 1));
    HYPERION_CHECK(!as.fetch(0x402000ULL, &probe, 1));
    std::uint8_t bss_byte = 0xFF;
    HYPERION_CHECK(as.read(0x402100ULL, &bss_byte, 1));
    HYPERION_CHECK_EQ(bss_byte, 0u);

    const std::uint64_t rsp =
        hyperion::setup_linux_x86_64_stack(as, {"./hello", "alpha", "beta"}, &loaded);
    HYPERION_CHECK(rsp != 0);
    HYPERION_CHECK_EQ(rsp & 0xFULL, 0ULL); // System V AMD64 16-byte stack alignment

    std::uint8_t argc_buf[8] = {};
    HYPERION_CHECK(as.read(rsp, argc_buf, 8));
    HYPERION_CHECK_EQ(r64(argc_buf), 3ULL);

    std::string out;
    hyperion::MinimalSyscallHandler syscalls(
        [&out](int, const std::uint8_t* p, std::size_t n) -> long {
            out.append(reinterpret_cast<const char*>(p), n);
            return static_cast<long>(n);
        },
        &as);

    hyperion::GuestState st;
    st.rip = loaded.entry;
    st.reg(hyperion::Gpr::kRsp) = rsp;

    const auto res = hyperion::run_program(as, st, 1000, &syscalls);
    HYPERION_CHECK(res.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK(res.exited_via_syscall);
    HYPERION_CHECK_EQ(res.exit_code, 3);
    HYPERION_CHECK_EQ(out, std::string("ELF64\n"));
}

HYPERION_TEST("elf_loader", "executes_dynamic_pt_interp_elf64_via_ld_linux") {
    std::ifstream interp_f("/lib64/ld-linux-x86-64.so.2", std::ios::binary);
    std::ifstream echo_f("/bin/echo", std::ios::binary);
    if (!interp_f || !echo_f) {
        return;
    }
    const std::vector<std::uint8_t> interp_bytes((std::istreambuf_iterator<char>(interp_f)),
                                                 std::istreambuf_iterator<char>());
    const std::vector<std::uint8_t> echo_bytes((std::istreambuf_iterator<char>(echo_f)),
                                               std::istreambuf_iterator<char>());
    const auto interp_info = hyperion::inspect_elf(interp_bytes.data(), interp_bytes.size());
    const auto echo_info = hyperion::inspect_elf(echo_bytes.data(), echo_bytes.size());
    if (!interp_info.ok() || !echo_info.ok() || !echo_info.has_interp) {
        return;
    }

    hyperion::GuestAddressSpace as(0x20000000ULL, 0x80000000ULL);
    hyperion::GuestState st;
    const auto loaded = hyperion::load_linux_elf_process(
        echo_bytes.data(), echo_bytes.size(), as, st, {"/bin/echo", "hyperion-dyn-ok"}, {}, "");
    HYPERION_CHECK(loaded.ok());

    std::string out;
    hyperion::MinimalSyscallHandler syscalls(
        [&out](int fd, const std::uint8_t* p, std::size_t n) -> long {
            if (fd == 1) {
                out.append(reinterpret_cast<const char*>(p), n);
            }
            return static_cast<long>(n);
        },
        &as);
    syscalls.enable_linux_compat("");

    const auto res = hyperion::run_program(as, st, 5'000'000ULL, &syscalls);
    HYPERION_CHECK(res.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK(res.exited_via_syscall);
    HYPERION_CHECK_EQ(res.exit_code, 0);
    HYPERION_CHECK_EQ(out, std::string("hyperion-dyn-ok\n"));
}
