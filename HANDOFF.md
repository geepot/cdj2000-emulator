# macOS emulator handoff

Development repository: `git@github.com:geepot/cdj2000-emulator.git`, branch
`codex/macos-nxs`. Parent research repository:
`https://github.com/geepot/cdj2000nxs-research.git`, branch `master`.
Keep the layout `CDJ/references/geepot-cdj2000-emulator` when practical.
The parent prototype remains useful evidence; this fork is the active emulator.

## Current checkpoint

Independent boot-blocker audit for the GUI task: BF531 selects the
`bfin_sic_537_io_write_buffer` path (SIC model switch 531..533), not the 52x
path. Its IMASK case forwards interrupts before storing the new mask, so a
masking write evaluates the old enabled bits. The shared forward helper emits
level 1 for every currently pending source, and CEC's port callback always
calls `_cec_raise` without inspecting level. These source facts motivate the
GUI task's trace of a stale shared-DMA interrupt after acknowledgement; they
do not yet prove the connected failure or justify suppressing interrupts.
Ensure any regression test exercises the actual 537/BF531 path. No interrupt
semantics changed in this audit.

Read-only handshake checker `python -m tools.cdj_dsp.boot_handshake EVENTS`
now distinguishes observed DSP readiness from packet-count progress. Against
`nxs-e7010-strict-baseline-1`, it reports 3,000 readiness reads and no ordered
handshake (exit 1). Against `nxs-spi-gap-strict-90s-1` and the recent legacy
profile-2 run, it identifies ready/clear/ack events 104211/104213/104215
(exit 0). Deferred profile-2 has the same sequence at 123144/123146/123148.
The result is transcript evidence only, with source-map and transcript hashes;
it does not authenticate provenance, assert strict semantics, or satisfy the
GUI/error-free-duration/repeated-cold-boot gates. Reset/start invalidates stale
ready evidence, and malformed/gapped transcripts fail rather than guessing.

Repeat profiling pair `runs/nxs-deferred-sync-profile-2` and
`runs/nxs-legacy-sync-profile-2` uses identical five input-artifact hashes and
neither changes at exit. Deferred MAIN waits: interrupt 11.79737s, MMIO read
4.31198s, MMIO write 2.95456s (19.06391s total); I/O main-loop wait 0.05223s.
Legacy I/O main-loop wait is 13.13850s, versus MAIN 0.02545/0.00861/0.00164s
at those same sites. This reproduces the contention direction. The first pair
straddled a simulator rebuild despite each run's stable hashes; use pair 2
for same-binary evidence. Host load and profiling overhead remain uncontrolled,
so these are not precise hardware-performance or sole-causation claims.
Full committed-state suite: 367 passed, 28 optional skips. Root has no live run.

Post-commit gate: `e1b0b3a` rebuild succeeds and 68 focused checkpoint/replay/
launcher tests pass. GUI task's fresh-only transport diagnostic is a regression:
the inspected frame shows E8709 COMMUNICATION ERROR and no 240-byte payloads
reach the GUI despite 18 such MAIN sends. Free pools reflect an earlier failed
handshake, NOT elimination of the real deadlock. Reject it as a fix; defaults
remain unchanged. The GUI task is tracing announcement/payload delivery.
Do not mix a new worker policy into that ongoing isolation experiment.

Potential scheduling follow-up (not implemented): a DSP worker may execute
outside BQL under a device-state mutex, using lock order BQL→device only and
thread-safe bottom-half publication after releasing the device mutex. QEMU
`hw/misc/edu.c` demonstrates this ownership pattern. Required work includes
generation-safe reset/shutdown, ordered HINT publication, immutable checkpoint
snapshots and explicit event-queue overflow. Critically, allowing host accesses
between individual core steps changes the current replay contract (which runs
a whole recorded slice atomically); implement and test those boundaries before
running a worker. Never unlock BQL around the existing interpreter without
protecting HPI, memory, peripherals and publication state. Prioritize the
remaining boot interaction gate over this performance-oriented follow-up.

Legacy profile comparison `runs/nxs-legacy-sync-profile-1` also completed
20 seconds with unchanged input hashes. It records the opposite contention:
main-loop BQL wait 13.33982s, versus MAIN interrupt 0.02559s, MMIO read
0.00830s and MMIO write 0.00175s. Legacy runs DSP in the vCPU MMIO callback
and blocks the I/O thread; deferred-v1 moves the work into I/O timer callbacks
and blocks the vCPU. Merely moving the callback does not provide balanced
execution. Both are profiling diagnostics with observer overhead, not a
controlled proof of the sole GUI deadlock cause. No root process remains live.

Measured lock contention: `runs/nxs-deferred-sync-profile-1` (20 seconds,
`--deferred-dsp-scheduling --qemu-sync-profile`) records BQL waits of 10.62940s
at MAIN interrupt handling (`cpu-exec.c:802`, 12,888 acquisitions), 4.84882s
at MMIO read (`cputlb.c:1984`, 131,024), and 3.81863s at MMIO write
(`cputlb.c:2498`, 8,665). The three sites total 19.29685s, while main-loop
reacquisition totals 0.03740s. This establishes severe MAIN lock contention
in this diagnostic, not sole causation of the GUI deadlock or hardware timing.
Raw report SHA-256:
`d449e17da73a57c0dc5c2a62d74a0a7d7e1501afccd403862990eceb7d5ba5c2`.
The opt-in profiler collects both total/mean sorted reports before teardown;
its observer overhead is explicit in run.json. A matched legacy comparison
is the next gate before selecting a scheduling correction.

Deferred-v1 is NOT a demonstrated GUI fix: the GUI task's completed 90-second
diagnostic reports unchanged link progress from approximately 35 seconds and
no MENU frame response. Stopped RAM retains free pools 52/60=0, mailbox44=34
and mailbox50=8: the same deadlock. The 20-second run spent 18.840210 seconds
executing DSP slices plus 0.445879 seconds reporting;
ending one timer-dispatch pass does not guarantee useful MAIN execution before
the next. Investigate actual CPU/BQL scheduling and the independent Blackfin
wall-clock model before promoting any policy. Full suite at this integration
point: 354 passed, 28 optional skips. Default remains legacy; milestone open.

First deferred diagnostic `runs/nxs-deferred-scheduler-diagnostic-1` completed
20 seconds: 18,386 callbacks, execution median 1.036ms and maximum 5.232ms
(legacy median 257.721ms). End state is 75,000,036 packets / 155,463,458 cycles,
eight DAC transfers and no scheduler/checkpoint errors. The inspected final
frame has rekordbox artwork and no error banner, but no interaction or liveness
claim is established. `runs/dsp-deferred-tail-replay-1` replays checkpoint 169
through the end, verifying 210 stops and exact repeat trace/state/memory and
coverage. Trace SHA-256:
`202f5cf64aa278d732fa75fbc2ab1aa85c20167a8a64f39f4b949d96d85f77ae`.
Both artifacts remain explicitly architecturally ineligible. This is a tail
replay, not complete startup validation. The GUI task is taking the separate
90-second interaction/pool-state diagnostic gate, which failed as noted above.

In-progress fairness diagnostic: QEMU now builds with opt-in
`CDJ_NXS_DSP_SCHEDULER=deferred-v1`. Its pure scheduler preserves the remainder
of a one-million-successful-step activation across 4,096-step slices, coalesces
additional triggers into one rearm, and stops at HINT/fault. Focused scheduler
tests and ASan/UBSan pass. Explicit schedule/begin/end events accompany every
slice; scheduler state is appended in schema 11. Focused scheduler, deferred
replay, launcher and migration tests pass (59 focused tests). Schema-11 state
size is 15,808 bytes, scheduler offset 15,776; schema-10 migration preserves
prior peripherals and initializes all-zero legacy scheduling. Readers derive
mode from checkpoint bytes and reject conflicting manifest labels. Fresh
`runs/dsp-deferred-tail-replay-2` again verifies the same 210 stops and exact
repeat with explicit deferred provenance. Default legacy execution is retained.
The next-timer deadline is
callback-completion virtual time plus 1ns to yield the dispatch pass: this is
a diagnostic host fairness policy, NOT a verified DSP clock ratio or eligible
milestone evidence. Do not present a possible GUI improvement as timing proof.

