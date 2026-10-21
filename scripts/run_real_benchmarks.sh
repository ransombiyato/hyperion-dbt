#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# End-to-end correctness & multi-sample statistical benchmark runner for real
# compiled `-O2 -static` glibc C binaries across:
#   - Native x86-64 hardware
#   - HYPERION Tier 0 Fast Interpreter (x86-64 host)
#   - Native AArch64 binary under `qemu-aarch64` (when available)
#   - HYPERION Tier 0 Fast Interpreter under `qemu-aarch64`
#   - HYPERION Tier 1 Native ARMv8 + NEON JIT under `qemu-aarch64`
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
eval "$("$REPO_ROOT/scripts/toolcache.sh" env)"

SAMPLES="${HYPERION_BENCH_SAMPLES:-5}"
NATIVE_HYPERION="${1:-$REPO_ROOT/build/dev/hyperion}"
CROSS_HYPERION="${2:-$REPO_ROOT/build/cross-test/cross-aarch64/hyperion}"

if [[ ! -x "$NATIVE_HYPERION" ]]; then
    echo "building native hyperion..." >&2
    "$REPO_ROOT/scripts/dev.sh" build
fi

QEMU_BIN="$(command -v qemu-aarch64-static || command -v qemu-aarch64 || true)"
AARCH64_GCC="$(command -v aarch64-linux-gnu-gcc || true)"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

WORKLOADS=(sieve sha256 qsort_glibc quicksort_c nqueens)
for w in "${WORKLOADS[@]}"; do
    gcc -O2 -static "$REPO_ROOT/benchmarks/workloads/${w}.c" -o "$WORKDIR/${w}_x86_64"
    if [[ -n "$AARCH64_GCC" ]]; then
        "$AARCH64_GCC" -O2 -static "$REPO_ROOT/benchmarks/workloads/${w}.c" -o "$WORKDIR/${w}_aarch64" 2>/dev/null || true
    fi
done

python3 - "$WORKDIR" "$NATIVE_HYPERION" "$CROSS_HYPERION" "$QEMU_BIN" "$SAMPLES" << 'PY'
import statistics
import subprocess
import sys
import time
import re
from pathlib import Path

workdir = Path(sys.argv[1])
native_bin = sys.argv[2]
cross_bin = sys.argv[3]
qemu_bin = sys.argv[4]
samples = int(sys.argv[5])
if samples < 3:
    raise SystemExit("HYPERION_BENCH_SAMPLES must be >= 3 per docs/BENCHMARK_RULES.md")

workloads = ["sieve", "sha256", "qsort_glibc", "quicksort_c", "nqueens"]

def measure(cmd, check_substr=None):
    # Discarded warmup run per BENCHMARK_RULES.md
    w = subprocess.run(cmd, capture_output=True, text=True, check=True)
    out_text = w.stdout
    if check_substr is not None and check_substr not in out_text:
        raise RuntimeError(f"Output mismatch!\nExpected: {check_substr}\nGot:\n{out_text}")
    m = re.search(r"^steps:\s+(\d+)", out_text, re.M)
    steps = int(m.group(1)) if m else 0
    times = []
    for _ in range(samples):
        t0 = time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        times.append(time.perf_counter() - t0)
    times.sort()
    med = statistics.median(times)
    q1 = statistics.quantiles(times, n=4)[0]
    q3 = statistics.quantiles(times, n=4)[2]
    return {
        "steps": steps,
        "min": times[0],
        "med": med,
        "iqr": q3 - q1,
        "max": times[-1],
    }

print(f"=== HYPERION Real Compiled `-O2 -static` Binary Benchmark (N={samples}, 1 warmup discarded) ===")
has_cross = bool(qemu_bin and Path(cross_bin).is_file())

for w in workloads:
    x86_elf = str(workdir / f"{w}_x86_64")
    a64_elf = workdir / f"{w}_aarch64"
    ref_out = subprocess.check_output([x86_elf], text=True).strip()
    nat_x86 = measure([x86_elf], ref_out)
    t0_x86 = measure([native_bin, "run", "--tier=0", x86_elf], ref_out)
    steps = t0_x86["steps"]
    mips_x86 = (steps / 1e6) / t0_x86["med"]
    ns_x86 = (t0_x86["med"] * 1e9) / steps
    print(f"\n[{w}] stdout=\"{ref_out}\" | guest_insns={steps:,}")
    print(f"  Native x86-64      : med={nat_x86['med']*1000:7.2f} ms (IQR {nat_x86['iqr']*1000:5.2f} ms, min {nat_x86['min']*1000:6.2f} ms)")
    print(f"  Tier 0 (x86-64)    : med={t0_x86['med']*1000:7.2f} ms (IQR {t0_x86['iqr']*1000:5.2f} ms) | {mips_x86:6.2f} MIPS | {ns_x86:6.2f} ns/insn")
    if has_cross:
        if a64_elf.is_file():
            nat_a64 = measure([qemu_bin, str(a64_elf)], ref_out)
            print(f"  Native AArch64(qemu): med={nat_a64['med']*1000:7.2f} ms (IQR {nat_a64['iqr']*1000:5.2f} ms)")
        t0_a64 = measure([qemu_bin, cross_bin, "run", "--tier=0", x86_elf], ref_out)
        t1_a64 = measure([qemu_bin, cross_bin, "run", "--tier=1", x86_elf], ref_out)
        assert t0_a64["steps"] == t1_a64["steps"] == steps, (
            f"Step mismatch: x86_t0={steps}, a64_t0={t0_a64['steps']}, a64_t1={t1_a64['steps']}"
        )
        mips_t0 = (steps / 1e6) / t0_a64["med"]
        mips_t1 = (steps / 1e6) / t1_a64["med"]
        ns_t1 = (t1_a64["med"] * 1e9) / steps
        speedup = t0_a64["med"] / t1_a64["med"]
        print(f"  Tier 0 (qemu-a64)  : med={t0_a64['med']*1000:7.2f} ms (IQR {t0_a64['iqr']*1000:5.2f} ms) | {mips_t0:6.2f} MIPS")
        print(f"  Tier 1 JIT(qemu-a64): med={t1_a64['med']*1000:7.2f} ms (IQR {t1_a64['iqr']*1000:5.2f} ms) | {mips_t1:6.2f} MIPS | {ns_t1:6.2f} ns/insn | {speedup:5.2f}x vs Tier 0")
PY
