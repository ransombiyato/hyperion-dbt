# DECISIONS

Architectural decisions with their reasoning. Format: context → decision →
consequences. Decisions are append-only; superseded entries are marked, not
deleted.

---

## D-001 — Documentation-as-memory, one file per concern

**Date:** 2026-10-19 · **Status:** accepted

**Context.** The project spans many sessions and multiple AI agents. Hidden
conversational memory is not a reliable store. A single README that doubles as
a changelog becomes unusable at scale.

**Decision.** Keep `README.md` as a stable orientation document, `STATUS.md`
for detailed current progress, `ROADMAP.md` for the full staged plan, and a
dedicated file per concern (`ARCHITECTURE.md`, `DECISIONS.md`,
`COMPATIBILITY.md`, `BENCHMARKS.md`, `TESTING.md`, `SECURITY.md`, ...).

**Consequences.** More files, but each stays small and stable. Every milestone
updates `STATUS.md` and `docs/agent-handoff.md`. The README does not grow into
a changelog.

---

## D-002 — No third-party code vendored until the licence/reuse analysis exists

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Reusing mature components (FEX, Box64, QEMU TCG) may be far safer
than reinventing complex subsystems, but silently vendoring code with
incompatible licences or unclear provenance is a real risk.

**Decision.** Phase 1 produces a written comparison and a licence inventory in
`THIRD_PARTY.md` *before* any third-party code is introduced. Reuse decisions
are recorded here.

**Consequences.** Slightly slower start; no licence or provenance surprises.
A future agent cannot accidentally import code without documenting it.

---

## D-003 — Flags: eager first, lazy later, always differential-tested

**Date:** 2026-10-19 · **Status:** accepted

**Context.** x86-64 arithmetic flags (CF, PF, AF, ZF, SF, OF) do not map
cleanly onto AArch64's NZCV. Correctness is easy to get subtly wrong.

**Decision.** Phase 2 implements flags *eagerly* (compute the full flag word
after each flag-setting instruction) for obvious correctness. Lazy flag
materialization is a Phase 5 optimization, introduced only with differential
tests proving equivalence with the eager implementation.

**Consequences.** Phase 2 is slower but trustworthy and serves as the oracle.
Optimization risk is isolated to Phase 5 behind tests.

---

## D-004 — IR between decoder and code generator

**Date:** 2026-10-19 · **Status:** accepted (implementation Phase 2+)

**Context.** Direct x86→AArch64 translation couples the decoder to the host
ISA and makes retargeting and optimization harder.

**Decision.** Introduce an explicit, typed intermediate representation. The
decoder never references AArch64; the code generator never references x86-64.

**Consequences.** An extra design layer to maintain, but a clean boundary for
optimization (Phase 5) and for the reference interpreter (Phase 2), which
executes the same IR the code generator consumes.

---

## D-005 — C++20, CMake, no mandatory third-party runtime dependency

**Date:** 2026-10-19 · **Status:** accepted

**Context.** The target device is Android 10 with no root and no package
manager. The core translator must build and run there without a dependency
ecosystem.

**Decision.** C++20, CMake ≥ 3.20, and a core library with **no** third-party
runtime dependency. Tooling (clang-format, clang-tidy, qemu-user) is
development-only.

**Consequences.** Portable to constrained targets. We forgo convenient
libraries for the core; tests use a small in-repo harness.

---

## D-006 — In-repo, dependency-free test harness

**Date:** 2026-10-19 · **Status:** accepted

**Context.** GoogleTest/Catch2 are convenient but add a dependency to the
target build and to every CI job. A translator needs many small,
table-driven and differential tests.

**Decision.** Implement a minimal harness (`tests/harness/test_harness.hpp`)
with test registration, `CHECK`/`CHECK_EQ`, filtering and CTest integration.
Revisit only if the harness becomes a bottleneck.

**Consequences.** No external dependency; trivial to run under `qemu-user`.
We give up some advanced features (mocking, parameterized fixtures) we do not
currently need.

---

## D-007 — Cross-compile + qemu-user as the standing AArch64 validation path

**Date:** 2026-10-19 · **Status:** accepted

**Context.** The reference target is AArch64, but development hosts are often
x86-64. Generated AArch64 code cannot be executed natively there.

**Decision.** Make `aarch64-linux-gnu-g++` + `qemu-aarch64` a first-class,
CI-enforced validation path, exercised from Phase 0 so it is proven before any
code generator exists. Static linking keeps the artifacts self-contained.

**Consequences.** AArch64 codegen can be developed and regression-tested on
x86-64 hosts. qemu-user is not cycle-accurate, so it validates *correctness*,
never performance; performance claims require real AArch64 hardware.

---

## D-008 — Host capabilities are probed, never assumed

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Page size, cache-line size and W^X policy differ across hosts and
affect code-cache design and correctness.

**Decision.** A `host_probe` component measures these at runtime and reports
them; benchmark and bug reports include the measured fingerprint.

**Consequences.** Code adapts to the real host. Probe results are included in
the benchmark JSON so measurements are attributable to an environment.

