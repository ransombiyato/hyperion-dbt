// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t solve(uint32_t full, uint32_t cols, uint32_t diag1, uint32_t diag2) {
    if (cols == full)
        return 1;
    uint64_t total = 0;
    uint32_t avail = full & ~(cols | diag1 | diag2);
    while (avail) {
        uint32_t bit = avail & (~avail + 1u);
        avail -= bit;
        total += solve(full, cols | bit, (diag1 | bit) << 1, (diag2 | bit) >> 1);
    }
    return total;
}

int main(int argc, char** argv) {
    const unsigned n = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 10) : 12u;
    const uint32_t full = (1u << n) - 1u;
    const uint64_t ways = solve(full, 0, 0, 0);
    printf("nqueens n=%u solutions=%llu\n", n, (unsigned long long)ways);
    return 0;
}
