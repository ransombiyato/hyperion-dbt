# Known issues

Bugs, limitations and blocked work. Newest first within each section. An entry
is removed only when the issue is genuinely resolved, and the resolution is
noted in `CHANGELOG.md`.

---

## Blocked / cannot proceed without external input

| ID | Issue | Needs |
| --- | --- | --- |
| BLOCK-001 | No measurements from the reference target device (Snapdragon 855, Android 10). | Physical access to the device, or a comparable AArch64 Android host. Until then, all target-device claims are unverified. |
| BLOCK-002 | Development host is x86-64, so AArch64 code cannot be executed natively. | Mitigated by the cross + `qemu-user` pipeline (D-007). True AArch64 execution requires AArch64 hardware. |
| BLOCK-003 | The kernel-as-process tier cannot be committed to until the AArch64 UML port is run on the reference device. | AArch64 Android hardware. Until then the tier stays provisional (D-012) and no kernel-level code is written. |
| BLOCK-004 | The environment does not persist between sessions: a fresh container has no toolchain even if `build/` survives. | Run `scripts/bootstrap.sh`. Do not treat stale artifacts as evidence. See `STATUS.md` §2. |

---

## Limitations (by design, current phase)

| ID | Limitation | Notes |
| --- | --- | --- |
| LIM-002 | Syscall layer is a partial slice only. | Phase 6. Twenty-seven numbers are implemented: `read`(0), `write`(1), `close`(3), `fstat`(5), `lseek`(8), `mmap`(9), `mprotect`(10), `munmap`(11), `brk`(12), `access`(21), `getpid`(39), `exit`(60), `getcwd`(79), `readlink`(89), `getuid`(102), `getgid`(104), `geteuid`(107), `getegid`(108), `getppid`(110), `gettid`(186), `getdents64`(217), `exit_group`(231), `openat`(257), `newfstatat`(262), `readlinkat`(267), `getrandom`(318), `statx`(332); every other number returns `-ENOSYS`. `mmap` is anonymous-only; `openat` is limited to `AT_FDCWD` and a bounded set of flags; the descriptor table maps to `FILE*`, so `pread`/`pwrite`/`fcntl`/`dup` are not expressible yet (D-024). |
| LIM-003 | The translation cache **never invalidates**. | Self-modifying guest code runs stale translations. Deliberate boundary for the first version; precise invalidation is Phase 7. |
| LIM-004 | `qemu-user` validates correctness, not performance. | Never quote qemu numbers as performance. |
| LIM-005 | Benchmark harness measures host only. | `guest_execution.available = false` until a Tier 1/Tier 0 benchmark exists (D-009). No Tier-1 performance measurement exists yet. |
| LIM-006 | No Windows/macOS guest support. | Out of scope, permanently. |
| LIM-007 | Target device has no KVM and no root; the design must not assume either. | Enforced as a project rule. |
| LIM-008 | **Kernel-level compatibility is investigated, not implemented.** | The user-mode translator cannot provide `systemd` as PID 1, containers, or kernel-dependent services. The kernel-as-process tier is provisional and unvalidated on the target (D-011, D-012). |
| LIM-009 | AArch64 UML is an **out-of-tree** dependency (38 unmerged commits). | If the tier is adopted, it carries a maintenance liability (R7 in `docs/architecture-feasibility.md`). |
| LIM-010 | **ALU memory operands are interpreter-only; `push`/`pop`/`ret` are interpreter-only and `push`/`pop` are cut from translated blocks.** | The interpreter executes ModRM memory operands through the `Memory` interface and implements `push`/`pop`/`ret` (with a guest stack). The Tier 1 translator now translates **MOV** memory operands (load and store, 32/64-bit) through the soft-MMU runtime helpers; every *other* memory operand (an ALU read-modify-write such as `add r, [m]`/`add [m], r`) is still refused with `TranslateError::kUnsupportedOperand`, so a block that reaches one stops with the PC on the instruction rather than mistranslating. `push`/`pop` remain block-cutting (`is_block_body_opcode`). The translated subset is register/immediate ALU plus `mov` (register, immediate, and memory), `nop`/`hlt`/`jmp`/`jcc`. |
| LIM-011 | **No cross-block register allocation.** | Register allocation is per block (D-021): a block keeps its most-used guest registers in callee-saved host registers, but every guest register is spilled back to `GuestState` at each block exit, so a value cannot stay in a host register across a block boundary. Correct; a Phase 5 follow-up. |
| LIM-012 | **No block chaining.** | Every block exit returns to the dispatcher for a hash lookup (`translation_cache.hpp`). Correct but slow; Phase 5. |
| LIM-013 | **Step-limit granularity differs between tiers.** | The interpreter steps one instruction at a time; the dispatcher runs whole blocks. On a step-limited run, tier 1 may stop a few instructions short of the limit (it never exceeds it). Only relevant to non-halting programs. |
| LIM-014 | `qemu-user` runs generated code, but it is **not the reference device**. | No measurement exists on the Snapdragon 855 / Android 10 target. |
| LIM-015 | **`openat` is limited to `AT_FDCWD` and a bounded flag set.** | Phase 6. `openat`(257) is implemented for `AT_FDCWD` and a small set of flags; a real `dirfd`, or a flag outside the set, returns `-ENOSYS` and is counted as unimplemented. The descriptor table maps to `FILE*`, so `O_APPEND`, `pread`/`pwrite`, `fcntl` and `dup` are not expressible yet (D-024); `O_NONBLOCK` is honoured as a flag. |
| LIM-016 | **The address-space memory bound is per-mapping, not total.** | A single `mmap` larger than `kDefaultMaxMapBytes` (1 GiB) is refused with `-ENOMEM` (D-029), but many mappings each under the cap can still sum large. A total-commit limit is Phase 10 work (SECURITY.md R5). |
| LIM-017 | **A translated memory access costs a helper call (soft MMU).** | Because guest address `A` is not host address `A`, every translated memory operand (MOV, the ALU read-modify-write forms, INC/DEC) lowers to `hyperion_load`/`hyperion_store` calls rather than a direct host load/store. Correct and shared with the interpreter's notion of a valid access, but slower than a hardware access; a fast-path (host-page caching, or direct access for identity-mapped regions) is Phase 5 optimization work. |
| LIM-018 | **An ALU memory operand recomputes its effective address.** | The load helper call clobbers the scratch registers that held the effective address, so a read-modify-write store recomputes `base + index*scale + disp` rather than keeping it live across the call. A few extra instructions per RMW through memory; a register that survives the call (or a spilled address) would remove them. |
| LIM-019 | **HYPERION cannot boot a machine image (ISO/disk/boot sector), and this is out of scope, not pending.** | There is no machine to boot: no BIOS, no IDT, no `int 0x10`/`int 0x13`, no devices, no privilege levels, and no 16-bit real-mode decoder (the first instruction of every BIOS boot sector is real-mode code). Booting is a kernel responsibility; HYPERION is a user-mode translator. Verified, not assumed: a correct bootable ISO (confirmed booting in `qemu-system-x86_64`) stops at `status: unsupported` on its first byte. See `docs/iso-boot-investigation.md`. |
| LIM-020 | **The ELF loader maps static images only; a loaded program still stops at the first unimplemented instruction.** | An ELF64 static executable is parsed, validated and mapped, and its startup stack is built (`hyperion info`/`run <elf>`). What is missing is *execution* of real programs: the instruction subset has no `push`/`pop`/`call`/`ret` (guest stack), no RIP-relative addressing, and not all ALU immediates, so a real `gcc -static` binary loads at its true entry and stops at `0x4015db` (`67 e8`, `addr32 call`) with an honest `kUnsupported` (the `0x67` prefix is now the first blocker, after the `0xC7` `mov r/m, imm32` increment). A **dynamically linked** image is rejected with `ElfError::kNeedsInterpreter` (there is no userspace dynamic loader). The next task is the guest stack (STATUS.md §4). |
| LIM-021 | **The release tarballs are pre-release prototypes, not usable products.** | They contain a working `hyperion` that builds, probes, tests and runs a raw blob through either tier; they do **not** run a real Linux program (LIM-020 and the small instruction subset). `--prerelease` is set on the GitHub release for this reason. |

