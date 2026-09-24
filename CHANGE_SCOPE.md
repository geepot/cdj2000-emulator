# Contribution scope

This branch contains two emulator profiles plus infrastructure shared by both.
The profile is selected explicitly:

```sh
# Original CDJ-2000 firmware and legacy panel profile
python -m tools.cdj_main.launch 2000 --sd runs/card.img

# CDJ-2000NXS firmware, panel map and NXS DSP path
python -m tools.cdj_main.launch nxs runs/nxs-demo \
  --sd runs/card.img --ui --seconds 3600
```

The launcher passes the remaining arguments to the existing profile-specific
entry point, so `--help` after a model shows that model's full options.

## Shared by CDJ-2000 and NXS

These changes are intended to remain useful to both profiles:

- the two-board Blackfin viewer, faceplate layout, input-control channel and
  mouse/keyboard hold handling;
- macOS-safe Blackfin build/runtime patches and the generic link/frame tools;
- MAIN DMAC source/destination addressing, serial/input plumbing and their
  firmware-free regression tests;
- generic QEMU build scripts, test harnesses, link decoding and documentation.

`emulator/qemu/cdj2000_main.c` contains both shared MAIN behavior and explicit
NXS-board conditionals. A change in that file must be tested against both
`cdj2000-main` and `cdj2000nxs-main`; it is not automatically NXS-only.

## CDJ-2000-only behavior

The original profile remains the compatibility path for the firmware and
layout that shipped in this repository:

- `tools/cdj_main/view_vm.py` and the legacy panel key map;
- QEMU machine `cdj2000-main`, its 64 MiB memory layout and original MAIN
  firmware transport;
- legacy update/media helpers and the original GUI board description.

No NXS address, panel contact, HPI or C6747 assumption should be added to this
path without a separately documented compatibility check.

## NXS-only behavior

The following additions target the experimental CDJ-2000NXS firmware and its
different hardware contracts:

- QEMU machine `cdj2000nxs-main`, 128 MiB SDRAM, NXS IIC/HPI and the NXS panel
  contact map;
- `tools/cdj_main/nxs_vm.py`, `nxs_panel.py`, NXS GUI state/container tools and
  the NXS-specific GUI board description;
- the C6747 peripheral models, UHPI transport, checkpoint/event replay and
  partial C674x interpreter used by the NXS DSP;
- NXS media insertion/listing diagnostics and the protected SD/USB test-image
  helpers;
- NXS-only evidence and regression tests (`test_nxs_*`, C6747 tests and DSP
  replay tests).

The NXS track-load work is a real firmware path: the test SD image reaches the
NXS browser, accepts a physical ENTER and LOAD, and clears the loading state
with the track duration displayed. It does **not** claim audible output; the
strict diagnostic profile does not run a serializer clock or capture audio.

## Review guidance

Start with the launcher and this file, then review the implementation by the
scope above. Firmware is supplied by the user and is never committed. Run the
host tests with:

```sh
.venv/bin/pytest -q
```

Connected emulator runs require local sockets and extracted firmware. They are
evidence for the named profile only and must not be used to infer behavior of
the other profile without a corresponding run.
