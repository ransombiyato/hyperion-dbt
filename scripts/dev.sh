#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Project HYPERION - developer task runner.
#
# Thin wrapper around CMake/CTest so the same commands work on a developer
# workstation, in CI, and on a constrained target device. Deliberately plain
# bash with no dependency beyond a C++20 compiler and CMake.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${HYPERION_BUILD_DIR:-${ROOT}/build/dev}"

usage() {
    cat <<'EOF'
usage: scripts/dev.sh <command>

commands:
  bootstrap   configure the default build tree
  build       build the default tree
  test        run the native test suite
  cross-test  build + run the AArch64 test suite under qemu-user
  sanitize    build and test with ASan/UBSan
  clang-build build + test with clang++ and warnings-as-errors (mirrors CI)
  format      check formatting (clang-format --dry-run)
  format-fix  apply clang-format
  bench       run the host benchmark harness
  clean       remove the default build tree
EOF
}

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' not found in PATH" >&2; exit 1; }
}

bootstrap() {
    need cmake
    cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
}

build() {
    # Configure on first use so `dev.sh build` works from a fresh clone.
    [ -f "${BUILD_DIR}/CMakeCache.txt" ] || bootstrap
    cmake --build "${BUILD_DIR}"
}

test() {
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
}

cross_test() {
    need cmake
    need aarch64-linux-gnu-g++
    # Prefer the repo-local cached emulator (tools/cache); it survives the
    # container resets that wipe /usr.
    local qemu="qemu-aarch64"
    if [ -n "${HYPERION_QEMU:-}" ]; then
        qemu="${HYPERION_QEMU}"
    elif [ -x "${ROOT}/tools/cache/bin/qemu-aarch64-static" ]; then
        qemu="${ROOT}/tools/cache/bin/qemu-aarch64-static"
    fi
    need "${qemu}"
    local dir="${ROOT}/build/cross-test"
    cmake -S "${ROOT}" -B "${dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DHYPERION_ENABLE_CROSS_TESTS=ON \
        -DHYPERION_AARCH64_CXX=aarch64-linux-gnu-g++ \
        -DHYPERION_QEMU_AARCH64="${qemu}" \
        -DHYPERION_BUILD_BENCHMARKS=OFF
    cmake --build "${dir}"
    ctest --test-dir "${dir}" --output-on-failure
}

sanitize() {
    local dir="${ROOT}/build/sanitize"
    cmake -S "${ROOT}" -B "${dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DHYPERION_ENABLE_SANITIZERS=ON
    cmake --build "${dir}"
    ctest --test-dir "${dir}" --output-on-failure
}

# CI also builds with clang++ and warnings-as-errors; gcc's -Wconversion is
# weaker than clang's -Wsign-conversion, so a build that is clean under gcc can
# still fail CI. Run this before pushing.
clang_build() {
    need cmake
    need clang++
    local dir="${ROOT}/build/clangcheck"
    cmake -S "${ROOT}" -B "${dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DHYPERION_WARNINGS_AS_ERRORS=ON
    cmake --build "${dir}"
    ctest --test-dir "${dir}" --output-on-failure
}

format_check() {
    need clang-format
    local files
    files="$(find "${ROOT}/src" "${ROOT}/include" "${ROOT}/tests" "${ROOT}/benchmarks" \
        -name '*.cpp' -o -name '*.hpp' 2>/dev/null)"
    # shellcheck disable=SC2086
    clang-format --dry-run --Werror ${files}
}

format_fix() {
    need clang-format
    local files
    files="$(find "${ROOT}/src" "${ROOT}/include" "${ROOT}/tests" "${ROOT}/benchmarks" \
        -name '*.cpp' -o -name '*.hpp' 2>/dev/null)"
    # shellcheck disable=SC2086
    clang-format -i ${files}
}

bench() {
    "${BUILD_DIR}/benchmarks/hyperion_bench" "$@"
}

clean() {
    rm -rf "${BUILD_DIR}"
}

case "${1:-}" in
    bootstrap) bootstrap ;;
    build) build ;;
    test) test ;;
    cross-test) cross_test ;;
    sanitize) sanitize ;;
    clang-build) clang_build ;;
    format) format_check ;;
    format-fix) format_fix ;;
    bench) shift; bench "$@" ;;
    clean) clean ;;
    ""|-h|--help|help) usage ;;
    *) echo "error: unknown command '$1'" >&2; usage; exit 1 ;;
esac
