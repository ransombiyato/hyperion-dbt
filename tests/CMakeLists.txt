# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Test suite. Uses CTest as the single entry point so that native tests and
# cross-architecture tests (run under qemu-user) look identical to CI.

# Shared test harness, linked into every test binary.
add_library(hyperion_test_harness STATIC harness/test_main.cpp)
target_include_directories(hyperion_test_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hyperion_test_harness PUBLIC hyperion::core hyperion_warnings)

# --- Native unit tests ------------------------------------------------------
add_executable(hyperion_unit_tests
    unit/test_version.cpp
    unit/test_host_probe.cpp
    unit/test_bench_stats.cpp
    unit/test_guest_state.cpp
    unit/test_decoder.cpp
    unit/test_aarch64_encoder.cpp
    unit/test_exec_mem.cpp
    unit/test_translator.cpp
    unit/test_translation_cache.cpp
    unit/test_interpreter.cpp
    unit/test_syscall.cpp
    unit/test_runtime_helpers.cpp
    unit/test_address_space.cpp
    unit/test_elf.cpp)
target_link_libraries(hyperion_unit_tests PRIVATE hyperion_test_harness)

add_test(NAME unit.native COMMAND hyperion_unit_tests)

# --- CLI smoke test ---------------------------------------------------------
#
# Drives the real `hyperion run` binary through both execution tiers. On an
# AArch64 host tier 1 executes generated code; on an x86-64 host it falls back
# to the interpreter. Either way both tiers must agree.
add_test(NAME cli.smoke
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_smoke.sh" $<TARGET_FILE:hyperion>)

# --- ELF-path CLI test ------------------------------------------------------
#
# `hyperion info`/`run` on a real (if tiny) ELF64 image, plus the two failure
# paths a user hits: a non-ELF file and a truncated ELF.
add_test(NAME cli.elf
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/elf_test.sh"
            $<TARGET_FILE:hyperion>
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tiny-static.elf")

# --- `hyperion` wrapper test ------------------------------------------------
#
# The installed `hyperion` command (scripts/hyperion) is a thin front end over
# the real binary. It is user-facing, so it must not drift from the CLI it wraps.
add_test(NAME cli.wrapper
    COMMAND ${CMAKE_COMMAND} -E env
            HYPERION_SRC=${CMAKE_SOURCE_DIR}
            HYPERION_BUILD_DIR=${CMAKE_BINARY_DIR}
            bash "${CMAKE_CURRENT_SOURCE_DIR}/cli/wrapper_test.sh"
            $<TARGET_FILE:hyperion>)

# --- Differential tests (interpreter vs. real x86-64 hardware) ---------------
#
# Only meaningful on an x86-64 host. On other hosts (the AArch64 cross-test
# path under qemu-user) the cases detect that and report a skip instead of a
# false pass. It is a separate binary so its hardware dependence is explicit.
add_executable(hyperion_differential_tests
    differential/test_differential.cpp)
target_link_libraries(hyperion_differential_tests PRIVATE hyperion_test_harness)
add_test(NAME differential.native_vs_interpreter COMMAND hyperion_differential_tests)

# --- Cross-architecture validation pipeline ---------------------------------
#
# On x86-64 developer hosts the AArch64 backend cannot be executed natively.
# This test proves the *validation path* works: it configures a second build
# tree with the AArch64 cross toolchain, builds the same test suite, runs it
# under qemu-user, and requires it to pass. Phase 3+ reuses exactly this
# mechanism to validate generated AArch64 code against the x86-64 oracle.
option(HYPERION_ENABLE_CROSS_TESTS "Run AArch64 tests under qemu-user" OFF)
set(HYPERION_AARCH64_CXX "" CACHE STRING "AArch64 cross compiler")
set(HYPERION_QEMU_AARCH64 "" CACHE STRING "AArch64 user-mode emulator")

if(HYPERION_ENABLE_CROSS_TESTS)
    add_test(NAME cross.aarch64_tests
        COMMAND ${CMAKE_COMMAND}
            -DHYPERION_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DHYPERION_CROSS_BINARY_DIR=${CMAKE_BINARY_DIR}/cross-aarch64
            -DHYPERION_AARCH64_CXX=${HYPERION_AARCH64_CXX}
            -DHYPERION_QEMU=${HYPERION_QEMU_AARCH64}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_cross_tests.cmake)
endif()

# --- Syscall inventory validation -------------------------------------------
#
# The seven syscall inventory files (docs/syscalls/U1..U4.md, E1..E3.md) are
# generated from docs/syscalls/inventory.tsv and must stay complete and
# consistent with the pinned kernel syscall table. This test fails if any
# syscall is missing, duplicated, misnumbered, or mis-tiered, so the inventory
# cannot silently drift.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME syscall.inventory_valid
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/gen_syscall_inventory.py --check)
endif()