---

## Correctness bugs

No open correctness bugs. Recorded history:

| ID | Symptom | Status |
| --- | --- | --- |
| BUG-001 | 64-bit `ADD` never set carry (`sum > mask` is impossible when the mask is `~0`). Found by the hardware differential. | Fixed (Phase 2). |
| BUG-002 | `Jcc` short jumps (`0x70..0x7F`) were decoded as `rel32`, producing wild targets. Found by the hardware differential. | Fixed (Phase 2). |
| BUG-003 | A block whose *first* instruction was unsupported was emitted as an empty fall-through, so the dispatcher looped forever. | Fixed (Phase 4); `translator.dispatcher_stops_at_unsupported` guards it. |
| BUG-004 | The code-fetch window shrank only to 15 bytes, so a program image shorter than that could not be translated. | Fixed (Phase 4); the CLI smoke test guards it. |
| BUG-005 | The dispatcher checked the step limit before running a block and then added the whole block count, so a 1,000,000-step run could retire 1,000,001 instructions. Found by the nested-loop differential. | Fixed (Phase 4); the step bound is now hard. |

When a bug is found, record: ID, symptom, minimal reproducer, affected commit,
severity (correctness/performance/cosmetic), status, and the test that guards
against regression.

---

## Toolchain and environment issues

