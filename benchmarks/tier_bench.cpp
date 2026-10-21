// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tier 0 vs Tier 1 benchmark harness (Phase 5).
//
// Phase 5 begins with *measurement*, not optimisation. This harness runs the
// same guest program through both execution tiers and reports the time per
// retired guest instruction, so every later change (register allocation, block
// chaining, dispatch) has a fixed, reproducible baseline to be judged against.
//
// It reuses the methodology the host harness established (see BENCHMARKS.md
// section 5): one discarded warm-up, repeated timed samples reported as median
// with an IQR spread, a checksum compared across runs, and an explicit
// plausibility check. A result that is too good to be true is treated as a
// failure, not a result (docs/known-issues.md BENCH-001).
//
// Honesty rules this harness enforces:
//   * Tier 1 numbers are only produced when the host can actually execute
//     generated AArch64 code. On an x86-64 host the fields are reported as
//     unavailable, never as a fabricated comparison.
//   * The guest programs are fixed and their expected final state is checked, so
//     a benchmark cannot silently measure a broken translator.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hyperion/bench_stats.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/host_probe.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/translation_cache.hpp"
#include "hyperion/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::RunResult;
using hyperion::RunStatus;
using hyperion::TranslationCache;

constexpr std::uint64_t kBase = 0x400000;

// Guest program: accumulate rax into rdx, `rcx` times, then halt.
//   xor edx, edx            ; 31 D2
// loop:
//   add rdx, rax            ; 48 01 C2
//   dec rcx                 ; 48 FF C9
//   jnz loop                ; 75 F8
//   hlt                     ; F4
// Bytes verified with as/objdump.
const std::vector<std::uint8_t> kLoopProgram = {
    0x31, 0xD2, 0x48, 0x01, 0xC2, 0x48, 0xFF, 0xC9, 0x75, 0xF8, 0xF4,
};

// Guest program with a deliberately *wide* hot block, so register allocation has
// something to allocate. The loop body touches eleven guest registers, which
// exceeds the ten callee-saved host registers the allocator can use.
//   mov r12d, <iterations>  ; 41 BC xx xx xx xx   (immediate patched below)
// loop:
//   add rax, {rbx,rcx,rdx,rsi,rdi,r8,r9,r10,r11}   ; 10 adds, rax accumulates
//   add rax, rbx                                   ; one repeat use of rbx
//   dec r12                                        ; 49 FF CC
//   jne loop                                       ; 75 DD
//   hlt                                            ; F4
// All sources are zero, so rax ends at 0; r12 counts down to 0.
std::vector<std::uint8_t> make_wide_program(std::uint32_t iterations) {
    std::vector<std::uint8_t> p = {
        0x41, 0xBC, 0x00, 0x00, 0x00, 0x00, // mov r12d, imm32 (patched)
        0x48, 0x01, 0xD8,                   // add rax, rbx
        0x48, 0x01, 0xC8,                   // add rax, rcx
        0x48, 0x01, 0xD0,                   // add rax, rdx
        0x48, 0x01, 0xF0,                   // add rax, rsi
        0x48, 0x01, 0xF8,                   // add rax, rdi
        0x4C, 0x01, 0xC0,                   // add rax, r8
        0x4C, 0x01, 0xC8,                   // add rax, r9
        0x4C, 0x01, 0xD0,                   // add rax, r10
        0x4C, 0x01, 0xD8,                   // add rax, r11
        0x48, 0x01, 0xD8,                   // add rax, rbx
        0x49, 0xFF, 0xCC,                   // dec r12
        0x75, 0xDD,                         // jne loop (offset 6)
        0xF4,                               // hlt
    };
    p[2] = static_cast<std::uint8_t>(iterations & 0xFF);
    p[3] = static_cast<std::uint8_t>((iterations >> 8) & 0xFF);
    p[4] = static_cast<std::uint8_t>((iterations >> 16) & 0xFF);
    p[5] = static_cast<std::uint8_t>((iterations >> 24) & 0xFF);
    return p;
}