---

## D-009 — Benchmark reports may never contain fabricated numbers

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Performance claims are easy to inflate and hard to verify.

**Decision.** Benchmark output uses explicit `available: false` markers with a
reason for any metric that cannot be measured yet. `BENCHMARKS.md` states
"unavailable" rather than guessing. Every result records host, compiler, build
configuration and methodology.

**Consequences.** Early reports look sparse, which is honest. No result can be
mistaken for a measurement that was not taken.

---

## D-010 — W^X assumed on AArch64

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Modern AArch64 Linux enforces W^X; a page cannot be writable and
executable simultaneously. Code caches must be written then made executable.

**Decision.** The runtime writes code into `PROT_WRITE` memory, switches to
`PROT_READ|PROT_EXEC`, and performs explicit instruction-cache maintenance
(`__builtin___clear_cache`). No design may assume self-modifying executable
pages.

**Consequences.** Slightly more bookkeeping per emitted block; correctness on
hardened kernels and on the target device.

---

## D-011 — Scope includes kernel-dependent workloads; system-mode is not ruled out

**Date:** 2026-10-19 · **Status:** accepted

**Supersedes:** the Phase 0 assumption that "system-mode or kernel emulation"
is an explicit non-goal. That assumption is withdrawn.

**Context.** An early scoping choice limited HYPERION to user-mode translation
and treated kernel/system-mode emulation as out of scope. That was made before
any measurement of what the reference device (unprivileged, no KVM, ARM64
Android) can actually do. It silently capped the project's compatibility
ceiling: a user-mode translator cannot provide `systemd` as PID 1, containers,
or other kernel-dependent behaviour, no matter how good its instruction
coverage becomes.

The project's stated goal is a broadly compatible Linux execution environment,
not merely an instruction translator. That goal requires deciding what to do
about kernel semantics rather than assuming they are unreachable.

**Decision.** Kernel-level compatibility is **in scope for investigation**, not
assumed impossible and not assumed solved. Concretely:

1. The **user-mode translator remains the primary path** and the focus of
   Phases 2–5. It has demonstrated feasibility on Android and the highest
   performance ceiling.
2. The **syscall layer must be abstracted behind an interface** so that syscalls
   can later be directed either to the host kernel or to a user-space kernel
   implementation. This is a design constraint on Phase 6, not a feature.
3. **System-mode and kernel-as-a-process approaches are evaluated, not
   implemented**, until a target-device question is answered (see D-012).

**Consequences.** The design space is wider and the risk is higher. This is
accepted because the narrower scope could not meet the actual goal. The
evaluation and its measured evidence live in
[`docs/architecture-feasibility.md`](docs/architecture-feasibility.md).
Phases 2–6 are unaffected in ordering; Phase 6 gains an interface requirement.

---

## D-012 — Kernel-as-a-process (UML) is a candidate compatibility tier, pending AArch64 validation

**Date:** 2026-10-19 · **Status:** provisional

**Context.** A user-mode translator cannot provide real kernel semantics. One
mechanism can: run a real Linux kernel as an unprivileged user-space process
(User-Mode Linux). This was **measured**, not assumed.

Measured on 2026-10-19, x86-64 Linux host, uid 10001, no root, no KVM:

- UML booted kernel `6.12.101` from an isolated ext4 image (`/dev/ubda`), with
  the host root confirmed *not* visible inside the guest.
- `systemd 257.13-1~deb13u1` ran as PID 1 and reached `multi-user.target` and
  `graphical.target`; `dbus`, `systemd-logind`, and `getty@tty1` started.
- All namespace types (`user`, `mount`, `pid`, `uts`, `ipc`, `net`) worked
  inside the guest.
- Guest was single-CPU.

**Decision.** Treat kernel-as-a-process as a **candidate compatibility tier**,
and record it as *provisional* rather than adopted, because the decisive
question is unanswered: **whether a maintainable AArch64 UML exists and runs
under Android 10's kernel.** All evidence above is from an x86-64 host.

No kernel-as-a-process code will be built before that question is answered by
measurement on the target device.

**Consequences.** If AArch64 UML works on the target, HYPERION can plausibly
offer a compatibility tier for kernel-dependent workloads while keeping the
fast user-mode path for ordinary programs. If it does not, kernel-dependent
workloads remain out of reach without root, and that limitation must be stated
plainly in `COMPATIBILITY.md`. Either way, the user-mode path is unaffected.

**Risks.** Android's seccomp and SELinux policies may block the `ptrace` and
`mmap` behaviour UML requires. Performance of the syscall boundary is
unmeasured. See the risk table in the feasibility document.

---

## D-013 — Build the core; reuse the oracles and the designs

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Phase 1 required a prior-art survey before any engine code, so that
HYPERION does not reimplement a solved problem, and does not vendor code it
cannot legally or practically use. The survey is
[`docs/prior-art.md`](docs/prior-art.md); all licence claims in it were verified
against upstream licence files.

