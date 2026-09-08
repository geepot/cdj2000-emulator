# Building

Two emulators, built separately, from sources that live outside this repository.
Neither build needs firmware.

## What you need

* **Python 3.10 or newer**, with `tkinter`. On Debian and Ubuntu that is a
  separate package, `python3-tk`.
* **A C toolchain**, `make`, `patch`, `tar`.
* For the MAIN board: **meson**, **ninja**, **pkg-config**, **glib2**,
  **pixman** -- QEMU's own dependencies.

On Windows this is developed and tested in an **MSYS2 MINGW64 shell**:

```sh
pacman -S --needed base-devel mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-{glib2,pixman,meson,ninja,pkgconf,python}
```

Use the MINGW64 shell, not Git Bash. Git Bash's `make` resolves `/bin/sh` to a
path containing a space, and GDB's build system does not quote it -- the build
then fails with `C:/Program: No such file or directory`, which looks like a
broken toolchain and is not one.

Python packages:

```sh
pip install -r requirements.txt        # Pillow, numpy
pip install -r requirements-dev.txt    # ... plus pytest
```

## The GUI board -- Blackfin, from GNU sim

```sh
sh scripts/build-bfin-sim.sh
```

The script downloads `gdb-17.2.tar.xz` if it is not already beside the
repository or in `build/`, unpacks it, applies `patches/0*-gdb-*.patch`,
configures the simulator only, builds it, and installs it as `bin/cdj-run`.
Re-running it is a no-op on an already-patched tree.

Point it at a tarball or an unpacked tree if you have one:

```sh
sh scripts/build-bfin-sim.sh /path/to/gdb-17.2.tar.xz
sh scripts/build-bfin-sim.sh /path/to/gdb-17.2
```

The script builds `libbfd`, `libiberty` and `libopcodes` first, because
`bfin/run` links against them and only GDB's top-level makefile knows to build
them. It passes `MAKEINFO=true` throughout -- `makeinfo` builds the manuals and
nothing else, is often not installed, and without this the build stops on
`doc/bfd.info` with an error that says nothing about the simulator. A failure in
`po/` (translation catalogues, which want `msgfmt`) is likewise ignored, and the
three libraries are then checked for directly, so a real failure still stops the
script.

The result is around 17 MB. If you end up with something near 50 KB you have
libtool's wrapper script rather than the simulator; the script checks for
exactly this and refuses, because the wrapper exits 127 with no output and is a
confusing way to spend an afternoon.

The simulator is compiled `-O3 -march=native -g` with `--enable-sim-inline`
and `--disable-sim-assert`, because the interpreter's inner loop is the whole
cost of a run. `--march=nocona` gives a binary that runs on any x86-64,
`--opt=-O2` the upstream level, `--profile` a `-pg` build installed as
`bin/cdj-run-pg` for gprof, and `--reconfigure` throws the object tree away
first -- configure only runs when there is no `config.status`, so a change of
flags does nothing without it. `CDJ_SIM_CFLAGS` replaces the flags outright.

## The MAIN board -- SH-4, from QEMU

QEMU is not vendored here. Clone it wherever you like:

```sh
git clone --depth 1 https://gitlab.com/qemu-project/qemu.git /c/qemu-src
sh scripts/build-qemu-sh4.sh /c/qemu-src
```

The script applies `patches/qemu-sh-intc-priority-imask.patch`, copies
`emulator/qemu/*` into `hw/sh4/`, adds the meson and Kconfig lines if they are
not there, configures for `sh4-softmmu` only, and builds. All of it is
idempotent, so re-run it after every change to `emulator/qemu/`.

QEMU **11.x or newer** is required.

Put the result on `PATH`, or point `CDJ_QEMU` at it:

```sh
export CDJ_QEMU=/c/qemu-src/build/qemu-system-sh4
```

Check the board is there:

```sh
qemu-system-sh4 -M help | grep cdj2000
```

### Rebuild immediately, and check the timestamp

If a build of the board fails, the **old** `qemu-system-sh4` is still on disk
and every later run keeps measuring the old behaviour, while looking exactly as
though your change did nothing. This is the most expensive silent error in the
project. After each build, check the binary is newer than the source you
changed.

## Tests

```sh
pytest -q                 # or: python -m pytest -q
```

Without firmware, roughly two dozen tests skip and the rest run. The skips are
the ones that read a real firmware image; see FIRMWARE.md. `tests/cstub/`
compiles `emulator/qemu/cdj2000_input.c` on the host against stub QEMU headers,
so the panel input protocol is tested without QEMU at all.

## Where things land

| directory | holds | in git? |
|---|---|---|
| `bin/` | the Blackfin simulator you built | no |
| `build/` | the unpacked GDB tree and its object tree | no |
| `firmware/` | boot images built from your own firmware | no |
| `packets/` | stimulus the tools generate | no |
| `runs/` | frames, logs and captures from runs | no |

All five are in `.gitignore` and all five can be moved: `CDJ_BIN_DIR`,
`CDJ_BUILD_DIR`, `CDJ_FIRMWARE_DIR`, `CDJ_PACKETS_DIR`, `CDJ_RUNS_DIR`.

With one exception. The board files in `emulator/` name their flash image as
`firmware/gui-flash-image.bin`, and the simulator resolves that itself, against
the working directory the launchers set -- the repository root. So
`CDJ_FIRMWARE_DIR` moves everything the Python side reads but not that one file.
Copy the board file and pass `--board` if you need it elsewhere.

