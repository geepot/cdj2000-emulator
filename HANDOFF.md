# macOS emulator handoff

Development repository: `git@github.com:geepot/cdj2000-emulator.git`, branch
`codex/macos-nxs`. Parent research repository:
`https://github.com/geepot/cdj2000nxs-research.git`, branch `master`.
Keep the layout `CDJ/references/geepot-cdj2000-emulator` when practical.
The parent prototype remains useful evidence; this fork is the active emulator.

## Current checkpoint

Compact protected loads now receive PROT timing before format-specific
lowering.  The stage-two return sequence at `0xc001dcda` uses compact Dpp
`LDW *++B15(8),B3` (`0x71f7`) under a PROT header; the old early return skipped
the four added NOP cycles, so the following `BNOP B3,5` sampled stale B3,
repeated the pop, and corrupted the stack.  Dpp and Dstk exact-encoding tests
now require the E5 load result to be published after five total cycles.
`python -m pytest -q tests/test_c674x.py` passes 12 tests and the standalone
C674x address/undefined sanitizer harness passes.

Rebuilt connected run `runs/nxs-prot-connected-2` advances beyond the old
25,099,500-packet point and fails closed at a later software-loop resource
conflict: 25,364,865 packets / 60,779,972 cycles, PC `0xc004f306`, compact
`0x2627`, B15=`0x11805ae8`, B3=`0xc004cc48`, B5=`0x118001f4`.  The bounded GUI
run exits zero and publishes a frame; neither fact establishes boot.  Exact
replay from checkpoint 64 reproduces the same state, counts, and fault:

```sh
python -m tools.cdj_dsp.replay \
  runs/nxs-prot-connected-2/dsp-checkpoints/00000000000000000064.cdjdsp \
  /tmp/dsp-prot-connected-replay-1 --steps 1000000 \
  --events runs/nxs-prot-connected-2/dsp-events.jsonl --verify-repeat
```

GNU libopcodes and the firmware bytes decode the new boundary as an II=1,
seven-cycle `SPLOOPD` copy loop with compact `SPKERNEL 3,0`: protected LDW,
four empty cycles, `MV .L2X A3,B4`, and `STW .D2T2 B4,*B5++`.  The current
scheduler enables the post-loop `MVK .L2 1,B4` while the buffered L2 move is
still live, correctly rejecting the combined packet under SPRUFE8B 7.15.
SPRUFE8B 7.6 and 7.9 confirm the general drain and fetch-delay formulas, but
do not yet resolve why this genuine sequence selects a three-cycle delay.
Do not add cycles or relax the resource-conflict check without independent
architectural evidence.  Tail replay coverage contains 773 confirmed source
packets, 1,098 confirmed instruction addresses, 988 encodings, 811 dynamic
edges, three probable addresses, and no unsupported-opcode fault.  Full boot,
interrupts, peripherals, and audio remain incomplete.

The external HPI transcript replayer now uses the common checkpoint reader
instead of assuming schema-1 offsets.  It validates schema-2 payload size and
checksum and obtains L2 after the recorded state size while continuing to
accept compatible schema-1 inputs.  The exact-repeat command below validates
all 110,209 connected events, 104,093 uploaded words, 14 HINT acknowledgements,
13 DSP HINT edges, and one DSPINT edge; replay and repeat SHA-256 are both
`f08c16567269ea54c0eba1b9b7a2dd6ed0668cc8b59ea28796d66bdc98667c98`.

```sh
python -m tools.cdj_dsp.event_replay \
  runs/nxs-sploopd-shared-connected-3 /tmp/dsp-event-replay-schema2 \
  --verify-repeat
```

### Previous stable-wait checkpoint

The confirmed connected/replay DSP path no longer stops on an unsupported
instruction. It reaches PC `0x11804904`, word `0x0001a120`, an unconditional
self-branch. This is a stable non-ISA boundary after DSP initialization, not
proof of full firmware boot or audio. Its purpose is not symbolized; it is
consistent with a terminal/interrupt wait, and DSP interrupt delivery remains
unimplemented.

The batch which reached that boundary implements the reachable families as
families rather than one encountered word at a time:

- compound `.M` `MPYIH`/`MPYIL` and rounded forms, `ABSSP`, scalar
  `ADDSP`/`SUBSP`, and compact/full scalar comparisons;