Boot-critical scheduler finding: `run_dsp` executes up to one million core
steps synchronously inside MAIN's HPI write callback, with no MAIN execution
between steps. With the launcher's non-icount QEMU clock, host time continues
advancing MAIN virtual time. Instrumented strict 20-second run
`runs/nxs-dsp-host-latency-1` records 91 callbacks: execution median 257.721ms,
maximum 311.222ms; reporting/checkpoint median 1.809ms, maximum 2.434ms.
The execution minimum 0.033ms includes short HINT yields. This establishes
long callback blocking, not by itself the complete cause of GUI pool depletion.
Next implementation priority is fair deferred DSP slices with explicit replay
events and checkpoint scheduling state. Merely reducing the per-trigger budget
would alter DSP progress/interleaving without preserving continuation.

Independent GUI capture audit (`/tmp/cdj-panel-capture-trace-2`): MAIN log
records 83 sends (14 status, 67 240-byte command-9 payloads, two 48-byte
command-0x10 payloads). SPORT dump records 29,946 status deliveries, 1,602
240-byte deliveries and 4,007 48-byte deliveries. Only one unique payload of
each length exists. Strict framing audit consumes all 2,777,800 bytes as
35,555 SPRX records, with no resynchronization, truncation or trailing bytes.
Dump SHA-256: `85f62fad71bb5001f9d90643cbdc0dbbd7d02f3034077bc29e4e762bde0fd97c`;
MAIN log SHA-256: `6738077450aced5921d28708e2bc2f0e8b5e8f4a5bd718afd6a49f56fdc45469`.
Reproduce the request-window report with:
`python -m tools.cdj_main.link_exchanges /tmp/cdj-panel-capture-trace-2/main.log --dump /tmp/cdj-panel-capture-trace-2/link.bin`.
There are 219 request-bit frames and 183 adjacent request pairs with identical
first six logged words; full-frame identity cannot be inferred from this log.
All 167 frames after the final MAIN send at guest time 38.4486 are plain status
polls. Payload recycling is proven; its causal role in pool exhaustion is still
under investigation. The dump has no timestamps, so do not imply exact alignment
between recycled deliveries and request events.

SPORT audit for the UI task: live simulator source is
`build/gdb-17.2/sim/bfin/dv-bfin_ppi.c` (SPORT resides in this file), mirrored
by patch 02. `bfin_sport_link_take` repeats a cached 64-byte status when no
fresh record exists. `bfin_sport_link_repeat_announced` also defaults ON,
allowing cached payloads whose length matches the last wire announcement to
repeat without new MAIN transmission. The older comment saying payloads are
always delivered once is stale. This is a candidate mechanism for duplicate
requests/pool exhaustion, not a demonstrated cause of the current NXS stall.
The UI task owns correlation and any transport correction; do not enable
announcement/CRC rewriting as a substitute for genuine transport semantics.

IIC divider helper now returns peripheral-clock cycles per SCL period from
ICCCR, tested across all 256 settings and the firmware's 0x0e (132 cycles).
It does not assume a board frequency or model START/STOP latency. Full suite
after the prior replay/controller batch: 329 passed, 27 optional skips.

Replay event-budget handling is corrected: step/packet/cycle ceilings emit
`event_budget_exhausted`, a failed diagnostic gate and no unsafe checkpoint.
Exact valid recorded stops at the limit still verify and save state; same-count
bad PC and backward counters remain genuine mismatches. Focused replay suites:
18 passed. Real one-step `runs/dsp-event-budget-diagnostic-1` reports exhaustion
at event 115240 without a final checkpoint. This is an incomplete validation
result, never a successful connected replay.

Standalone SH7764 IIC frontend now has tested idle-line readback, transactional
unsupported-mode rejection, W0C status, separate RX/TX and explicit empty-bus
address-NACK/STOP events. All 128 write addresses NACK; there is no identity
device. It is deliberately not integrated into MAIN yet. Firmware-shaped
ICCCR read-modify-write testing yields 0x0e: the prior logged value 2 was an
artifact of zero-read MMIO. Manual section 16.3.9 gives SCL=Pck/132 at 0x0e;
at the board's modeled 54MHz this is approximately 409kHz and nine periods
take 22us. Actual NXS IIC Pck, START/filter latency and STOP timing remain
unverified; do not silently inherit timer acceleration or claim cycle accuracy.
Further primary-source audit narrows Pck: service schematic p91 straps mode3,
and SH7764 Table 10.2 specifies Pck=2*EXTAL. The service schematic labels X2
26.975MHz (DSS1185-A), while its block diagram/parts list give 26.965MHz
(DSS1185). Thus documented Pck candidates are 53.950/53.930MHz, not exactly
54MHz. Preserve that source discrepancy until fitted hardware is checked.
No local authoritative IC14 register specification or physical bus capture
was found; firmware identity expectations alone are not independent device
evidence. A non-secret identity-register capture is the next useful input.

Strict tail replay `runs/dsp-spi-gap-tail-replay-1`, starting at long-run
checkpoint 364 with its transcript and a 10-million-step cap, verifies the
final nine connected stops through 333,099,500 packets / 614,081,898 cycles.
Repeat trace, coverage, final state and memory match exactly; no DSP fault.
Trace SHA-256: `eb03cdd4392ae535a8b962276a73d5163e5b36d82297c17b57feb2e0eef618fb`.
This validates that tail only, not the full transcript or GUI liveness.

Launcher integrity audit: NXS now explicitly sets `CDJ_LINK_LINK_ROWS=off`
as well as `CDJ_REQ_STATUS_FRESH=0`; the legacy board otherwise enables a
browse-command rewrite. Tests assert the actual child environment and manifest
(25 boot-evidence tests pass). No rewrite log was observed in the preceding
90-second run, so an actual mutation is not established, but future genuine
validation must use the explicit off policy. The UI task is independently
investigating the stalled MAIN-to-GUI link and unconfirmed MENU response in
that run. A stable final image is not GUI liveness evidence.

Post-gap gate: full suite with TI oracle enabled passes 321 tests / 27 skips;
timed SPI harness also passes ASan/UBSan. `runs/dsp-spi-gap-strict-1` repeats
one million steps from the previous SPI fault with eight DAC writes and no
fault. Rebuilt `runs/nxs-spi-gap-strict-90s-1` runs 90 seconds without a DSP
fault, ending by phase budget at 333,099,500 packets / 614,081,898 cycles.
Final inspected frame is normal `Not Loaded.` without an error banner.
Transcript SHA-256: `b1cd011017e968eafaf148bf5530c38dc19193d985f47927306ceb6396a06d9c`.
Frame SHA-256: `9ea6b13c57b8b37eb842d2811e0519f0499a7de86e9d8025bd48dbbbd9f0978b`.
A ten-second `down 20 08` / `up 20 08` on panel port 6084 was accepted,
with held state and empty queue observed, but this run did not visually confirm
UTILITY. Therefore interaction, frame-timeline audit and repeat cold boot are
still required; do not mark the milestone complete from this run alone.
Transcript audit confirms MAIN read ready=1 at event 104211 and wrote the
acknowledgement at event 104215. There are 346 recorded DSP stops, all with
zero fault words (the stop log still determines their meaning). Input hashes
did not change between launch and exit. Frame content and source timestamp
remain unchanged from approximately 40 seconds to the end, so this run does
not establish GUI liveness. The shared SPI clock-adapter harness independently
passes ASan/UBSan after the final inter-transfer-gap correction.