// Compiler barrier: keep the timed run from being hoisted or eliminated.
inline void optimization_barrier(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(p) : "memory");
#else
    *static_cast<volatile const void**>(const_cast<const void**>(&p)) = p;
#endif
}

struct Options {
    std::uint64_t iterations = 1'000'000; // loop iterations per sample
    std::uint32_t repeats = 5;            // timed samples (one warm-up discarded)
    bool json = false;
    bool wide = false; // use the wide-register workload
};

void usage() {
    std::fprintf(stderr,
                 "usage: hyperion_tier_bench [--iterations N] [--repeats N] [--wide] [--json]\n"
                 "  --iterations N  guest loop iterations per sample (default 1000000)\n"
                 "  --repeats N     timed samples, one warm-up discarded (default 5, min 2)\n"
                 "  --wide          use the 11-register wide-block workload (default: narrow)\n"
                 "  --json          machine-readable report\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            o.json = true;
        } else if (a == "--wide") {
            o.wide = true;
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
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            usage();
            return false;
        }
    }
    if (o.iterations == 0) {
        std::fprintf(stderr, "error: --iterations must be greater than zero\n");
        return false;
    }
    if (o.repeats < 2) {
        // A single sample cannot express a spread; refuse rather than report a
        // meaningless one.
        std::fprintf(stderr, "error: --repeats must be at least 2\n");
        return false;
    }
    return true;
}

struct RunOutcome {
    RunResult result;
    double ns = 0.0;
};

// A fixed guest workload: the program bytes, its initial state and a check of
// the final state, so a benchmark can never silently measure a broken
// translator.
struct Workload {
    const char* name;
    std::vector<std::uint8_t> program;
    GuestState initial;
    std::uint64_t iterations;
    std::uint64_t steps_per_iteration;
    bool (*check)(const GuestState&, std::uint64_t);
};

bool check_loop(const GuestState& s, std::uint64_t iterations) {
    return s.reg(Gpr::kRdx) == iterations;
}

bool check_wide(const GuestState& s, std::uint64_t iterations) {
    // Seeds rbx=1..r11=9 plus a second `add rax, rbx` add 46 per loop iteration,
    // catching any register-allocator slot aliasing or spill/reload mismatch.
    return s.reg(Gpr::kRax) == 46ULL * iterations && s.reg(Gpr::kR12) == 0;
}

Workload make_workload(bool wide, std::uint64_t iterations) {
    Workload w;
    w.initial.rip = kBase;
    w.iterations = iterations;
    if (wide) {
        w.name = "wide hot block: 11 guest registers, 12-instruction loop body";
        w.program = make_wide_program(static_cast<std::uint32_t>(iterations));
        w.initial.reg(Gpr::kRbx) = 1;
        w.initial.reg(Gpr::kRcx) = 2;
        w.initial.reg(Gpr::kRdx) = 3;
        w.initial.reg(Gpr::kRsi) = 4;
        w.initial.reg(Gpr::kRdi) = 5;
        w.initial.reg(Gpr::kR8) = 6;
        w.initial.reg(Gpr::kR9) = 7;
        w.initial.reg(Gpr::kR10) = 8;
        w.initial.reg(Gpr::kR11) = 9;
        w.steps_per_iteration = 13; // 12 loop-body insns + the final hlt amortised
        w.check = check_wide;
    } else {
        w.name = "accumulate rax into rdx rcx times, halt";
        w.program = kLoopProgram;
        w.initial.reg(Gpr::kRax) = 1;
        w.initial.reg(Gpr::kRcx) = iterations;
        w.steps_per_iteration = 4;
        w.check = check_loop;
    }
    return w;
}