- reverse-cross `.L` subtraction plus the signed/unsigned 40-bit
  `ADD`/`ADDU`/`SUB`/`SUBU` register-pair family;
- `.L/.S/.D` `ANDN`, compact LSDx1 zero/one/negate/decrement/increment/XOR-one,
  compact non-saturating `.S/.D` add/subtract, and compact immediate/register
  shifts; and
- compact/full `SPLOOPD`, including all compact II values 1..16 and the TI
  four-cycle delayed initial test/decrement rule. Full `SPLOOPD` may load ILC
  in parallel with setup. Conditional reload (Figure H-6), interrupt return,
  and saturating compact operations with delayed CSR.SAT remain fail-closed.

The earlier `0x020c9572` was not scalar `ADDU`; complete format/field decoding
showed a compound MPYLI/MPYIL packet. The stale historical description remains
below only as superseded checkpoint history. Table-driven tests cover unit,
side, cross path, register-pair validity, predicates, parallel conflicts,
rounding/status behavior, shift bounds, software-loop intervals, and fault
atomicity. AddressSanitizer/UndefinedBehaviorSanitizer passes.

SPRUFE8B sections 7.5.1.2 and 7.9 establish that `SPLOOPD` begins observing ILC
after setup while forcing loop termination false and suppressing ILC decrement
for the first three loop cycles. The implementation schedules
`loaded ILC + ceil(4/II)` iterations and begins decrementing ILC only at later
II boundaries. Focused tests exercise ILC=0 and parallel ILC loads for every
supported II. `runs/dsp-shared-wait-boundary-1 --verify-repeat` starts at the
2,854,320-packet compact-shift checkpoint and first reaches the stable branch
at 2,864,814 packets / 6,486,387 cycles. Trace SHA-256 is
`62e8d267bac31019d1d03412fb0c092a4321b8938662d70099fb129438f12477`.

The first post-`SPLOOPD` integration boundary was a genuine `STDW` to
`0x80002000`, inside the C6747 128 KiB shared RAM documented by SPRS377F Table
3-4. Connected and standalone buses now map `0x80000000..0x8001ffff`; UHPI can
also address this region. Checkpoint schema 2 losslessly records shared RAM
between L2 and sparse EMIFB SDRAM. Schema-1 checkpoints remain readable and
restore the previously uncaptured region as zero; manifests label this legacy
assumption. The checkpoint remains native-ABI-bound and checks magic, schema,
endianness, component sizes, payload size/checksum, sparse page counts,
truncation, and trailing data.

`runs/dsp-shared-ram-1 --verify-repeat` executes a full one-million-step budget
without a fault and ends at 3,854,320 packets / 12,423,423 cycles, spinning at
the same branch. Its schema-2 repeat files match byte-for-byte across CPU,
peripherals, L2, shared RAM, and logical 32 MiB SDRAM. The shorter breakpoint
run above records the first arrival instead of inflating progress with wait-loop
iterations.

The rebuilt connected MAIN/Blackfin/C674x run is
`runs/nxs-sploopd-shared-connected-3`. It exits the bounded GUI run with status
zero and publishes a frame. Its one-million-packet cooperative DSP quantum is
a host scheduling choice, not C6747 timing evidence; HINT still yields
immediately. The run records 67 schema-2 checkpoints and 110,209 ordered events,
and reaches the same `0x11804904` branch with no DSP fault. Starting from its
DSP-start checkpoint, `runs/dsp-sploopd-shared-connected-events-1
--verify-repeat` gates all 40 connected DSP stops, exact packet/cycle counts,
HPI/phase state, and byte-identical repeat state/memory. Trace SHA-256 is
`92ea9014bcec73588b3477f43927b2b51401225b2a53de912d2db10585314639`.

The control-flow-aware replay gate is now
`runs/dsp-sploopd-shared-coverage-3`. Replay records exact instructions in each
successfully completed source packet at fetch time, so later HPI writes cannot
silently change the inventory. Software-loop source fetches, scheduler-only
cycles and direct idle cycles are separate. Source transitions, direct
branch/call targets, loop-source packets, predicates, cross paths, unit/side,
compact/full encodings, delay slots and affected architectural-state classes
are emitted in `coverage.json`. Addresses reached only by an uncompleted direct
target remain probable code; all other memory remains unclassified rather than
being guessed as code.

