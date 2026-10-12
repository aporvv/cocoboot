# Booting with an external device tree (DTB)

Modern ARM Linux kernels (anything a PXA port targets today) need a
flattened device tree.  Since 0.6.2 Cocoboot can load a `.dtb` file
from the SD card and hand it to the kernel in `r2`, the way U-Boot
does — so you can iterate on your device tree without rebuilding the
kernel image every time (no more `CONFIG_ARM_APPENDED_DTB`).

## How Cocoboot works

Cocoboot is a PalmOS application (`.prc`) with two halves:

1. **m68k side** (`m68k/`) — the PalmOS GUI program:
   * reads `/cocoboot.conf` from the first volume that has it,
   * loads the kernel (and optionally initrd/DTB) from the SD card
     into large `FtrPtrNew` allocations,
   * pushes pointers/sizes onto a small big-endian "ARM stack" and
     enters native code with `PceNativeCall()`.

2. **ARM side** (`arm/`, the `armc` code resource) — the actual
   bootstrap, `boot_linux()` in `arm/boot.c`:
   * validates everything that can still fail (including the DTB)
     *while PalmOS is still alive*,
   * converts every pointer from PalmOS virtual addresses to physical
     ones using the OS page tables,
   * masks interrupts, jumps through an identity-mapped trampoline,
     switches the MMU and caches off,
   * writes the ATAG list at `ram_base + 0x100` (always, harmless and
     useful for debugging),
   * copies the initrd to `ram_base + <machine-specific offset>`,
   * copies the DTB to a free megabyte-aligned spot near the top of
     RAM and patches `/chosen` (see below),
   * jumps to the kernel with `r0 = 0`, `r1 = machine id`,
     `r2 = DTB address` (or the ATAG address when no DTB is
     configured — the kernel tells them apart by the `0xd00dfeed`
     magic at `r2`).

## What the loader does to your DTB

`arm/dtb.c` contains a small self-contained FDT patcher.  On a copy
of your blob (with 4 KiB of slack) it:

* sets `/chosen/bootargs` to the command line shown in the GUI
  (creating `/chosen` if needed).  Leave the command line blank to
  keep the `bootargs` baked into your `.dts`;
* if an initrd was loaded, sets `/chosen/linux,initrd-start` and
  `/chosen/linux,initrd-end` to where the initrd was copied —
  so a DT boot gets the same initrd the ATAG boot would have had.
  (With `CONFIG_ARM_ATAG_DTB_COMPAT` no longer required.)

Your original file on the SD card is never modified.

The blob is validated *before* interrupts are masked.  If it is
broken, boot is refused and the GUI log shows `DTB error: fd7b000n`:

| code       | meaning                                    |
|------------|--------------------------------------------|
| `fd7b0001` | bad magic (not `0xd00dfeed`)               |
| `fd7b0002` | unsupported FDT version (need >= 16)       |
| `fd7b0003` | header sizes/offsets inconsistent          |
| `fd7b0004` | corrupt struct block                       |
| `fd7b0005` | command line too long for the patch buffer |
| `fd7b000a` | no safe place in RAM for the DTB           |

## Usage

`cocoboot.conf`:

```
cmdline = console=ttyS0,115200 root=/dev/mmcblk0p1
kernel  = /zImage
initrd  = /initrd.gz      # optional
dtb     = /palm_t5.dtb    # or "none" for classic ATAG boot
```

The log line for the DTB shows `[ FDT ]` when the magic is right.

Kernel config notes:

* `CONFIG_OF=y`, `CONFIG_ATAGS` optional, `CONFIG_ARM_APPENDED_DTB`
  **not** needed (harmless if the DTB in r2 is valid — it takes
  precedence);
* the DTB must describe memory (`memory@...` node) — cocoboot does
  not inject a memreserve/ATAG-converted memory node;
* keep the DTB well under 1 MB.

## Building

With Nix (builds the whole prc-tools 2.3 cross toolchain, PilRC and
Cocoboot from source):

```
nix build                     # -> ./result/share/cocoboot/cocoboot.prc
nix develop                   # shell with m68k/arm-palmos-gcc, build-prc, pilrc
```

Without Nix you need prc-tools-remix, a PalmOS 5 SDK registered with
`palmdev-prep`, and PilRC on `PATH`; then plain `make` works.

## Host unit tests for the FDT patcher

`arm/dtb.c` is platform-independent; `test/run-dtbtest.sh` compiles it
natively, patches sample DTBs and verifies the result with `dtc` and
`fdtget` (needs `device-tree-compiler` installed):

```
sh test/run-dtbtest.sh
```
