# Targeted emulator optimizations (2026-09-10)

The DSP read dispatcher checks shared RAM, enabled SDRAM, and global/local L2
before probing peripheral models. These ranges do not overlap MMIO. SDRAM's
live enable bit, alignment requirements, and bounds are preserved. Peripheral
reads retain their original dispatch order.

C674x packet fetch reuses a fetch-block header within that call only. It reads
a new header when crossing a 32-byte boundary and discards the cache on return.
No guest cycles or writes occur during fetch, and the read callback contract is
side-effect-free. Thus code changes between packets remain immediately visible;
there is no persistent decoded-code cache or checkpoint ABI change.

Blackfin patch 08 skips the full RGB comparison after unchanged scans; see
`patches/README.md`. The existing changed-line conversion and changed-frame
publication policies remain in effect.

## Validation and measurement

New tests exercise packet boundaries, compact instructions, updated instruction
memory, missing headers, RAM boundaries, the local L2 alias, SDRAM enablement,
MMIO fallback, and framebuffer publication. The local QEMU and Blackfin binaries
were rebuilt using the repository build scripts.

An isolated `-O3` C674x fetch benchmark on this host fetched ten million
8-instruction packets using a trivial side-effect-free memory callback. Across
three alternating before/after trials, the old code used 0.240-0.244 CPU seconds
and the new code used 0.203-0.208 seconds (about 15% less CPU time). Read callbacks
fell from 160 million to 90 million, or 16 to 9 per packet. This measures packet
fetch only, not instruction execution, real firmware, boot time, or overall
emulator speed. No end-to-end speedup is claimed.

The first pass left full CPU-state copies and persistent instruction predecoding
for separately measured work. The second pass below narrows packet execution's
copy without changing the CPU layout. Persistent instruction predecoding remains
unimplemented.


## Second pass: packet state and loop scheduling

`cdj_c674x_execute` now copies only the mutable scalar/pipeline prefix through
`offsetof(CdjC674x, loop)`: 3,024 bytes rather than 6,592 on this host. The
transactional copy and success-only commit remain, including atomic decode
failure. The loop schedule and retained instruction array are neither read nor
written by packet execution; loop setup/stepping still owns them. Branches can
idle a loop by changing prefix fields without clearing its retained storage.
The CPU layout and checkpoint ABI are unchanged. Future helpers called with the
partial working copy must not access its uncopied tail.

The loop scheduler visits only origins congruent to the current cycle modulo
the initiation interval, preserving their ascending order and all filtering,
capacity, finite-iteration, and predicate-epilog rules.

`tools/cdj_dsp/benchmark_core.c` supplies a reproducible synthetic benchmark
(build command in its header). Three alternating trials against the first-pass
sources, compiled with `-O3`, gave:

| Workload (3 million calls) | Before CPU seconds | After CPU seconds |
| --- | --- | --- |
| Single MVK packet execution | 0.405–0.408 | 0.181–0.187 |
| 48-origin loop scheduling, II=8 | 0.083–0.086 | 0.028–0.029 |

These are about 55% and 67% less CPU time for the selected isolated workloads,
respectively. They do not establish a whole-emulator or firmware speedup.

The loop regression test compares against the frozen original algorithm across
326,144 combinations, checking output order, rejection, and complete scheduler
state. CPU tests cover retained-tail preservation on branch retirement and
rollback after a conflicting parallel write. Core, circular-addressing, and
scheduler harnesses also passed AddressSanitizer and UndefinedBehaviorSanitizer.

## Dispatch table (2026-09-10)

`cdj_c674x_execute`'s if/else-if decode ladder became a table of 114 rows plus
one `arm_*` function each, and the function went from 1,679 lines to 805. The
transactional copy described above is unchanged - still one
`memcpy(&out, cpu, offsetof(CdjC674x, loop))` with commit only on success - and
the change was held to a byte-identical proof rather than to a passing suite:
all 65,536 compact verdicts, 1,854 decode verdicts, 32 control-register rows and
5 packet-atomicity cases are identical before and after.

It costs about **9% on the synthetic dispatch benchmark**: selection is now a
linear scan of 114 rows where the ladder short-circuited on its common cases.
That is a measured regression on an isolated workload, not an end-to-end one, and
it is recorded here rather than hidden because the obvious fix does not work as
stated - several rows constrain no bits in 6:2 (masks 0x0000000c, 0x0000001c,
0x0000003c, 0x0000010c, 0x0ffffffe, 0x0f830ffe), so they would belong in every
bucket of a `w & 0x7c` bucketing. Anyone optimising this should re-run
`tools/cdj_dsp/benchmark_core.c` and the probe sweeps together.

