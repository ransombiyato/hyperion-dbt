// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void quicksort(int32_t* a, int lo, int hi) {
    while (lo < hi) {
        if (hi - lo < 16) {
            for (int i = lo + 1; i <= hi; ++i) {
                int32_t key = a[i];
                int j = i - 1;
                while (j >= lo && a[j] > key) {
                    a[j + 1] = a[j];
                    --j;
                }
                a[j + 1] = key;
            }
            return;
        }
        int mid = lo + ((hi - lo) >> 1);
        int32_t pivot = a[mid];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot)
                ++i;
            while (a[j] > pivot)
                --j;
            if (i <= j) {
                int32_t t = a[i];
                a[i] = a[j];
                a[j] = t;
                ++i;
                --j;
            }
        }
        if (j - lo < hi - i) {
            if (lo < j)
                quicksort(a, lo, j);
            lo = i;
        } else {
            if (i < hi)
                quicksort(a, i, hi);
            hi = j;
        }
    }
}

int main(int argc, char** argv) {
    const int n = (argc > 1) ? atoi(argv[1]) : 100000;
    int32_t* a = (int32_t*)malloc((size_t)n * sizeof(int32_t));
    if (!a)
        return 1;
    uint32_t x = 0x12345678u;
    for (int i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        a[i] = (int32_t)x;
    }
    quicksort(a, 0, n - 1);
    uint64_t acc = 0;
    for (int i = 0; i < n; ++i) {
        if (i > 0 && a[i - 1] > a[i])
            return 2;
        acc = acc * 31u + (uint32_t)a[i];
    }
    printf("quicksort_c n=%d min=%d max=%d hash=%016llx\n", n, a[0], a[n - 1],
           (unsigned long long)acc);
    free(a);
    return 0;
}
