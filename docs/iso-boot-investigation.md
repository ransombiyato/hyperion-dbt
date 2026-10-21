# Booting an ISO with HYPERION — investigation and result

**Date:** 2026-10-19
**Question asked:** can the final product boot a small/tiny ISO?
**Result:** **No, and not for the reason a missing feature list would suggest.**
Two *independent* blockers each stop it before any boot logic runs. This
document records the experiment so the negative result is evidence, not
folklore.

---

## 1. What was actually tested

A genuinely bootable ISO was built and verified *outside* HYPERION first, so
that the ISO itself could not be the reason for a failure:

1. A 512-byte boot sector was written in 16-bit real-mode assembly
   (`cli`, segment setup, `ss:sp` at `0x7c00`, then a serial-output loop on
   COM1 that prints `HYPERION-ISO-BOOT-OK` and `hlt`s), assembled with
   `as --32` and linked with `ld -m elf_i386 --oformat binary -Ttext 0x7c00`.
   The trailing `0x55 0xAA` signature is present.
2. The sector was placed on an El Torito no-emulation boot image and an ISO was
   produced with `genisoimage -b boot.img -no-emul-boot -boot-load-size 1
   -boot-info-table`.
3. The ISO was booted in `qemu-system-x86_64 -nographic`. SeaBIOS reported
   `Booting from DVD/CD...` and the guest's serial output appeared. The raw
   sector was also booted directly as a floppy image and printed the same
   string. **The ISO and the sector are correct.**

## 2. What HYPERION did with it

```
$ hyperion run bootsect.bin --tier=0 --max-steps=50
status: unsupported
steps:  0
detail: unknown or unsupported opcode
rip =0x0000000000400000
```

The first byte of the image is `0xFA` — `CLI`, a 16-bit real-mode instruction.

## 3. Why this cannot work (two independent reasons)

**Blocker A — HYPERION is a user-mode translator, not a system emulator.**
There is no machine to boot. HYPERION implements no BIOS, no interrupt
descriptor table, no `int 0x10`/`int 0x13`, no devices, no memory-mapped I/O,
no privilege levels, and no boot protocol. Booting is a *kernel* responsibility;
the kernel-as-process tier in `docs/architecture-feasibility.md` is provisional
and explicitly unvalidated on the reference device (LIM-008, D-012). A boot
sector is not a user-mode program and nothing in the current design accepts one.

**Blocker B — real mode is out of scope.**
HYPERION decodes a small subset of **64-bit long mode** x86-64. The first
instruction of every BIOS boot sector is 16-bit real-mode code; there is no
decoder mode for it, and no plan to add one. Even with a complete ELF loader,
a boot sector would still fail at its first instruction.

There is also no ELF loader yet (`hyperion run` consumes a raw code blob), which
is a *third* gap, but fixing it would not make booting work: A and B are
independent of it.

## 4. What this means for the roadmap

The ISO question is not a task to schedule; it is outside the product's
category. The correct reading of "can it run a small program" is the *user-mode*
question, and the honest state of that is:

- `hyperion run <blob>` executes a raw x86-64 code blob through either tier.
- A real ELF program cannot run yet: there is no loader, and the translated
  subset is far too small (`push`/`pop`/`call`/`ret`, RIP-relative addressing
  and every SIMD form are unsupported).

The nearest *useful* next step — and the one this investigation produced — is
the ELF64 loader (`hyperion info <elf>`), which is a real prerequisite for
running anything from a filesystem. See `STATUS.md` §4.

## 5. Reproducing

```sh
# the boot sector and ISO used above
as --32 -o boot.o boot2.s          # source in the commit that added this file
ld -m elf_i386 -Ttext 0x7c00 --oformat binary -o bootsect2.bin boot.o
genisoimage -o hyperion-test.iso -b boot.img -no-emul-boot \
            -boot-load-size 1 -boot-info-table isoroot
qemu-system-x86_64 -nographic -boot d -cdrom hyperion-test.iso   # boots fine
hyperion run bootsect2.bin --tier=0 --max-steps=50               # status: unsupported
```