## macOS migration

The `codex/macos-nxs` branch is bringing this project to macOS. Install Xcode
Command Line Tools and Homebrew `make`, `gmp`, and `mpfr` before running
`scripts/build-bfin-sim.sh`. On macOS the script uses `gmake`, BSD-compatible
tar arguments, system zlib and Homebrew's arithmetic libraries. `CDJ_MAKE`
can select another GNU make executable. Patch 03 fixes BSD sed module
registration, Blackfin sign extension on LP64 hosts, and a missing POSIX
header in SPORT socket handling. Applied-patch checksums are stored in the
GDB source tree; changing a patch requires a fresh source tree.

Extract a locally supplied combined NXS updater:

```sh
python3 -m tools.cdj_gui.nxs_container /path/to/C2KNXS.UPD firmware/nxs/updates
python3 -m tools.cdj_gui.extract firmware/nxs/updates/C2KGUI.UPD firmware/nxs
```

The splitter validates the four manifest lengths, NXS component identities,
and all CRCs before writing byte-identical component files. `container.json`
records the source and component SHA-256 hashes. All generated files remain
under ignored `firmware/`.

This does **not** establish NXS MAIN compatibility. The QEMU board and proxy
still contain original-CDJ-2000 firmware assumptions. The NXS MAIN register
map, native MAIN–GUI link, flash geometry and real C674x DSP execution remain
migration work. GUI ELF loading bypasses the resident flash bootloader.

Initial input verification on this Mac: all four extracted component files
match the earlier independent NXS parser byte-for-byte. The five GUI ELF
regions also match its materialized LDR data. The earlier harness additionally
staged four INIT bytes at `0xff800060`; this fork omits them. That boot-stage
assumption needs reconciliation before claiming equivalent startup behavior.

Verified on Apple Silicon macOS (2026-09-08): `bin/cdj-run` builds as a
Mach-O arm64 executable, and rerunning the build succeeds. A five-second
stock NXS GUI run reaches core-timer and SPORT initialization and exits at
the configured wall-clock limit without a reported CPU exception. It reports
unconnected GPIO ports 2 and 4; this is not a full boot or display validation.
Host suite: 156 passed, 42 skipped. Skipped tests need additional firmware or
runtime fixtures. QEMU has not yet been built on this Mac for this fork.

### Diagnosing GUI startup on macOS

Patch 04 enables the file-backed framebuffer on POSIX hosts. The build
explicitly clears `SDL_CFLAGS`: SDL autodetection otherwise selects a backend
that does not publish the PPM file consumed by the Python viewer. Existing
SDL-built `gui.o` is rebuilt when switching to the file backend.

Useful viewer overrides:

```sh
--env BFIN_FAST_LZSS= --env BFIN_STATS=5 --env BFIN_EXCEPTION_TRACE=1 \
--env BFIN_GUI_TRACE=1 --env BFIN_GUI_FRAME_TRACE=1
```

Empty values remove an environment variable. Statistics include guest PC,
instruction count, display DMA, completed/published frames and received link
bytes. The native NXS GUI-only run now publishes frames and displays
`E-8709: COMMUNICATION ERROR`, with `link_rx=0`, because MAIN is not connected.
Some image regions are visibly corrupt; framebuffer publication is verified,
but image fidelity is not. This run does not demonstrate full-system boot.

### Experimental full NXS board pair

QEMU source tested on macOS: `55347990687e7bc5b6b0d624f290025726e8fbfa`
(version 11.1.50). The interrupt trace-event patch context was refreshed for
that revision. Build with:

```sh
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
```

`cdj2000nxs-main` has 128 MiB SDRAM, matching the NXS boot stack at
`0xac000000`. The original `cdj2000-main` retains 64 MiB. With 64 MiB, stock
NXS reset code entered the error loop at `0xa00004e6`; with 128 MiB it reaches
the application and sends GUI status records. Peripheral fidelity and DSP
execution remain incomplete; this profile is explicitly experimental.

The GUI launchers now enable `BFIN_PARALLEL_WRITEBACK=1` by default. Without
it, a parallel arithmetic/store packet writes the newly calculated register
value instead of the old value, corrupting resource relocation. The
instruction-level regression in `tests/bfin/parallel-store.s` fails with
stored value 11 instead of 7 when the fix is disabled, and passes when enabled.
The NXS GUI-only screen becomes readable with this fix.

Prepare `firmware/nxs/main-firmware.bin` using the validated NXS loader, then:

```sh
python -m tools.cdj_main.nxs_vm runs/nxs-check --seconds 60
```

This diagnostic runner connects QEMU's request and status channels on ports
5980 and 5982, with panel control on 5984. It uses stock firmware traffic,
without a link proxy or replay file. Each new run directory contains commands,
explicit environment overrides, logs, traffic and the framebuffer. A zero
exit means the bounded run completed and produced a frame; it does not certify
full boot, input fidelity or actual C674x DSP execution.

Direct-link verification (45 seconds, `runs/nxs-linked-two-channels`):
stock NXS MAIN and GUI exchanged bidirectional traffic and the GUI rendered
`Not Loaded.` with `E-7010: DSP DEVICE ERROR`. The communication error cleared.
This identifies the next blocker as DSP support; it does not validate audio
or the original model's behavioral DSP against NXS. Host suite including the
new Blackfin instruction regression: 157 passed, 42 skipped.
