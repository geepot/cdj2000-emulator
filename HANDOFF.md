# macOS emulator handoff

Development repository: `git@github.com:geepot/cdj2000-emulator.git`, branch
`codex/macos-nxs`. Parent research repository:
`https://github.com/geepot/cdj2000nxs-research.git`, branch `master`.
Keep the layout `CDJ/references/geepot-cdj2000-emulator` when practical.
The parent prototype remains useful evidence; this fork is the active emulator.

## Current checkpoint

PSC batch: `cdj_c6747_psc.c` now supplies PSC0/PSC1 MDCTL, MDSTAT, PTCMD and
PTSTAT to both replay and connected DSP execution. Module/domain assignments
and initial states follow SPRUH91D Tables 8-1/8-2. GO snapshots NEXT; status
changes after eight successful DSP steps. This is synthetic step timing, not
measured PSC clock timing. Reads have no side effects. Clock/reset wires are
not connected. DSP local reset starts deasserted as part of the existing ROM
handoff abstraction; self-reset requests stop. FORCE, emulation interrupts,
auto-sleep/wake and power-domain control remain unsupported. Repeated GO while
busy is ignored (model assumption). MDSTAT retains its old state until completion.
PTCMD reads return zero: firmware uses read/OR/write, but this write-only
register's readback is not hardware-verified. Do not claim physical readiness.

Also added the 12 register/immediate ADDAB/H/W and SUBAB/H/W forms with linear
address scaling and wraparound. Circular addressing still stops. Corrected the
loop limit: source fetches are not the 14 LBC-indexed storage slots (TI 7.7.3.3).
NOP/setup fetches may exceed 14; 48-cycle/112-tag/issue limits remain enforced.

Replay `runs/dsp-psc-batch-4` and `runs/dsp-psc-batch-repeat` agree at 833 packets /
958 cycles, stopping at `0x118021a0`, opcode `0x031c3ec0` (next .D arithmetic
family to decode). Trace SHA-256:
`3f16df16bb63ac57916c6dbb6ae76250090736f77c7986d5a8eba0d699de891c`.
Full suite: 165 passed, 43 skipped; PSC and CPU sanitizer harnesses pass.
Rebuilt connected run `runs/nxs-psc-batch-connected` matches 833/958 and the
same opcode/PC. It records MDCTL+GO writes for PSC0 modules 0/1/2 and PSC1
modules 3/4. GUI exits 0 after 15 seconds with a frame. Full boot is incomplete.

### Previous SPMASK checkpoint

Latest batch: full-width and compact SPMASK, functional-unit classification,
load-time exclusion, buffered suppression before issue-capacity checks, and
epilog replacement are implemented. Compact Figure G-3 predicated MVK covers
both banks, both register subsets, all four predicates, constants 0/1 and L/S/D.
SPMASKR, interrupt/reload behavior, masked multicycle/control operations,
unknown unit formats and general functional-unit conflict checking remain open.

`runs/nxs-mask-batch-connected` verifies 608 packets / 710 cycles, stopping on
`0x42140264` at `0x11801f34`: LDW from A5=`0x01c10128`, PSC0 PTSTAT.
The address is confirmed by SPRUH91D Table 8-6 and section 8.6.10. The register
is unmapped; no fabricated zero/ready value was supplied. The 15-second GUI
run exits 0 and publishes a frame. Replays `runs/dsp-mask-batch-1` and `-2`
are byte-identical (trace SHA-256
`9a59481768c021d9438a616a0866ae02763b056923bb8b19838f481aae2b0722`) and match
the connected stop. Full boot remains incomplete. Suite: 164 passed, 43 skipped;
CPU harness passes address/undefined-behavior sanitizers.

Next batch: PSC0/PSC1 register state and transition behavior using SPRUH91D
chapter 8, especially Tables 8-1/8-2 (module/reset/domain assignments), section
8.3.2 (MDCTL NEXT -> PTCMD GO -> PTSTAT busy -> MDSTAT), and register reset
values. Do not clear PTSTAT without modeling the requested transition. Physical
clock/reset effects and transition latency need explicit fidelity boundaries.

### Previous compact NOP checkpoint

The latest connected MAIN/Blackfin run, `runs/nxs-compact-nop-connected`,
uploads 13,781 DSP words and executes 606 DSP packets / 708 cycles. It stops at
`0x11801f26`, compact opcode `0x2d66`: `SPMASK S1` (TI Figure H-8).
The preceding `0xec6e` is now implemented as compact `NOP 8`, including its
eight dynamic-length loop cycles. The GUI exits 0 after 15 seconds and produces
a frame. This does not establish full boot.

The latest checkpoint adds **partial SPLOOPW scheduling**: predicate history,
three-cycle delayed stage-boundary tests, mandatory initial execution, no
count-driven epilog, and unchanged ILC/RILC. Synthetic tests cover both predicate
polarities, II=1..14 and a late predicate update. Termination during loading,
interrupt/reload behavior, and full multistage timing remain incomplete or need
further verification. Do not equate these tests with complete SPLOOPW accuracy.
An earlier loop-test binary was twice killed by macOS with SIGKILL; its sanitizer
build passed, and the subsequent expanded harness and full suite pass. Cause
was not established.

Two deterministic replays, `runs/dsp-compact-nop-replay-1` and `-2`, agree
byte-for-byte with each other and agree with the connected stop. Neither has
pending memory operations; SYSCFG is relocked by firmware. Trace SHA-256:
`3b913fb57072982daae15a4b2210bbbedadcb5f4741b484b0c7ca9e68d35fc88`.
The new connected L2 dump matches the prior capture, SHA-256:
`d9a798cd6ba9e4d09cabca6c4ca124f55e9194be58c8ef7440f5799cf5217ecc`.

Compact NOP tests cover counts 1..8 and a synthetic SPLOOPW loop with NOP 8
and a late predicate update. TI section 7.10 remains the timing reference;
these tests do not prove all multistage/interrupt cases. GNU GDB 17.2's
`include/opcode/tic6x-opcode-table.h` explicitly corrects TI Figure H-9's
operand label: the N3 field encodes count minus one, citing TI dis6x.

## Batch development workflow

Use `tools.cdj_dsp.inventory` before extending the next cluster. It scans
explicit ranges using the locally built GNU format table, produces per-address
candidate families and group counts, and optionally annotates replay PC visits.
It never executes or skips unsupported instructions. It is a discovery report,
not mnemonic disassembly, code/data separation, reachability, or proof of ISA
support. The broad uploaded range has 13,978 candidates across 70 families;
the current loop range has 23 candidates across 13 families. Broad counts
include data and cannot be interpreted as instruction coverage percentages.

The first inventory-driven batch implemented full/compact SPMASK with
functional-unit classification and loop load/replay suppression, plus nearby
predicated MVK. The current blocker is the PSC peripheral described above.
The unfinished MVK-only SPMASK attempt was removed: it rejected unmasked
instructions and did not implement buffered suppression. Do not resurrect that
special case. Implement the family, validate synthetic schedules and replay,
then rebuild/run connected firmware after substantial advancement.
See BUILD.md for inventory commands. Full control-flow-aware disassembly and
an exact supported-opcode coverage report remain to be built.

## Validation and tools

Latest full fork suite: 164 passed, 43 skipped. The CPU standalone harness
passes AddressSanitizer/UndefinedBehaviorSanitizer. The extra skip relative to
the old machine is the optional Blackfin assembler/linker regression. Tests run with:

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