In-progress strict SPI follow-up: `runs/nxs-spi-timed-strict-1` is a rebuilt
20-second connected diagnostic without functional DSP overrides. Firmware
completed eight timed WM8740 writes and ended at a cooperative phase budget,
75,099,500 packets / 152,246,445 cycles, without a DSP fault. The inspected final
frame has no error banner. This is NOT the required 60-second/repeated/interactive
milestone. That binary predates the additional mandatory two-module-clock
inter-transfer CS-inactive gap now being tested; rerun connected validation.
The new SPI state is appended in schema 10, preserving schema-9 state on import.
Single-word reference edges are RX at 783 and DAC latch at 814 core clocks for
the observed CPU:SPI-module ratio 2:1; the subsequent idle gap is four core clocks.
Internal RX publication latency has not been measured against hardware.

The earlier SPKERNEL-only strict run received DSP ready=1 but displayed an auth
error, not E-7010. A separate firmware trace identifies MAIN's identity probe
through MMIO 0xffe70000 (register 0 expects 5, register 1 expects 1), unrelated
to DSP SPI. Do not synthesize those bytes merely to satisfy firmware checks.
Whether that error recurs in longer current strict runs remains to be tested.

Primary milestone is now a genuine cold connected boot without E-7010, with
at least 60 seconds of subsequent fault-free operation and basic GUI interaction,
repeated from cold startup. No exploratory DSP switches qualify. Instruction
inventory expansion is secondary to the actual startup path. This milestone
remains incomplete. Compact SPKERNEL decoding and the reached strict SPI1
path are corrected. Current blockers are continued GUI communication and
MAIN's missing SH7764 IIC controller, not the historical SPI1 stop below.

Auth controller evidence: SH7764 manual R01UH0360EJ0300 Table 16.2 maps
0xffe70000 as IIC. Firmware first polls ICMCR (+4), expecting idle FSCL bit
0x40; the generic peripheral trap returns zero. The 90-second run's main.log
lines 544-577 captures configuration and this poll, without a START write.
This establishes failure before any authentication-chip transaction, not a
missing-key or identity mismatch. A standalone controller model is in progress;
an absent slave must NACK, never receive fabricated identity bytes.

Long replay diagnostic `runs/dsp-spi-gap-connected-replay-1` verified stops
through sequence 107841 (125,099,500 packets), then exited at host event 107876
when its 100-million-step budget was exhausted. No complete/repeat gate was
produced. The tool currently mislabels this condition as an event mismatch;
budget handling is under repair without relaxing genuine divergence checks.

NXS-specific handshake trace (do not substitute original-CDJ `caution.py`
addresses): MAIN loader `0x041fc698` calls final handler `0x041fc646`, which
polls `0x1183fff4` for exactly 1 and acknowledges through `0x1183fff0`.
NXS status function `0x042a6524` stores device 1 at `0x04cf2468`; state 2 maps
to caution code 2, whose table entry at `0xa40b5eb8` is `{2,0x7010}`.
Success calls status `(1,1)` at `0x041fc794`; failure calls `(1,2)` at
`0x041fc7b0` or `0x041fc7bc`. DSP stage 1 publishes ready with its store at
`0x1180304c`. Trace applies to MAIN firmware SHA-256
`02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85`.

Confirmed compact SPKERNEL decoder correction:
SPRUFE8B Figure H-7 visually labels compact SPKERNEL bits 15:14 as field[4:3],
9:7 as field[2:0], and bit 0 as field[5]. The previous/GNU scatter instead maps
these to field[5:4], [3:1], and [0]. Literal H-7 plus Table 3-29 interprets
`0xdc66` at II=1 as stage 6, whereas previous/GNU decoding says stage 3.
TI C6000 CGT 8.5.0.LTS, installed by the user under `/Applications/ti`,
independently assembles and disassembles 3,0 as `9c67`, 6,0 as `dc66`, and
24,0 as `1f66`. `tests/ti/spkernel-oracle.asm` reproduces these results with
zero assembler errors/warnings; the optional `C6X_TI_BIN` test invokes both
tools. TI asm6x SHA-256:
`a57a504751cd6d849f89b07a3e84557b51587ece3ffd9746ed46b0b44b09e2f3`;
dis6x SHA-256:
`65c54eca98ef6f50c5e4a02411fb73f7f61d3c0d529aa1112d810327e0a1722b`.
The correction changes only compact field assembly, not loop timing or conflict
checks. Tests cover all 64 fields at nine II values, valid/invalid cycle fields,
full/compact schedule equivalence and exact `dc66`; the new harness passes
ASan/UBSan. Core plus field/oracle tests: 19 passed; complete suite with the TI
oracle enabled: 290 passed, 45 optional skips. The original PDF
pages 482 and 766 were visually checked; packet grouping and dynlen=7 were
independently rechecked and do not explain the conflict.

Fresh strict `runs/nxs-spkernel-h7-strict-1` passes the old conflict and stops
at PC `0xc004f42c`, word `0x021002f4`, 27,100,109 packets / 66,413,077 cycles:
`unaligned or unmapped scalar memory access`. The next access is SPI1's
currently functional-only WM8740 transmit path; do not simply remove that gate
and call its immediate completion cycle-accurate. Strict replay from checkpoint
64, `runs/dsp-spkernel-h7-strict-replay-1`, verifies all three following connected
stops, exact repeat state/memory and coverage. Trace SHA-256:
`ffa4168433f13b92b8c095911ff6a617bb9df42c3c4118ab7cb295d4abba0651`.
This proves the decoder fix passes the old blocker, not successful startup.
Earlier strict transcripts encode the old decoder failure and are no longer
expected to match after that point. Historical GNU SPKERNEL operand reports
likewise require correction; GNU agreement was not an independent oracle.

Fresh rebuilt `runs/nxs-e7010-strict-baseline-1` ran for 75 seconds and visibly
shows `E-7010: DSP DEVICE ERROR`. The DSP stops at the unchanged strict loop
conflict: 25,364,865 packets / 60,779,972 cycles, PC `0xc004f306`, word `0x2627`.
MAIN reads DSP readiness address `0x1183fff4` exactly 3,000 times, always zero
(first event 104210). Its transcript SHA-256 is
`1924222cae24905987a7dc8388760ff615e658615ad604596f3a172bfd23e758`;
final PPM SHA-256 is
`7f9a690c3fefc5ce337708898eb49f0a0860a944037a7fce1c6a574d6d321c92`.

The comparison `runs/nxs-e7010-timing-only-diagnostic-1` also ran 75 seconds,
using only `--functional-dsp-timing`, NOT the functional audio scheduler.
MAIN reads the same readiness address once, receiving 1 (event 104211), then
writes 0 to `0x1183ffec` and 1 to `0x1183fff0`. The inspected final screen is
the normal `Not Loaded.` player screen without an error banner. DSP execution
ends at its cooperative budget, 287,099,500 packets / 531,739,182 cycles, not
an instruction fault. Transcript SHA-256:
`475168aa0b05f4b1c3077d8425bd851438a47fe12ed4b8af02683f1ad74d391b`;
final PPM SHA-256:
`9ea6b13c57b8b37eb842d2811e0519f0499a7de86e9d8025bd48dbbbd9f0978b`.
Both used rebuilt QEMU SHA-256
`60a6e93ed80a839fdca0c34883e5e95e2ab5247022044ccd89e58f3fb76fd1f5`.
This isolates the readiness failure from the coarse audio scheduler. It does
not validate the timing workaround, prove continuous banner absence, or establish
interaction, audio, strict boot, or milestone completion.