The exact-repeat gate verifies all 40 connected stops and byte-identical trace,
coverage, state, L2, shared RAM and SDRAM. It reports 1,525 confirmed source
packets, 1,957 confirmed instruction addresses, 1,721 distinct observed
encodings, 1,581 dynamic source transitions, 114 direct branch/call targets,
56 software-loop source packets, five probable uncompleted target addresses,
and zero unsupported faults. Trace SHA-256 is
`c74366da1e1b0d13f78ea5be70f6ee284c8fcebfd460df0a4f70a7e6a49d8562`;
coverage SHA-256 is
`4d3faa11813fd445747af2803d87863bfb9aa55a4a182865b2e6741873f0684f`.
The 27,099,500-packet / 71,188,299-cycle delta includes 1,734,733 observed
transitions around the final self-branch and must not be read as useful boot
progress. A completed packet proves only that the current core accepted the
observed encoding, not architectural correctness or that its predicate body
executed.

Replay accepts a run directory as input and selects its newest structurally
valid checkpoint with connected-manifest or exact-repeat-gate provenance. One
command now always emits coverage, provenance hashes and packet/cycle deltas;
`--verify-repeat` also gates the coverage artifact. The complete suite is 184
passed / 43 skipped; core, checkpoint and replay coverage ASan/UBSan harnesses
pass. Full boot remains incomplete: the DSP interrupt controller and interrupt
delivery, DMA/EDMA activity, physical peripheral timing, storage/control
interaction, and audio generation are not established. The next subsystem
boundary is interrupt-controller/delivery investigation at the stable branch,
while the five probable packets remain deliberately unclaimed. Do not infer
readiness from the large packet count accumulated in wait/service loops.

### Previous SYSCFG checkpoint

SYSCFG CFGCHIP0-4 are implemented as a family from SPRUH91D 10.5.14-18:
documented reset values, reserved-value checks, legal CAP/AMUTE/USB reference
selectors, read-only USB status masking, CFGCHIP4 read-zero clear pulses and
kicker-protected commits. CFGCHIP0 PLL_MASTER_LOCK makes modeled PLL writes
complete without effects; clearing it requires the existing KICK unlock flow.
PLLDIV1-7 retain documented post-release GO programmability. Physical eCAP,
HPI pin selection, TBCLK, USB PHY, EMIF clock mux and AMUTE latch effects are
not yet connected. `amute_clear_pulses` is diagnostic bookkeeping only;
privilege faults remain unmodeled.

Genuine firmware unlocks KICK0/1, sets CFGCHIP1 HPIENA+HPIBYTEAD to `0x18000`,
then relocks both keys. `runs/dsp-cfgchip-1 --verify-repeat` reaches 1,190
packets / 1,469 cycles, PC `0x11802fd0`, full LDW `0x020c0264`, reading the
DSP-side HPIC at `0x01e10030`. Trace SHA-256:
`e4143aed27d7a25d38001782924b05dab01cec7681cb6f16b118cb93f96a0469`.
Final CFGCHIP state is `[0,0x18000,0xef00,0xff00]`; KICK is relocked.
Rebuilt connected `runs/nxs-cfgchip-connected` agrees exactly; GUI exits 0
with a frame at the 15-second bound. Suite: 171 passed / 43 skipped; combined
SYSCFG/PLL/CPU sanitizer passes. Full boot/audio remain incomplete.

Next implement DSP-side HPI registers and connect HPIC.HINT/DSPINT semantics to
the existing SH4-side UHPI model. Parent evidence identifies `0x01e10030` bit 2
as per-chunk HINT flow control. Standalone replay needs an explicit deterministic
host-event script or fail-closed stop; it must not fabricate incoming chunks or
handshake edges. CFGCHIP1 now proves HPI is enabled in byte-address mode.

### Previous PLL-enable checkpoint

PLLEN now latches when source, power, reset and operating-point fields are
valid. TI describes the lock delay as an application wait before PLLEN, not a
write rejection, and PLLSTAT exposes no lock bit. Firmware sets PLLEN only four
modeled OSCIN periods after reset release. The model therefore records sticky
`early_enable` and keeps the conservative lock countdown running without
claiming analog lock. In PLL mode, OSCIN time advances rationally as
`N*POSTDIV*SYSCLK1/M` per DSP cycle; the remainder is retained exactly and
clock-mux changes reset that phase. PLLDIV1-7 latches remain writable after
release for the documented GO flow; PLLM/PREDIV/POSTDIV remain guarded.