The finding that forces this decision: **FEX-Emu already implements almost
exactly the pipeline this project's roadmap describes** — x86-64 decoding, a
custom IR, an interpreter, an AArch64 JIT, a code cache with a disk tier, a
syscall layer and library thunks — under the permissive MIT licence, on the
target architecture, and in production use. Box64 covers the same ground with a
different technique (direct dynarec, no IR).

**Decision.**

1. **Build** the decoder, IR, reference interpreter, AArch64 code generator and
   code cache. These are the stated research object; reusing them would remove
   the project's reason to exist.
2. **Reuse as external processes, never linked**: `qemu-x86_64` and
   `qemu-aarch64` (differential-testing oracles), Zydis (independent decoder
   opinion), LLVM/binutils tooling (object inspection). This keeps GPL-2.0 QEMU
   out of the binary, per `THIRD_PARTY.md`.
3. **Reuse as design references, read but not vendored**: FEX's IR and
   `ThunkLibs` design, Box64's dynarec structure, dynarmic's AArch64 emitter.
4. **Defer the library-forwarding decision to Phase 9**, where a proven design
   is worth more than novelty.
5. **State the positioning honestly**: HYPERION is an experimental research
   platform for measuring translation and optimisation techniques. It is not a
   FEX competitor, and users who want to run software should use FEX.

**Consequences.** The project must not drift into "a worse FEX". The measurable
value is the comparison of design choices and the AI-driven optimisation loop
(Phase 8), not raw compatibility. Early phases stay small because nothing large
is being vendored. The cost is that correctness oracles must be wired up
deliberately rather than inherited.

**Supersedes:** nothing. This is the Phase 1 reuse-vs-build boundary required by
`ROADMAP.md` and anticipated by D-002.

---

## D-014 — The decoder contract is the subset boundary; no implicit widening

**Date:** Phase 2.
**Status:** Accepted.

**Context.** A translator is only as trustworthy as its definition of what it
claims to handle. The temptation is to decode "most" of an opcode and treat the
rest as close enough, which produces silently wrong execution.

**Decision.** `COMPATIBILITY.md` §2 is the single normative statement of the
supported subset. The decoder accepts exactly those encodings and rejects
everything else with a specific `DecodeError`. Where an encoding is *related*
to a supported one but not implemented (byte forms `0x88`/`0x8A`, rel32 `Jcc`,
RIP-relative addressing), it is rejected, not approximated.

**Consequences.** Some common encodings are refused for now, which is visible
and testable. In exchange, "it decoded" means "we implement it", which is the
property the differential tests depend on.

---

## D-015 — AArch64 code is emitted by a checked encoder, never hand-computed

**Date:** Phase 3.
**Status:** Accepted.

**Context.** The first temptation in a code generator is to write raw 32-bit
instruction words with comments. That is unverifiable, and a single wrong bit
produces an exception or, worse, silent wrong answers.

**Decision.** All AArch64 emission goes through `a64::Encoder`, which builds
words from named fields and throws on an out-of-range register or immediate.
Every encoding in the encoder has a unit test whose expected word is the output
of GNU `as`/`objdump` for the same instruction, not a value written from memory.

**Consequences.** Encodings are independently checked. Adding an instruction
requires capturing a binutils reference, which is a deliberate friction.

---

## D-016 — Executable memory is W^X: write, then execute, never both

**Date:** Phase 3.
**Status:** Accepted.

**Context.** Generated code must live in executable memory. The simple approach
is one `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)` region. That is exactly what W^X
policy (and, on Android, SELinux) exists to prevent.

**Decision.** `ExecMem` allocates read-write, is filled through a checked
`write()`, and is then made read-execute by `finalize()`, which also clears the
instruction cache. After `finalize()` the region is immutable through this API.

**Consequences.** The project does not need to weaken W^X to run its own code.
The cost is that code cannot be patched in place, which matters later for
self-modifying-code support (Phase 7) and will need a deliberate re-decision
then. See also D-010.

---

## D-017 — Flags are recomputed in generated code, not delegated to the interpreter

**Date:** Phase 3.
**Status:** Accepted.

**Context.** x86-64 flags (CF/PF/AF/ZF/SF/OF) are an awkward match for AArch64's
NZCV. The tempting shortcut is to have generated code call back into the
reference interpreter to compute flags. That would make the differential test
circular: both paths would be "verified" by the same code.

**Decision.** The translator computes every flag in emitted AArch64:

- SF, ZF, CF, OF come directly from `ADDS`/`SUBS` + `CSET` (N, Z, C/borrow, V).
- AF is `bit 4 of (a XOR b XOR result)`, an identity that holds for both ADD
  and SUB (verified exhaustively for 32- and 64-bit operands).
- PF is the inverted XOR-fold of the low byte's bits, computed with eight
  `UBFX`/`ADD` steps.

**Consequences.** The differential test is a genuine independent check: the
generated code and the interpreter share no flag logic. The cost is that PF and
AF are relatively expensive; Phase 5 replaces PF with a table or `CNT` lookup
once performance is measured. This decision also means an unsupported flag
combination must fail loudly rather than fall back to the interpreter.