The launcher now accepts optional `--frame-interval N`. Complete P6 frames are
saved with monotonic observation times and hashes under `frames/`; missing,
incomplete and duplicate observations remain explicit. Duplicate images do not
prove renderer liveness. `run.json` records hashes of resolved QEMU, simulator,
and three firmware inputs before launch and after exit, flagging differences.
This is not continuous input-mutation monitoring or an automatic boot oracle.
Focused boot-evidence and DSP replay tests: 31 passed.
Connected instrumentation check `runs/nxs-e7010-frame-evidence-1` captures
complete frames at approximately 5.10, 10.06, 15.09 and 20.01 seconds, with an
explicit missing frame at launch. The last two hashes match the strict error
screen above, and no binary/firmware input hashes differ at exit. This is
evidence of the continuing failure, not a successful boot.

Exact semantic inventory tooling now runs GNU libopcodes only at confirmed
source addresses, validates checkpoint hash, captured words/headers and decoded
widths, and retains mnemonic aliases, predicates, units and exact operand text.
It never promotes probable code or data to confirmed execution, and does not
infer semantic test coverage. `runs/dsp-semantic-inventory-2.json` resolves all
5,396 confirmed addresses to 68 mnemonic names and 262 mnemonic/unit/width
groups, with zero unresolved decodes. Independently regenerated report 3 is
byte-identical; SHA-256:
`c6cfe963e0d6e85a2d2fb2177ec79dc9078004402836d50c113d7b1a353f0c47`.
GNU frontend executable SHA-256:
`7845ca22cad4f80b632c90586dbecf8206a4429170b8804be2fed0fab6e6e5e7`.
52 inventory/coverage tests pass, including actual GNU batch decoding when
`C6X_DISASSEMBLER` is set. No core or firmware execution changed in this batch.

False-only semantic-test priorities from this exact inventory: BNOP 61,
MVK 53, LDW 22, STW 20, OR 15, AND/ADD/STB 11 each, B/STH 8 each,
LDHU 6, MVKH/LDBU/ADDAD 5 each, CMPEQ 3, ADDAH 2, MPYSP/EXTU 1 each.
These total 248 addresses. Map their exact operand forms to focused reference
tests next; these counts do not assert that the opcodes are unimplemented.
In particular, GNU identifies the false-only `m_mpy`-format instruction as
MPYSP, demonstrating why format names alone were insufficient for prioritizing
instruction-family work. Full parity and the strict timing milestone remain
unproven.

AMR/circular-addressing batch implements AMR MVC read/write (id 0, mask
0x03ffffff), per-base A4-A7/B4-B7 BK0/BK1 selection, all 32 block-size fields,
and shared circular arithmetic for scalar/pair loads/stores and
ADDAB/ADDAH/ADDAW/ADDAD/SUBAB/SUBAH/SUBAW. Other base registers and ordinary
ADD/SUB remain linear. Nonaligned transfers wrap each byte within the circular
buffer; blocks smaller than 32 bytes are undefined by TI and fail closed.
Pending transfers retain issue-time circular width in their size field's high
byte, preserving native checkpoint layout. Current readers are required for
these checkpoints; old interpreters do not understand the encoded width.
References: SPRUFE8B 2.8.3 and 3.9.2, including examples 3-4 through 3-6.

The independent table harness covers all block widths, both banks, eligible
bases, 14 address-arithmetic variants, 12 memory addressing modes, aligned
and wrapped nonaligned 4/8-byte accesses, E3 sampling/E5 publication,
AMR changes after issue, reserved modes, false predicates, and wrapped memory
overlap/high-half register conflicts. Checkpoint tests preserve pending wrapped
loads/stores. Full suite: 246 passed, 44 skipped; core and circular harnesses
pass ASan/UBSan. The source inventory motivated this batch with 72 false-only
load/store addresses; it does not establish exercised circular-mode firmware.

AMR pipeline limitation remains explicit: after an executed MVC AMR, use of
A4-A7/B4-B7 in the immediately following packet fails closed with
`AMR use interlock not implemented`. A NOP permits the new mode to be used.
SPRUFE8B 7.15.2 specifies a normal one-cycle stall but a missed-stall exception
for loop-buffer instructions. Exact pre-predicate stall prediction (including
false-predicated MVC), buffered-versus-memory distinctions, and IERR exception
delivery are not modeled. This is not complete cycle-accurate AMR support.
Reexamining 7.9.4/7.15 did not justify the strict SPLOOPD two-cycle workaround;
the existing conflict and explicitly exploratory timing mode remain unchanged.

Current-source `runs/dsp-circular-transcript-replay-1` matches all 71 previous
connected stops, exact repeat state/memory, coverage and genuine TX records.
It ends by phase budget at 58,099,500 packets / 120,392,283 cycles; trace
SHA-256 `d523861d05899844d20c80a2ec9aab6fb752fe4e207a0822e87af32851f5e969`,
coverage `bff725aa7ff787d5bbb5e793a8e36fd0c2fce97f8c6de6b493d151109664a668`.
Inventory: 4,469 source packets, 5,396 instruction addresses, 4,585 encodings,
33 probable addresses, 4,843 edges and zero faults. Strict regression
`runs/dsp-circular-strict-replay-1` repeats its one connected stop at the same
25,364,865 packets / 60,779,972 cycles and SPLOOPD conflict; trace
`35734efde2f1b3aa09ad2e5ab6735d5b19fd7dd6c592305347f9dd069e391c33`.

Fresh `runs/nxs-circular-connected-1` and
`runs/dsp-circular-connected-replay-1` verify all 69 stops, exact repeat
state/memory, coverage and TX. Final phase budget: 56,099,500 packets /
116,888,772 cycles. Transcript: 105,288 events, SHA-256
`e5c8aa47c4920464f48fbcba99c8ce93b4f27453cb56d1a8eeb8d7c400c9a5cc`;
replay trace `3bb133579bc576f82174c8f9bc3fe9402e1ed35b6a4377a819bfdfb1b6b629c2`;
coverage `a91b6914b80b0aabcdde348535bf14f0f883dd6b27d804d8a335f0c43f35619d`.
All 59,988 XBUF words are zero (SHA-256
`f84639275492f868d73c8bff2009bcbaaaa7a76b911ff133dd36b230489a17b4`).
Exploratory timing/audio remain ineligible for architectural validation;
neither full boot nor working audio is established.

Source-predicate format audit now recognizes compact conditional MVK's
CC=A0/!A0/B0/!B0 (SPRUFE8B Figure G-3), independently of side and RS.
Explicitly audited unconditional formats are classified using section 3.6 and
appendices C-H; unknown/ambiguous formats still remain unavailable. Reanalysis
`runs/dsp-source-predicate-complete-1.json` of the 71-stop saturation transcript
classifies all 5,396 observed instruction addresses: 5,148 true-observed and
248 false-only, with zero unclassified predicate formats. This does not prove
issue-time execution, buffered-loop predication, SPMASK behavior or semantics.
The 248 false-only addresses remain targeted semantic-test candidates, not
demonstrated unsupported instructions. Instruction-family completeness remains
unproven even though this particular predicate-format inventory is classified.

Standalone coverage generation now always marks architectural validation as
ineligible because it does not evaluate execution-mode provenance. Use replay
gates for validation; reanalyzing an exploratory checkpoint must not upgrade
its eligibility. A fresh 100,000-step exploratory repeat in
`runs/dsp-predicate-format-replay-1` passes without faults to 56,199,500 packets /
117,064,013 cycles. No CPU changes or connected run were needed for this
report-only batch; strict loop timing and audio limitations are unchanged.
The 35 focused coverage/replay tests pass. Reanalysis report SHA-256:
`a1b2c228467c278f7c661ba3a7d6af0511517e33b1811047295892055edfe743`.
Continuation trace SHA-256:
`d60920bc0b3136321b971864de647d5003652f19ced538c908ec93b9e7343f03`;
continuation coverage SHA-256:
`a322c4bd13dcf494999698d1987406c66477fb215ae963d543b385080d990d9e`.

