# SECURITY

Threat model and safe execution boundaries for Project HYPERION.

---

## 1. What HYPERION is, security-wise

HYPERION is a **user-mode dynamic binary translator**. It decodes untrusted
x86-64 guest machine code and generates AArch64 code that executes **inside the
same process**, with the same privileges as the translator. It is *not* a
sandbox, a hypervisor, or a security boundary, and it must never be described
as one.

**Consequence:** running an untrusted guest program under HYPERION is
equivalent, in risk, to running that program natively. Guest code is only as
trusted as the user running the translator.

---

## 2. Assets and adversaries

| Asset | Risk |
| --- | --- |
| Host process integrity | A decoder/codegen bug could produce wrong or corrupting host code |
| Host filesystem / credentials | Guest syscalls are forwarded to the host kernel |
| Translator memory | Decoding and code emission handle untrusted-length input |
| CI credentials | Build scripts must not require or leak secrets |

**Adversary model.** A malicious *guest binary* attempting to escape the
translator's assumptions: malformed/overlong instructions, self-modifying code,
hostile syscall arguments, memory-ordering exploits. A compromised
*dependency* is also in scope for supply-chain review.

**Out of scope.** A malicious *host user*: HYPERION runs with the user's own
privileges and provides no privilege boundary against them.

---

## 3. Security requirements

### R1 — No privilege escalation
The translator requires **no root and no KVM**. It must run unprivileged on the
target device. Any feature needing elevated privileges is rejected or made
optional and off by default.

### R2 — Memory-safe decoding
The decoder is the primary attack surface: it consumes attacker-controlled,
variable-length byte streams. Requirements:

- Every read is bounds-checked against the mapped guest region.
- Instructions are length-limited (15 bytes maximum); the decoder must reject
  overlong encodings rather than over-read.
- Invalid encodings produce an explicit error, never undefined behaviour.
- Fuzz-tested from Phase 2 (see `TESTING.md` §8).

### R2b — Memory-safe image loading
The ELF64 loader (`src/core/elf.cpp`) is the second attack surface: an ELF image
is attacker-controlled bytes. Requirements:

- Parsing is separated from mapping. `parse_elf64()` reads only the file buffer,
  validates every field and program header against the actual file length
  (including `p_filesz <= p_memsz` and `p_offset + p_filesz <= filesize`), and
  allocates nothing; a malformed image yields a distinct `ElfError`, never
  undefined behaviour or a partial mapping.
- Segment mapping goes through `GuestAddressSpace`, so the same per-mapping size
  bound (R5) and the same page-granular permission checks apply as for `mmap`.
- A **dynamically linked** image is refused (`ElfError::kNeedsInterpreter`)
  rather than loaded, because loading it would need a userspace dynamic loader
  that does not exist here.
- Robustness is tested on hostile input: a bounded deterministic pseudo-fuzz
  (`elf.survives_random_and_mutated_input`) plus one unit case per rejection rule,
  clean under ASan/UBSan.

The loader is **not** a sandbox and does **not** validate the program's intent;
it only guarantees that the image is internally consistent and maps inside the
guest address space. See §4.

### R3 — Generated code is produced, never imported
The translator emits code only from validated IR through its own code
generator. It never `mmap`s guest bytes as executable and never `exec`s a guest
binary.

### R4 — W^X discipline
Code memory is written as `PROT_WRITE`, switched to `PROT_READ|PROT_EXEC`, and
flushed with explicit instruction-cache maintenance. No page is writable and
executable at once (D-010).

### R5 — Bounded resources
Code caches, translation tables and decoded-block storage have explicit bounds
or eviction policies, so a hostile guest cannot force unbounded host memory
growth.

The guest address space is the other place where a guest-controlled size reaches
a host allocation: `mmap`'s length is chosen by the guest, and each mapping is
backed by host memory. A per-mapping cap (`GuestAddressSpace::kDefaultMaxMapBytes`,
1 GiB by default, configurable) refuses an oversized request with `-ENOMEM`
*before* any allocation is attempted, and every backing allocation is wrapped so
that a failed `std::bad_alloc` becomes a clean `-ENOMEM` rather than an
uncaught exception that terminates the host process (D-029). The per-mapping cap
bounds a single request but not the *total* address-space size (many mappings
could still sum large); a total-commit limit is tracked as Phase 10 work.
Resource limits are tested in Phase 10.

### R6 — No secrets in the repository
No tokens, keys or credentials are committed. `.gitignore` excludes local
artifacts. CI uses repository secrets only via the platform's secret mechanism,
never inline. Any accidental secret is rotated, not merely deleted.

### R7 — Safe library forwarding
Host library forwarding (Phase 9) is **opt-in** and **allow-listed**. A guest
must not be able to call arbitrary host functions. Forwarded calls are validated
at the ABI boundary.

---

## 4. Safe execution boundaries (current)

At Phase 0 there is **no guest execution at all**, so the attack surface is
limited to the CLI and the host probe, which read only host metadata and take no
untrusted input.

When execution lands, the boundaries will be:

- Guest code runs in-process, unprivileged, in the guest's own address space.
- The translator's own metadata (code cache, tables) is kept out of guest-visible
  address ranges where practical.
- No network, no elevated syscalls, no host filesystem access beyond what the
  guest's own syscalls request and the host kernel permits.

**Never** run untrusted guest code outside the intended isolation boundary, and
never run it with elevated privileges.

### 4.1 Kernel-as-a-process tier: additional boundaries (provisional)

If the kernel-as-a-process tier (`docs/architecture-feasibility.md`,
`DECISIONS.md` D-012) is ever adopted, it changes this threat model and must be
re-reviewed before any code is written. Notes for that review:

- The guest kernel is **not a security boundary by itself.** UML is a
  compatibility mechanism, not a sandbox; running an untrusted kernel inside it
  does not confine that kernel's effects to a hypervisor-grade boundary.
- The guest kernel is **unprivileged with respect to the host**, which is a real
  containment property and must be preserved: no root, no KVM, no host devices.
- The guest kernel's host-side syscalls are the new attack surface, and the
  UML `ptrace`/`seccomp` interception paths in particular.
- A malicious guest could attempt to use the guest kernel to attack the host
  kernel. That is a host-kernel security question, not something HYPERION can
  fix, and it must be stated plainly rather than implied to be safe.
- File and network isolation for the guest must be configured explicitly; it is
  not granted by default.

**Current status:** none of this is implemented, and the tier is unvalidated on
the reference device. No untrusted workload should be run through it.

---

## 5. Reporting a vulnerability

Open a private security advisory on the repository, or contact the maintainers
directly. Do not open a public issue for an unpatched vulnerability. Include:

- affected commit/version, host architecture and OS,
- a minimal reproducer or guest instruction sequence,
- observed versus expected behaviour,
- an assessment of impact.

We will acknowledge, reproduce, and record the fix in `CHANGELOG.md` and, where
relevant, `docs/known-issues.md`.

---

## 6. Dependency and supply-chain policy

- The core has **no third-party runtime dependency** (D-005), minimizing supply
  chain surface.
- Every dependency is recorded in `THIRD_PARTY.md` with version and licence.
- New dependencies require justification and licence review (see
  `CONTRIBUTING.md` §7).
- Build scripts (`scripts/bootstrap.sh`) install only distribution packages from
  the official distribution archive.