This is an explicit fidelity boundary: DSP instructions continue during the
catalog lock window, matching observed genuine firmware, but unstable analog
clock behavior and downstream physical clock consumers are not simulated.
`early_enable` is evidence of that approximation, not success or lock. The
custom D810K013 may differ from catalog C6747 timing; no such difference is
claimed without measurement.

`runs/dsp-pll-enable-1 --verify-repeat`: 1,179 packets / 1,450 cycles,
PC `0x11802fac`, compact `0x117d` (`LDW *B6,B7`) from SYSCFG1 CFGCHIP1 at
`0x01c14180`. Trace SHA-256:
`51c7b92af18570445973ea76e14d4b5883547db6ff91795eb88680ab9a2d1047`.
Final diagnostics: OSCIN=1430, reset-age=17, lock-wait=411, early-enable=true.
Rebuilt connected `runs/nxs-pll-enable-connected` agrees exactly; GUI exits 0
with a frame at the 15-second bound. Suite: 171 passed / 43 skipped; PLL
address/undefined sanitizer passes. Full boot/audio remain incomplete.

Next implement the coherent SYSCFG CFGCHIP0-4 family from SPRUH91D 10.5.14-18,
including reset values, masks, kicker protection and PLL_MASTER_LOCK coupling.
The current code has already written both documented KICK keys. Inventory
`runs/pll-enable-next-inventory.json` covers discovery candidates around the
new point (206 candidates / 21 format families); candidates are not proof of
executable code or missing implementation.

### Previous guarded reset-release checkpoint

PLL reset release is modeled in oscillator periods while still in bypass:
NXS OSCIN=16.9344 MHz, one CPU cycle spans the active SYSCLK1 divider ratio.
Time uses the old divider through a GO completion edge. Reset release requires
software-selected bypass, square-wave input, power on, at least 17 qualified
OSCIN periods of reset assertion, and catalog-valid PLL reference/output
frequencies. Release starts ceil(2000*N/sqrt(M)) periods of conservative wait;
the integer bound avoids floating-point rounding. Reset/power-down cancels the
wait; configuration changes while released are rejected. No PLLSTAT lock bit
was invented and PLLEN remains unsupported even after the bound expires.
Catalog timing applied to the custom D810K013 is an explicit assumption.

`runs/dsp-pll-release-1 --verify-repeat`: 1,166 packets / 1,427 cycles,
PC `0x11802ed8`, compact `0x0134`, rejected PLLCTL=`0x1c9` (PLLEN).
Trace SHA-256: `eb9d086289e93c47dba8ac9dcbf4f4746b7d54382b1dd5c825863bd183040fdb`.
Connected `runs/nxs-pll-release-connected` agrees, including OSCIN=1427,
reset-age=17 (saturated minimum counter), lock-wait-remaining=414. GUI exits 0
with a frame at the 15-second bound. Suite: 171 passed / 43 skipped; PLL
address/undefined sanitizer passes. Full boot/audio remain incomplete.

**Next discrepancy to resolve:** firmware attempts PLL enable only four modeled
OSCIN periods after reset release (E1 validation; an accepted store would commit
two cycles later), versus a catalog maximum lock wait of 418. Do not bypass this
guard or claim firmware is wrong. Recheck custom-chip PLLRST meaning, actual
clock/stall timing and manual applicability. SPRUH91D Table 7-5 even qualifies
PLLRST as "if supported". The bit-4 discrepancy remains separate evidence that
catalog PLL register behavior may differ from this custom DSP. Observed bytes
0x11802ecc..2ed8 are STW; LDW; NOP 4; OR-immediate 1; STW, with no explicit
long wait. Parent compact decoder corroborates the memory forms but incorrectly
labels nearby compact NOP 0x0c6e; it is not an independent timing oracle.
This discrepancy is superseded by the register-semantic resolution above;
physical clock consumers remain future work.

### Previous cycle-edge clock checkpoint