## Decode memo and bounded copy-in (2026-09-28)

A connected profile (`analysis/iterations/15-dsp-lookup`) put the DSP
thread's time in the packet-entry state copy (70% of all memmove), the
114-row first-match arm scan (~17%), and fetch. Two exact changes:

* `cdj_c674x_arm_lookup` memoizes row selection per instruction word in a
  4,096-entry direct-mapped, thread-local table. Selection is a pure function
  of the word (`cdj_c674x_arm_table_predicates_are_word_only`), so a hit
  returns what the scan would.
* `cdj_c674x_execute` copies the scalar prefix, the live queue entries and
  8 spare slots per queue, not the whole 3,024-byte prefix. (Superseded by
  in-place execution below.)

Replay of checkpoint 400 (`tools/cdj_dsp/replay.c`, 20 M steps, `-O2`, three
alternating trials): 3.94–3.97 → 3.33–3.38 user CPU seconds, about 15% less.
Replay includes coverage/trace printing the live board does not do. The arm
scan fell from 15% of samples to 1.5%, and memmove from 23% to 17%. Traces
and final checkpoints from checkpoints 1, 25 and 250 (1 M steps each) are
byte-identical to HEAD's.

Connected: two QEMU builds from one QEMU tree and toolchain (HEAD vs this
change), four alternating 60-second headless NXS boots (`nxs_vm --seconds 60
--frame-interval 0.2`). The DSP executed 400.1M and 402.1M packets on HEAD
against 482.1M and 490.1M with the change, about 21% more in the same wall
time. The GUI exited 0 in all four, no run logged a DSP fault, and all four
final screens are byte-identical. The firmware-free QEMU integration tests
and the full pytest suite (934 passed, 33 skipped) pass on the new binary.

## In-place packet execution (2026-09-28)

After the change above, the connected profile still spent ~20% of the DSP
thread in the two per-packet memcpys, the larger of them the commit back into
the CPU. `cdj_c674x_execute` now backs up the scalar prefix and live queue
entries, runs the packet (`execute_packet`) directly on the CPU, and restores
the backup if the packet fails. Queue appends go through `append_load` and
`append_store`, which first save the slot's old bytes into the backup, so a
rollback leaves exactly the bytes a discarded scratch copy would. Bus and tick
callbacks read `cpu.cycles` and `cpu.packets` mid-packet (HPIC event records),
so those two are still written only when the packet retires. The DSP runs
under QEMU's global lock, so no other thread observes a half-executed packet.

The original scratch-copy transaction remains behind
`CDJ_C674X_COPY_TRANSACTIONS` and for an invalid incoming queue count.
`tests/test_c674x_transaction.py` builds it as the byte-exact reference, with
two new cases in which a queue append is followed by a rejected parallel
instruction. Removing either append-time save or the rollback fails the gate.

Replay traces and final checkpoints (checkpoints 1, 25, 250) are
byte-identical to the previous commit's; replay CPU 3.34–3.41 → 3.05–3.06 s.
Connected, alternating 60-second NXS boots against the previous commit:
480.1M and 484.1M DSP packets before, 561.1M and 566.1M after (~17% more;
~40% more than before the decode memo). The GUI exited 0 in every run, no run
logged a DSP fault, and every final screen is byte-identical. The full suite
passes on the new binary.

## Per-cycle path trims (2026-09-28)

Four exact changes to code that runs every DSP cycle or packet:

* The arm-lookup memo is a relaxed `_Atomic uint64_t` table (word and row in
  one entry) instead of a thread-local one; on macOS every thread-local access
  was a `_tlv_get_addr` call. The rollback extent is recorded once at the end
  of issue and passed as a parameter, instead of per-append thread-locals.
* `cdj_spi_core_tick` returns early for the valid idle SPI transfer state,
  which `cdj_c6747_spi_wm8740_advance` would only have validated. Any other
  state, invalid ones included, takes the full call.
* `cdj_c674x_loop_functional_timing()` is an inline read of an exported flag.
* `cdj_c6747_pll_tick` skips the division when no whole OSCIN period elapsed.

Replay output is byte-identical (checkpoints 1, 25, 250); replay CPU is
2.94–2.96 → 2.73–2.78 s. Connected alternating 60-second boots: 563.1M and
573.1M DSP packets before, 601.1M and 602.1M after (~6%), identical final
screens, no faults, full suite passing.

## Idle timers, SPI clock and fetch order (2026-09-28)