---

## D-018 — The dispatcher's step limit is a hard bound; blocks are indivisible

**Date:** Phase 4.
**Status:** Accepted.

**Context.** The Tier 1 dispatcher runs whole translation blocks, not single
instructions, so it cannot stop in the middle of a block. The first
implementation checked the step limit *before* running a block and then added
the block's instruction count, which let a run limited to N instructions retire
more than N. The nested-loop differential caught this: tier 1 reported 1,000,001
retired instructions against a limit of 1,000,000, while the interpreter stopped
at exactly 1,000,000.

**Decision.** `max_steps` is a hard bound. Before running a block, the dispatcher
checks whether the block's instruction count fits in the remaining budget; if it
does not, the run stops with `kStepLimit` without starting the block. A
step-limited Tier 1 run may therefore stop a few instructions short of the limit
and report fewer steps than the interpreter, but it can never exceed the limit
or run past it.

**Consequences.** Step counts are exactly comparable between tiers for programs
that halt (the only case the differential asserts equality on). For programs
that hit the limit, the differential asserts `tier1_steps <= interpreter_steps
<= max_steps`. A future tier that can interrupt a block mid-way would remove the
granularity difference; until then this is documented as LIM-013 in
`docs/known-issues.md`.

---

## D-019 — The translation cache's hash map stays authoritative; the direct table is only a fast path

**Date:** Phase 5.
**Status:** Accepted.

**Context.** The dispatcher resolved the next guest PC through an
`std::unordered_map` after every block exit — a hash computation plus a pointer
chase per block, on the hottest path in the system. A small direct-mapped table
indexed by the low bits of the PC could answer almost every lookup in one array
access, but any fast path that can return a *wrong* block is a correctness
hazard, and a wrong block is exactly the kind of bug that runs silently.

**Decision.** `TranslationCache` gained a 4096-entry direct-mapped table
(`direct_[pc & 0xFFF]`). `lookup()` accepts a slot **only if `block->start ==
pc`**; any mismatch (a collision, or a never-seen PC) falls through to the hash
map, which remains the single source of truth. The fast path is therefore a pure
cache with no authority, so a collision is a safe slow-path miss, never a wrong
block. `install()` writes both structures so they cannot disagree.

**Consequences.** The correctness argument for the fast path is trivial (a slot
is used only when it holds exactly the requested block), which is why this
optimization was accepted on the strength of a deterministic counter rather than
a timing measurement. A test forces two PCs into the same slot and checks both
run correctly with no extra translation. If the table is ever made larger or
associative, the same `block->start == pc` guard must be retained.

---

## D-020 — An optimization that cannot be measured deterministically is judged on a deterministic proxy, and no speedup is claimed

**Date:** Phase 5.
**Status:** Accepted.

**Context.** Hyperion's AArch64 code is executed under `qemu-user` on an x86-64
development host. qemu has already translated the generated AArch64 to host code,
so the wall-clock cost of a change inside the generated code is heavily damped
and often below the run-to-run noise. Three of the five Phase 5 optimizations
(flag liveness, dispatch cache, immediate folding) move generated-code size or
dispatch counts, not measured time.

**Decision.** Each optimization must be justified by a **deterministic** proxy
that the change is expected to move: generated-code byte count, emitted
instruction count, or a dispatch/access counter. Where wall-clock is genuinely
measurable (the parity fast path), it is reported alongside; where it is not
(flag liveness, dispatch cache, immediate folding), the honest statement is "the
proxy moved, the wall-clock effect is not measurable here and is not claimed".
The proxy is then pinned as a regression guard in CI so the win cannot silently
regress.

**Consequences.** BENCHMARKS.md reports deterministic evidence for every
optimization and never presents a `qemu-user` timing delta as a hardware result.
The proxy must be causally tied to the change (the guard test asserts the specific
expected direction, e.g. a dead-flag `add` costs fewer words than a live one), so
it cannot be satisfied by an unrelated change. When real ARM hardware becomes
available, the timing claims can be revisited; the deterministic guards stay
valid regardless.

---

## D-021 — Guest-register allocation is per block, conservative, and gated on profitability

**Date:** Phase 5.
**Status:** Accepted.

**Context.** With flags recomputed in generated code and `rip` written once per
block, the remaining dominant cost is that every guest register access is a
load or store of `GuestState` in memory. Keeping the hot registers in host
registers within a block is the obvious next step, but a naive version (allocate
every register touched) *regressed* both code size and time on a small block,
because the save/restore prologue/epilogue cost more than the loads it removed.

**Decision.** Allocation is scoped to a single translation block and guarded by
a profitability model:

1. **Per block, not global.** A register is loaded at block entry, kept in a
   callee-saved host register (x19..x28) for the block, and stored at block exit.
   There is no cross-block allocation and no live-range splitting, so
   `GuestState` stays authoritative at every block boundary and the block
   signature is unchanged.
