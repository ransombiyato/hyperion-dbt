# OpenHands Merge & Integration Guide: `ransombiyato/hyperion-dbt-v2` -> `ransombiyato/hyperion-dbt`

**Target audience:** OpenHands agent maintaining `https://github.com/ransombiyato/hyperion-dbt`  
**Common ancestor commit:** `03487fc` (`syscall: implement statx(332)`)  
**Source fork (this repository):** `https://github.com/ransombiyato/hyperion-dbt-v2` (`main`)  
**Upstream repository:** `https://github.com/ransombiyato/hyperion-dbt` (`main`, currently around `9f87e35`)  
**Separate full-system ISO-booting fork (DO NOT merge back into user-mode repo unless explicitly asked):** `https://github.com/ransombiyato/gallyx`

---

## 1. Mission Summary for OpenHands

Both `ransombiyato/hyperion-dbt` (`705ca40..9f87e35`) and `ransombiyato/hyperion-dbt-v2` (`fa971a8..HEAD`) branched independently from `03487fc`:

- **`ransombiyato/hyperion-dbt` (`705ca40..9f87e35`)** added:
  - Release packaging workflow & static tarball script (`.github/workflows/release.yml`, `scripts/package.sh`)
  - Initial C++ call-out soft-MMU helpers (`include/hyperion/runtime_helpers.hpp`, `src/core/runtime_helpers.cpp`) for register/memory `MOV` + `ALU`/`INC`/`DEC`
  - Initial `ELF64` program loader (`include/hyperion/elf.hpp`, `src/core/elf.cpp`, `tests/unit/test_elf.cpp`, `tests/cli/elf_test.sh`, `tests/fixtures/tiny-static.elf`)
  - `docs/iso-boot-investigation.md` and `SECURITY.md` additions

- **`ransombiyato/hyperion-dbt-v2` (`fa971a8..HEAD`)** fixed **9 critical correctness/UB bugs** in the base codebase and built the **complete, verified Box64-style user-mode x86-64 -> ARMv8+NEON engine** capable of running **unmodified `gcc 14.2 -O2 -static` (`glibc 2.41`) Linux C binaries end-to-end**:
  - **27,835 / 27,835 unique instructions** across real static `glibc 2.41` binaries decoded & executed (`120,313,691` dynamic instructions with **`0` step discrepancy** between `x86_64` Tier 0, `aarch64` Tier 0, and `aarch64` Tier 1 JIT).
  - **Tier 0 throughput:** **`55.8–60.7 MIPS`** (`16.5–17.9 ns/insn`) via 512-entry direct-mapped `SoftTlb` (`page_tag` -> `host_addend`) + 4096-entry decoded `ICache`.
  - **Tier 1 AArch64 + 128-bit ARMv8 NEON JIT throughput:** **`104.49×` speedup** on `hyperion_tier_bench` (`2.54 ns/insn`, `393.3 MIPS` even under `qemu-aarch64`) and **`5.17×` to `20.12×` end-to-end speedup (`10.43×` aggregate)** across 5 full `glibc 2.41` binaries (`sha256`, `nqueens`, `quicksort_c`, `qsort_glibc`, `sieve`).
  - **42 CI-verified Linux x86-64 syscalls** (`python3 tools/gen_syscall_inventory.py --check` green with `0` errors) + refcounted unbuffered POSIX `OpenFileDescription` file table (`dup`/`dup2`/`dup3`/`fcntl`/`pread64`/`pwrite64`/`readv`/`writev`).
  - **100% clean across all 8 gates:** `clang-format`, `shellcheck`, `gen_syscall_inventory.py --check`, `GCC 14 -Werror`, `Clang 19 -Werror`, `ASan + UBSan` (including all 5 real `glibc` binaries), `aarch64` cross-test, and **`clang-tidy -warnings-as-errors='*'`** across all 16 `.cpp` translation units.

**Your job is to merge both branches cleanly in `ransombiyato/hyperion-dbt` so NOTHING good from either branch is lost and ALL 9 critical bug fixes remain intact.**

---

## 2. Critical Bugs Fixed in `hyperion-dbt-v2` (MUST NOT Regress During Merge!)

Verify that every single one of these fixes stays present after merging:

1. **AArch64 shifted-`imm12` truncation bug (`src/core/translator.cpp` / `encode_imm12`):**
   - Base `03487fc` passed `imm & 0xFFF` instead of `imm >> 12` when `shift12 == true`, turning `ADD reg, 0x123000` into `ADD reg, 0` (`NOP`)! `v2` uses checked `encode_imm12(val, imm12, shift12)`.
2. **`GuestAddressSpace` heap data wipe + missing region splitting (`src/core/address_space.cpp`):**
   - `brk()` previously resized region storage destructively and `mprotect()`/`munmap()` could not split an existing VMA across page boundaries. `v2` pre-reserves heap backing memory, implements `split_region_at()`, enforces `MemPerm::kExec` on instruction fetch, and provides fast `translate_page()` for `SoftTlb`.
3. **Executable-only `code_epoch_` invalidation (`src/core/address_space.cpp`):**
   - Heap `brk()` / RW `mmap()` in `glibc` `malloc` MUST NOT bump `code_epoch_`, otherwise every `sbrk` flushes the entire JIT `TranslationCache`! Only mutations affecting `MemPerm::kExec` bump `code_epoch_`.
4. **Unconditional post-`SYSCALL` data `SoftTlb` flush (`src/core/interpreter.cpp` & `src/core/translation_cache.cpp`):**
   - `brk()`, `mmap()`, `munmap()`, and `mprotect()` alter page permissions/mappings without changing `code_epoch_`. Always call `tlb.flush()` right after `dispatch_syscall(...)`.