* `cdj_c6747_timers_tick` skips a timer whose ENAMODE12 and ENAMODE34 are
  both 0: every mode's counter, prescaler and compare path is gated off then.
  The NXS firmware configures both timers (TGCR) during boot but never writes
  TCR in a 60-second boot, so this per-cycle call did nothing.
* `cdj_spi_core_tick` replaces its per-cycle divide/modulo with subtraction;
  `cdj_spi_clock_ready` makes numerator equal the denominator, so it loops once.
* NXS `dsp_read` tests L2 (and its local alias) first. The windows are
  disjoint, and firmware executes from L2, which previously paid a cross-file
  L1D call per instruction fetch.

Replay output byte-identical; full suite passes. Connected alternating
60-second boots: 596.1M and 601.1M DSP packets before, 613.1M and 623.1M
after (~3%), identical final screens, no faults.

## Fetch-block fast path (2026-09-28)

`cdj_c674x_fetch` made one read callback for the fetch-packet header and one
per instruction word. Boards may now register `cdj_c674x_set_fetch_block`: a
side-effect-free hook returning a host pointer to a whole 32-byte-aligned
fetch block when it is plain memory the paired read callback would return
word for word, or NULL, which falls back to read. The pointer is used only
within one fetch call, so nothing is cached and code writes stay visible to
the next fetch. NXS (`dsp_memory_span`) and the replay board (`memory_span`)
register it; both helpers cover exactly their read path's RAM windows, which
are disjoint from every peripheral, and every window is 32-byte aligned.

Replay output byte-identical; replay CPU 2.65–2.72 → 2.05–2.09 s (replay's
read path probes every peripheral before RAM). Connected alternating 60-second
boots: 618.1M and 622.1M DSP packets before, 642.1M and 641.1M after (~3.5%),
identical final screens, no faults, full suite passing.

## Exact idle-loop skip and GUI head start (default in `nxs_vm`, 2026-09-28)

In the legacy scheduler a DSP activation runs until HINT or its 1,000,000
packet budget, synchronously inside the SH-4's MMIO write. After the DSP
acknowledges MAIN's command it sits in its polling loop, so ~95% of
activations ended by exhausting the budget while MAIN was frozen: 88% of the
SH-4 thread was DSP execution. (`--fast-dsp` does not help: 65,536 packets
often end before the DSP's acknowledgement, DSPINT never falls, and the DSP
is starved.)

The board (`CDJ_NXS_DSP_IDLE_SKIP=1`; `nxs_vm` sets it unless
`--no-dsp-idle-skip`) proves when the DSP spins: at a clean step (no queued
loads/stores, branch or SPLOOP) it anchors PC, registers, control registers
and delayed-control state. Any device access other than a pure GPIO read
(the idle loop polls the boot-phase inputs), an EDMA completion, a McASP
slot tick that moved data, or more than 64 distinct written RAM words voids
the anchor. When the DSP returns to the anchor PC with identical state, every
RAM word it wrote since holds its anchor value again, and no timer, PLL
countdown, SPI transfer or PSC transition can fire, the whole system state
has repeated with a period of P packets and C cycles. Running k more
periods is then exactly: packets += kP, cycles += kC,
`cdj_c6747_pll_ticks(kC)` (closed form, checked against k*C single ticks in
`tests/cstub/c6747-pll.c`), and a kC shift of each delayed-control cycle the
loop rewrites once per period. Dead load/store queue slots, which checkpoints
store verbatim, are handled by always leaving one whole period to execute
after a skip: it repeats every append of the loop at the same phase, and only
a checkpoint reads dead slots. The skip takes as many periods as fit before
the end of the activation budget and before the next McASP slot the DSP can
observe. With functional audio a slot is invisible when, probed on scratch
EDMA/McASP copies, it stages no RAM write, raises no EDMA completion and reads
no RAM word the loop transiently rewrites; invisible slots inside a skip are
then run in order at their exact packet counts (with TX capture, whose
records carry per-slot counters, every slot counts as visible).
`tests/test_dsp_idle_skip.py` compiles the board's helpers against the real
peripheral models; removing the memory, timer or delayed-control check fails
it.

Exactness on stock NXS firmware, full capture, same binary with and without
the skip, over the common prefix of a 45-second boot: all 120,717 event
records (every packets and cycles value included) and all 513 DSP checkpoints
are byte-identical; with `--functional-dsp-audio`, 118,238 events and 446
checkpoints are. The skip changes wall time only.

MAIN then reaches its GUI link before the slower simulated GUI has booted and
waits for a retry, so `--gui-head-start SECONDS` starts the GUI first (its
link connection is lazy and retried); with the skip it defaults to 2.0 s
(0.5 s measured too short, 1.0-2.0 s equivalent; the head start alone gives
nothing). It was 1.5 s until the SH-4 fast paths below made MAIN's early boot
twice as fast; 1.5 s then lost the race in 3 of 4 `--dsp-model` boots, each
costing a ~3 s retry.

