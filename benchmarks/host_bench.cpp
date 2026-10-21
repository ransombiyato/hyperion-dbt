// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Host benchmark harness.
//
// Scope: establishes a reproducible baseline *methodology* before any guest
// execution exists. It records the measured host fingerprint, runs a small
// deterministic calibration workload, and emits machine-readable JSON.
//
// Methodology implemented here (see BENCHMARKS.md section 5):
//   * repeated runs, reported as median with spread - never a single sample;
//   * one discarded warm-up run, so the first (cold-cache) sample does not
//     pollute the result;
//   * a correctness checksum compared across runs, so a benchmark cannot
//     silently measure a broken build;
//   * an explicit noise indicator (IQR / median) so a reader can tell a
//     trustworthy measurement from a noisy host.
//
// It intentionally contains no guest-execution metrics: those fields are
// reported as unavailable until the translator exists, so that reports can
// never be mistaken for fabricated performance claims.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hyperion/bench_stats.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/host_probe.hpp"
#include "hyperion/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Compiler barrier. Prevents the optimiser from hoisting, sinking, merging or
// eliminating the surrounding computation, and forces the accumulator to be
// materialised rather than folded.
//
// This is not decoration. Without it the harness reports a *fabricated*
// measurement: the repeated call is loop-invariant, so -O2 hoists it out of the
// repeat loop and every timed region collapses to near zero. That failure is
// silent - the checksum stays correct - so it must be guarded structurally.
// See docs/known-issues.md BENCH-001.
inline void optimization_barrier(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(p) : "memory");
#else
    // Fallback for compilers without inline asm: a volatile store to a volatile
    // pointer the optimiser cannot reason about. Weaker, but still prevents
    // elimination of the store.
    *static_cast<volatile const void**>(const_cast<const void**>(&p)) = p;
#endif
}

// Deterministic integer workload. The result is checked against an independent
// evaluation, so a benchmark cannot silently measure a broken build.
std::uint64_t calibration_workload(std::uint64_t iterations) {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state += i;
    }
    return state;
}

// One timed sample. The barrier forces the work to happen between the two clock
// reads and to be observable afterwards.
std::uint64_t timed_sample(std::uint64_t iterations, double& seconds_out) {
    const auto start = Clock::now();
    const std::uint64_t result = calibration_workload(iterations);
    optimization_barrier(&result);
    const auto end = Clock::now();
    seconds_out = std::chrono::duration<double>(end - start).count();
    return result;
}

struct Options {
    std::uint64_t iterations = 1'000'000;
    std::uint32_t repeats = 9; // odd, so the median is a real sample
    bool json = false;
};

void usage() {
    std::fprintf(stderr, "usage: hyperion_bench [--iterations N] [--repeats N] [--json]\n"
                         "  --iterations N  work per sample (default 1000000)\n"
                         "  --repeats N     timed samples, one warm-up discarded "
                         "(default 9, minimum 2)\n"
                         "  --json          machine-readable report\n");
}