5. **16-byte `ExecArena::allocate()` alignment (`src/core/exec_mem.cpp`):**
   - Embedded 64-bit literal pools inside AArch64 stubs use `LDR Xt, label` requiring 8-byte alignment; `ExecArena::allocate()` aligns all allocations to `16` bytes (`(bytes + 15u) & ~15u`).
6. **`x29` frame-pointer preservation in `jit_budget_epilogue` (`src/core/translator.cpp`):**
   - `enter_jit` saves host `x29, x30` at `[sp, #0]` (`128`-byte frame). Never clobber `x29` inside `jit_budget_epilogue` before `ldp x29, x30, [sp], #128; ret` — compute elapsed steps directly in `x25` (`sub x1, x25, x26`).
7. **RMW memory-store `Jcc` fusion guard (`prev_wrote_mem` in `src/core/translator.cpp`):**
   - Never reuse host `NZCV` flags across an instruction that stored its result back to memory (`ADD [mem], reg; JZ ...`), because `emit_tlb_addr` executes `SUBS`/`CMP` on the page tag and clobbers `NZCV` after the ALU op! `v2` sets `prev_wrote_mem` so RMW memory ALU ops materialize `RFLAGS` before storing.
8. **`POP RSP`, accumulator-immediate (`04`/`05`/`2C`/`2D`/`3C`/`3D`), and `PACKUSWB` saturation (`SQXTUN`/`SQXTUN2`):**
   - Preserved and regression-tested in `tests/unit/test_decoder.cpp` and `tests/unit/test_translator.cpp`.
9. **Unbuffered refcounted `OpenFileDescription` (`include/hyperion/syscall.hpp`, `src/core/syscall.cpp`):**
   - Replaces buffered `std::fstream` so `dup`/`dup2`/`dup3`, `fcntl`, `lseek`, `pread64`/`pwrite64`, and `readv`/`writev` obey exact POSIX shared file-offset semantics.

---

## 3. Exact File-by-File Resolution Matrix (`hyperion-dbt` vs `hyperion-dbt-v2`)

Run inside `ransombiyato/hyperion-dbt`:

```bash
git remote add v2 https://github.com/ransombiyato/hyperion-dbt-v2.git
git fetch v2 main
git checkout -b merge-v2 main
git merge --no-commit v2/main || true
```

Resolve each file group cleanly as follows:

| File / Directory | Resolution Action |
|---|---|
| **Keep from `upstream` (`hyperion-dbt`)** | |
| `.github/workflows/release.yml`, `scripts/package.sh` | **Keep `upstream`!** Great release tarball CI & packaging scripts — just make sure `CMakeLists.txt` compiles `v2`'s core sources. |
| `docs/iso-boot-investigation.md`, `SECURITY.md` | **Keep `upstream`** (and cross-link `docs/real-world-evaluation.md` + `https://github.com/ransombiyato/gallyx` where system-mode ISO booting continues). |
| `tests/fixtures/tiny-static.elf`, `tests/cli/elf_test.sh` | **Keep `upstream` tests** AND adapt any CLI output assertions to match the unified `hyperion info` / `hyperion run` format so BOTH `elf_test.sh` and `cli_smoke.sh` run in CTest! |
| **Take from `v2` (`hyperion-dbt-v2`)** | |
| `include/hyperion/{guest_state,decoder,interpreter,aarch64_encoder,translator,translation_cache,address_space,syscall,exec_mem}.hpp` & matching `src/core/*.cpp` | **Take `v2` completely.** `v2`'s inline `SoftTlb` + full x86-64/SSE2 + ARMv8/NEON JIT completely supersedes `runtime_helpers.{hpp,cpp}` (`runtime_helpers` can be removed from `CMakeLists.txt` or kept only if `test_runtime_helpers.cpp` exercises it as a standalone unit test). |
| `include/hyperion/elf_loader.hpp`, `src/core/elf_loader.cpp` vs `include/hyperion/elf.hpp`, `src/core/elf.cpp` | Either unify behind one header or keep `elf_loader.{hpp,cpp}` for execution (`load_elf_file` sets up full `PT_LOAD` W^X + BSS + System V `argc`/`argv`/`envp`/`auxv`/`AT_RANDOM` stack) and wire `hyperion info` to support all fields checked by both `cli_smoke.sh` and `elf_test.sh`! |
| `src/cli/main.cpp`, `scripts/hyperion`, `install.sh` | Merge CLI subcommands (`info`, `run` auto-detecting ELF64 magic `\x7fELF` vs `--raw` blob) and change `REPO_SLUG` in `install.sh` back to `"ransombiyato/hyperion-dbt"` once merged into the main repo! |
| `benchmarks/workloads/*.c`, `scripts/run_real_benchmarks.sh`, `docs/real-world-evaluation.md` | **Take from `v2`.** Provides reproducible `N = 5` real compiled `glibc` benchmarks. |
| `docs/syscalls/*` | Run `python3 tools/gen_syscall_inventory.py --write` if needed, then verify `python3 tools/gen_syscall_inventory.py --check` exits `0`. |

---

## 4. Mandatory Post-Merge Verification Checklist (Do Not Push Until Every Line Exits `0`!)

```bash
find include src tests benchmarks -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.c' \) -exec clang-format -i {} +
./scripts/dev.sh format
shellcheck install.sh scripts/*.sh scripts/hyperion tests/cli/*.sh experiments/uml/run.sh
python3 tools/gen_syscall_inventory.py --check
./scripts/dev.sh build
./scripts/dev.sh test
./scripts/dev.sh clang-build
./scripts/dev.sh sanitize
./scripts/dev.sh cross-test
clang-tidy -p build/clangcheck -warnings-as-errors='*' src/core/*.cpp src/cli/main.cpp benchmarks/*.cpp
./scripts/run_real_benchmarks.sh
```