Wall time from launch, `--lightweight`, same binary:

| Milestone | Full execution | Idle skip + head start |
| --- | ---: | ---: |
| Player screen (from GUI launch) | 9.9 s | 4.2 s |
| SD browser lists `TESTTONE.WAV` (`--test-track --source-key-when-ready`) | 150.5 s | 38.7 s |
| Track loaded, duration reply after ENTER, ENTER | 194.3 s | 49.4 s |

The loaded-track screen shows TRACK 01 with REMAIN 00m:10s. With
`--functional-dsp-audio` the DSP streams McASP slots continuously from early
boot and takes an EDMA completion interrupt every 32 slots (~32K packets);
each window still executes the handler plus two idle-loop periods, so the SD
listing improves less, 185 s to 152 s, and playback
advances at ~0.05-0.07x real time: replaying a mid-playback snapshot shows
only ~30% of packets in the idle loop, the rest in 3,197 distinct PCs of
audio work, and the functional model clocks one slot per 1,024 packets, so
real time needs ~90M packets/s against ~10M today. Playback is bound by raw
DSP execution speed.

## Loop-buffer transactions (2026-09-28)

Counting copies over 5M steps of a mid-playback snapshot (functional audio)
found `loop_step` moving 5.2 KB in and 5.2 KB out on every one of 571K
loop-buffer cycles, 5.9 GB against 4.2 GB for all per-packet backups
together, and SPLOOP setup and the IDLE/padding path copying the whole
6.5 KB CPU twice. Now:

* `loop_step` copies the scalar prefix and live queue entries, the loop
  schedule's scalar fields, and only for an unsealed (loading) loop its
  tags/count arrays and, when it fetches, the instruction buffer; a sealed
  step reads the committed schedule through `cdj_c674x_loop_issue_*_from`,
  which take the tags/count arrays from a separate struct. It commits queue
  slots up to the extent execute reports having written
  (`execute_transaction`), so untouched dead slots keep their bytes.
* SPLOOP setup copies and commits up to the instruction buffer, which it
  never writes.
* The IDLE/padding path executes in place (execute is transactional) and
  restores its idle count on failure.

Replay of the playback snapshot: identical trace and final checkpoint,
0.75-0.76 -> 0.71-0.72 s; boot checkpoints 1/25/250 identical; 45-second
full-capture boots against the previous binary give identical events and
DSP checkpoints (508, and 452 with functional audio).

The issue loop reuses the pre-scan's per-instruction NOP count and SPMASK
decode, the load/store overlap check is skipped with no store queued, and the
retire loop skips a load due more than two cycles out (neither its E3 read nor
its retirement can fall in the cycle) with one test: about 1.5% on the
playback replay, identical output.

## C674x interpreter performance (upstream summary)

The NXS DSP interpreter runs synchronously inside the SH-4's MMIO path, so its
speed bounds the whole emulated machine. These changes make it faster without
changing what it computes: every one keeps traces, events and DSP checkpoints
byte-identical to the previous build.

### Changes

* **Decode memo.** Row selection in the 114-row conditional dispatch table is
  a pure function of the instruction word (`also` predicates read only the
  word; see `cdj_c674x_arm_table_predicates_are_word_only`), so
  `cdj_c674x_arm_lookup` memoizes it per word in a direct-mapped table of
  relaxed 64-bit atomics (word and row in one entry).
* **In-place packet execution.** Each packet used to run against a scratch
  copy of the 3 KB CPU prefix that was copied back on success.
  `cdj_c674x_execute` now backs up the scalar prefix and live queue entries,
  runs the packet (`execute_packet`) directly on the CPU, and restores the
  backup if it fails. Queue appends go through `append_load`/`append_store`,
  which save the overwritten slot first, so a rollback leaves exactly the bytes
  a discarded copy would. `cycles` and `packets` still change only when the
  packet retires, because bus callbacks read them mid-packet. The old copy
  transaction remains behind `CDJ_C674X_COPY_TRANSACTIONS`, and
  `tests/test_c674x_transaction.py` builds it as the byte-exact reference
  (with cases where an append is followed by a rejected parallel instruction).
* **Loop-buffer transactions.** `loop_step` copied 5.2 KB in and out on every
  loop-buffer cycle. A sealed loop step now copies only the scalar prefix, live
  queue entries and the loop's scalar fields, reads the committed tags/count
  schedule through `cdj_c674x_loop_issue_*_from`, and commits queue slots up to
  the extent execute reports writing. SPLOOP setup stops short of the
  instruction buffer, and the IDLE/padding path executes in place.