Saturating arithmetic batch: full-width SADD (.L/.S), SSUB (.L), SSHL (.S),
including every scalar and signed-40-bit SADD/SSUB operand form, now shares
semantics with compact L3/S3/Ssh5/S2sh variants. SSHL uses the low six register
count bits, including nonzero saturation for counts 32..63. Results appear in
E1; CSR.SAT and per-unit SSR flags appear in E2. SSR MVC reads/writes mask to
six bits; functional-unit sets win simultaneous MVC clears. References:
SPRUFE8B SADD pp422-424, SSHL pp493-494, SSUB pp499-500, SSR 2.9.13,
compact figures D-4/F-22/F-25/F-26. The delayed-status queue uses size sentinel
34 without changing checkpoint layout; round-trip tests preserve it. Do not
resume new in-flight status effects with an older interpreter.

Independent table tests exercise sides, cross paths, compact RS/SAT selectors,
false predicates, 32/40-bit boundaries, poisoned high pair bits, masked shift
counts, delayed/sticky flags, MVC precedence and transactional failure.
Full suite: 239 passed, 44 skipped with local socket permission. Core and new
family harnesses pass ASan/UBSan. These tests establish the new semantics;
the connected traces do not establish that saturation paths are exercised.
Packed saturation, saturating multiply and the separate SAT instruction are
not covered by this batch.

Current-source replay `runs/dsp-saturation-transcript-replay-1` matches all 71
stops from `nxs-return-bnop-connected-1`, plus exact repeat state/memory,
coverage and TX capture. It ends at 58,099,500 packets / 120,392,283 cycles;
trace SHA-256 `d523861d05899844d20c80a2ec9aab6fb752fe4e207a0822e87af32851f5e969`.
Coverage: 4,469 source packets, 5,396 instruction addresses, 4,585 encodings,
33 probable addresses, zero execution faults. Source predicate audit across
this whole transcript: 2,976 true-observed, 231 false-only, 2,189 unavailable.
These are source observations, not buffered issue or architectural parity.
Strict `runs/dsp-saturation-strict-replay-1` still reproduces the documented
SPLOOPD conflict, with one verified stop and exact repeat;
trace SHA-256 `35734efde2f1b3aa09ad2e5ab6735d5b19fd7dd6c592305347f9dd069e391c33`.

Fresh connected `runs/nxs-saturation-connected-2` and deterministic
`runs/dsp-saturation-connected-replay-1` match all 69 stops and repeat exactly
through phase-budget exhaustion at 56,099,500 packets / 116,888,772 cycles.
Transcript: 105,288 events, SHA-256
`e5c8aa47c4920464f48fbcba99c8ce93b4f27453cb56d1a8eeb8d7c400c9a5cc`.
Replay trace: `3bb133579bc576f82174c8f9bc3fe9402e1ed35b6a4377a819bfdfb1b6b629c2`;
coverage: `4ad02a41a78849e6f3f3efffb1b8ff725ed1ceb1bbde0bc618b5315da97cff27`.
Counts equal the preceding 71-stop inventory, except 4,839 dynamic edges.
All 59,988 captured XBUF words are zero; TX SHA-256
`f84639275492f868d73c8bff2009bcbaaaa7a76b911ff133dd36b230489a17b4`.
Both connected/replay gates use explicitly exploratory timing/audio and remain
ineligible for architectural validation. No full boot or working audio claim.

Predicate audit tooling now records the six predicate-register boolean states
before successful source fetch steps. Coverage reports distinguish true-observed,
false-only, unavailable and empty observations; legacy traces remain unavailable.
This is not an issue-time or buffered-loop execution oracle and does not account
for SPMASK suppression. No CPU semantics or checkpoint ABI changed.
`runs/dsp-predicate-audit-2` continues the latest connected replay for 100,000
steps with exact repeat equivalence and no fault, ending at 58,199,500 packets /
120,567,521 cycles. Of 1,217 observed instruction addresses, 710 have true
source-predicate observations, 49 only false, and 458 are format-specific and
unclassified. Those 49 are audit candidates, not demonstrated missing opcodes.
Next inventory work should map these encodings to focused semantic tests and
resolve compact predicate classification without assuming unknown formats are
unconditional. Strict loop timing, zero-only audio and full-boot limitations
below are unchanged.

Current-source strict regression `runs/dsp-return-bnop-strict-replay-1`
starts at strict connected checkpoint 64, verifies the remaining connected
stop twice, and reproduces `0xc004f306` / `0x2627` at 25,364,865 packets /
60,779,972 cycles. Trace SHA-256:
`275349621eb30b740a71f7d9373cf0b82069d454558a12d5d615254754a03c06`.
Coverage now correctly records one execution fault, zero unsupported encodings,
773 source packets, 1,098 instruction addresses, and four probable addresses.

Remaining milestone evidence gaps, in priority order:

- Strict SPLOOPD epilog behavior remains unresolved. A passing repeat of its
  conflict is not a passing firmware execution. The +2-cycle exploratory
  schedule cannot establish the strict milestone.
- Confirmed packet fetches do not establish true-predicate execution of every
  instruction. Existing grouped format coverage is not a complete mapping
  from each encoding to tested semantic cases; this inventory audit remains
  required before declaring instruction-family completeness.
- Returned-loop SPMASK reconstruction and ISR-local loop replacement retain
  the documented exploratory limitations. SPRUFE8B 7.13 describes draining
  and re-piping the loop and requires software to save ILC/RILC/ITSR; it does
  not by itself justify inventing a second persistent hardware loop context
  across an ISR. Any replacement design needs that distinction resolved.
- Remaining packed/multiply saturation, AMR-use interlocks, reload forms and unimplemented
  control registers remain fail-closed. Current traces have not established
  them as terminal missing-opcode frontiers; probable code is not confirmation.

Returned-loop immediate BNOP now follows SPRUFE8B 7.13.2: full-width and
compact `BNOP label,n` become `NOP n+1` while piping up after interrupt
return. They do not redirect execution or enter the loop buffer. Register
branches and ordinary loop branches retain existing checks. Tests cover all
full-width N values, both sides, true/false predicates and four compact forms.
The C674x sanitizer and 17 focused tests pass. Exploratory continuation
`runs/dsp-return-bnop-replay-1` repeats exactly for 100,000 steps to
56,199,500 packets / 117,064,013 cycles with no fault; trace SHA-256 is
`6b1f1766c1a0d0295f2b394ba4110913ec58eb3fca09d9c159f7f22d98b3fc3f`.
Rebuilt connected `runs/nxs-return-bnop-connected-1` ends by phase budget at
58,099,500 packets / 120,392,283 cycles. Its 105,362-event transcript hash is
`52b503613ac5fe450c04edf7664625e2582659f39d4b6fa483adb86b7f242cbb`.
This is regression evidence; the new return case is established by focused
tests, not claimed as newly observed in firmware. Strict SPLOOPD timing and
the other documented approximations remain unresolved.

Replay now preserves exploratory ancestry across explicit checkpoint resumes.
Switching back to strict execution cannot make previously approximate state
eligible for architectural validation. Inherited approximation descriptions
and ineligibility propagate through manifest, repeat gate and coverage; strict
automatic selection rejects these descendants. Repeat equivalence remains a
separate reproducibility result. Tests exercise two consecutive strict resumes
from an exploratory checkpoint to prevent loss of ancestry after one hop.