The CPU now has an optional per-cycle board-clock callback, invoked before E3
bus effects on every cycle (including inserted NOPs, loop cycles and truncated
branch delays). Both DSP hosts bind it to PLL ticking. GO now takes eight
subsequent DSP cycles rather than eight successful step calls; it remains a
synthetic phase-alignment delay. PSC deliberately retains its separately
documented step-based approximation. Callbacks cannot fail/mutate CPU state;
like bus commits, their external effects cannot roll back a broken bus callback.
CPU reset clears the binding; future serialized checkpoints must rebind it.

`runs/dsp-cycle-clock-1` passes repeat and exact prior-trace equivalence against
`runs/dsp-protected-loop-1`, retaining the 1,162/1,420 PLLRST-release stop below.
Suite: 171 passed / 43 skipped; CPU and combined PLL/CPU sanitizer harnesses
pass. The new integration harness tests E3 GO start, eight NOP 1 calls versus
one NOP 8, and a PROT load whose E3 observes GO completion within the same call.
Rebuilt `runs/nxs-cycle-clock-connected` agrees at the same stop/counts; its
15-second GUI run exits 0 with a frame, not a completed boot.

Clock research for the next batch: parent `docs/dsp/dsp-hardware.md` traces
X501/IC506/IC16 to OSCIN = 16.9344 MHz (RRV4356 pp12,13,96), with firmware x23
giving 389.4912 MHz. Visually checked SPRS377F Table 6-4 p73: reset assertion
minimum 1000 ns; maximum lock wait **2000*N/sqrt(M)** OSCIN cycles (text
extraction loses the radical). At N=1,M=23 this rounds up to 418 OSCIN cycles;
1000 ns rounds up to 17 input-clock periods. These are datasheet bounds, not
measured lock timing on the custom DSP. Next implement oscillator-domain
elapsed time and reset/lock transition validation, including divider ratios,
bypass/source selection and physical clock consumers. PLLRST release/PLLEN
still fail closed; do not equate a configuration latch or elapsed bound with a
firmware-visible lock indication. The missing-ROM initial clock state remains
an explicit assumption.

### Previous protected-load checkpoint

Protected loop loads now expand into four empty program-stream cycles while
buffered operations continue issuing (SPRUFE8B 3.10, 7.7.3.3). PROT is removed
from the buffered/direct lowered instruction, so replay does not reinsert fetch
delays. Tests compare every cycle against explicit LD; NOP 4 for II=1..7,
full/compact loads, compact RS banks, masked one-shot execution, false full-width
predicates, and E3 sampling/E5 writeback with changing RAM. Invalid parallel
SPKERNEL/protected-load and multiple-multicycle packets fault atomically.
Interrupt restart and the assembler's preceding-packet SPKERNEL restriction
are not fully modeled; these tests are not a hardware timing oracle.

`runs/dsp-protected-loop-1 --verify-repeat` matches at 1,162 packets / 1,420
cycles, PC `0x11802ecc`, compact word `0x0134`. The rejected store is
PLLCTL (`0x01c11100`) = `0x1c8`: PLLRST release, still deliberately unsupported.
Trace SHA-256: `65dba25926a35dc30506d2cb4bbf955a47dafb5e23c53bdfbecab93c2df5a1c0`.
Rebuilt connected run `runs/nxs-protected-loop-connected` agrees at the same
stop/counts, GUI exit 0 and a frame after the 15-second bound. Suite: 170 passed /
43 skipped; CPU address/undefined sanitizer passes. No new peripheral response
was added. PLL/PSC timing assumptions below still apply. Full boot/audio remain
incomplete. Next: model PLL reset-release/clock timing from SPRUH91D 7.2.2 and
the C6747 datasheet timing requirements; do not simply latch reset release and
claim clock lock. Firmware has exited its GO-status polling loop.

### Previous immediate BNOP checkpoint

Immediate BNOP is implemented for both units, all NOP counts 0..7, predicates
and signed 12-bit displacements. Full-width instructions inside header-based
fetch packets scale displacement by two; ordinary packets scale by four
(SPRUFE8B pp165-167). Taken branches truncate NOP counts above five at transfer;
false predicates retain the full NOP delay. CALLP/loop control guards include
this form, and loop-body execution remains explicitly unsupported.

