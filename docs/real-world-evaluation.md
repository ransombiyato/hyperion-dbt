# Real-World Usability & Performance Evaluation (`hyperion-dbt` vs `hyperion-dbt-v2`)

This document records the unvarnished technical evaluation comparing the original
[`ransombiyato/hyperion-dbt`](https://github.com/ransombiyato/hyperion-dbt) (`03487fc`)
against **`ransombiyato/hyperion-dbt-v2`** across **real compiled `gcc 14.2 -O2 -static`
(`glibc 2.41`) Linux C binaries**, full AArch64 cross-execution, sanitizers, and
apples-to-apples statistical benchmarks.

---

## 1. Executive Verdict

1. **Original `ransombiyato/hyperion-dbt` (`03487fc`) was NOT usable on real compiled binaries.**
   - It had **no ELF64 loader**, **no System V AMD64 process startup stack (`argc/argv/envp/auxv`)**,
     only **27 syscalls** (missing TLS `arch_prctl`, `uname`, `pread64`/`writev`, `fcntl`, `dup*`,
     `clock_gettime`), and decoded only **14 register/basic instructions** (`MOV`, 6 ALU ops,
     `INC`/`DEC`, `PUSH`/`POP`, `JMP`/`Jcc rel8`, `NOP`, `HLT`, `RET`, `SYSCALL`).
   - Its Tier 1 AArch64 translator **rejected every stack instruction, every memory operand,
     every `CALL`/`RET`, every shift/rotate/multiply/divide, `SETcc`/`CMOVcc`, `LEA`, `TEST`,
     near `Jcc rel32`, string ops, and all SSE2 SIMD**.
   - Even within that tiny subset, our full line-by-line audit uncovered critical correctness
     bugs: `adds_imm`/`subs_imm` ignored `shift12` (silently compiling `add rax, 0x1000` into
     `add rax, 1` whenever `AF` was dead), `pop rsp` added `+8` *after* overwriting `rsp`,
     `SIB mod=00, base=5` failed to decode `[disp32]`, `Memory::fetch` ignored NX (`PROT_EXEC`),
     `set_brk()` wiped existing heap bytes on page growth, and `std::FILE*` fd wrappers corrupted
     file offsets across syscalls.

2. **`hyperion-dbt-v2` IS genuinely usable and genuinely fast on real single-threaded static
   x86-64 Linux C/C++ binaries (`gcc -O2 -static` with full `glibc 2.41`).**
   - Across **5 non-trivial `gcc 14.2 -O2 -static` (`glibc 2.41`) C workloads** (`sieve`,
     auto-vectorized `sha256`, `qsort_glibc`, `quicksort_c`, and recursive bitboard `nqueens`),
     `hyperion-dbt-v2` decodes **`27,835 / 27,835` (`100.00%`)** unique compiler/libc instructions
     and retires **`120,313,691` dynamic x86-64 + SSE2 instructions** from `_start` through
     `__libc_start_main`, TLS initialization, `ptmalloc2` (`brk`/`mmap`), SSE2 vector loops,
     formatted `printf`, and `exit_group(0)` with **100% bit-exact output** and **exact `0`-step
     discrepancy (`120,313,691 == 120,313,691`)** between Tier 0 and Tier 1.
   - **Performance (Apples-to-Apples, `N = 5` samples after 1 discarded warm-up):**
     - **Microbenchmark (`hyperion_tier_bench`, `3,000,002` retired instructions on AArch64):**
       Tier 0 (`797.05 ms`, `265.68 ns/insn`) vs Tier 1 JIT (`7.63 ms`, **`2.54 ns/insn`**) =
       **`104.5×` speedup** (`393.3 MIPS` even under `qemu-aarch64`).
     - **Full End-to-End `gcc 14.2 -O2 -static` (`glibc 2.41`) C Binaries (`120.3M` instructions):**
       Tier 1 Native ARMv8 + 128-bit NEON JIT achieves **`5.17×` to `20.12×` end-to-end process
       speedup over Tier 0** (**`10.43×` aggregate speedup**, `33.68 s → 3.23 s`), reaching
       **`71.86 MIPS`** on `sha256` and **`62.18 MIPS`** on `nqueens` — fast enough that even
       running inside `qemu-aarch64` on an x86-64 host, Tier 1 JIT **beats the native x86-64 C++
       interpreter in raw wall-clock seconds** (`287.8 ms` vs `361.1 ms` on `sha256`;
       `633.1 ms` vs `648.8 ms` on `nqueens`)!

---

## 2. Measured Multi-Sample Benchmark Results (`N = 5`, 1 Warm-Up Discarded)

Reproduce every number below from a clean build with one command:

```bash
./scripts/dev.sh cross-test
./scripts/run_real_benchmarks.sh
```

- **Host environment:** Linux `6.1.158+` `x86_64` (2 logical vCPUs, 4 KiB pages), `GCC 14.2.0`
  (`RelWithDebInfo`, `-O2 -g -DNDEBUG`), `glibc 2.41`, `qemu-aarch64` `10.0.13`.
- **Guest binaries:** Compiled directly from `benchmarks/workloads/*.c` with
  `gcc -O2 -static` (`x86_64`) and `aarch64-linux-gnu-gcc -O2 -static` (`aarch64`).

| Workload (`gcc 14.2 -O2 -static`, `glibc 2.41`) | Original `03487fc` | Dynamic x86-64 Insns | Native `x86_64` Median | Tier 0 (`x86_64` host) Median (`MIPS`) | Native `aarch64` (`qemu-a64`) | Tier 0 (`qemu-a64`) Median | **Tier 1 ARMv8+NEON JIT (`qemu-a64`) Median (`IQR`)** | **Tier 1 vs Tier 0 Speedup** |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`sieve`** (`1,000,000` primes) | Failed at insn #4 | `16,990,070` | `4.71 ms` | `293.62 ms` (`57.9 MIPS`) | `27.70 ms` | `4,665.15 ms` | **`903.06 ms`** (`97.6 ms`, `18.8 MIPS`) | **`5.17×`** |
| **`sha256`** (`256 KiB` auto-vectorized SSE2) | Failed at insn #4 | `20,682,677` | `2.87 ms` | `361.06 ms` (`57.3 MIPS`) | `28.88 ms` | `5,792.50 ms` | **`287.84 ms`** (`10.1 ms`, **`71.9 MIPS`**) | **`20.12×`** |
| **`qsort_glibc`** (`50,000` `int64` `glibc` `qsort`) | Failed at insn #4 | `21,606,066` | `6.85 ms` | `387.25 ms` (`55.8 MIPS`) | `55.12 ms` | `6,450.06 ms` | **`864.64 ms`** (`32.8 ms`, `25.0 MIPS`) | **`7.46×`** |
| **`quicksort_c`** (`100,000` `int32` Hoare sort) | Failed at insn #4 | `21,672,163` | `9.00 ms` | `378.96 ms` (`57.2 MIPS`) | `34.10 ms` | `6,046.26 ms` | **`539.53 ms`** (`20.4 ms`, `40.2 MIPS`) | **`11.21×`** |
| **`nqueens`** (`N = 12` bitboard recursion) | Failed at insn #4 | `39,362,715` | `8.43 ms` | `648.81 ms` (`60.7 MIPS`) | `41.81 ms` | `10,722.29 ms` | **`633.08 ms`** (`33.3 ms`, **`62.2 MIPS`**) | **`16.94×`** |
| **Total / Aggregate (`5` full binaries)** | **`0 / 5`** | **`120,313,691`** | **`31.86 ms`** | **`2,069.70 ms` (`58.1 MIPS`)** | **`187.61 ms`** | **`33,676.26 ms`** | **`3,228.15 ms` (`37.3 MIPS` overall)** | **`10.43×`** |

### Verified Bit-Exact Outputs & Instruction Counts

Every configuration above asserted both bit-identical stdout AND exact instruction count
(`x86_64 Tier 0 == aarch64 Tier 0 == aarch64 Tier 1 JIT`):

- `sieve`: `"sieve limit=1000000 primes=78498 checksum=0826a1753149ea53"` — **`16,990,070`** steps
- `sha256`: `"sha256 bytes=262144 digest=57749a2fa767585495d60eb772edc13638a8026cdd5770c7ab3b6437164aa9d6"` — **`20,682,677`** steps
- `qsort_glibc`: `"qsort_glibc n=50000 min=-9222952651363270109 mid=-13312439519853621 max=9223090793484727192"` — **`21,606,066`** steps
- `quicksort_c`: `"quicksort_c n=100000 min=-2147327570 max=2147439410 hash=1268567e6ff52830"` — **`21,672,163`** steps
- `nqueens`: `"nqueens n=12 solutions=14200"` — **`39,362,715`** steps

---

## 3. Architectural Advances Built in `hyperion-dbt-v2`

1. **Full Baseline x86-64 + 128-bit SSE2 Frontend (`decoder.{hpp,cpp}`, `guest_state.hpp`):**
   - `FS:` segment override (`0x64`), `66h` operand-size prefix, `67h` 32-bit address-size prefix,
     `F2h`/`F3h` `REPNE`/`REP` prefixes, high-byte `AH`/`CH`/`DH`/`BH` vs `SPL`/`BPL`/`SIL`/`DIL`.
   - Complete integer & bit-manipulation ISA (`MOV`, `MOVZX`, `MOVSX`/`MOVSXD`, `LEA`, `ADD`, `ADC`,
     `SUB`, `SBB`, `AND`, `OR`, `XOR`, `NOT`, `NEG`, `INC`, `DEC`, `CMP`, `TEST`, `SHL`, `SHR`,
     `SAR`, `ROL`, `ROR`, 1/2/3-operand `IMUL`, widening `MUL`/`IMUL`, 8–64-bit `DIV`/`IDIV`,
     `CBW`/`CWDE`/`CDQE`/`CWD`/`CDQ`/`CQO`, `CMOVcc`, `SETcc`, `BSWAP`, `BSF`/`BSR`/`TZCNT`/`LZCNT`,
     `BT`/`BTS`/`BTR`/`BTC`, `XCHG`, `CMPXCHG`, `XADD`, `PUSH`, `POP`, `LEAVE`, `CALL`, `RET`,
     `JMP`, `Jcc rel8/rel32`, `REP MOVS*`/`STOS*`/`CMPS*`/`SCAS*`, `CPUID`, `RDTSC`, `SYSCALL`, `HLT`).
   - 40+ 128-bit SSE/SSE2 packed-integer and scalar floating-point instructions required by the
     x86-64 psABI baseline (`MOVUPS`/`MOVAPS`/`MOVDQU`/`MOVDQA`, `MOVD`/`MOVQ`, `MOVLPD`/`MOVHPD`,
     `PAND`/`POR`/`PXOR`/`PANDN`, `PMOVMSKB`, `PCMPEQ*`/`PCMPGT*`, `PADD*`/`PSUB*`, `PMINUB`/`PMAXUB`,
     `PUNPCK*`, `PACKUSWB`/`PACKSS*`, `PSHUFD`/`PSHUFLW`/`PSHUFHW`, `PSRL*`/`PSLL*`/`PSRA*`,
     `UCOMISS`/`UCOMISD`, `CVTSI2SD`/`CVTTSD2SI`, scalar `ADD/SUB/MUL/DIV SS/SD`).

2. **512-Entry Software MMU `SoftTlb` + 4096-Slot Decoded ICache (`interpreter.{hpp,cpp}`, `address_space.{hpp,cpp}`):**
   - Direct-mapped 4 KiB page translation table (`read_entries[512]` + `write_entries[512]`, `2 MiB`
     working set per permission class) caching `host_addend = host_page - page_va`.
   - Separate code invalidation epoch (`code_epoch_` — incremented *only* when `MemPerm::kExec`
     mappings are mapped, unmapped, re-protected, or modified) so heap growth (`brk()`/`mmap(RW)`)
     never flushes compiled JIT blocks while unconditionally flushing data `SoftTlb` entries on
     every syscall to preserve memory safety across `std::vector` backing resizes.

3. **Full-System AArch64 + 128-bit ARMv8 NEON JIT (`aarch64_encoder.{hpp,cpp}`, `translator.{hpp,cpp}`, `translation_cache.{hpp,cpp}`):**
   - Shared trampoline ABI (`enter_jit`, 4096-slot in-JIT `DirectJumpEntry` jump table in `x28`-based
     `ExecContext`, shared `jit_budget_epilogue` in `x29`, shared `jit_exit_epilogue`) executing millions
     of chained blocks in native AArch64 registers without returning to C++.
   - Inline 8-instruction AArch64 `SoftTlb` load/store fast path with out-of-line cold miss stubs
     and instruction-granular architectural fault exits.
   - Branchless straight-line `CSEL` conditional block exits (eliminating internal host branches inside
     translated blocks) and native self-loop superblocks (`loop_top` in-register backedges).
   - Native 128-bit ARMv8 NEON SIMD lowering (`LDR/STR Qn`, `AND/ORR/EOR/BIC V0.16B`, `ADD/SUB V0.4S/V0.2D`,
     `SHL/USHR`, `ZIP1/ZIP2`, `SQXTUN/SQXTUN2`, `EXT`), cutting `sha256` execution time by **4.77×**
     over scalar-helper JIT and **20.12×** over Tier 0.

---

## 4. Current Honest Limitations (What `hyperion-dbt-v2` Does NOT Yet Support)

Per project engineering rules, unsupported boundaries are stated plainly:

1. **Dynamic ELF executables (`PT_INTERP` / `ld-linux-x86-64.so.2`):** Static (`-static`) and
   static-PIE binaries run out of the box; dynamically linked binaries with `PT_INTERP` are
   cleanly refused by `elf_loader` with a clear diagnostic rather than crashing midway.
2. **Multi-threading (`clone`, `futex`, shared address spaces):** `GuestState` + `GuestAddressSpace`
   currently model a single thread of execution (`42` Linux syscalls implemented; `clone`/`futex`
   return `-ENOSYS`).
3. **POSIX signal delivery (`rt_sigaction` guest trampolines) & non-SSE2 ISA extensions:**
   x87 FPU, AVX/AVX2 (`VEX`/`EVEX` 256/512-bit), and BMI1/BMI2 are not yet decoded.