2. **Callee-saved host registers only.** x19..x28 are preserved by the AArch64
   ABI, so generated code that borrows them cannot corrupt its host caller.
3. **Profitability gate.** A register is allocated only when its body memory-op
   count exceeds the step increase in prologue/epilogue cost
   (`alloc_cost(n) = 2n + 2*ceil(n/2)`). This prefers *not* to allocate when
   unsure, which is why the 3-instruction benchmark loop is left alone.
4. **Oracle unchanged.** The reference interpreter is untouched; the change is
   accepted only if the full differential suite (which compares `rip` and flags)
   passes, so a misallocation cannot pass silently.

**Consequences.** The change is a measured win on blocks large enough to pay for
the prologue (608 vs 620 bytes, 9.05 vs 10.70 ns/guest-instruction on the `--wide`
workload, BENCHMARKS.md §4.2.6) and a no-op on tiny blocks. The cost is that the
profitability model is a static count, not a measured cost, and a register used
across a block boundary is still reloaded; both are documented limits, not
hidden. Writing the many-register differential test immediately exposed a
pre-existing halted-`rip` bug, which is the intended behaviour of the "oracle
unchanged, full state compared" rule.

---

## D-022 — Tools that the container wipes are cached inside the repository

**Date:** Phase 5.
**Status:** Accepted.

**Context.** The development container does not persist `/usr` between sessions,
but the repository directory does. A session can therefore start with the build
tree intact and no `cmake`, `qemu-aarch64` or AArch64 cross compiler, making a
previous green build unverifiable. Re-running the package bootstrap every session
is slow and depends on a network mirror.

**Decision.** The emulator and (optionally) the cross toolchain are cached under
`tools/cache/`, which is **git-ignored** so binaries and archives never enter
version control. `scripts/bootstrap.sh` populates the cache; `scripts/toolcache.sh`
manages it (`populate`, `restore`, `status`, `env`). The benchmark and cross-test
scripts prefer the cached `qemu-aarch64-static` when it exists, so a run uses the
same emulator across sessions.

**Consequences.** Recovery after a reset is `scripts/toolcache.sh restore`
(offline) rather than a full package install. The cache is machine-specific and
must never be committed; `.gitignore` enforces that, and the restore path is
verified by deleting the sysroot and restoring it (STATUS.md §2). If a cached
binary is ever stale or suspect, `HYPERION_QEMU` overrides it and `populate`
refreshes it from the system package.

---

## D-023 — The guest address space is explicit, permissioned, and owned by the caller

**Date:** Phase 6, second syscall slice.
**Status:** Accepted.

**Context.** The Phase 6 memory syscalls (`mmap`/`mprotect`/`munmap`/`brk`) need
somewhere to allocate. The interpreter's `FlatMemory` is a single flat region
with no permissions and no notion of mapped versus unmapped; using it as the
guest address space would make `mprotect` meaningless and let a guest read any
address. A real program also needs a heap that `brk` can grow without colliding
with `mmap` allocations.

**Decision.** Introduce `GuestAddressSpace`
(`include/hyperion/address_space.hpp`): a set of regions, each with a base,
length and `MemPerm` (read/write/exec), with page-granular checks on every
access. `map_image` maps a file image with explicit permissions;
`map_anonymous` allocates an anonymous region; `protect`/`unmap` change or
remove regions; `set_brk` grows or shrinks the heap inside a **reserved**
`[heap_base, heap_end)` window created at construction, so the heap and `mmap`
allocations can never overlap. The handler receives the address space from the
caller (a pointer, not a global and not owned), so the same instance is shared
with the interpreter or translation cache.

The `mmap` syscall wrapper maps guest `PROT_*`/`MAP_*` bits to `MemPerm` and
implements **anonymous private mappings only**. A request without `MAP_ANONYMOUS`
is reported unimplemented (`-ENOSYS`, counted in
`RunResult::unimplemented_syscalls`), never mapped as something else. For
`MAP_ANONYMOUS` the kernel ignores `fd`/`offset`, so the handler does not inspect
them — matching the kernel rather than inventing a stricter rule.

**Consequences.** Guest pointers are validated against a permissioned region
before every access, so a bad pointer is a returned error, never a host read.
A memory syscall's *return value* and its *implemented* status are tracked
separately (`MemResult`): a file-backed `mmap` returns a negative value that is
`-ENOSYS` **and** is counted as unimplemented, so a partial implementation can
never be silently reported as support. The `FlatMemory` type remains for tests
of the interpreter and decoder; it is not the guest address space.


---

## D-024 — Guest file descriptors are a handler-owned table of borrowed host streams, and `open` is deferred

**Context.** The second syscall slice gave the guest memory (`mmap`/`brk`) but
no way to read or write a file beyond `write` to fd 1/2. Real programs read
their input, so the next U4 numbers are `read`(0), `close`(3) and `lseek`(8).
The question is what a "guest file descriptor" is when the guest runs
in-process with no kernel: the handler could forward the guest's integer
straight to the host kernel (fast, but the guest can then name any host
descriptor the process happens to hold — a real capability leak), or it could
own an explicit mapping.