`runs/dsp-bnop-immediate-1 --verify-repeat` matches at 1,147 packets / 1,402
cycles, PC `0x11802ea8`, word `0x21940264`: protected LDW in a software loop.
Trace SHA-256: `27e10b6bd271c1faa90f8b2699979602ad6bc64ab47944353ce2ca7400c4a379`.
The load targets PLLSTAT through A5. Next implement protected-load scheduling
and verify interaction with SPLOOPW/SPMASK, including false predicates and
the four inserted cycles; do not simply remove the guard. Nearby discovery
inventory `runs/pll-branch-inventory.json` has 104 candidates in 22 families.
Suite: 170 passed / 43 skipped; CPU sanitizer passes. PLL approximations below
still apply. Full boot and audio remain incomplete.
Rebuilt connected run `runs/nxs-bnop-immediate-connected` matches the same
stop and counts. GUI exits 0 with a frame at the 15-second bound.

### Previous PLL GO checkpoint

PLL legacy-bit and divider-GO batch supersedes the bit-4 stop below. Bit 4
now retains writes as an explicitly unverified C6747 compatibility assumption:
Linux v6.1 `drivers/clk/davinci/pll.c` names it PLLDIS and clears it during
reset/enable. This corroborates a legacy software pattern, not the physical
meaning on this chip, and does not override the conflict with TI Table 7-5.
Replay final state records `pll_legacy_bit4_used`; connected PLL writes log it.

GO snapshots seven programmed divider values, reports GOSTAT busy, then commits
the snapshot after eight successful DSP steps. This latency is synthetic, not
PLL/OSCIN cycles. Writes during GO and repeated GO while busy stop. Writing
PLLCMD=0 clears its command latch without cancelling a pending transition.
PLLSTAT.STABLE assumes oscillator-counter completion before ROM handoff; it
is not PLL lock. PLLEN activation/PLLRST release still stop; physical clocks,
alignment details and clock ratios affecting CPU/peripherals are not modeled.
These assumptions are included in replay manifests.

`runs/dsp-pll-go-final` passes repeat and baseline equivalence against
`runs/dsp-pll-go-1`: 1,142 packets / 1,392 cycles, unsupported `0x3021a121`
at `0x11802e94`. Trace SHA-256:
`c10c5972c7502b54a66e135a2c45003157c0d535f2662e24775d1f0e394fe1bd`.
Suite: 170 passed / 43 skipped; PLL sanitizer passes. Next decode the nearby
instruction family and continue PLL reset/lock timing. Full boot/audio remain
incomplete; increased counts are conditional on the documented assumptions.
Rebuilt connected run `runs/nxs-pll-go-connected` matches the stop and logs
PLLDIV3/5/7 and GO writes. GUI exits 0 with a frame at the 15-second bound.

### Previous strict PLL configuration checkpoint

PLL reset-held configuration batch adds PLLCTL, OCSEL, PLLM, PREDIV,
POSTDIV, OSCDIV and PLLDIV1-7 to both DSP execution paths. Power-on defaults
are an explicit substitute for unknown boot-ROM handoff state. No clock
outputs, lock indication, GO completion, PLLRST release or PLLEN activation
are supplied. Config values are latches, not active clock ratios.

`runs/dsp-pll-config-1 --verify-repeat` passes exact repeat equivalence:
1,117 packets / 1,351 cycles, PC `0x11802e14`, compact store `0x1144`.
Firmware attempts PLLCTL=`0x1c0`, clearing reserved bit 4 after writing `0x1d0`.
SPRUH91D Table 7-5 says bit 4 must retain default one, so this request stops.
This is an unresolved manual/firmware conflict, not evidence firmware is wrong.
Next investigate older TI C6747 PLL/system guides and vendor initialization
code, and revalidate the exact instruction sequence before relaxing this guard.
Clock transitions and initial handoff state remain independent open issues.
Trace SHA-256: `12e7edca378df4a2687f3a5f42c686556cee4c4c3c6708194ec3faf6075fcc24`.
PLL sanitizer harness passes. Full boot/audio remain incomplete.
Full suite: 170 passed / 43 skipped. Rebuilt connected run
`runs/nxs-pll-config-connected` matches 1,117/1,351 and the same stop. Its
15-second GUI run exits 0 with a frame, not a completed boot.

### Prior replay tooling and I2C checkpoint