* **Fetch block hook.** `cdj_c674x_set_fetch_block` lets a board return a host
  pointer to a whole 32-byte fetch block of plain memory, replacing one read
  callback per instruction word. Nothing is cached across fetches, so code
  writes stay visible. The NXS board and the replay tool register it.
* **Per-cycle and per-read trims.** Timers with both ENAMODE fields clear are
  skipped (the NXS boot never enables them); the SPI tick has an exact idle
  fast path and no per-cycle division; the PLL tick skips its division when no
  whole input period elapsed; `cdj_c674x_loop_functional_timing()` is inline;
  the NXS `dsp_read` tests L2 first (the windows are disjoint and firmware
  executes from L2); the issue loop reuses the pre-scan's NOP/SPMASK decode;
  and the queue scans skip entries that cannot act this cycle.

### Measurements (macOS, Apple silicon)

Same QEMU tree and toolchain, `upstream/main` against this branch, 60-second
full-capture NXS boots through `tools.cdj_main.nxs_vm`:

| Boot | DSP packets, upstream | DSP packets, this branch | Events / checkpoints compared |
| --- | ---: | ---: | --- |
| default | 406M | 654M | 118,238 / 446, all identical |
| `--functional-dsp-audio` | 377M | 603M | 117,165 / 417, all identical |

Standalone replay (`tools/cdj_dsp/replay.c`): boot checkpoints give identical
traces and final checkpoints; 5M steps of a mid-playback snapshot with
functional audio take 1.18-1.20 s upstream and 0.68-0.69 s here, with an
identical trace and final checkpoint.

The pytest suite gives the same result on both builds (680 passed; the same
26 checkpoint/deferred-replay tests fail on `upstream/main` in this
environment).

## SH-4 MAIN translation fast paths (2026-10-05)

`patches/qemu-sh4-tcg-fast-paths.patch`, adapted from Stijn Jacobs'
cdj-nxs2-qemu (see `patches/README.md` for the knobs). Profiled first, on
stock NXS with `--dsp-model` and the confirmed rekordbox USB
(`runs/cards/rekordbox-usb/card.img`), boot -> Not Loaded, USB mount, browse,
Enter -> loaded, with macOS `sample` on the QEMU process and the ported
`CDJ_JCPROF`.

What the profile said (TCG thread, unpatched):

* The vCPU never idles: MAIN's idle state is a busy loop, ~50 % of sampled PCs
  in `strlen` (`0x04388e48`) and `strncmp` (`0x04388500`) called from
  `0x04311438..0x0431144e`. TB lookups ran at ~35 M/s, split evenly between
  `jsr` and `rts`, with 99.6 % jump-cache hits: the cost is the
  `helper_lookup_tb_ptr` call itself (12-15 % self, 18-21 % inclusive), not
  qht misses.
* Stores into pages holding code: `notdirty_write` ->
  `tb_invalidate_phys_range_fast` was 11 % of the boot-phase thread
  (`page_collection_lock`, QTree, malloc), and 8.5 M such stores in a run
  overlapped no TB at all.
* MMIO dispatch (~4 %), `cpu_io_recompile` (<1 %), `helper_ld_fpscr` (1 %) and
  lock waits (3 %) were minor.

Throughput, MAIN alone under `-icount shift=2,sleep=off` (4 ns per
instruction, so guest seconds per host second is instruction throughput),
same firmware, card and `CDJ_NXS_DSP_MODEL=1`, 30 host seconds:

| build | guest s / host s (30 s) | host s to guest 1 s | to guest 32 s |
|---|---:|---:|---:|
| before (two runs) | 4.38, 4.62 | 1.66, 1.66 | 7.98, 8.23 |
| after (three runs) | 5.51, 5.31, 5.38* | 0.85, 0.86, 0.86 | 6.40, 6.43, 6.68 |
| after, all knobs =0 | 4.67 | 1.66 | 8.24 |
| after, `CDJ_SMC_FASTREJECT=0` | 4.56 | 1.66 | 7.97 |
| after, `CDJ_TB_XPAGE=0` / `CALLPRED=0` / `FPSCR=0` | 5.34 / 5.14 / 5.23 | 0.85-0.87 | 6.67-6.68 |

(*the third "after" run included a since-removed return-prediction
experiment.) The store fast-reject is the large term: the first guest second
of boot halves (1.66 -> 0.86 s) and the run gains ~15-20 %; call prediction,
cross-page chaining and FPSCR chaining are a few percent each, at the noise
floor of this busy host. With `CDJ_JCPROF`, `jsr` lookups fall from 428 M to
17 M per run.