**Decision.** The handler owns an explicit **guest descriptor table**
(`guest fd -> host FILE*`). `read`/`close`/`lseek` look the guest fd up in the
table; a fd that is not present is `-EBADF`. The table starts with fd 0/1/2 on
the process's stdin/stdout/stderr, and the embedder can attach an already-open
host stream under another guest fd with `MinimalSyscallHandler::adopt()`. The
handler does **not** own the streams: `close` removes the guest's view of a
descriptor but never `fclose`s the borrowed stream.

`open`/`openat` were deferred in this increment (see D-025 for the decision that
later landed them): adding them means the guest chooses the host path, which is
a policy decision (sandbox root, allow-list, `AT_FDCWD` handling) that deserved
its own increment rather than being smuggled in with `read`. Until then a guest
can read only descriptors the host adopted.

**Consequences.** A guest integer can never name an arbitrary host descriptor:
the table is the boundary. The trade-off is expressiveness — a `FILE*` table
cannot represent `O_NONBLOCK`, `O_APPEND`, `pread`/`pwrite`, `fcntl` or `dup`,
which is acceptable while `open` is absent and will be revisited when it lands.
`read` validates the guest destination through the same `Memory` interface as
`write`, so a bad pointer is `-EFAULT`, not a host read.

---

## D-025 — `openat` forwards guest paths to the host kernel; Hyperion is not a sandbox

**Context.** D-024 deferred `open`/`openat` because letting the guest choose a
host path is a policy question. The options were: (a) forward the guest path
straight to the host kernel, (b) confine the guest under a sandbox root or an
allow-list, or (c) keep `open` unimplemented and force embedders to `adopt()`
every file. Option (c) blocks nearly every real program. Option (b) is a
security posture that SECURITY.md explicitly disclaims: Hyperion "is not a
sandbox, a hypervisor, or a security boundary."

**Decision.** Forward guest paths to the host kernel (option a), and state it
loudly. `openat`(257) is implemented for `AT_FDCWD` only; any other `dirfd`
returns `-ENOSYS` (a real `*at()` is not implemented, and resolving against the
wrong base would be worse than refusing). The pathname is read from guest memory
with a bounded `read_guest_cstr` (`-EFAULT` on a bad pointer, `-ENAMETOOLONG`
past `PATH_MAX`). Only an explicit set of `O_*` flags is honoured
(`O_RDONLY`/`O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC`/`O_APPEND`/`O_EXCL`); any
other flag returns `-ENOSYS` and is reported **unimplemented**, never silently
dropped. The opened stream is handler-owned (`GuestDescriptor::owned`), so
`close` releases it; `adopt()`ed streams remain borrowed.