Replay gates are now built in: `--verify-repeat` compiles once and compares two
fresh-process traces; `--expect-trace PATH` additionally requires exact equality
with an explicit baseline. `gate.json` records hashes and outcomes. A mismatch
exits 1 without replacing the baseline. A passing gate establishes equivalence,
not correctness or boot. Compilation uses snapshotted source/header bytes so
the manifest hashes describe the compiled inputs even if the worktree changes.
`runs/dsp-pll-baseline-gate` passes both checks against the previous connected
checkpoint's replay trace. Firmware/core behavior is unchanged by this tool batch.
Full suite after this tooling change: 169 passed / 43 skipped.

PLL preparation: SPRUH91D Figure 7-4 gives PLLCTL POR value `0xf2`, including
reserved-one bits 7:6 and 4; do not model its reset as zero. Section 7.2.2
requires four OSCIN cycles for bypass selection, divider GO transitions, and
device-specific PLL lock delay. PLLSTAT.STABLE is oscillator-counter completion,
not a PLL lock flag (7.4.17). PLL_MASTER_LOCK defaults unlocked (7.3); its
future SYSCFG integration must preserve lock protection. Power-on state is
not necessarily the unavailable boot-ROM handoff state. These findings inform
the next implementation; no PLL registers have been mapped by this batch.

I2C GPIO-mode and long-offset memory batch: ICMDR and pin configuration/output
latches are shared between replay and connected execution for both I2C ports.
Firmware selects GPIO pins, then enables an idle slave controller with STT=0.
TI 22.3.16 specifies internal SCL/SDA=1 in GPIO mode; this does not establish
external pin values. Reset release is supported only in that idle GPIO mode.
Transfers, status, physical input, ACK/NACK and interrupt delivery remain
unsupported. PDSET/PDCLR reads are indeterminate per TI and explicitly stop.

All eight full-width long-offset scalar load/store forms now use unsigned
scaled 15-bit displacements from B14/B15 with no base update. They retain the
E1/E3/E5 memory pipeline. Tests cover both data banks, both bases, boundary
offsets, signed loads and neighboring-byte preservation. Static inventory
`runs/i2c-next-inventory.json` finds 96 candidates across seven formats in
`0x11803080:0x11803200`, dominated by long-offset memory; this is discovery,
not validated executable-code coverage.

Replays `runs/dsp-i2c-long-1` and `runs/dsp-i2c-long-repeat` match at 1,088
packets / 1,289 cycles, PC `0x11802dc8`, compact word `0x114d`, loading
PLLCTL at B6=`0x01c11100` (C6747 datasheet PLL register map).
Trace SHA-256: `5b7056642605e60cb33cf2761e01ab1a8a3df304ffc481c20a1a4dc135fd7fc8`.
Suite: 168 passed / 43 skipped; CPU and I2C sanitizer harnesses pass.
Rebuilt connected run `runs/nxs-i2c-long-connected` matches this stop and
records ICMDR=0, ICPFUNC=1, ICPDIR=0, ICMDR=0x20. GUI exits 0 with a frame
at the 15-second bound; that is not proof of full boot.
Next: PLL reset/clock configuration and transition semantics. Never invent
clock lock/readiness; the existing PSC step timing remains an approximation.
Full boot and audio are incomplete.

### Previous GPIO and compact bit-field checkpoint

GPIO configuration and compact bit-field batch: all eight C6747 GPIO banks
have DIR, OUT_DATA, SET/CLR_DATA, rising/falling trigger configuration and
BINTEN storage shared by replay and connected execution. DIR resets to all
ones (inputs), unlike McASP. Input reads and interrupt status/delivery remain
unsupported; no external pin or event state is fabricated. Output latches
retain writes while configured as inputs: an explicit, unmeasured interpretation
of the manual's distinction between output-register state and pin drive.

Compact Sc5 CLR/SET/EXTU and S2ext signed/unsigned byte/halfword extracts
follow TI Figures F-27/F-28. Sc5 EXTU always writes low A0/B0, regardless of
RS. Tests cover all Sc5 constants, banks/subsets and all S2ext variants.

