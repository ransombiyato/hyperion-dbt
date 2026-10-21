# TESTING

How to build, test, fuzz and differentially validate Project HYPERION.

---

## 1. Prerequisites

A C++20 compiler and CMake ≥ 3.20. For the full workflow (cross-architecture
validation, formatting, sanitizers) run:

```bash
scripts/bootstrap.sh
```

which installs on Debian/Ubuntu: `build-essential cmake ninja-build clang
clang-format clang-tidy llvm gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
binutils-aarch64-linux-gnu qemu-user qemu-user-static gdb-multiarch`.

It also caches the emulator and cross toolchain under `tools/cache/`
(git-ignored), because the development container does not persist `/usr` between
sessions but the repository does. After a reset, `scripts/toolcache.sh restore`
reinstates the cached toolchain offline; `scripts/toolcache.sh status` shows what
is cached. The benchmark and cross-test scripts use the cached
`qemu-aarch64-static` automatically.

---

## 2. Standard workflow

```bash
scripts/dev.sh bootstrap   # configure build/dev
scripts/dev.sh build       # build
scripts/dev.sh test        # run CTest
```

Or directly:

```bash
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

Presets are also provided: `cmake --preset debug`, `release`, `sanitize`,
`cross-aarch64`.

---

## 3. Test suite layout

| Location | Contents |
| --- | --- |
| `tests/harness/` | dependency-free harness (`HYPERION_TEST`, `HYPERION_CHECK`) |
| `tests/unit/` | unit tests, one file per component |
| `tests/differential/` | interpreter vs. real x86-64 hardware |
| `tests/cli/` | end-to-end CLI smoke test (`cli_smoke.sh`) |
| `tests/run_cross_tests.cmake` | cross-arch configure/build/run driver |
| `benchmarks/` | harness smoke tests (`bench.smoke`, `tier_bench.smoke`) |

Registered CTest tests:

| Test | What it proves |
| --- | --- |
| `unit.native` | core components behave correctly on the host |
| `differential.native_vs_interpreter` | the interpreter agrees with real x86-64 |
| `cli.smoke` | `hyperion run` executes through both tiers and they agree |
| `bench.smoke` | the benchmark harness runs and emits valid output |
| `tier_bench.smoke` | the Tier 0 vs Tier 1 benchmark runs and both tiers are correct (on x86-64 only Tier 0 executes; Tier 1 is reported unavailable) |
| `cross.aarch64_tests` | the suite passes when compiled for AArch64 and run under `qemu-user` |

`tests/cli/cli_smoke.sh` writes a small x86-64 blob, runs it through tier 0 and
tier 1, and requires the same status and step count. On a host that cannot
execute generated code, tier 1 falls back to the interpreter (and says so); the
cross pipeline runs the same script with `HYPERION_EMULATOR` set to `qemu-aarch64`
and additionally requires tier 1 to have translated a non-zero number of blocks,
so a vacuous "ok" cannot pass.

### Writing a test

```cpp
#include "harness/test_harness.hpp"