Directory-based replay selection now filters candidates by requested timing
and audio modes and event-transcript hash, in addition to checksum/provenance.
This prevents a newer exploratory checkpoint from displacing the strict
candidate during automatic selection. Explicit paths still permit deliberate
mode transitions and need separate consideration of inherited approximations.

Coverage fault classification now separates `faults`/`execution_faults` from
decoder-only `unsupported`/`unsupported_faults`. A nonempty terminal fault is
recognized regardless of the stop reason: connected traces use the fault text
as their reason. Previously these connected faults were omitted from coverage.
Reanalysis of `runs/dsp-splx-strict-replay-1` reports one execution fault at
`0xc004f306` (`0x2627`, parallel register write conflict), zero unsupported
encodings, and six probable addresses. Its coverage `validation_eligible` is
false; exact-repeat success still describes reproducibility of the failure.
Historical coverage hashes describe the old reports and are not rewritten.

### Latest integrated DSP batch (schema 9)

This section supersedes the schema-8 checkpoint below.  The C674x core now
drains interruptible SPLOOP/SPLOOPW schedules, preserves the selected request,
vectors only after the software-pipeline epilog, and returns through the real
`B IRP`/SPLX path.  SPLOOPW uses its delayed predicate during normal execution
and keeps termination false for the first three cycles after interrupt return,
as required by SPRUFE8B 7.10.3 and 7.13.  A predicate that terminates during
interrupt drain vectors with the post-loop PC instead of incorrectly
restarting the loop.  Focused tests cover both outcomes.

Returned loops containing SPMASK follow the documented program/buffer
selection reversal: SPMASKed program-memory operations become NOPs and exact
retained unmasked operations execute.  Strict mode still fails closed because
the interpreter does not model exact retained-buffer pipe-up timing.  Breadth
mode retains loop tag/length/II metadata in the checkpointed loop context and
reconstructs the schedule from exact PC/word/header matches in the stable
program image.  A real ISR at `0x1180249a` contains its own compact SPLOOP, so
breadth mode permits it to replace retained validation metadata and rebuilds
the interrupted loop from current program bytes on the later SPLX return.
Self-modifying loop bodies are unsupported and this is explicitly labeled as
an approximation.

SPI1 now has a functional-only, board-specific WM8740 control endpoint.  It
accepts only the observed 16-bit, MSB-first, CS0 configuration and valid DAC
register writes; SPI1 SOMI returns `0xffff` because the board leaves it
disconnected and the C6747 pin has an internal pull-up.  Unsupported SPI
configurations still fail closed.  `SPIBUF` correctly preserves sticky RXOVR.
Checkpoint schema 9 adds lossless WM8740 programmed/active attenuation,
unlock, last-word and transfer-count state.  The recorded schema-8 migration
`runs/dsp-schema8-to-9-migration-2` passes exact trace, state and memory repeat
gates.

The next connected blocker was an older B0 delayed result colliding with the
first ISR `MVKH B0`.  SPRUFE8B Figure 5-4/5.4.4 requires older non-annulled
execute stages to finish before the handler.  Breadth mode now inserts the
minimum empty cycles needed to retire already-issued loads/stores before ISR
fetch.  Exact interrupt-entry latency is not claimed; strict mode retains its
fail-closed collision behavior pending a full pipeline timing model.

The deterministic batch first advanced 1,000,000 packets beyond the old
27.1-million checkpoint, then another 5,000,000 packets after the interrupt
entry fix.  `runs/dsp-post-interrupt-pipedown-5m-replay-2` ends by its step
limit at 33,100,114 packets / 76,600,256 cycles with zero unsupported faults;
its trace SHA-256 is
`5121630448324766fed2c412853a65b212d66bb85f6feb1bfcfe4b71260066d1`
and its exact repeat/state/memory gate passes.

Fresh connected evidence is
`runs/nxs-dsp-wm8740-sploopw-functional-3`.  MAIN, Blackfin and C674x execute
together for the 15-second budget and record 96 schema-9 checkpoints, 105,288
ordered events and 69 DSP stops.  The final stop is a phase budget, not a
fault, at 56,099,500 packets / 116,888,772 cycles.  Its event transcript
SHA-256 is
`e5c8aa47c4920464f48fbcba99c8ce93b4f27453cb56d1a8eeb8d7c400c9a5cc`.
The complete independent replay `runs/dsp-wm8740-connected-replay-3` verifies
all 69 connected stops twice, exact final state/memory, coverage and the
59,988-record transmit capture byte-for-byte.  Replay trace SHA-256 is
`7fe32d6bda3f0dce5c285c9b116f299e328cf8ae842a5ac0690159c1417537a8`;
coverage SHA-256 is
`f5809e49118533611c3c125b7493fa33506f02519ea1886222163511c217de39`.
Coverage contains 4,469 confirmed source packets, 5,396 confirmed instruction
addresses, 4,585 distinct encodings, 4,843 dynamic edges, 33 probable targets
and zero unsupported faults.

The capture contains 29,994 words from each active McASP serializer, all zero,
with SHA-256
`f84639275492f868d73c8bff2009bcbaaaa7a76b911ff133dd36b230489a17b4`.
The WM8740 receives eight genuine control transfers and ends with program
registers `[511, 511, 8, 25, 0]`, active attenuation `[255, 255]`, and last
word `0x619`.  This proves deterministic firmware-driven control and transport,
not full DSP parity, full boot, nonzero audio, PCM correctness, or working
host audio.  Functional SPI completion, returned-loop reconstruction,
minimum interrupt pipe-down and packet-driven McASP slots remain explicitly
ineligible for cycle-accuracy claims.

### Latest integrated DSP batch (schema 8)

This section supersedes the older chronological status below.  The current
source combines the broad C674x ISA batches with CPU interrupt recognition,
C6747 INTC pulse delivery, cache-control registers, McASP transmit state,
EDMA3CC transfers/events, SYSCFG master-priority registers, transactional
McASP/EDMA integration, a labeled coarse functional-audio scheduler, genuine
XBUF-word capture, and a fail-closed subset of TSR.SPLX loop-return handling.
Checkpoint schema 8 preserves all of that state.  Its native state is 15,704
bytes; the final component is 7,712 bytes.  Exact one-step migrations from
schemas 6 and 7 are recorded in `runs/dsp-schema6-to-8-migration-2` and
`runs/dsp-schema7-to-8-migration-2`; both repeat/state/memory gates pass.

The latest strict connected run is `runs/nxs-dsp-splx-strict-1`.  It records
65 checkpoints, 110,208 ordered events and 39 DSP stops, then reproduces the
known fail-closed SPLOOPD conflict at exactly 25,364,865 packets / 60,779,972
cycles, PC `0xc004f306`, compact `0x2627`.  Standalone
`runs/dsp-splx-strict-replay-1` verifies all 39 connected stops and exact
repeat state/memory.  This is architectural-validation-eligible evidence of
the failure, not successful boot.

The strict schedule has now been reconstructed against SPRUFE8B 7.6, 7.9.4
and 7.15.  It is an II=1, 128-iteration, seven-cycle SPLOOPD body with
`SPKERNEL 3,0`.  At the documented post-fetch cycle, buffered
`MV .L2X A3,B4` overlaps post-loop `MVK .L2 1,B4`, a prohibited register/unit
conflict.  The existing functional two-cycle delay reaches a safe drain cycle,
but the ISA does not justify making that delay strict.  TI compiler defect
SDSCM00042974 produced this class of malformed epilog overlap; that is a
plausible lead, not proof about this firmware.  Keep strict mode fail-closed
until the fragment can be checked on a C6747 ISS/EVM or D810K013 hardware.
TSR.SPLX is now synchronized with normal loop activation and clearing.  The
implemented B IRP return subset preserves idle SPLX, restarts returned SPLOOPD
with ordinary SPLOOP counting, and suppresses operations parallel with the
return setup.  Returned SPMASK retained-buffer reversal and SPLOOPW return
remain fail-closed; interrupt-time loop draining and the full retained-buffer
state are not implemented.  This is partial architectural bookkeeping, not
complete interruptible software-loop support.