Replays `runs/dsp-gpio-fields-1` and `runs/dsp-gpio-fields-repeat` match:
1,064 packets / 1,238 cycles, PC `0x11802d90`, compact word `0x105d`.
The unmapped load is I2C0 ICMDR at B4=`0x01c22024` (SPRS377F Table 6-88;
SPRUH91D 22.3.9). Trace SHA-256:
`0227158054c66c770fdd905ca5d5ca54f3960f5ad876395efa1938fe8dac99bf`.
Suite: 167 passed / 43 skipped; CPU and GPIO address/undefined sanitizers pass.
Rebuilt connected run `runs/nxs-gpio-fields-connected` matches the stop and
records DIR45=`0xffffbfff`. The 15-second GUI run exits 0 with a frame.
Next: I2C controller state and attached-device inventory, preserving failures
for unmodeled transfers rather than manufacturing ACK or success.
Full boot and audio remain incomplete.

### Previous McASP and full-width bit-field checkpoint

McASP pin-register and full-width bit-field batch: PFUNC, PDIR, PDOUT and
write aliases PDSET/PDCLR now share a model between replay and connected DSP
execution. C6747 has 16/12/4 serializers (SPRS377F Table 6-43). Input pin
readback, clock/reset release, serializers, FIFO/DMA and audio remain unsupported;
no pin values or audio-ready status are invented. Nonzero reserved writes stop.

Full-width CLR/SET/EXT/EXTU support constant and register forms, both banks and
register cross paths. Tests cover all 1,024 field-parameter pairs per form.
Register counts above ten bits stop (conservative for CLR; explicitly invalid
for SET/EXT/EXTU in TI's text). Compact bit-field forms remain incomplete.

Replays `runs/dsp-mcasp-fields-1` and `runs/dsp-mcasp-fields-repeat` are identical:
1,051 packets / 1,214 cycles, stopping at PC `0x11802d5c`, word `0x02140264`,
LDW from A5=`0x01e26060`: GPIO DIR45 (SPRUH91D Table 20-1).
Trace SHA-256: `33cce07cf17dfaca1965aa1311c7829c9e947f891e4b9b173dfd6312d411dda7`.
Rebuilt connected run `runs/nxs-mcasp-fields-connected` agrees at the same stop
and records configuration writes to all three McASP ports.
The 15-second bounded GUI run exits 0 and publishes a frame, not a full boot.
Suite: 166 passed / 43 skipped; CPU and McASP address/undefined sanitizers pass.
Next: GPIO direction/output/edge-control register family, with physical inputs
and interrupt routing explicitly separated from configuration storage.
Full boot and audio remain incomplete.

### Previous logic/address checkpoint

Latest instruction/header batch: ADDAD register/immediate is implemented
(there is no SUBAD), and full-width OR/XOR now cover L/S/D register/immediate
forms with cross paths. PROT/BR loop guards now inspect instruction kinds,
rather than rejecting every operation in a fetch packet carrying those bits.
PROT still protects loads; protected loads and actual control transfers inside
the loop body retain explicit unsupported stops.

Replays `runs/dsp-logic-batch-1` and `runs/dsp-logic-batch-repeat` are identical:
1,003 packets / 1,156 cycles, stopping at PC `0x11802ca8`, opcode `0x020c0264`,
LDW from `0x01d00014`. This is McASP0 PDIR: see the C6747 datasheet memory
map and SPRUH91D section 24.1.4. Trace SHA-256:
`dd45d5fd1c6f2f86898f5de6a88530df1817bc30c5abf61759c873a56074d5db`.
Full suite: 165 passed, 43 skipped; CPU address/undefined sanitizer passes.
Rebuilt connected run `runs/nxs-logic-batch-connected` agrees at 1,003/1,156
and the same stop. GUI exits 0 with a frame at the 15-second bound.
Next batch: McASP control initialization with explicit clock/reset/audio limits.
Full boot and audio are incomplete.

### Previous PSC checkpoint

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
predicated MVK. Later batches advanced through PSC and McASP pin configuration;
the current blocker is loop protected-load scheduling, as recorded at the top.
The unfinished MVK-only SPMASK attempt was removed: it rejected unmasked
instructions and did not implement buffered suppression. Do not resurrect that
special case. Implement the family, validate synthetic schedules and replay,
then rebuild/run connected firmware after substantial advancement.
See BUILD.md for inventory commands. Full control-flow-aware disassembly and
an exact supported-opcode coverage report remain to be built.

## Validation and tools

Latest full fork suite: 170 passed, 43 skipped. The CPU standalone harness
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
With equivalence gates enabled, zero additionally means those comparisons
passed; a deterministic unsupported-instruction/peripheral stop may still pass.

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