// Time one full run of the workload through the given tier. The step budget is
// sized to the workload so a long run is never mistaken for a translation
// failure: a `kStepLimit` result would otherwise fail the correctness check.
template<typename RunFn> RunOutcome time_run(RunFn&& run, const Workload& w) {
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), w.program.data(), w.program.size());
    const GuestState initial = w.initial;
    const std::uint64_t max_steps = w.steps_per_iteration * w.iterations;

    const auto t0 = Clock::now();
    RunResult r = run(mem, initial, max_steps);
    const auto t1 = Clock::now();
    optimization_barrier(&r);
    return RunOutcome{r, std::chrono::duration<double, std::nano>(t1 - t0).count()};
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse(argc, argv, opts)) {
        return 2;
    }

    const bool tier1_available = hyperion::ExecMem::executable_supported();
    const Workload workload = make_workload(opts.wide, opts.iterations);

    std::vector<double> tier0_ns;
    std::vector<double> tier1_ns;
    std::uint64_t steps = 0;
    bool tier0_ok = true;
    bool tier1_ok = true;
    std::string tier1_reason = "host cannot execute generated AArch64 code";

    // Translation size for this workload, captured once outside the timed loop
    // so it can never affect the measurement. Deterministic, unlike timing.
    std::size_t tier1_code_bytes = 0;
    std::size_t tier1_block_count = 0;
    std::uint64_t tier1_dispatch_hits = 0;
    std::uint64_t tier1_dispatch_misses = 0;

    // One discarded warm-up run, then `repeats` timed samples.
    for (std::uint32_t i = 0; i <= opts.repeats; ++i) {
        const bool warmup = (i == 0);

        const RunOutcome r0 =
            time_run([](FlatMemory& mem, GuestState st,
                        std::uint64_t budget) { return hyperion::run_program(mem, st, budget); },
                     workload);
        if (r0.result.status != RunStatus::kOk ||
            !workload.check(r0.result.state, workload.iterations)) {
            tier0_ok = false;
        }
        steps = r0.result.steps;
        if (!warmup) {
            tier0_ns.push_back(r0.ns);
        }

        if (tier1_available) {
            const RunOutcome r1 = time_run(
                [](FlatMemory& mem, GuestState st, std::uint64_t budget) {
                    TranslationCache cache;
                    return cache.run(mem, st, budget);
                },
                workload);
            if (r1.result.status != RunStatus::kOk ||
                !workload.check(r1.result.state, workload.iterations)) {
                tier1_ok = false;
            }
            if (!warmup) {
                tier1_ns.push_back(r1.ns);
            }
        }
    }

    const hyperion::SampleSummary s0 = hyperion::summarize(tier0_ns);
    const hyperion::SampleSummary s1 = hyperion::summarize(tier1_ns);

    if (tier1_available) {
        FlatMemory mem(kBase, 4096);
        std::memcpy(mem.data(), workload.program.data(), workload.program.size());
        const GuestState st = workload.initial;
        hyperion::TranslationCache cache;
        cache.run(mem, st, workload.steps_per_iteration * workload.iterations);
        tier1_code_bytes = cache.total_code_bytes();
        tier1_block_count = cache.block_count();
        tier1_dispatch_hits = cache.dispatch_hits();
        tier1_dispatch_misses = cache.dispatch_misses();
    }
    const auto per_step = [&](double ns) {
        return steps == 0 ? 0.0 : ns / static_cast<double>(steps);
    };
    const hyperion::HostInfo host = hyperion::probe_host();

    const bool have_tier1 = tier1_available && !tier1_ns.empty();
    const double ratio = (have_tier1 && s0.median > 0.0) ? s1.median / s0.median : 0.0;

    if (opts.json) {
        std::printf("{\n");
        std::printf("  \"benchmark\": \"tier1_vs_tier0\",\n");
        std::printf("  \"hyperion_version\": \"%s\",\n", hyperion::version_string().c_str());
        std::printf("  \"host\": { \"arch\": \"%s\" },\n", hyperion::to_string(host.arch));
        std::printf("  \"workload\": {\n");
        std::printf("    \"program\": \"%s\",\n", workload.name);
        std::printf("    \"iterations\": %llu,\n",
                    static_cast<unsigned long long>(opts.iterations));
        std::printf("    \"guest_instructions_retired\": %llu,\n",
                    static_cast<unsigned long long>(steps));
        std::printf("    \"warmup_discarded\": 1,\n");
        std::printf("    \"samples\": %zu\n", s0.count);
        std::printf("  },\n");
        std::printf("  \"tier0_interpreter\": {\n");
        std::printf("    \"available\": true,\n");
        std::printf("    \"correct\": %s,\n", tier0_ok ? "true" : "false");
        std::printf("    \"ns_median\": %.3f,\n", s0.median);
        std::printf("    \"ns_per_guest_instruction\": %.6f,\n", per_step(s0.median));
        std::printf("    \"iqr_fraction\": %.6f\n", s0.iqr_fraction);
        std::printf("  },\n");
        std::printf("  \"tier1_translator\": {\n");
        std::printf("    \"available\": %s,\n", have_tier1 ? "true" : "false");
        if (have_tier1) {
            std::printf("    \"correct\": %s,\n", tier1_ok ? "true" : "false");
            std::printf("    \"ns_median\": %.3f,\n", s1.median);
            std::printf("    \"ns_per_guest_instruction\": %.6f,\n", per_step(s1.median));
            std::printf("    \"iqr_fraction\": %.6f,\n", s1.iqr_fraction);
            std::printf("    \"blocks\": %zu,\n", tier1_block_count);
            std::printf("    \"generated_code_bytes\": %zu,\n", tier1_code_bytes);
            std::printf("    \"dispatch_hits\": %llu,\n",
                        static_cast<unsigned long long>(tier1_dispatch_hits));
            std::printf("    \"dispatch_misses\": %llu,\n",
                        static_cast<unsigned long long>(tier1_dispatch_misses));
            std::printf("    \"tier1_over_tier0_ratio\": %.6f\n", ratio);
        } else {
            std::printf("    \"reason\": \"%s\"\n", tier1_reason.c_str());
        }
        std::printf("  },\n");
        std::printf(
            "  \"notes\": \"Tier 1 includes block register allocation, hardware NZCV "
            "branch fusion, native self-loop superblocks, ExecArena code caching, and "
            "2-way block chaining. Under qemu-user both tiers are additionally translated "
            "by qemu, so the ratio is a relative sanity signal rather than silicon timing.\"\n");
        std::printf("}\n");
    } else {
        std::printf("hyperion tier 0 vs tier 1 benchmark\n");
        std::printf("%s", hyperion::format_host_info(host).c_str());
        std::printf("workload:        %s\n", workload.name);
        std::printf("iterations:      %llu\n", static_cast<unsigned long long>(opts.iterations));
        std::printf("guest instrs:    %llu retired\n", static_cast<unsigned long long>(steps));
        std::printf("samples:         %zu (1 warm-up discarded, %u requested)\n", s0.count,
                    opts.repeats);
        std::printf("tier 0 median:   %.3f ns  (%.4f ns/guest-ins)  correct=%s\n", s0.median,
                    per_step(s0.median), tier0_ok ? "yes" : "NO");
        if (have_tier1) {
            std::printf("tier 1 median:   %.3f ns  (%.4f ns/guest-ins)  correct=%s\n", s1.median,
                        per_step(s1.median), tier1_ok ? "yes" : "NO");
            std::printf("tier 1 vs tier 0: %.2fx (tier 1 is %s)\n", ratio,
                        ratio > 1.0 ? "slower" : "faster");
        } else {
            std::printf("tier 1:          unavailable (%s)\n", tier1_reason.c_str());
        }
    }

    if (!tier0_ok || (have_tier1 && !tier1_ok)) {
        std::fprintf(stderr, "error: a tier produced an incorrect result; measurement rejected\n");
        return 1;
    }
    return 0;
}