Milestones, full two-board scenario (wall seconds; boot from launch to the
`Not Loaded.` screen, mount from the USB key to the USB browser, Enter ->
loaded to the command-5 duration reply), `--gui-head-start 2.0`, alternating
runs:

| build | boot | mount | browse | Enter -> loaded | QEMU CPU s (whole run) |
|---|---|---|---|---|---|
| before (4) | 7.31-7.65 (7.44) | 0.27-0.29 | 0.27-0.28 | 0.86-0.92 | 24.6-25.0 |
| after (4) | 6.81-7.86 (7.23) | 0.27-0.29 | 0.27-0.28 | 0.87-1.08 | 22.8-24.4 |

With `--dsp-model` none of these is bound by SH-4 speed: mount and load are
sub-second either way and boot is set by the simulated GUI and the link
handshake, so the gain shows as throughput, not latency. The Not Loaded and
USB-browser screens are byte-identical between builds; the loaded screen
matches outside the blinking NOW LOADING text in every run where it was not
mid-redraw (one in four for both builds).

## Persistent packet cache and fast paths (2026-10-05)

Slices E, F and I of `analysis/emulator-nxs2-qemu-evaluation.md` (in the
mods repository), after Stijn Jacobs' cdj-nxs2-qemu (`c66x_step.c` packet
cache and `fast_cycles`, `make m1`); credit in THIRD_PARTY.md. All of it is in
`emulator/qemu/cdj_c674x.c`; no board file changed. This supersedes "nothing
is cached across fetches" above.