HYPERION_TEST("component", "behaviour") {
    HYPERION_CHECK_EQ(actual, expected);
}
```

Test binaries accept `--filter=substring` and `--list`.

---

## 4. Cross-architecture validation

The reference target is AArch64; development hosts are often x86-64. Generated
AArch64 code cannot run natively on x86-64, so validation is done by
cross-compiling and executing under `qemu-user`:

```bash
scripts/dev.sh cross-test
```

This configures `build/cross-test/cross-aarch64` with
`cmake/toolchains/aarch64-linux-gnu.cmake` (static linking), builds the test
suite, runs it under `qemu-aarch64`, and fails the CTest run if it does not pass.

**Limits.** `qemu-user` validates *correctness*, never performance, and it is
not a cycle-accurate model. Performance claims require real AArch64 hardware.
AArch64-hosted runs (`unit.native` on an AArch64 machine) additionally verify
the pipeline without qemu.

**Translator differential.** `tests/unit/test_translator.cpp` uses this pipeline
directly. Its scope tests (which instructions are accepted and which are refused)
run on any host; its differential tests execute the *generated AArch64* and
compare against the reference interpreter. Those differential cases compile only
under `__aarch64__` and report `SKIP` on x86-64 rather than a false pass. There
are two levels:

- **Single-instruction** — one instruction, every register and all six flags
  compared; plus an 847-case operand sweep over carry/overflow/parity boundaries.
- **Whole-program** — a counted loop and taken/not-taken branches run through
  `TranslationCache` and compared against `run_program` on final `GuestState`
  and step count. `translator.dispatcher_*` additionally proves an unsupported
  instruction stops the run (leaving `rip` on it) and an infinite loop is
  reported as a step limit rather than hanging.

`tests/unit/test_translation_cache.cpp` covers the cache directly: it must
refuse to run when the host cannot execute generated code, and on AArch64 it
must cache blocks (not re-translate each visit) and agree with the interpreter.

---

## 5. Sanitizers

```bash
scripts/dev.sh sanitize
```

Builds with AddressSanitizer + UndefinedBehaviorSanitizer and runs the suite.
The translator manipulates raw memory and generates executable code, so
sanitizers are expected to be central to correctness work from Phase 2 onward.
Note: ASan interacts with custom code caches; where it must be disabled for a
specific test, document why in the test.

---

## 6. Clang build (matches CI)

```bash
scripts/dev.sh clang-build
```

CI builds every target twice, once with gcc and once with clang, both with
`HYPERION_WARNINGS_AS_ERRORS=ON`. Clang's `-Wsign-conversion` (enabled by
`-Wconversion`) is stricter than gcc's, so a change that builds cleanly with gcc
can still fail CI. Run this before pushing a change that touches integer
conversions or container indexing.

---

## 7. Formatting and static analysis

```bash
scripts/dev.sh format       # check (fails on drift)
scripts/dev.sh format-fix   # apply
clang-tidy -p build/dev src/core/*.cpp   # static analysis (config: .clang-tidy)
```

CI enforces `clang-format --dry-run --Werror` on all sources, using **pinned
clang-format 19.1.7** (see the `format` job in `.github/workflows/ci.yml`).
Formatting rules change between releases, so use the pinned version locally to
avoid spurious drift.

### Warning flags

The warning set in `CMakeLists.txt` includes **`-Wsign-conversion`** in addition
to `-Wconversion`. This matters: clang turns `-Wsign-conversion` on as part of
`-Wconversion`, but gcc does not, so a gcc-only host would silently miss a
signed index reaching `vector::operator[]` that the clang CI job rejects. With
the flag enabled on both compilers, `scripts/dev.sh build` alone is enough to
catch that class of bug — clang is no longer required locally to reproduce it.

---

## 8. Differential testing (Phase 2+)

The core correctness strategy:

1. **Reference interpreter** executes the same IR as the code generator; it is
   the trusted oracle.
2. **Native x86-64 oracle**: the same test program is compiled natively for
   x86-64 and executed on the host; its observable output (registers, flags,
   memory, exit code, stdout) is compared with the translated run.
3. **QEMU oracle**: guest binaries may also be run under `qemu-x86_64` for an
   independent third opinion.
4. **Randomized differential testing**: generate random instruction sequences
   with a known-good assembler, execute under both the oracle and the
   translator, compare full guest state after each instruction.

A test that only checks a subset of state is not a differential test. Record
which state fields are compared.

### Verifying AArch64 encodings against binutils

The AArch64 encoder's tests use expected words taken from GNU `as`/`objdump`,
not values written from memory. To add or re-check an encoding, assemble the
instruction and read the word out of the disassembly:

```sh
printf '.text\nadd x3, x4, x5\n' > /tmp/ref.s
aarch64-linux-gnu-as -o /tmp/ref.o /tmp/ref.s
aarch64-linux-gnu-objdump -d /tmp/ref.o | tail -n +8
#   0:   8b050083        add     x3, x4, x5
```

Paste the hex word into the test with a comment naming the instruction it came
from. If the encoder and the test disagree, one of them is wrong — find out
which before changing either. Never "fix" a test by copying the encoder's
output; that makes the test tautological and worthless.

---

## 9. Fuzzing (Phase 2+)

- **Decoder fuzzing**: feed random byte streams to the decoder; it must never
  crash, never read out of bounds, and must reject invalid encodings. Suitable
  for libFuzzer.
- **Semantics fuzzing**: random operand values against the oracle.
- **Differential block fuzzing**: random valid basic blocks, compared
  instruction-by-instruction.

Fuzz targets live in `tests/fuzz/` (created in Phase 2) and run in CI on a
time budget, with longer runs on demand.

---

## 10. CI

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Purpose |
| --- | --- |
| `build & test (gcc)` / `build & test (clang)` | compile clean with `-Werror` and run the native suite; clang is stricter (`-Wsign-conversion`) |
| `AArch64 cross tests (qemu-user)` | AArch64 cross-build + tests under `qemu-user` |
| `clang-format` | `clang-format --dry-run --Werror` (pinned 19.1.7) |
| `ASan/UBSan` | clang + Address/UndefinedBehavior sanitizer test run |
| `syscall inventory & shellcheck` | inventory validator + shellcheck on scripts |

A red CI job blocks merging. Never mark a milestone complete while CI is failing.
Run `scripts/dev.sh clang-build` locally before pushing: a gcc-clean tree can
still fail the clang job.

`.github/workflows/release.yml` runs on a `v*` tag (or manually): it builds,
runs the full test suite, packages both architectures with `scripts/package.sh`,
smoke-tests the packaged binaries (the AArch64 one under `qemu-user`, with a real
`--tier=1` translation whose result is checked), verifies `SHA256SUMS`, and
publishes a GitHub pre-release. A release is never published from a build that
fails its own tests.

---

## 11. Release packaging (local)

```bash
scripts/package.sh              # dist/hyperion-<v>-{x86_64,aarch64}-linux.tar.gz + SHA256SUMS
scripts/package.sh --no-cross   # x86-64 only (no cross compiler required)
hyperion package                # same, via the wrapper
```

To verify a tarball by hand, exactly as the release workflow does:

```bash
tar -xzf dist/hyperion-<v>-x86_64-linux.tar.gz -C dist
dist/hyperion-<v>-x86_64-linux/hyperion version          # runs natively
tar -xzf dist/hyperion-<v>-aarch64-linux.tar.gz -C dist
qemu-aarch64 dist/hyperion-<v>-aarch64-linux/hyperion version
printf '\x48\xb8\x01\x00\x00\x00\x00\x00\x00\x00\x48\x83\xc0\x02\xf4' > /tmp/t.bin
qemu-aarch64 dist/hyperion-<v>-aarch64-linux/hyperion run /tmp/t.bin --tier=1 --dump
# must report: 1 basic block(s) translated / status: ok / rax =0x0000000000000003
(cd dist && sha256sum -c SHA256SUMS)
```

The last check is the one that matters: it proves the shipped AArch64 binary
really translates and executes generated code rather than being a stub. It is
*not* a performance measurement (`qemu-user` is not the target hardware).