The latest breadth run is `runs/nxs-dsp-tx-capture-3`, using
`--functional-dsp-timing`, `--functional-dsp-audio`, and
`--capture-dsp-tx`.  It reaches the genuine
DSP scheduler/poll loop and ends by budget at 27,099,500 packets / 66,177,094
cycles, not at an unsupported instruction.  Exact replay in
`runs/dsp-tx-capture-replay-3` verifies all 40 connected stops, state/memory,
and the transmit capture byte-for-byte.  Coverage contains 3,447 confirmed source packets, 4,276
confirmed instruction addresses, 3,654 distinct encodings, 3,560 dynamic
edges, 13 probable direct targets and zero unsupported faults.  All probable
targets use already exercised decoder families.  This shows strong reachable
functional breadth; it is explicitly not cycle-validation, full-ISA, boot or
working-audio evidence.

The functional-audio mode advances active McASP1/2 transmit slots once per
1,024 successful DSP packets and services resulting EDMA requests atomically.
It consumes only genuine firmware/EDMA XBUF writes and records underruns rather
than inventing samples.  `dsp-tx.jsonl` records 3,348 newly latched 32-bit
serializer words: 1,674 from McASP1 serializer 0 and 1,674 from McASP2
serializer 3.  Its SHA-256 is
`72c70733a4b7016cde086e322d754cdd17dbc4e13ca652a79f06e5667ab0e3b2`.
Every captured word is zero.  Source-ring inspection also found no nonzero
payload, so the current upstream playback/control/storage path has not supplied
audio; the result must not be described as silence successfully rendered.  The
mode has no physical sample clock, PCM interpretation, host audio, receive path
or rate matching.  Stimulating the genuine upstream playback path and then
adding a measured McASP clock/PCM sink are higher-value next steps than
speculative ISA families: the currently reached instruction inventory has no
missing-opcode frontier.

Replay now accepts independent packet and cycle delta ceilings in addition to
step limits, reports progress and approximation/eligibility fields, writes an
exact resumable checkpoint, and emits `failure.json` for fail-closed outcomes.
Diagnostic single-run checkpoints are explicitly labeled and remain distinct
from repeat-gated checkpoints.  A connected transcript still must reach its
next recorded stop; an earlier user ceiling is not accepted as a partial
connected validation.

The complete socket-enabled suite reports 192 passed / 43 optional skips.
CPU, INTC, McASP, EDMA and schema-8 checkpoint ASan/UBSan harnesses pass, and
the current QEMU SH4 build completes.  A visible MAIN run can still stop on a
panel/GUI contract outside this DSP milestone; a rendered frame or GUI exit
status must never be presented as DSP boot or audio success.

The strict validation baseline is intentionally unchanged: 25,364,865 DSP
packets / 60,779,972 cycles, PC `0xc004f306`, compact word `0x2627`, where the
II=1 `SPLOOPD` epilog produces a genuine L2 resource conflict. Strict mode is
still the default for connected and standalone replay. The opt-in environment
variable `CDJ_NXS_DSP_FUNCTIONAL_TIMING=1`, exposed by both runners as
`--functional-dsp-timing`, adds exactly two cycles to the post-loop fetch for a
delayed software loop. This is a development run-ahead approximation for
inventorying downstream firmware, not resolved C674x timing. Its manifests say
`dsp_timing_mode: functional-runahead` and
`architectural_validation_eligible: false`; results obtained in this mode must
not be cited as strict or cycle-validation evidence.

The C6747 cache/memory-system control block at `0x01840000` is now modeled as a
functional register family from SPRUFK5A chapters 2-4, with device map/reset
details from SPRUH91D and SPRS377F. It covers L2CFG, L1PCFG/L1PCC,
L1DCFG/L1DCC, documented block-operation base/count registers, global
writeback/invalidate commands, and the C6747-valid MAR ranges. L1P/L1D reset to
the device's 32 KiB/max-cache encoding and L2 resets to all RAM. Unsupported or
reserved registers and MAR ranges fail closed. Cache data/tags, cache misses,
privilege checks, arbitration, and operation timing are not modeled: all
backing memory remains unified and coherent, cache operations complete
immediately, and operation registers consequently read zero. This is sufficient
register-level initialization behavior, not evidence of cache or cycle
accuracy. EDMAWEIGHT and L2ALLOC0-3 remain unmodeled and fail closed if reached.

Checkpoint schema 6 appends cache-control state and accepts schemas 1-5 only
after validating their original payload/checksum, then initializes the absent
cache state to documented reset values. The current native ABI state is 9,520
bytes with final peripheral-tail component size 1,528. Cache, checkpoint,
software-loop, deterministic replay, inventory, and coverage focused tests
report 13 passed. The broader restricted-sandbox run reports 181 passed / 43
skipped; four failures and four setup errors require localhost socket binds and
are environmental rather than observed semantic failures. A socket-enabled
full-suite rerun is still required before recording a final suite result.

The full-width `.M` non-saturating scalar 16-by-16 multiply family is now
implemented as one batch: signed, unsigned and mixed-sign low/high-halfword
forms plus both signed-immediate forms, on both register sides and cross paths.
Results use the documented E2 publication path and existing delayed-result
conflict checks. Table-driven tests cover all 18 opcodes across sides/cross
paths, signedness, delay, and false predicates. Saturating `SMPY` forms remain
fail closed until delayed CSR.SAT semantics are implemented rather than
approximated.

Two connected MAIN/Blackfin/C674x runs exercise this downstream work in the
explicitly non-validating run-ahead mode. `runs/nxs-cache-functional-1` passes
the strict conflict, observes genuine SPI1 and L1PCFG/L1DCFG initialization,
then stops at 25,378,653 packets / 60,793,894 cycles, PC `0xc0038a40`, word
`0x01a86e80`, decoded as `MPYUS .M1 A3,A10,A3`. After the multiply-family
batch, `runs/nxs-mpy-functional-1` passes that instruction and stops at
25,380,302 packets / 60,796,809 cycles, PC `0xc003b3a0`, compact `0xccf7`,
decoded as `SUBAW .D2 B15,6,B15`. Both runs have schema-6 checkpoints, 110,208
ordered events and 39 DSP stops, exit the bounded GUI process with status zero,
and publish a frame. Those last two facts do not establish boot. The next
high-leverage ISA batch is the compact `.D` address-arithmetic family around
this `SUBAW` and the nearby `ADDAW`; full boot, interrupt delivery, cache
timing, peripherals beyond their modeled register surfaces, and audio remain
incomplete.

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

The full-width MVC interrupt/control batch is now reference-backed rather than
accepting only the previously reached ILC/RILC and floating-point controls. It
implements CSR, IFR/ISR, ICR, IER, ISTP, IRP and NRP reads/writes with register
masks, IER's fixed reset-enable and set-only NMIE, synthesized ISTP.HPEINT, and
the documented one-delay-slot visibility of ISR/ICR changes. Delayed IFR set
and clear effects reuse the existing checkpointed result queue without changing
the CPU structure ABI; simultaneous set wins over clear. Reset now exposes
C674x CPU ID `0x14`, little-endian mode, IER `1`, and the C6747 HOST1CFG ROM
ISTP default `0x00700000`. CPU interrupt recognition/vectoring is still absent.

PCC/DCC are architecturally ignored on C674x. PWRD behavior is device-specific;
the current core explicitly ignores MVC writes to CSR.PWRD because physical CPU
power-down is not modeled. This is an approximation, not evidence of sleep-mode
support. Focused tests and the standalone ASan/UBSan harness pass. The full suite
passes 185 tests with 43 optional skips. Exact replay on the unmodified loop
scheduler still reproduces the connected 25,364,865-packet conflict and passes
repeat verification:

