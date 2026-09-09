# macOS emulator handoff

Development repository: `git@github.com:geepot/cdj2000-emulator.git`, branch
`codex/macos-nxs`. Parent research repository:
`https://github.com/geepot/cdj2000nxs-research.git`, branch `master`.
Keep the layout `CDJ/references/geepot-cdj2000-emulator` when practical.
The parent prototype remains useful evidence; this fork is the active emulator.

## Current checkpoint

The unmodified connected MAIN/Blackfin run last verified in
`runs/nxs-c674x-pinmux-bank` uploads 13,781 DSP words, executes 597 DSP packets /
699 cycles, and programs all 20 SYSCFG pinmux registers. It stops at
`0x11801f20`, opcode `0x4683e000`: `[B1] SPLOOPW 14`.

The latest checkpoint adds **partial SPLOOPW scheduling**: predicate history,
three-cycle delayed stage-boundary tests, mandatory initial execution, no
count-driven epilog, and unchanged ILC/RILC. Synthetic tests cover both predicate
polarities, II=1..14 and a late predicate update. Termination during loading,
interrupt/reload behavior, and full multistage timing remain incomplete or need
further verification. Do not equate these tests with complete SPLOOPW accuracy.
An earlier loop-test binary was twice killed by macOS with SIGKILL; its sanitizer
build passed, and the subsequent expanded harness and full suite pass. Cause
was not established.

Standalone real-firmware replay with this code (`runs/dsp-sploopw-initial`)
advances to 598 packets / 700 cycles and stops at `0x11801f24`, compact opcode
`0xec6e`. It has one pending store. SYSCFG is relocked by the preceding firmware
write. **This latest change has not yet had a connected QEMU/Blackfin run.**
Next: verify SPLOOPW timing against TI SPRUFE8B section 7.10 and its examples,
implement the next compact instruction, rebuild QEMU and run connected firmware.

## Validation and tools

Latest full fork suite: 163 passed, 42 skipped. CPU and loop standalone harnesses
pass AddressSanitizer/UndefinedBehaviorSanitizer. Tests run with:

```sh
.venv/bin/python -m pytest -q
```

Set `BFIN_AS` and `BFIN_LD` to the local Blackfin binutils binaries to enable their
optional tests. See `BUILD.md` for macOS dependencies, patched GDB 17.2 Blackfin
simulator, QEMU build, and firmware preparation. The QEMU checkout used here was
revision `55347990687e7bc5b6b0d624f290025726e8fbfa` under `build/qemu`.

```sh
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_RUN --seconds 15
.venv/bin/python -m tools.cdj_dsp.replay runs/NEW_RUN/dsp-l2.bin runs/NEW_REPLAY
.venv/bin/python -m tools.cdj_dsp.replay runs/NEW_RUN/dsp-l2.bin runs/NEW_BREAK --break-pc 0x11801f20
```

All output directories must be new. Replay compiles the current sources into a
temporary executable; JSONL captures PCs/cycles, writes, final registers and
pending memory counts. Manifest hashes identify input and source versions.
Replay exit zero means a report was written, not successful firmware boot.

`emulator/qemu/cdj_c674x.c` is the partial instruction/pipeline core;
`cdj_c674x_loop.c` the loop scheduler; `cdj_c6747_syscfg.c` protected register
storage; `cdj2000_nxs_hpi.c` the MAIN-facing UHPI and DSP integration.
`tests/cstub/c674x*.c` and `tests/test_dsp_replay.py` contain focused regressions.

## Local assets and limits

Build trees, binaries, Python environments, firmware outputs, and run artifacts
are not transferred by these source commits. Copy the local supplied updater
and useful ignored run directories privately, or regenerate them on the new
machine. Original `C2KNXS.UPD` SHA-256:
`b17c0f715fb8dd4187f4857409a99c40a3708b383cc0d360a983d291ded81097`.
Useful copied runs: `nxs-c674x-pinmux-bank`, `dsp-sploopw-initial` and
`dsp-replay-before-sploopw`. Manuals are in the parent `references/vendor/`:
`sprufe8b.pdf` (ISA) and `spruh91d.pdf` (C6747 SoC).

MAIN uses the experimental `cdj2000nxs-main` 128 MiB profile. Keep
`BFIN_PARALLEL_WRITEBACK=1`; disabling it corrupts GUI resource relocation.
The direct runner uses local ports 5980, 5982 and 5984 by default. Blackfin has
an unresolved intermittent illegal-instruction/double-fault occurrence documented
in BUILD.md; later bounded runs exited cleanly, which does not close the issue.

DSP entry from L2[0] explicitly substitutes for the missing boot-ROM handoff;
registers initially zero. The DSP currently runs synchronously on first DSPINT
with a finite startup step budget, not continuously alongside MAIN. SYSCFG
pinmux is configuration storage, not physical routing. PSC, interrupts, clocks,
remaining ISA, storage/audio integration, full controls and native polish remain
incomplete. No DSP-ready responses are fabricated. Full boot is not achieved.

Continue toward genuine full-device firmware testing, then polish and measured
optimization. Preserve source work, add focused tests and coherent commits;
keep proprietary/generated firmware out of new commits. The handoff push is
authorized; ask before future pushes unless the new session authorizes them.
