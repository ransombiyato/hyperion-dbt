// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Runtime probing of the *host* machine.
//
// HYPERION targets AArch64 Linux but is developed on mixed hosts. Capabilities
// that affect translation correctness (page size, instruction-cache geometry,
// W^X support) are therefore probed at runtime and reported, never assumed.
#ifndef HYPERION_HOST_PROBE_HPP
#define HYPERION_HOST_PROBE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace hyperion {

// Architectures the probe can positively identify.
enum class HostArch : std::uint8_t {
    kUnknown,
    kX86_64,
    kAarch64,
};

const char* to_string(HostArch arch);

// Snapshot of measured host properties.
struct HostInfo {
    HostArch arch = HostArch::kUnknown;
    std::string os_name; // e.g. "Linux"
    std::string kernel_release;
    std::uint32_t logical_cpus = 0;
    std::size_t page_size = 0;       // getpagesize()/sysconf result
    std::size_t cache_line_size = 0; // 0 when undeterminable
    bool transparent_hugepages = false;
    bool has_wx_violation_support = false; // true when W^X enforcement is expected
};

// Probe the running host. Never throws; fields stay at defaults on failure.
HostInfo probe_host();

// Multi-line, human-readable report suitable for bug reports and benchmark logs.
std::string format_host_info(const HostInfo& info);

} // namespace hyperion

#endif // HYPERION_HOST_PROBE_HPP
