// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    const uint32_t limit = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 1000000u;
    uint8_t* composite = (uint8_t*)malloc(limit + 1);
    if (!composite)
        return 1;
    memset(composite, 0, limit + 1);
    composite[0] = 1;
    composite[1] = 1;
    for (uint32_t p = 2; (uint64_t)p * p <= limit; ++p) {
        if (!composite[p]) {
            for (uint32_t m = p * p; m <= limit; m += p) {
                composite[m] = 1;
            }
        }
    }
    uint32_t count = 0;
    uint64_t checksum = 0;
    for (uint32_t i = 2; i <= limit; ++i) {
        if (!composite[i]) {
            ++count;
            checksum = checksum * 1315423911ULL ^ i;
        }
    }
    printf("sieve limit=%u primes=%u checksum=%016llx\n", limit, count,
           (unsigned long long)checksum);
    free(composite);
    return 0;
}