* **Decode record.** Everything the issue loop derived from an instruction
  before touching state (pre-scan NOP/SPMASK, compact lowering, CALLP,
  DINT/RINT, unconditional class, arm row, and the compact format, now
  `compact_form()` in the issue loop's test order) is one pure function,
  `decode_instruction`. Uncached packets decode on the fly with it.
* **Packet cache.** Per thread, direct-mapped on pc/2 (16K entries), for
  direct steps: fetched packet, decode records, fast-path eligibility.
  Invalidation is by content: an entry keeps the raw 32 bytes of every fetch
  block its fetch read, and each hit re-obtains them through the board's
  fetch-block hook and compares. DSP stores, HPI uploads, EDMA, L1D/SDRAM
  remaps are all seen at the next fetch with no hook in any writer; words
  that came through the read callback are never cached.
* **Fast path** (`execute_fast`) for cached packets that write no control
  register during issue: execute in place with no prefix copy. E1 register
  results are deferred until issue has passed every check
  (`CdjC674xDefer`), so a failing packet is declined byte-for-byte and the
  transactional path reproduces the fault. Retirement can fail only on a
  queue entry, so registers, control registers and live queue entries are
  snapshotted only when an entry can act within the packet; such a fault is
  restored as the transactional rollback would leave it (retirement faults
  now report the packet's entry pc, which is what they always reported).
* **Single-instruction path** (`execute_single`), ~90% of NXS packets: one
  register-only arm (MVK/MVKH/ADD/SUB/AND/OR/XOR/CMP/shifts/bit fields/MVC
  read...), B, B reg or NOP n, when no queue entry acts in its cycles.
* `CDJ_C674X_PACKET_CACHE=0` disables all of it, `=decode` keeps only the
  cache; `cdj_c674x_set_packet_cache()` does the same for tests.

Not done: the per-hit fetch-block call (`dsp_memory_span`, ~7% of the DSP
thread in a profile) could go if the board declared mapping-stable windows; McASP-paced
batching of NOP runs needs board cooperation (each cycle ticks the board).

### Exactness

* Same QEMU binary, `CDJ_C674X_PACKET_CACHE=0` against default, 45-second
  full-capture stock NXS boots: all 214,401 events and 3,045 DSP checkpoints
  of the common prefix byte-identical; with `--functional-dsp-audio`,
  121,309 events and 529 checkpoints. Final screens identical. Against the
  pre-change binary: 234,677 events and 3,593 checkpoints identical.
* Replay (`tools/cdj_dsp/replay.c`): checkpoints 1, 25, 250 (1M steps), 400
  (5M) and the first-play snapshot (5M, strict and functional audio) give
  identical traces and final checkpoints against the pre-change core, and
  cache off against on.
* `tests/cstub/c674x-packet-cache.c`: 3,000 random programs (self-modifying
  stores, host code rewrites between steps, fetch-window withdrawal, failing
  store commits and E3 reads) run in lockstep cached+fast vs uncached;
  whole CPU and memory compared after every step (3.4M packets, 1,977
  faults). Removing validation, the register/branch/slot rollback, the
  deferral, the entry-pc fault, the single path's window or predicate, or
  its branch break each fails it. Clean under ASan/UBSan.

### Measurements (Apple silicon, loaded shared host)

| Workload | Before | After |
| --- | ---: | ---: |
| `benchmark_core` step-alu, 30M packets | 21.3 M/s | 77 M/s |
| `benchmark_core` step-mem (LDW/STW loop) | 19.1 M/s | 37 M/s |
| replay ckpt 400, 20M steps, user s | 2.20-2.21 | 1.70-1.71 |
| replay first-play, functional audio, 5M | 0.67-0.68 | 0.56-0.57 |
| 60 s boot `--no-dsp-idle-skip`, packets | 649M, 648M | 844M, 857M |
| same, `--functional-dsp-audio` | 585M | 737M |
| same binary, cache off vs on | 603M | 852M |

Boots ran pairwise at the same time on a host other agents were loading;
`--lightweight`, so no capture cost. With the core cheaper, the DSP thread
is now roughly half board work (`dsp_cycle_tick`, timers, PLL, interrupt
delivery), which bounds further core-only gains.

## Decoder cross-check (2026-10-05)

`tools/cdj_dsp/decode_crosscheck.c` sweeps an address range from a DSP
checkpoint through `cdj_c674x_fetch` and `cdj_c674x_describe` (the family the
issue loop executes); `python -m tools.cdj_dsp.decode_crosscheck` compares
it with GNU objdump (`gobjdump -D -z -EL -b binary -m tic6x`): instruction
boundaries and sizes, parallel bits, family vs mnemonic, and every compact
instruction the core rewrites to 32 bits re-disassembled from the rewritten
word. Over the NXS stage1 (0x11801da0-0x11804d80) and stage2
(0xc0000000-0xc0058320) images, matching the Ghidra program
`dsp-stage1-0x11801da0.bin`: 90,273 instructions, 1,582 rewritten compact
operands, 0 disagreements; the only fetch rejections are 818 blocks of
0xffffffff fill that objdump also calls undefined. Operands of in-place
compact forms and of 32-bit arms are outside this check.

## DSP on its own thread (`nxs_vm --dsp-thread`, opt-in, 2026-10-05)

`CDJ_NXS_DSP_THREAD=1` runs the C674x on its own host thread instead of inside
MAIN's HPI MMIO write (pattern after Stijn Jacobs' cdj-nxs2-qemu; see
THIRD_PARTY.md). The DSP runs at most one quantum (1 ms) ahead of
`QEMU_CLOCK_VIRTUAL` and gives up lag rather than catching up. MAIN, on every
HPI access, reset or boot-phase change, waits with the BQL released until the
DSP has executed `--dsp-thread-access-packets` (default 256) packets since
MAIN's previous access. The synchronisation argument is the comment above
`NxsDspThread` in `cdj2000_nxs_hpi.c`; `tests/test_dsp_thread.py` compiles that
code against a pthread shim and checks pacing, the BQL release, the lock
hand-over at MAIN's target, torn-mailbox freedom, HINT delivery and shutdown.
Threaded runs write no checkpoints and are not replay evidence.

The synchronous default is unchanged. Same 45 s boots, `develop` binary versus
this one: all 233,789 event records and 3,569 checkpoints are byte-identical
over the common prefix, and 122,123 events and 551 checkpoints with
`--functional-dsp-audio`.

Stock NXS firmware, the confirmed rekordbox USB, `--functional-dsp-audio
--lightweight`, Ethernet on an isolated `link_hub`, scenario driven through
`tools.cdj_main.dev`. Seconds of wall time, single runs, polled at about 1 s.
The host was shared with other agents' emulators (load average 10-38).

| Milestone | synchronous | `--dsp-thread` |
| --- | ---: | ---: |
| Launch to `Not Loaded.` | 13.6 | 8.2 |
| USB press to the USB root list | 134.8 | 1.2 |
| Enter on [TRACK] to the track list | 1.8 | 0.7 |
| Enter on Bang Bang to the duration reply | 19.6 | 1.15 |
| Play to 150 counter frames (1 s of track) | 45 | 19 |
| Pro DJ Link keep-alives per minute (largest gap) | 1.1 (77 s) | 23.0 (2.8 s) |

Both loaded the track with its waveform and BPM and played. The threaded run
logged no DSP fault and its DSP-paced WAV has 128,042 frames, 44,311 of them
nonzero. Playback stays bound by DSP speed (one McASP slot per 1,024 packets).

Variants that lost:

- MAIN waiting for virtual-time parity with a 150 Mpackets/s DSP clock: MAIN
  waited 99.5% of the time and had not reached `Not Loaded.` after 280 s.
- No per-access wait (`--dsp-thread-access-packets 0`): MAIN's 3,000-read
  ready poll saw about 13 DSP packets per read and the deck showed E-7010.
- 64 and 512 packets per access also worked: play to 150 frames in 27 s and
  41 s.

## McASP slots on the DSP thread's virtual clock (`--audio-clock virtual`, opt-in, 2026-10-05)

Slice G of `analysis/emulator-nxs2-qemu-evaluation.md` (mods repository), after
Stijn Jacobs' cdj-nxs2-qemu `CDJ_C6X_MHZ` (peripherals on virtual time, a
configurable DSP clock); credit in THIRD_PARTY.md.
`nxs_vm --functional-dsp-audio --dsp-thread --audio-clock virtual`
(`CDJ_NXS_DSP_AUDIO_CLOCK=virtual`) replaces "one slot per 1,024 packets" with
slot deadlines at the configured McASP1 rate (88,200 slots/s for 44.1 kHz
stereo) on the DSP thread's clock, `epoch + packets / rate`
(`--dsp-clock-mpps`, default 150, i.e. 1,700.7 packets per slot). The DSP runs
at most a quantum ahead; when it is behind, the thread gives the lag up as
before, and the slots the slip jumped fire at once (chunks are 2,048 packets in
this mode, so a burst is a few dozen slots) and are counted as underruns: time
is never stalled. `cdj_dsp_audio_clock.h` holds the exact deadline arithmetic;
`tests/test_dsp_audio_clock.py` drives the board's `thread_audio_tick` and
`dsp_thread_slip` with a host faster and ten times slower than the clock
(every slot gets 1,700-1,701 packets and none is late; slots still follow
virtual time with >= 85% underruns; stop/restart; 48 kHz reconfiguration).
The idle skip stops before each slot edge. Host audio (`--host-dsp-audio-wav`)
is allowed with it.

