#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Repo-local tool cache.
#
# The development container does NOT persist /usr between sessions, but the
# repository directory does. Re-running scripts/bootstrap.sh after every reset
# is slow and depends on the package mirror, so the tools the cross-validation
# pipeline needs are copied into tools/cache/ (git-ignored, so binaries never
# enter git) and reused from there when present.
#
# Cached by default:
#   bin/qemu-aarch64-static   the user-mode AArch64 emulator used to execute
#                             Hyperion's generated AArch64 code on an x86-64 host
#
# Optionally cached with --with-toolchain (much larger, ~150 MB):
#   toolchain/*.tar.gz        the AArch64 cross compiler, its internal libs and
#                             the /usr/aarch64-linux-gnu sysroot
#
# Usage:
#   scripts/toolcache.sh populate [--with-toolchain]
#   scripts/toolcache.sh restore        # reinstate a cached toolchain after a reset
#   scripts/toolcache.sh status
#   scripts/toolcache.sh env            # print export lines for the current shell
#
# Environment:
#   HYPERION_TOOLCACHE   override the cache directory (default tools/cache)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache="${HYPERION_TOOLCACHE:-${root}/tools/cache}"
bin_dir="${cache}/bin"
tc_dir="${cache}/toolchain"

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
fi

# The tools we prefer to run from the cache. Each is a "system name:cache name"
# pair; a cached copy always wins so a session reset cannot change which
# compiler or emulator a run used.
qemu_system="${HYPERION_QEMU:-qemu-aarch64}"
qemu_cached="${bin_dir}/qemu-aarch64-static"

die() {
    echo "toolcache: $*" >&2
    exit 1
}

# A statically linked qemu is preferred because it needs no interpreter or
# shared libraries, which is exactly what makes it survive a container reset.
cache_qemu() {
    mkdir -p "${bin_dir}"
    local src=""
    if command -v qemu-aarch64-static >/dev/null 2>&1; then
        src="$(command -v qemu-aarch64-static)"
    elif [ -x /usr/bin/qemu-aarch64-static ]; then
        src=/usr/bin/qemu-aarch64-static
    elif command -v "${qemu_system}" >/dev/null 2>&1; then
        src="$(command -v "${qemu_system}")"
    fi
    [ -n "${src}" ] || die "no qemu-aarch64-static or ${qemu_system} found; run scripts/bootstrap.sh first"

    cp -f "${src}" "${qemu_cached}.tmp"
    chmod 0755 "${qemu_cached}.tmp"
    mv -f "${qemu_cached}.tmp" "${qemu_cached}"
    echo "  cached ${qemu_cached}  ($(du -h "${qemu_cached}" | cut -f1), from ${src})"
}

cache_toolchain() {
    mkdir -p "${tc_dir}"
    local tarball="${tc_dir}/aarch64-toolchain.tar.gz"
    local paths=()
    local p
    for p in /usr/aarch64-linux-gnu /usr/lib/gcc-cross/aarch64-linux-gnu \
        /usr/bin/aarch64-linux-gnu-gcc /usr/bin/aarch64-linux-gnu-g++ \
        /usr/bin/aarch64-linux-gnu-ld /usr/bin/aarch64-linux-gnu-as \
        /usr/bin/aarch64-linux-gnu-ar /usr/bin/aarch64-linux-gnu-objdump \
        /usr/bin/aarch64-linux-gnu-nm /usr/bin/aarch64-linux-gnu-strip; do
        [ -e "${p}" ] && paths+=("${p}")
    done
    shopt -s nullglob
    for p in /usr/lib/x86_64-linux-gnu/libopcodes*arm64* \
        /usr/lib/x86_64-linux-gnu/libbfd*arm64* \
        /usr/lib/x86_64-linux-gnu/libsframe* \
        /usr/lib/x86_64-linux-gnu/libctf* \
        /usr/lib/x86_64-linux-gnu/libzstd*; do
        [ -e "${p}" ] && paths+=("${p}")
    done
    shopt -u nullglob
    [ ${#paths[@]} -gt 0 ] || die "no AArch64 cross toolchain found; run scripts/bootstrap.sh first"

    echo "  archiving ${#paths[@]} path(s) into ${tarball} ..."
    tar -czf "${tarball}.tmp" -C / "${paths[@]#/}"
    mv -f "${tarball}.tmp" "${tarball}"
    echo "  cached ${tarball}  ($(du -h "${tarball}" | cut -f1))"
}

populate() {
    local with_toolchain=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
        --with-toolchain) with_toolchain=1 ;;
        *) die "unknown argument: $1" ;;
        esac
        shift
    done
    echo "==> populating tool cache at ${cache}"
    cache_qemu
    [ "${with_toolchain}" -eq 1 ] && cache_toolchain
    echo "==> done"
}

