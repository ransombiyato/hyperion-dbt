#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Build self-contained release tarballs so a user can run HYPERION without a
# toolchain. Produces, under <outdir> (default dist/):
#
#   hyperion-<version>-x86_64-linux.tar.gz
#   hyperion-<version>-aarch64-linux.tar.gz
#   SHA256SUMS
#
# Each tarball contains a statically linked `hyperion` binary, the licence, and
# the three documents a first-time user needs (README, STATUS, COMPATIBILITY).
# Static linking is deliberate: the target device is AArch64 Android with no
# root, so a single file with no shared-library or loader dependency is the
# artefact that actually runs there.
#
# Usage:
#   scripts/package.sh [--out DIR] [--no-cross] [--version X.Y.Z]
#
# AArch64 packaging needs aarch64-linux-gnu-g++; pass --no-cross to skip it
# (the x86-64 tarball is still produced).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/dist"
DO_CROSS=1
VERSION=""

while [ $# -gt 0 ]; do
    case "$1" in
        --out)      OUT="$2"; shift 2 ;;
        --out=*)    OUT="${1#*=}"; shift ;;
        --version)  VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --no-cross) DO_CROSS=0; shift ;;
        -h|--help)  sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; exit 2 ;;
    esac
done

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' is required" >&2; exit 1; }
}

need cmake
need tar

if [ -z "${VERSION}" ]; then
    VERSION="$(sed -n 's/^inline constexpr int kVersionMajor = \(.*\);/\1/p' \
        "${ROOT}/include/hyperion/version.hpp")"
    minor="$(sed -n 's/^inline constexpr int kVersionMinor = \(.*\);/\1/p' \
        "${ROOT}/include/hyperion/version.hpp")"
    patch="$(sed -n 's/^inline constexpr int kVersionPatch = \(.*\);/\1/p' \
        "${ROOT}/include/hyperion/version.hpp")"
    VERSION="${VERSION}.${minor}.${patch}"
fi

mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

# A generator must be named explicitly when the default one is unknown; Ninja
# is the documented tool and is checked for, with a Makefile fallback.
# An array, so the generator name is one argument and shellcheck sees no
# unquoted word splitting.
GENERATOR=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR=(-G Ninja)
fi

# SHA-256, whichever tool the host has.
hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "error: need sha256sum or shasum" >&2; exit 1
    fi
}

stage() {
    # $1 = arch label, $2 = binary path
    local label="$1" bin="$2"
    local name="hyperion-${VERSION}-${label}-linux"
    local dir="${OUT}/${name}"
    rm -rf "${dir}"
    mkdir -p "${dir}"
    install -m 0755 "${bin}" "${dir}/hyperion"
    for doc in README.md STATUS.md COMPATIBILITY.md LICENSE; do
        [ -f "${ROOT}/${doc}" ] && install -m 0644 "${ROOT}/${doc}" "${dir}/${doc}"
    done
    tar -C "${OUT}" -czf "${OUT}/${name}.tar.gz" "${name}"
    rm -rf "${dir}"
    echo "  ${OUT}/${name}.tar.gz"
}

echo "Packaging HYPERION ${VERSION}"

# --- x86-64 -----------------------------------------------------------------
echo "Building x86-64 (static)"
cmake -S "${ROOT}" -B "${ROOT}/build/package-x86_64" "${GENERATOR[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DHYPERION_BUILD_TESTS=OFF \
    -DHYPERION_BUILD_BENCHMARKS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static" >/dev/null
cmake --build "${ROOT}/build/package-x86_64" >/dev/null
stage x86_64 "${ROOT}/build/package-x86_64/hyperion"

# --- aarch64 ----------------------------------------------------------------
if [ "${DO_CROSS}" -eq 1 ]; then
    if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
        echo "Building aarch64 (static, cross)"
        cmake -S "${ROOT}" -B "${ROOT}/build/package-aarch64" "${GENERATOR[@]}" \
            -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchains/aarch64-linux-gnu.cmake" \
            -DCMAKE_BUILD_TYPE=Release \
            -DHYPERION_BUILD_TESTS=OFF \
            -DHYPERION_BUILD_BENCHMARKS=OFF >/dev/null
        cmake --build "${ROOT}/build/package-aarch64" >/dev/null
        stage aarch64 "${ROOT}/build/package-aarch64/hyperion"
    else
        echo "Skipping aarch64: aarch64-linux-gnu-g++ not found" >&2
    fi
else
    echo "Skipping aarch64 (--no-cross)"
fi

# --- checksums --------------------------------------------------------------
echo "Writing checksums"
(
    cd "${OUT}"
    : > SHA256SUMS
    for f in hyperion-*.tar.gz; do
        [ -f "$f" ] || continue
        printf '%s  %s\n' "$(hash_file "$f")" "$f" >> SHA256SUMS
    done
)
cat "${OUT}/SHA256SUMS"
