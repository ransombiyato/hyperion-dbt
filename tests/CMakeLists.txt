# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors

add_library(hyperion_test_harness STATIC harness/test_main.cpp)
target_include_directories(hyperion_test_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hyperion_test_harness PUBLIC hyperion::core hyperion_warnings)

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
    unit/test_elf.cpp
    unit/test_elf_loader.cpp)
target_link_libraries(hyperion_unit_tests PRIVATE hyperion_test_harness)

add_test(NAME unit.native COMMAND hyperion_unit_tests)

add_test(NAME cli.smoke
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_smoke.sh" $<TARGET_FILE:hyperion>)

add_test(NAME cli.elf
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/elf_test.sh"
            $<TARGET_FILE:hyperion>
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tiny-static.elf")

add_test(NAME cli.wrapper
    COMMAND ${CMAKE_COMMAND} -E env
            HYPERION_SRC=${CMAKE_SOURCE_DIR}
            HYPERION_BUILD_DIR=${CMAKE_BINARY_DIR}
            bash "${CMAKE_CURRENT_SOURCE_DIR}/cli/wrapper_test.sh"
            $<TARGET_FILE:hyperion>)

add_executable(hyperion_differential_tests
    differential/test_differential.cpp)
target_link_libraries(hyperion_differential_tests PRIVATE hyperion_test_harness)
add_test(NAME differential.native_vs_interpreter COMMAND hyperion_differential_tests)

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

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME syscall.inventory_valid
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/gen_syscall_inventory.py --check)
endif()