restore() {
    local tarball="${tc_dir}/aarch64-toolchain.tar.gz"
    if [ ! -f "${tarball}" ]; then
        echo "toolcache: no cached toolchain at ${tarball}; run 'populate --with-toolchain' first" >&2
        return 1
    fi
    echo "==> restoring AArch64 cross toolchain from ${tarball} (needs root)"
    ${SUDO} tar -xzf "${tarball}" -C /
    ${SUDO} ldconfig
    echo "==> restored; verify with: aarch64-linux-gnu-g++ --version"
}

# Host build tools (cmake/ninja/shellcheck) are not in the base image, so a
# container reset removes them. Small .deb files are committed under
# tools/vendor/host-debs so the build can be restored without a package mirror.
# apt resolves any missing dependency from the network when it can, and reports
# clearly when it cannot.
vendor_dir="${root}/tools/vendor/host-debs"

install_host() {
    if [ ! -d "${vendor_dir}" ]; then
        die "no vendored host tools at ${vendor_dir}"
    fi
    local debs=("${vendor_dir}"/*.deb)
    if [ ! -e "${debs[0]}" ]; then
        die "no .deb files in ${vendor_dir}"
    fi
    echo "==> installing ${#debs[@]} vendored host package(s) (needs root)"
    # --no-download prefers the local files; apt fetches only unmet dependencies.
    if ! ${SUDO} apt-get install -y --no-download "${debs[@]}"; then
        echo "toolcache: offline install failed; refreshing package index and" >&2
        echo "           retrying with the network for unmet dependencies ..." >&2
        ${SUDO} apt-get update
        ${SUDO} apt-get install -y "${debs[@]}"
    fi
    echo "==> done; verify with: cmake --version && ninja --version && shellcheck --version"
}

status() {
    echo "cache directory: ${cache}"
    if [ -x "${qemu_cached}" ]; then
        echo "  qemu      : ${qemu_cached} ($(du -h "${qemu_cached}" | cut -f1))"
    else
        echo "  qemu      : not cached"
    fi
    local tarball="${tc_dir}/aarch64-toolchain.tar.gz"
    if [ -f "${tarball}" ]; then
        echo "  toolchain : ${tarball} ($(du -h "${tarball}" | cut -f1))"
    else
        echo "  toolchain : not cached"
    fi
    if [ -d "${vendor_dir}" ]; then
        echo "  host debs : ${vendor_dir} ($(du -sh "${vendor_dir}" 2>/dev/null | cut -f1), committed)"
    else
        echo "  host debs : not vendored"
    fi
    local t
    for t in cmake ninja shellcheck clang-format; do
        printf '  %-10s: %s\n' "${t}" "$(command -v "${t}" 2>/dev/null || echo 'not installed')"
    done
}

print_env() {
    echo "export HYPERION_TOOLCACHE='${cache}'"
    if [ -x "${qemu_cached}" ]; then
        echo "export HYPERION_QEMU='${qemu_cached}'"
        echo "export PATH='${bin_dir}':\"\$PATH\""
    fi
}

command="${1:-status}"
shift || true
case "${command}" in
populate) populate "$@" ;;
restore) restore ;;
install-host) install_host ;;
status) status ;;
env) print_env ;;
*) die "unknown command: ${command} (expected populate|restore|install-host|status|env)" ;;
esac