// Returns false on a malformed command line.
bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            o.json = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else if (a.rfind("--iterations=", 0) == 0) {
            o.iterations = std::strtoull(a.c_str() + std::strlen("--iterations="), nullptr, 10);
        } else if (a == "--iterations" && i + 1 < argc) {
            o.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (a.rfind("--repeats=", 0) == 0) {
            o.repeats = static_cast<std::uint32_t>(
                std::strtoul(a.c_str() + std::strlen("--repeats="), nullptr, 10));
        } else if (a == "--repeats" && i + 1 < argc) {
            o.repeats = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "error: unrecognised argument '%s'\n", a.c_str());
            usage();
            return false;
        }
    }
    if (o.iterations == 0) {
        std::fprintf(stderr, "error: --iterations must be greater than zero\n");
        return false;
    }
    if (o.repeats < 2) {
        // A spread is meaningless with one sample, and the warm-up would leave
        // nothing to report.
        std::fprintf(stderr, "error: --repeats must be at least 2\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse(argc, argv, opts)) {
        return 2;
    }

    const hyperion::HostInfo host = hyperion::probe_host();

    // Independent reference evaluation of the workload, used to verify every
    // timed sample. Computed once, outside any timed region.
    const std::uint64_t expected = calibration_workload(opts.iterations);

    std::vector<double> ns_per_op;
    ns_per_op.reserve(opts.repeats);
    std::uint64_t checksum = 0;
    bool checksum_stable = true;

    for (std::uint32_t i = 0; i < opts.repeats; ++i) {
        double seconds = 0.0;
        const std::uint64_t result = timed_sample(opts.iterations, seconds);

        if (i == 0) {
            checksum = result;
            checksum_stable = (result == expected);
            continue; // discard the warm-up sample
        }
        if (result != expected) {
            checksum_stable = false;
        }

        ns_per_op.push_back(seconds * 1e9 / static_cast<double>(opts.iterations));
    }

    const hyperion::SampleSummary summary = hyperion::summarize(ns_per_op);

    // Sanity guard against the optimiser defeating the measurement (BENCH-001).
    // A real sample cannot complete in less than one clock cycle per iteration,
    // so an implausibly small figure means the workload was eliminated rather
    // than executed. Reporting it would be a fabricated result.
    constexpr double kImplausibleNsPerOp = 0.05; // ~1 cycle at 20 GHz
    const bool measurement_plausible = summary.median >= kImplausibleNsPerOp;

    if (opts.json) {
        std::printf("{\n");
        std::printf("  \"schema\": \"hyperion.bench.host.v2\",\n");
        std::printf("  \"build\": \"%s\",\n", hyperion::build_summary().c_str());
        std::printf("  \"host\": {\n");
        std::printf("    \"arch\": \"%s\",\n", hyperion::to_string(host.arch));
        std::printf("    \"kernel\": \"%s\",\n", host.kernel_release.c_str());
        std::printf("    \"logical_cpus\": %u,\n", host.logical_cpus);
        std::printf("    \"page_size\": %zu,\n", host.page_size);
        std::printf("    \"cache_line_size\": %zu\n", host.cache_line_size);
        std::printf("  },\n");
        std::printf("  \"calibration\": {\n");
        std::printf("    \"iterations\": %llu,\n",
                    static_cast<unsigned long long>(opts.iterations));
        std::printf("    \"repeats_requested\": %u,\n", opts.repeats);
        std::printf("    \"warmup_discarded\": 1,\n");
        std::printf("    \"samples\": %zu,\n", summary.count);
        std::printf("    \"ns_per_op_min\": %.6f,\n", summary.min);
        std::printf("    \"ns_per_op_p25\": %.6f,\n", summary.p25);
        std::printf("    \"ns_per_op_median\": %.6f,\n", summary.median);
        std::printf("    \"ns_per_op_p75\": %.6f,\n", summary.p75);
        std::printf("    \"ns_per_op_max\": %.6f,\n", summary.max);
        std::printf("    \"ns_per_op_mean\": %.6f,\n", summary.mean);
        std::printf("    \"ns_per_op_stddev\": %.6f,\n", summary.stddev);
        std::printf("    \"iqr_fraction\": %.6f,\n", summary.iqr_fraction);
        std::printf("    \"checksum\": \"0x%016llx\",\n",
                    static_cast<unsigned long long>(checksum));
        std::printf("    \"checksum_stable\": %s,\n", checksum_stable ? "true" : "false");
        std::printf("    \"measurement_plausible\": %s\n",
                    measurement_plausible ? "true" : "false");
        std::printf("  },\n");
        std::printf("  \"guest_execution\": {\n");
        std::printf("    \"tier1_native_aarch64\": %s,\n",
                    hyperion::ExecMem::executable_supported() ? "true" : "false");
        std::printf("    \"benchmark_driver\": \"hyperion_tier_bench\"\n");
        std::printf("  }\n");
        std::printf("}\n");
    } else {
        std::printf("hyperion host benchmark\n");
        std::printf("%s", hyperion::format_host_info(host).c_str());
        std::printf("iterations:      %llu\n", static_cast<unsigned long long>(opts.iterations));
        std::printf("samples:         %zu (1 warm-up discarded, %u requested)\n", summary.count,
                    opts.repeats);
        std::printf("ns/op min:       %.4f\n", summary.min);
        std::printf("ns/op p25:       %.4f\n", summary.p25);
        std::printf("ns/op median:    %.4f\n", summary.median);
        std::printf("ns/op p75:       %.4f\n", summary.p75);
        std::printf("ns/op max:       %.4f\n", summary.max);
        std::printf("ns/op stddev:    %.4f\n", summary.stddev);
        std::printf("iqr/median:      %.4f\n", summary.iqr_fraction);
        std::printf("checksum:        0x%016llx (%s)\n", static_cast<unsigned long long>(checksum),
                    checksum_stable ? "stable" : "UNSTABLE");
        std::printf("measurement:     %s\n",
                    measurement_plausible ? "plausible" : "IMPLAUSIBLE (workload optimised away)");
        std::printf("guest execution: measured separately by hyperion_tier_bench (tier 1 %s)\n",
                    hyperion::ExecMem::executable_supported() ? "active" : "cross-only");
    }

    // A benchmark whose checksum moved was measuring a broken build, and one
    // whose samples are impossibly fast measured nothing at all. Both are
    // failures, not results.
    if (!checksum_stable || !measurement_plausible) {
        std::fprintf(stderr, "error: benchmark result rejected (%s%s%s)\n",
                     checksum_stable ? "" : "unstable checksum",
                     (!checksum_stable && !measurement_plausible) ? "; " : "",
                     measurement_plausible ? "" : "implausible timing");
        return 1;
    }
    return 0;
}