Exactness: the synchronous modes do not reach the new code. Same 45 s
full-capture stock boots, `develop` (6d7b58a) binary against this one: all
257,469 events and 4,209 DSP checkpoints of the common prefix byte-identical;
with `--functional-dsp-audio`, 125,453 events and 641 checkpoints.

**Result: the clock works, real-time playback does not.** Stock NXS, the
confirmed rekordbox USB, `--lightweight`, `link_hub`, same scenario as above,
shared host:

| | packets (default) | `--audio-clock virtual` |
| --- | ---: | ---: |
| USB press to the USB root list | 1.2 s | 35.6 s |
| Enter on Bang Bang to the duration reply | 0.9 s | 1.9 s |
| After PLAY | 150 frames in 17 s, then 1.15 frames/s (0.008x) | E-8302 CANNOT PLAY TRACK (300A) |
| McASP slots per virtual second | ~5,000 | 88,200.5 |
| DSP packets/s | ~5.0 M | ~3.4 M |
| slots short of their budget (underruns) | n/a | 97.8% (39 of 1,700.7 packets per slot) |
| MAIN blocked in the per-access DSP wait (whole run) | 80% | 78% |
| QEMU CPU | 113% | 117% |
| DSP-paced WAV | 415,566 frames, 69,309 nonzero | 18,373,237 frames (416.6 s), all zero |

The slot clock tracks virtual time exactly (8,820,051 slots in 100.000
virtual s), but from the moment McASP1 starts in boot the DSP never gets
ahead of it again: no pacing wait, no idle skip, 391 of 400 s slipped. The
firmware's real-time audio duty (an EDMA completion ISR every 32 slots, plus
decode once a track plays) needs more packets per second than the interpreter
delivers on that code (3.4-5 M/s; the playback-snapshot replay above put
playback demand near 63 M packets/s), so the ISRs starve the background
tasks, MAIN's stream and command traffic waits on the DSP, and the load fails.
Fewer packets per MAIN access (`--dsp-thread-access-packets 32`) gives E-7010
in boot; a 4 Mpps DSP clock (what the host sustains) gives E-7010 before
McASP even starts.

So the mode stays opt-in and is not proposed as the `--dsp-thread` default:
real time needs a DSP roughly 15-20x faster on audio code. The profile of the
starved DSP thread points at the first levers: 59% of step samples in the
transactional (non-fast) packet path, and every RAM store walking the full peripheral
probe chain in `dsp_write` before reaching L2/SDRAM (twice: check and commit).
