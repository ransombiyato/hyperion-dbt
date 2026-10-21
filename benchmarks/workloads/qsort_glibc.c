// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int cmp_i64(const void* a, const void* b) {
    const int64_t va = *(const int64_t*)a;
    const int64_t vb = *(const int64_t*)b;
    return (va > vb) - (va < vb);
}

int main(int argc, char** argv) {
    const size_t n = (argc > 1) ? (size_t)strtoul(argv[1], NULL, 10) : 50000u;
    int64_t* arr = (int64_t*)malloc(n * sizeof(int64_t));
    if (!arr)
        return 1;
    uint64_t s = 0xdeadbeefcafef00dULL;
    for (size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        arr[i] = (int64_t)s;
    }
    qsort(arr, n, sizeof(int64_t), cmp_i64);
    for (size_t i = 1; i < n; ++i) {
        if (arr[i - 1] > arr[i])
            return 2;
    }
    printf("qsort_glibc n=%zu min=%lld mid=%lld max=%lld\n", n, (long long)arr[0],
           (long long)arr[n / 2], (long long)arr[n - 1]);
    free(arr);
    return 0;
}