| ID | Issue | Status |
| --- | --- | --- |
| ENV-001 | `g++-aarch64-linux-gnu` is required in addition to `gcc-aarch64-linux-gnu`; installing only the C cross-compiler breaks the cross test with "CXX compiler not found". | Resolved in `scripts/bootstrap.sh`, which installs both. |
| ENV-002 | ASan interacts with custom executable code caches. | Not yet encountered (no code cache). Expected in Phase 3; document per-test exclusions when they arise. |
| ENV-003 | CI prints a Node.js 20 deprecation annotation on every job: `actions/checkout@v4` is forced to run on Node.js 24. | Cosmetic; jobs pass. Resolve by bumping the pinned action version when a Node-24 action release is available. |

---

## Benchmarking defects

| ID | Issue | Status |
| --- | --- | --- |
| BENCH-001 | The Phase 1 benchmark harness reported a **fabricated** result. `-O2` hoisted the loop-invariant calibration call out of the repeat loop, so every timed region collapsed to ~0.00003 ns/op (~30 ps per iteration — far below one clock cycle) while the checksum stayed correct, so the error was silent. Detected by checking that total time scales with `--iterations`. | **Fixed.** An `asm` optimisation barrier now keeps the work between the clock reads, and the harness rejects an implausible timing (faster than one cycle per iteration) instead of reporting it. Regression test `bench.timing_not_eliminated`. |

---

## Rejected optimization candidates

Recorded so a future agent does not repeat a dead end.

| ID | Candidate | Verdict |
| --- | --- | --- |
| OPT-001 | Clear CF/OF/AF in `emit_flags_logical` by storing the AArch64 **zero register** (`strb wzr, …`) instead of `movz kT1, #0` + three `strb`s. | **Rejected — not encodable.** `strb w31, [x8, #7]` is an assembler error: the zero register is not a valid *source* for a store. Verified against `aarch64-linux-gnu-as`. No instruction saved. |
| OPT-002 | Invert PF in `emit_parity` with a logical-immediate `eor w3, w3, #1` instead of `movz kT1, #1` + `eor`. | **Deferred — not worth the cost.** The encoding exists (`eor w3, w3, #1` = `0x52000063`) but Hyperion has no logical-immediate encoder, so it needs a `BitMasks` decoder (~40 lines) plus tests for a single-instruction saving on a cold path. Low value; revisit only if PF generation shows up hot in a profile. |

---

## Open questions

| ID | Question | Impact |
| --- | --- | --- |
| Q-001 | Which upstream components, if any, should be reused rather than built? | Phase 1 decides; affects everything downstream. |
| Q-002 | Custom IR versus direct translation for the first code generator? | Phase 1/2; see D-004 for the current lean. |
| Q-003 | How to model x86 TSO memory ordering on weakly ordered AArch64? | Phase 7; affects correctness of multithreaded guests. |
| Q-004 | Does the AArch64 UML port build and boot under Android 10 on the Snapdragon 855, with no root and no KVM? | Decides whether the kernel-as-process tier is real (R1/R2). Blocks any kernel-level code. |