```sh
python -m tools.cdj_dsp.replay \
  runs/nxs-prot-connected-2/dsp-checkpoints/00000000000000000064.cdjdsp \
  /tmp/dsp-control-validation --steps 1000000 \
  --events runs/nxs-prot-connected-2/dsp-events.jsonl --verify-repeat
```

The C6747 megamodule interrupt-controller register family is now implemented
from SPRUFK5A chapter 7. It covers four banks of EVTFLAG, EVTSET, EVTCLR,
EVTMASK, MEVTFLAG, EXPMASK and MEXPFLAG plus INTMUX1-3, including reset values,
events 0-3's fixed masks, derived masked views, 7-bit selector fields, and
fail-closed access direction/width checks. The recorded UHPI DSPINT rising edge
now latches documented C6747 system event 34. CPU interrupt recognition,
acknowledgement/vectoring, exception/drop state, AEG, and same-cycle incoming
event versus EVTCLR arbitration are still absent and must not be inferred from
the register model.

Checkpoint schema 3 appends INTC state while preserving schema-1/2 migration.
Legacy payloads are checksummed before the missing state is reset, including
their native trailing padding. The current ABI is 8,080 state bytes with final
component size 88; schema-2 migration replay produces checkpoint SHA-256
`21764b341723214c2072504543b0a0f4fb804c2d5932b4f977d6dad1ee5073ea`.
Focused tests report 23 passed; the complete suite reports 186 passed / 43
optional skips; INTC, checkpoint, and replay ASan/UBSan harnesses pass.

The rebuilt connected run `runs/nxs-intc-connected-1` writes 65 schema-3
checkpoints and 110,208 ordered events, and reproduces the unmodified scheduler's
exact 25,364,865-packet / 60,779,972-cycle conflict. GUI exit zero and a frame
are not boot evidence. `runs/dsp-intc-connected-replay-1 --verify-repeat` gates
all 39 connected stops plus byte-identical state and memory; trace SHA-256 is
`e87f6d32b6cadc3760cd733776071dbf7b41a8f73ba8e0216b9dc2cf87c1eab3`,
coverage SHA-256 is
`3d40ec0fd84c512be54c10474e45496e938688591cefcdc462db67495007ee51`,
and final checkpoint SHA-256 is
`d4126e3436399b5785a1ba7944032a7df0610a9525af5e995ddf02b40e05cdfd`.

One explicitly non-validating run-ahead temporarily added the still-unproven
two-cycle SPLOOPD drain adjustment and relaxed only the captured terminal-stop
comparison. Both edits were removed immediately. With the INTC batch, genuine
firmware clears EVTCLR0-3, programs EVTMASK0-3 and INTMUX3, then reaches
25,364,958 packets / 60,780,074 cycles and PC `0xc004ef88`. GNU libopcodes
decodes word `0x020c0264` as `LDW .D1T1 *+A3(0),A4 || NOP 5`, with A3
`0x01c20024`; SPRS377F section 6.22 identifies this as Timer64P0 TGCR. This is
downstream inventory only, not validation of SPLOOPD timing, INTC execution, or
the exploratory packet count.

Both C6747 Timer64P instances are now modeled as the complete documented
register family from SPRUH91D chapter 28: revision ID, emulation/GPIO control,
counter/period/control/global control, watchdog control, reload/capture,
interrupt status/enable, and eight compare registers. Tests cover reset and
reserved-bit masks, 64-bit TIM12/TIM34 shadow reads, Plus-mode read-reset,
counter reset controls, status W1C, atomic check-phase access, and fail-closed
reserved offsets. Timer clock progression, external pins, output pulses,
watchdog reset, DMA events, and INTC delivery are explicitly not implemented.

Checkpoint schema 4 appends both timer states. Schema-3 migration replay passes
repeat/state/memory gates and produces an 8,280-byte state with final component
size 288 and checkpoint SHA-256
`9593afb2207aaedb23d7370de148207a66391c9b3c0ecfd6a5c397827b74c7c3`.
The complete suite reports 187 passed / 43 skipped, and Timer64P, checkpoint,
and replay ASan/UBSan harnesses pass. The rebuilt connected run
`runs/nxs-timer-connected-1` has 65 schema-4 checkpoints, 110,208 events and 39
DSP stops, reproducing the unchanged validated conflict. Its repeat gate is
`runs/dsp-timer-connected-replay-1`: trace SHA-256
`e87f6d32b6cadc3760cd733776071dbf7b41a8f73ba8e0216b9dc2cf87c1eab3`,
coverage SHA-256
`f9f2014e4f1fe7bf200a7fc9148d9c5a5a190df96823891f4af67a4a95058e38`,
and final checkpoint SHA-256
`dfa2a229fd3b6b26ffe4802f5d788c5b825391888cab1e1fdc6017ed73440b81`.

A second explicitly non-validating run-ahead completed the observed Timer64P0/1
initialization writes and reached 25,364,997 packets / 60,780,156 cycles at PC
`0xc004f4d0`, `STW A4,*A3`, A3=`0x01e12000`. SPRS377F section 6.17 identifies
that address as SPI1 SPIGCR0. The exploratory SPLOOPD/transcript edits were
removed; SPI0/1 are the next coherent peripheral family.

Both C6747 SPI instances now implement the complete register family from
SPRUH91D chapter 27: module reset/mode control, interrupt enable/level/flags
and vector side effects, GPIO pin control, transmit configuration,
receive-buffer read clearing, delay/default chip-select, and four format
registers. Reserved fields are masked. External pin reads require explicitly
supplied pin evidence, and enabled data-register writes fail closed because no
physical SPI slave, transfer clock/timing, DMA request, or INTC delivery is
modeled. The reached initialization sequence does not transfer data.

Checkpoint schema 5 appends both SPI states and migrates schema 1-4 only after
validating each old payload. Schema-4 migration produces an 8,448-byte state,
final component size 456, and repeat checkpoint SHA-256
`c7975769c91f1d2fcec1f8752545cbc573f4a398034296719c5d890a7e9323d7`.
The complete suite reports 188 passed / 43 skipped; SPI and checkpoint
ASan/UBSan harnesses pass. The rebuilt connected run
`runs/nxs-spi-connected-1` has 65 schema-5 checkpoints, 110,208 events and 39
DSP stops, reproducing the unchanged validated SPLOOPD conflict.
`runs/dsp-spi-connected-replay-2 --verify-repeat` gates all stops and exact
state/memory: trace SHA-256
`e87f6d32b6cadc3760cd733776071dbf7b41a8f73ba8e0216b9dc2cf87c1eab3`,
coverage SHA-256
`43ec202de4627962042ac0a48988ab7d48cc0ea4392eeee055fe0a52f1074695`,
and final checkpoint SHA-256
`ce921817e55e21a936c30a02d1a3d327ba14347a1344a4e25e60151ac0195464`.

One explicitly non-validating run-ahead temporarily restored the still
unproven two-cycle SPLOOPD drain hypothesis. It observed all nine SPI1 setup
writes, including `SPIFMT0=0x00021810`, then reached 25,365,052 packets /
60,780,224 cycles at PC `0xc004f568`, `STW B5,*A3`, A3=`0x01840020`, B5=7.
SPRS377F Table 3-2 identifies this as L1PCFG; the next instruction writes the
same value to L1DCFG at `0x01840040`. The temporary edit was removed and the
stable QEMU binary rebuilt. DSP memory-system/cache control is the next
coherent family. This downstream inventory does not validate SPLOOPD timing,
cache behavior, full boot, or audio.

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

Latest full fork suite: 185 passed, 43 skipped. The CPU standalone harness
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