**Consequences.** The guest can open whatever the *host process* may open —
exactly the SECURITY.md boundary ("no host filesystem access beyond what the
guest's own syscalls request and the host kernel permits"). Anyone who needs a
stricter boundary must impose it externally (chroot/namespace/container/PRoot);
Hyperion will not pretend to provide one. This keeps `openat` honest and
composable: a sandbox can later be layered as an `openat` interposer without
changing the ABI the guest sees. `fstat`(5) lands in the same increment and
writes the **guest** (x86-64) `struct stat` layout field-by-field, because the
host layout differs on AArch64 — a `memcpy` would silently misplace fields, the
same class of bug the register-allocation sign-conversion fix targeted.

---

## D-026 — Process-identity getters report the host process; Hyperion is single-process, single-threaded

**Context.** `getpid`/`gettid`/`getuid`/`geteuid`/`getgid`/`getegid`/`getppid`
are among the most frequently called syscalls (the trace ranks them near the
top), and a real program's startup calls them before almost anything else. The
question is what identity to report. Hyperion runs the guest *in-process*: there
is no separate guest kernel, no guest PID namespace, and no guest thread table.

**Decision.** Report the host process's real values by forwarding to the host
kernel. Do not synthesise a fake guest PID or namespace. `gettid` reports the
host thread id and therefore assumes **single-threaded** execution; this is
explicit and must be revisited when `clone`(56) lands.

**Consequences.** For a single-process, single-threaded guest — which is the
only kind Hyperion can run today — the host values are *indistinguishable from
the truth*: the guest's own `getpid()` returns the same number the process would
see natively. There is no lying and no emulated-namespace bookkeeping to keep
consistent. The trade-off is that once threading exists, `gettid` must change
(threads would all report the host's main-thread tid), and a future PID-namespace
feature would have to override these. Both are recorded as limitations rather
than papered over.

**Related.** `getcwd`(79) likewise forwards to the host working directory, with
the kernel's `-ERANGE`/`-EFAULT`/`-EINVAL` error semantics. `getrandom`(318)
fills the guest buffer from the host CSPRNG in 4 KiB chunks and validates the
`GRND_*` flag bits, returning `-EINVAL` for any undefined bit — matching the
kernel rather than silently masking. None of these touch guest memory except
through the validated address space (`-EFAULT`).

---

## D-028 — `access`/`readlink` forward to the host, with kernel-faithful edge cases

**Context.** `access`(21) and `readlink`(89) are common startup probes (glibc and
shells use them heavily). Both are inherently host-relative: the answer depends
on the host process's credentials and the host filesystem, exactly like
`openat` (D-025).

**Decision.** Forward both to the host. `access` restricts `mode` to the four
defined bits and returns `-EINVAL` for anything else. `readlink` mirrors the
kernel's surprising-but-documented behaviour: no NUL terminator, silent
truncation when the target exceeds `bufsiz`, and `-EINVAL` for `bufsiz == 0` or
a non-symlink path. Both read the path with the existing bounded
`read_guest_cstr`, and `readlink` clamps the guest `bufsiz` to 4096 rather than
allocating a guest-controlled buffer.

**Consequences.** No new policy: these are the same "forward to the host, never
silently do the wrong thing" rules as the rest of the file layer. `access` is
explicitly *not* a security boundary (a guest could re-`openat` anyway). The `at`
variants (`readlinkat`/`faccessat`) remain unimplemented and are listed in
STATUS.md's limitations.

---

## D-027 — One guest `struct stat` writer shared by `fstat` and `newfstatat`

**Context.** `fstat`(5) already built the guest (x86-64) `struct stat` field by
field, because the host (AArch64) layout differs and a `memcpy` would put values
at the wrong offsets. Adding `newfstatat`(262) — glibc's real `stat`/`lstat`
entry point — would have duplicated that 20-line, offset-sensitive builder,
creating a second place where an ABI offset could drift.

**Decision.** Extract the builder into a single `write_guest_stat(memory, buf,
host_stat)` and call it from both `do_fstat` and `do_newfstatat`. Keep the
field-by-field construction (never `memcpy`), and keep the guest offsets
declared once in `namespace guest_stat`. `newfstatat` maps
`AT_SYMLINK_NOFOLLOW` to the host `lstat` and everything else to `stat`; any
flag outside `{0, AT_SYMLINK_NOFOLLOW}` and any `dirfd` other than `AT_FDCWD`
returns `-ENOSYS` and is reported unimplemented.

**Consequences.** The ABI-sensitive layout is written in exactly one place, so
`stat`/`lstat`/`fstatat`/`fstat` cannot disagree about an offset. `newfstatat`
inherits the `AT_FDCWD`-only boundary of D-025 and the "unimplemented is
counted, never silent" rule. A dangling symlink is the test that pins the
follow/no-follow distinction: `stat` returns `-ENOENT`, `AT_SYMLINK_NOFOLLOW`
reports `S_IFLNK`.

---

## D-029 — The guest address space bounds and survives host allocations

**Context.** Each guest mapping is backed by host memory sized by a
guest-controlled value (`mmap`'s `length`). Two problems follow. First, an
enormous request (`mmap(0, 1<<60, ...)`) either throws `std::bad_alloc` out of
`std::vector::assign` — which, with no handler anywhere on the path, terminates
the whole host process — or, if it happens to fit, commits gigabytes of host
memory. Both are host-process failures driven by guest input, and neither is
acceptable for a translator that must survive arbitrary guest code.

**Decision.** (1) Add a per-mapping cap, `kDefaultMaxMapBytes` (1 GiB), checked
in `map_anonymous` *before* allocating; an oversized request returns `nullopt`,
which `do_mmap` reports as `-ENOMEM`. The cap is a constructor parameter so a
test can lower it and a future configuration file can tune it. (2) Wrap every
region backing allocation in a helper that catches `std::exception` and returns
`nullopt`, so even an allocation that fails below the cap (host genuinely out of
memory) becomes `-ENOMEM` rather than an uncaught exception. `map_image`,
`map_anonymous` and the `brk` heap growth all use it.

**Consequences.** A guest cannot terminate the host by asking for too much
memory; it sees the same `-ENOMEM` a real kernel would return. The cap is
per-mapping, not a total-address-space limit: many mappings under the cap can
still sum large, and a total-commit limit remains open (Phase 10, SECURITY.md
R5). `map_image` (the program loader) is not capped, because the image is
supplied by the host harness, not the guest.

---

## D-030 — `getdents64` forwards the host directory buffer verbatim

**Context.** `getdents64`(217) is the most frequent syscall in the trace sample
that was still unimplemented (3941 calls, `docs/syscalls/U4.md`) and the one
that turns `opendir`/`readdir` from "returns -ENOSYS" into working code. The
obvious question is whether the result must be rebuilt field by field, as
`fstat`/`newfstatat` do for `struct stat`.

**Decision.** Forward the host's bytes verbatim. `struct linux_dirent64` is a
fixed UAPI layout — `u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char
d_name[]`, `sizeof == 24` — and, unlike `struct stat`, it is **identical on
x86-64 and AArch64**. There is no ABI-sensitive translation to perform, so the
handler reads entries from the host directory fd and copies them into the guest
buffer unchanged. This also inherits the kernel's exact error semantics for
free: `-EBADF` (not open), `-ENOTDIR` (not a directory), `-EINVAL` (`count`
smaller than one 24-byte record) — verified on the host, including that `count
== 23` is `-EINVAL` and `count == 24` succeeds.

**Consequences.** Directory listing works in both tiers with no layout risk. The
guest `count` is clamped to a bounded read (8 KiB) before the host call, so a
huge request cannot drive one unbounded allocation (the same rule as `read`);
a short read is the correct result and the guest simply calls again. Because the
record layout is shared, this is a deliberate exception to the "rebuild
ABI-sensitive structures field by field" rule (D-027): the rule applies when the
host and guest layouts *differ*, which here they do not.

**Related change.** `openat` now honours `O_DIRECTORY` (guest `0x10000`, host
AArch64 `0x4000` — translated explicitly, never passed through), `O_NONBLOCK`
and `O_CLOEXEC`. glibc's `opendir` opens with `O_RDONLY|O_NONBLOCK|O_DIRECTORY|
O_CLOEXEC`, so without this a directory could never be opened and `getdents64`
would be unreachable. Flags still outside the set (`O_NOFOLLOW`, `O_PATH`, …)
remain `-ENOSYS`.

---

## D-031 — `statx` forwards the host structure verbatim

**Context.** `statx`(332) is the next-highest-frequency implementable syscall in
the trace sample (950 calls) after `getdents64`. `struct stat` had to be rebuilt
field by field because its layout differs between x86-64 and AArch64 (D-027).
The question is whether `struct statx` has the same problem.

**Decision.** It does not. `struct statx` is defined by the kernel's UAPI using
only fixed-width `__u32`/`__u64` fields and `struct statx_timestamp` (`__s64`
seconds, `__u32` nanoseconds), with explicit padding to a fixed 256-byte layout.
There is no `long`/`time_t`-width difference to accommodate, so the layout is
**identical on x86-64 and AArch64** and the host's bytes are copied into the
guest verbatim — the same reasoning as `linux_dirent64` (D-030). The handler
issues `SYS_statx` into a raw 256-byte buffer rather than declaring its own
`struct statx`, so it does not depend on the host's headers either, and the guest
offsets in `guest_statx` are asserted against the UAPI by the tests.

**Consequences.** The guest sees exactly what the host kernel reports, including
`stx_mask` (the host fills only the fields the guest requested). `AT_FDCWD` only,
as with `openat`/`newfstatat`; `AT_SYMLINK_NOFOLLOW` is the sole honoured flag,
and anything else (`AT_EMPTY_PATH`, …) is reported unimplemented rather than
silently dropped. The rule this establishes: rebuild a structure field by field
only when the host and guest layouts *differ*; when the UAPI fixes the width,
forward the bytes and let the kernel supply the semantics.

---

## D-032 — Memory operands are lowered to soft-MMU runtime helpers

**Context.** The AArch64 code generator was register-only: `validate_body`
rejected every instruction with a memory operand, so Tier 1 stopped at the first
load or store. Every real program loads and stores, which made this the largest
gap between the tiers. The obstacle is that the guest address space is a set of
host-backed regions at *arbitrary guest addresses* (`address_space.hpp`): guest
address `A` is not host address `A`, so generated code cannot dereference a guest
address directly.

**Decision.** A memory operand is lowered to a call into a runtime helper
(`hyperion_load`/`hyperion_store`, `runtime_helpers.hpp`), which resolves the
address through the same `Memory` interface the interpreter uses and copies the
bytes. The generated code computes the effective address
(`base + index*scale + disp`, mod 2^64) and calls the helper with it; the
helper's result is the value (load) or a fault flag (both). The current `Memory`
is held in a thread-local, installed by `TranslationCache::run` around the run
and restored on exit (RAII), so a block invoked with no dispatcher faults rather
than dereferencing a stale pointer, and nested runs do not clobber each other.

This is the standard **soft MMU** approach and it was chosen over a shadow page
table or a host-identity mapping because it keeps **one** definition of "valid
access" shared with the interpreter: the differential tests compare the two
tiers through the same interface, and there is no second address-translation
mechanism to keep correct. The helper ABI is the C ABI (`extern "C"`); a load
returns `LoadResult{value, fault}` in x0/x1 under AAPCS64, which is why the two
values travel together rather than through an out-pointer.

**Consequences.** MOV memory operands work in Tier 1 today; a faulting access
exits the block with `BlockResult::kMemoryFault` and the dispatcher maps it to
`RunStatus::kFault`, with `rip` on the faulting instruction — never a silent
wrong value. The ALU read-modify-write forms and INC/DEC on memory reuse exactly
this path (helper load, register operation, helper store), so they inherit the
same fault contract rather than adding a second one. An unsupported memory form
is still refused, not mistranslated. The cost is a helper call per access
(LIM-017); a fast path for identity-mapped regions or a host-page cache is
Phase 5 optimization work, and it must preserve the single-definition property
above. `push`/`pop`/`ret` remain interpreter-only until a guest stack is mapped.
