# Firmware iteration analysis — 2026-09-10

The highest-return next work is to make DSP replay cheap to build and analyze, restore effective incremental QEMU builds, and measure connected responsiveness separately from execution throughput. A larger interpreter rewrite is premature until those costs are removed.

The existing performance work is committed as `c16d1cb`. This analysis uses that implementation, including RAM-first QEMU reads, packet-local fetch-header reuse, smaller transactional packet copies, loop-origin filtering, and unchanged-frame comparison avoidance. The separate untracked PCM bank evidence files were left untouched. No additional runtime optimizations were implemented during this audit.

## Measurements and scope

Current-source experiments used Apple clang 21 on arm64 macOS. The replay workload resumes `runs/dsp-post-interrupt-pipedown-5m-replay-2/final.cdjdsp` for 300,000 steps, using its exploratory timing/audio modes. It is a real firmware continuation, but **not a strict timing, full-boot, or audio validation**. Timings below exclude the Python launcher unless explicitly described as analysis. Builds and runs were repeated after the test suite ended; unrelated host activity was not controlled.

| Experiment | Baseline | Candidate | Result |
|---|---:|---:|---|
| Replay C compilation, one sample | `-O0`: 0.311 s | `-O2`: 0.850 s | Optimization increases cold compilation cost |
| Replay execution, two warm trials | 0.301–0.303 s | 0.177–0.181 s | About 40% less execution wall time |
| Replay execution, first trial | 0.446 s | 0.255 s | Reported separately from warm trials |
| Coverage analysis, one sample each | 0.582 s | 0.146 s | About 4× faster after removing step records |
| Analysis process peak RSS | 339.7 MB | 91.7 MB | About 73% lower |
| Trace size | 29.42 MB | 3.52 MB | About 88% smaller |

All six compiler-comparison runs produced byte-identical traces and final checkpoints. The compact-trace experiment filtered only standalone `step` records **after execution**; coverage output remained identical. This establishes an analysis opportunity, not an already-implemented runtime speedup or equivalence of every trace consumer. It excludes filtering time, and does not establish equivalence for other firmware workloads. The proposed producer-side mode would avoid generating these records in the first place.

The complete test suite passed with localhost socket access: **497 passed, 31 skipped in 56.15 s**. Its duration overlapped an earlier short benchmark and is a directional local baseline, not an isolated performance measurement. The initial sandboxed attempt had socket-related failures; the unrestricted rerun resolved them. No fresh full connected boot was run for this analysis.

Scripts, raw measurements, and test output are in [analysis/iteration-audit-2026-09-10](analysis/iteration-audit-2026-09-10/). Run the scripts from the repository root; they use `/tmp/cdj-iteration-audit` and the local firmware checkpoint named above. They retain no proprietary firmware in the report directory.

## Prioritized opportunities

| Priority | Change | Benefit | Scope / risk |
|---|---|---|---|
| 1 | Content-addressed replay builds plus an optimized profile | Faster repeated replay and many tests | Small–medium; include complete build identity |
| 1 | Compact standalone traces and streaming analysis | Lower latency, memory, and disk use | Medium; preserve evidence semantics |
| 1 | Copy QEMU sources only when content changes | Restore incremental build behavior | Small; low semantic risk |
| 2 | Observe executed packets directly for coverage | Remove duplicate fetches and CPU copies | Medium; precise retirement semantics required |
| 2 | Share DSP bus/device integration between replay and QEMU | Avoid divergent fixes and duplicated work | Medium–large; incremental extraction |
| 2 | Workload-specific regression commands and checkpoint catalog | Avoid unnecessary cold boots and repeated compilation | Medium; retain complete acceptance gates |
| 3 | Improve connected DSP scheduling fairness | Lower control latency and GUI stalls | Larger correctness scope; throughput alone insufficient |
| 3 | Optimize remaining loop/idle transactions | Improve sustained DSP execution | Profile first; rollback must survive |
| 4 | Dirty-page checkpoints, buffered capture, GUI transport changes | Reduce observer and storage overhead | Workload-dependent; measured lower priority |

### 1. Cache the replay build and explicitly select optimization

[replay.py](tools/cdj_dsp/replay.py:344) snapshots source/header bytes, compiles all 18 C sources in a new temporary directory, and discards the executable after every invocation. The compiler command has no optimization flag. Tests that invoke the launcher repeatedly pay compilation repeatedly: three replay tests individually took 2.21–3.98 seconds in the full suite.

Keep the source snapshot contract. Key a persistent build cache by source and header contents, compiler identity/version, flags, target/ABI, and a build-recipe version; atomically publish successful binaries and record their hash and build configuration in manifests. Include toolchain/system-header changes in invalidation or provide an explicit cache epoch. Use separate debug/sanitizer and optimized profiles. Do not reuse a binary merely because source mtimes match.

Caching and optimization should ship together: in this short workload, the extra 0.54-second compile cost exceeds the roughly 0.12-second execution saving. Simply adding `-O2` can make short cold invocations slower. Test cache hits, invalidation, concurrent builders, failed builds, and strict/functional repeat equivalence. Optimized execution needs architecture regressions and sanitizer checks because compiler optimization can expose undefined behavior.

### 2. Make detailed standalone tracing optional; stream analysis

[replay.c](tools/cdj_dsp/replay.c:1224) prints a JSON record on every standalone step. Connected-event replay already uses compact dynamic coverage rather than millions of step records. [coverage.py](tools/cdj_dsp/coverage.py:234) materializes every JSON event and traverses that list repeatedly; [replay.py](tools/cdj_dsp/replay.py:540) also reads entire trace files for coverage, hashing, and comparison.

Introduce explicit summary and detailed trace modes. Retain ordered external events, faults, stop state, coverage, mode/provenance information, and deterministic digests; retain detailed output for instruction debugging. Streaming hashing and comparison can preserve byte-exact verification without loading both files. Stream validation and keep only necessary event classes in the analyzer. Validate malformed and duplicate records and ordering as before; silently discarding validation failures is not an optimization.

The measured sample confirms that coverage does not require its step records. Audit other consumers and version the trace contract before changing defaults. This is also a storage opportunity: the local `runs/` directory occupies **52 GB**. Add explicit retention/export tooling that preserves checkpoint ancestry and referenced transcripts; do not delete old runs automatically.

### 3. Stop invalidating the QEMU build on every invocation

[build-qemu-sh4.sh](scripts/build-qemu-sh4.sh:80) unconditionally copies every board `.c` and `.h` into QEMU. Unchanged files receive new mtimes, causing Ninja to reconsider/rebuild their dependents. Compare contents before copying, and update generated build wiring only when its contents change. This is a code-established dependency problem; build wall-time savings were not measured here.

Acceptance: a second build with no edits performs no compilation; changing one C source rebuilds that object and links; changing a shared header rebuilds only actual dependents; adding/removing source files refreshes wiring correctly.

The Blackfin script already avoids repeated configure and checksum-stamps patches, but its make calls do not request parallel jobs. Add a bounded configurable job count for clean builds and track the effective flag configuration. A changed simulator patch currently requires a fresh source tree; a disposable prepared-tree workflow could make patch iteration less error-prone. This is less useful for firmware-only edits than fixing replay and incremental QEMU builds.

### 4. Remove work introduced by coverage, then address integration drift

[coverage_capture](tools/cdj_dsp/replay.c:117) copies the complete CPU and fetches the packet; the execution path then fetches it again. Both standalone and connected replay also copy `cpu` into `before`. This work remains despite the recently reduced transaction in `cdj_c674x_execute`.

Expose an optional observation hook for successfully executed source packets and the small pre-execution metadata coverage actually needs. Preserve the distinction between direct fetches, loop fetches, scheduler/idle cycles, and failed packets. Compare coverage, trace, and complete final state against the current implementation across loop loading, interrupts, compact instructions, faults, and modified instruction memory. Measure with and without coverage before assuming its copies dominate.

There is also concrete integration drift: QEMU's `dsp_read` now checks RAM first, while replay's [read_bus](tools/cdj_dsp/replay.c:322) still probes peripheral models first. Device ticking, staged EDMA writes, SPI/audio integration, and memory mapping have parallel implementations. Extract pure shared memory/device operations in small steps, keeping QEMU scheduling and host I/O in adapters. Start with mapping parity rather than a broad rewrite. Differential tests should feed equivalent transactions through both integrations and compare state, errors, and event order.

### 5. Organize development around the smallest valid replay

Existing checkpoints, provenance checks, event replay, breakpoints, and independent step/packet/cycle budgets are valuable infrastructure. Provide named commands for core semantics, connected DSP replay, GUI packet replay, and full connected acceptance. Cache compiled C harnesses and add explicit test markers; retain the full suite in acceptance/CI. Socket and UI tests need a collision audit before running them concurrently.

Maintain a small catalog of fixtures selected by semantic milestone, firmware hash, emulator version, timing/audio mode, and transcript identity. Automatic selection by newest compatible file is helpful but does not identify the closest useful milestone. Resume near a DSP problem and cold-boot only when the changed behavior can affect prior state or cross-board interaction.

DSP checkpoints cannot restore the complete player. A future whole-system warm start must capture MAIN, GUI, peripherals, timers, link queues, media overlays, and host scheduling together. Most board code has no complete migration state and the Ethernet device explicitly marks migration unsupported. QEMU `savevm` alone is not a shortcut to a correct two-board snapshot. Firmware changes before a snapshot boundary can invalidate its state even if its file format remains compatible.

### 6. Separate connected responsiveness from throughput

Historical logs, not current-binary measurements:

| Run | DSP callbacks | Sum execution | Sum reporting | Longest execution callback |
|---|---:|---:|---:|---:|
| `nxs-sic-mask-strict-120s-2` | 459 | 117.733 s | 0.885 s | 313.667 ms |
| `nxs-legacy-sync-profile-1` | 89 | 19.547 s | 0.155 s | 322.661 ms |
| `nxs-deferred-sync-profile-2` | 17,744 | 18.766 s | 0.457 s | 5.122 ms |

These callbacks perform substantial synchronous DSP work. The measurements support investigating fairness, but do not establish BQL ownership for every measured interval or a matched end-to-end speedup. Existing deferred scheduling uses 4,096-step slices and rearms one virtual nanosecond after callback completion. Short callbacks alone do not demonstrate that MAIN gets enough execution; repository evidence already reports continued GUI stalls in this mode.

Record callback p50/p95/p99/max, MAIN tick progress, GUI dropped lag, and panel-command-to-firmware-response latency on the same timeline. Correlate with QEMU sync profiles and a host CPU profile. Any new scheduling policy must preserve captured event boundaries and remain explicitly exploratory until validated. Moving the DSP to another thread creates shared-memory ordering and checkpoint consistency problems; it is not the first implementation step.

### 7. Defer expensive or weakly supported changes

Remaining full CPU transactions in `loop_step`, idle stepping, and loop setup are candidates after profiling. Unlike the narrowed packet transaction, these paths can mutate loop state; copying only a prefix would be incorrect. A smaller working-state type or staged changes could reduce copying while preserving failure rollback.

Checkpoint writing scans 32 MiB of SDRAM for nonzero pages. Dirty-page tracking can help, but needs every writer, DMA, reset, and restore path covered, plus explicit handling of pages becoming zero. The historical strict run spent less than 1% of the reported execution-plus-reporting total in reporting, so compression should not lead this workload's plan.

Event and audio capture flush on individual records. Buffered writes with explicit checkpoint-boundary flushes could help capture-heavy runs, but require reliable finalization: the launcher terminates child processes during teardown. Preserve sequence numbers and error propagation, and do not claim `fflush` provides durable storage.

The GUI already skips unchanged publications and the viewer checks mtime before decoding. Prior documentation identifies wall-clock waits and burst lag as separate from interpreter speed. Reprofile current Blackfin rendering before changing framebuffer transport or adding a JIT. Higher MIPS cannot eliminate genuine firmware waits in a wall-clock-paced connected system. Deterministic fast-forward would require coordinated virtual time across both boards and their link.

## Implementation sequence and acceptance

1. Add a repeatable phase benchmark: preparation, compile/cache, execution, reporting, coverage, verification, and total wall time. Record build and input hashes, RSS, trace size, and fidelity modes.
2. Fix content-aware QEMU mirroring; add optimized cached replay builds. Verify strict and exploratory fixtures separately.
3. Add compact tracing and streaming analysis, then eliminate coverage's duplicate fetch/copy work. Preserve byte-exact checks within each mode and state/coverage equivalence across modes.
4. Extract shared DSP bus operations and publish milestone-oriented replay commands. Measure the resulting edit-to-verdict time.
5. Reprofile connected boot and interaction on the committed optimized binaries. Pursue scheduler fairness and remaining interpreter hotspots using those results.

Keep a workload panel of strict failure reproduction, exploratory DSP continuation, interrupt/loop replay, cold connected startup, and interactive panel response. A change is useful only if it reduces the relevant edit-to-verdict or response time while preserving its declared correctness contract. Microbenchmark gains alone should not be promoted to whole-emulator speed claims.

## Whole-emulator follow-up: SH-4, Blackfin, and their link

A fresh 35-second connected run, `runs/whole-emulator-audit-20260910`, was captured after the initial analysis. Both installed binaries were sampled for three seconds with macOS `/usr/bin/sample`, near the end of the run. The QEMU mirror of the three recently optimized DSP sources matched the repository. Binary hashes, run statistics, the Blackfin dispatcher prologue, and both profiles are saved beside the earlier measurements.

The launcher used its default NXS configuration, no attached media, and five-second frame captures. GUI exited normally at its wall budget and produced a frame. This was a performance diagnostic: no panel interaction or clean-player-screen acceptance was performed. Samples include sleeping threads, are affected by profiler overhead, and cover one short startup interval. Percentages below use the relevant thread's sample count, not a sum across all process threads.

### SH-4: its device callbacks are the immediate execution bottleneck

In the QEMU CPU thread, **2,261 of 2,286 samples** were inside an MMIO write reached from translated guest execution. Within that subtree, 2,157 samples were at the sampled `execute_dsp` call site. Across the full run, 205 DSP callbacks accounted for **33.91 seconds of execution**, **0.396 seconds of reporting**, and a longest execution callback of **231.9 ms**.

This is stronger evidence than simply seeing a busy QEMU process: the SH-4 thread is spending much of this interval running the DSP inside a device write. Optimizing the SH-4 translator itself would have little opportunity in this sample. The critical cross-board sequence is SH-4 MMIO → DSP work → MMIO return → SH-4 resumes. Reducing DSP transaction/copy cost benefits MAIN directly; changing when it yields requires event-order validation.

The profile still contains substantial memory movement and clearing inside DSP execution, despite the committed prefix-copy optimization. Two `cdj_c674x_execute` copy call sites contributed 339 and 301 leaf samples in `_platform_memmove`; a clearing call site contributed 274 in `_platform_memset`. These counts overlap their parent execution samples and must not be added to them. This raises profiling of transactional state above speculative SH-4 instruction optimization. EDMA/McASP transaction copies and repeated peripheral dispatch are additional, smaller visible costs.

SH-4-specific opportunities remain:

* **Fixed-source DMA fills.** `cdj_dmac_start` intentionally handles fixed/decrementing address modes one transfer unit at a time. NXS uses a fixed zero word to clear BSS. Add a proven ordinary-RAM fill path that repeats the source unit in bounded chunks. Keep MMIO, overlapping/aliasing regions, address modes, faults, and completion/interrupt behavior on the existing path unless equivalence is established. The code already documents corruption caused by treating a fixed source as ordinary `memcpy`.
* **HPI transfers.** These currently issue a read and write through QEMU's address space for each word. Mapping the ordinary-RAM side in safe chunks could avoid part of the dispatch cost while preserving each HPI register access, address increment, and event. Batching both ends indiscriminately would change device behavior. This is a boot/upload candidate, not a demonstrated steady-state hotspot in the new sample.
* **Panel I/O.** `cdj_input_poll` calls nonblocking accept/recv during panel exchanges. A QEMU readiness callback could fill a host command queue, with commands still applied at the current firmware boundary. Measure exchange frequency and syscall cost first; this was not a leading sampled cost. Bound each drain to prevent a busy client monopolizing device work.
* **Incremental builds.** Content-aware mirroring remains the simplest SH-4 development-loop improvement. Profile TCG translation, MMIO exits, and guest-PC distribution again after the synchronous DSP cost is reduced. No evidence here justifies a new SH-4 execution engine.

### Blackfin: several independent, actionable opportunities

The new run's last stats line reported **1.089 billion instructions**, **1.238 million events**, **628,213 DMA callbacks**, **2,099 scanned frames**, **10 published frames**, **70.424 million spin-path entries**, and **8.94 seconds parked**. Guest time tracked wall time at 400 MHz with zero reported dropped lag. These counters are not CPU percentages. In particular, the spin count records entries to the short-wait path; it does not prove every counted instruction was useless.

**First: move the cold LZSS accelerator out of the instruction dispatcher.** `interp_insn_bfin` contains a 4 KiB local decompression window inside its rarely entered acceleration branch. Inspection of the installed ARM64 binary shows that its entry unconditionally calls `___chkstk_darwin` and reserves `0x1040` bytes before testing the acceleration PC. Stack probing alone was the leaf in **49 of 2,384 Blackfin samples**. The NXS launcher does not enable this accelerator, yet it still pays that function prologue. Extract the accelerator into a separate non-inlined cold helper so ordinary instructions use a small stack frame. This is a particularly clean host optimization: preserve all acceleration checks and guest effects, then compare instruction-counted packet replay, final frames, and registers. Do not disable stack protection globally to obtain the saving.

**Second: reduce work while parked near an event deadline.** `bfin_wall_sync` sleeps only when the remaining wait exceeds 300 microseconds, and wakes 200 microseconds early. Otherwise it returns to the outer instruction loop and increments `spins`. This is consistent with the large spin count and the sample's clock/interpreter activity. For a verified self-loop or IDLE, remain in a host wait/event-delivery path until an interrupt or other state-changing event requires execution, rather than repeatedly decoding the parked instruction. Preserve hardware-loop exclusions, PC changes, PLL wake-up, link readiness, and interrupt semantics. Measure wake latency and power/CPU use together; reducing wakeups by delaying events would not be a valid equivalent optimization.

**Third: optimize host clock reads without changing the time contract.** Blackfin queries `CLOCK_MONOTONIC` from its timing loop. The sample includes clock conversion through `_mach_boottime_usec` and `gettimeofday`, plus `mach_absolute_time` leaves. A cheaper monotonic host-clock implementation or fewer redundant reads could help. Benchmark candidate implementations and verify suspend/resume, monotonicity, unit conversion, and event deadlines; do not blindly substitute wall time or a clock with different sleep semantics.

**Fourth: keep SPORT capture files open.** `bfin_sport_link_dump` performs getenv/open/write/close for each captured receive, and the NXS launcher enables it by default. The profile contains **38 leaf samples in file open under SPORT receive**, with further close/write activity. Keep the capture stream open and preserve record order, errors, and explicit flush/close boundaries. Maintain capture semantics rather than suppressing repeated firmware-visible receives. This reduces observer overhead and is simpler than replacing the link transport.

**Fifth: eliminate disabled trace-helper calls.** Formatting is already gated inside `fmtconst_str`, but that function still appeared in 22 leaf samples with ordinary tracing disabled. Gate at the call site or make the disabled path inline, preserving enabled trace behavior. Register-name/trace helpers also appear and deserve the same audit. After these changes, reprofile decoder dispatch and load/store/MMU work. A persistent predecode cache is a later candidate requiring code-write, DMA, flash, mapping, and privilege invalidation; it is a much larger correctness change.

The sample had **511 of 2,384 samples in `select`**, which represents waiting, not interpreter CPU cost. That sleep fraction must be excluded before claiming CPU-only speedup estimates. The numeric sample shares above establish where to investigate, not guaranteed savings.

### Display, DMA events, and cross-board timing

Do not prioritize framebuffer file transport for this workload: 2,099 scans produced only 10 publications, and publication did not lead the profile. The prior changed-line conversion and committed unchanged-frame comparison optimization are already doing useful work. PPI still schedules scanlines back-to-back before waiting for the remainder of its 60 Hz frame period. This explains a significant baseline of DMA/event activity without proving an event storm bug. Safe batching must stop at observable DMA status, interrupts, descriptor changes, or guest memory changes. Rendering once per frame from a final buffer could change tearing and firmware-visible ordering.

SPORT's empty-receive retry is already 1 ms in wall-clock mode; increasing it merely to lower CPU load can delay firmware handshakes. The code documents a prior receive-latency experiment that preserved startup but harmed later responsiveness. Similarly, dropping repeated live-link records or switching to the diagnostic fresh-only mode is a behavior change, not a transparent performance optimization.

The largest future firmware-iteration improvement would be a trustworthy coordinated checkpoint of both boards and their link, followed by deterministic replay to the changed behavior. Existing DSP-only checkpoints cannot deliver that. Before that investment, use isolated GUI packet replay for display-only edits, DSP replay for core edits, and connected runs for MAIN/GUI protocol or timing changes. Any acceleration tied to stock addresses must remain guarded when firmware is modified.

### Revised order after profiling both boards

1. Keep the replay-cache, compact-analysis, and incremental-build work at the front of the development-loop plan.
2. Add Blackfin cold-helper extraction, persistent SPORT capture, and disabled-trace call elimination as small host-only candidates.
3. Measure Blackfin parked-loop/clock improvements and DSP transactional-copy improvements independently, then together in connected runs.
4. Validate SH-4 progress and panel response before changing cooperative DSP scheduling. Preserve timing-mode labels and external-event provenance.
5. Investigate RAM-only DMA batching for boot/media workloads; consider decoder caches and coordinated snapshots only after the smaller changes establish a new baseline.

No further emulator source changes were made during this follow-up. It completes the analysis across execution, device callbacks, timing, display, inter-board I/O, build/test overhead, and state reuse; the proposed savings beyond the recorded experiments still need implementation and A/B validation.

## Implementation ledger

Baseline for this series: `c16d1cb`. Preserved source is under
`/tmp/cdj-perf-baseline`; original installed binaries are under
`/tmp/cdj-perf-baseline-bin`. Relocated QEMU/Blackfin build directories were
regenerated before measurement; this repair is excluded from optimization times.
Each row below measures its own workload; percentages must not be added.

### Iteration 01 — content-aware QEMU source mirroring

Changed `scripts/build-qemu-sh4.sh` to compare source bytes before copying.
Three unchanged builds before: **1.646, 1.330, 1.250 s**, each compiling **32**
objects. After: **0.503, 0.477, 0.487 s**, each compiling **zero** objects.
Median edit-free build time improved **2.73× (63.4% less time)**. QEMU's own
version-header generator still runs; this is not a claim of zero Ninja work.

Validation: the real build succeeded in every trial. Deliberately changing the
mirrored `cdj_c6747_pll.c` caused exactly one compilation when the script restored
it; changing mirrored `cdj_c674x.h` caused three dependent compilations. Both
files were restored byte-for-byte from repository source. No guest behavior
changed. Raw results: `analysis/iterations/01-build-*.json`.

### Iteration 02 — optimized, content-addressed replay builds

Replay now defaults to `-O2` and caches binaries using source/header contents,
compiler identity/version, target, build flags, and relevant environment hashes.
Compiler-discovered dependencies include system headers. Cached executables are
hash-checked and copied into the run's private snapshot before use; atomic cache
publication supports concurrent builders. `--build-profile debug` retains `-O0`,
`--no-build-cache` forces compilation, and `CDJ_REPLAY_CACHE` selects the cache.
`CDJ_REPLAY_CACHE_EPOCH` provides explicit invalidation after external toolchain
or linker changes not reflected in compiler/header identity. This is a trusted
local development cache, not a hermetic build or remote artifact trust system.

The complete 300,000-step exploratory continuation with `--verify-repeat`
(including launcher, two executions, coverage and gates) took **3.115, 2.854,
2.883 s** at baseline. Candidate: **3.484 s cold**, then **2.627, 2.574 s warm**.
Comparing warm medians gives **1.10× (9.4% less time)**. Cold compilation is
slower; large detailed-trace analysis still dominates this short workload.
All six runs have identical trace, final-checkpoint, and semantic coverage hashes.
Eight focused tests passed, including real compiler cache hit, external-header
invalidation, profile separation, corrupted binary rejection and failed-build
nonpublication. The existing replay fault/budget/repeat tests also passed.

Measurements explicitly set `DEVELOPER_DIR=/Library/Developer/CommandLineTools`
for both versions (Apple clang 21.0.0 / clang-2100.3.34.2): the default host
selection mixed Xcode's linker with a newer SDK and could not link. That failed
attempt is excluded. Reproduce with `tools/cdj_dsp/benchmark_replay.py`; raw
results are `analysis/iterations/02-replay-{before,after}.json`.

### Iteration 03 — compact standalone replay diagnostics

Added `--trace-mode compact`; detailed remains the default. Compact mode omits
only standalone `step` records at their producer, preserving external events,
coverage, PCM observations, faults, budgets, and stop state. The launcher clears
inherited compact-mode environment state and records the chosen mode in both
manifest and gate. Repeat trace equality is explicitly scoped to that mode.

Same workload and compiler as iteration 02: **2.125 s cold**, then **1.202,
1.207 s warm**, versus iteration 02's warm **2.627, 2.574 s**. Warm median
improvement for this iteration: **2.16× (53.7% less time)**. Relative to the
starting warm baseline: **2.38× (58.0% less time)** for the complete replay,
repeat, analysis, and verification command. Trace size fell from **29,416,880
to 3,521,306 bytes** per execution (88.0% smaller).

Final checkpoint and semantic coverage hashes match every prior measured run;
compact traces match their own repeats. A focused test compares detailed events
minus `step` against compact events, exact final state, and semantic coverage,
including a fail-closed instruction fault and inherited environment override.
All **33 replay/checkpoint/deferred tests passed**. Raw results:
`analysis/iterations/03-replay-compact.json`. These are workflow gains from
reduced diagnostics, not an interpreter throughput or connected-boot claim.

### Iteration 04 — Blackfin cold LZSS helper

Patch 09 moves the optional 4 KiB decompressor window into a non-inlined helper.
The installed ARM64 dispatch prologue drops from a 0x1040-byte local allocation
plus saved registers and stack probing to a **96-byte frame with no probe**.
The accelerator's accepted-bank behavior and declined-path probe ordering remain
unchanged. The simulator was rebuilt and installed.

Three alternating fixed-tick NXS startup-slice trials before: **7.102, 7.040,
6.914 s**; after: **6.959, 6.900, 6.852 s**. Median improvement: **1.020×
(2.0% less time)**. This is a small observed gain, with overlapping ranges;
no larger whole-player speedup is inferred. Every run stopped at exactly
**60,031,090 ticks / 56,950,784 instructions**, with the same framebuffer hash.
No TX file was emitted during this early startup slice, so TX equivalence is not
claimed. The earlier 300-million-tick exploratory sample is excluded from this
matched comparison.

Six focused tests passed, including actual-helper synthetic decompression,
unsupported-bank decline, framebuffer publication, and SIC mask forwarding.
The optional accelerator is not enabled in the NXS benchmark; its enabled path
is covered by the synthetic test. Reproduce using `tools/cdj_gui/benchmark_sim.py`
with `--ticks 60000000`; it uses instruction-counted time and a captured SPRX
packet fixture, not the connected wall-clock timing mode. Binary/input hashes,
all trials, and prologue are in `analysis/iterations/04-blackfin-*`.

### Iteration 05 — persistent SPORT capture descriptor

Patch 10 opens the run-local receive transcript once and closes it at normal
exit. Every complete SPRX record is still flushed, preserving visibility for
live readers and records written before abrupt process termination. This
removes open/close churn, not record capture. The configured path is fixed for
the process lifetime; external log rotation is not supported by this mode.
The rebuilt simulator is installed.

Three alternating synthetic capture trials (100,000 64-byte records): before
**1.992, 1.933, 1.905 s**; after **0.272, 0.161, 0.157 s**. Median capture-path
speedup **11.97× (91.6% less time)**. All six files were **7,200,000 bytes**
with identical SHA-256. These figures apply only to capture I/O; they are not a
12× firmware or whole-emulator claim. The fixed-tick benchmark from iteration
04 emits no SPORT TX/receive capture in its early slice and cannot measure this
change meaningfully.

Both focused tests passed: the actual compiled helper opens only once,
records are visible before close, and normal/abrupt exits produce identical
bytes. Benchmark code: `tools/cdj_gui/benchmark_capture.py`; raw results:
`analysis/iterations/05-sport-capture.json`.

### Iteration 06 — reject non-device writes before transaction copies

Added side-effect-free EDMA and McASP write-window predicates, sharing each
model's existing address locator. Both QEMU and standalone replay now reject
non-device addresses before copying transactional EDMA/McASP state. Addresses
inside those windows still use the complete register/value/state validation
and commit paths. This does not skip writes or change the device schedule.

Three alternating native replay trials (one million successful steps, compact
trace, -O2): before **0.590, 0.433, 0.425 s**; after **0.489, 0.380, 0.382 s**.
Median speedup: **1.132× (11.7% less time)**. All six runs ended at
**34,100,114 packets / 78,352,698 cycles**, with identical traces and final
checkpoints and no fault. This isolates native execution plus trace/checkpoint
output; Python launch/build/coverage costs are excluded.

All **51 focused CPU/device/replay/checkpoint/deferred tests passed**. The new
mapping test checks every address around both bus windows, all widths 0..8,
RAM aliases, and that accepted writes are never filtered out. QEMU rebuilt
successfully with the same guards. Reproducible script and raw results:
`analysis/iterations/06-benchmark-native.py` and `06-native-guard.json`.

The interruption cleared the earlier /tmp benchmark snapshots. They were
recreated from immutable commits under ignored `build/performance`: baseline
`c16d1cb`, pre-iteration-06 `4ae1778`. Both native comparison binaries were
rebuilt with the same pinned Command Line Tools compiler. No result depends on
an executable recovered from an uncertain temporary directory.

### Final combined comparison and validation

All six optimizations were benchmarked and committed separately:

| Iteration | Commit | Measured scope | Median result |
| --- | --- | --- | --- |
| 01 | `5a757e0` | Unchanged QEMU build | 2.73× faster |
| 02 | `84fa8ff` | Warm detailed replay workflow | 1.10× faster |
| 03 | `cec7587` | Compact replay versus iteration 02 | 2.16× faster |
| 04 | `89e427b` | Blackfin fixed-tick startup slice | 1.020× faster; overlapping ranges |
| 05 | `4ae1778` | Synthetic SPORT capture I/O | 11.97× faster |
| 06 | `13cef8f` | Native DSP replay versus iteration 05 | 1.132× faster |

The final complete replay workflow is **2.58× faster: 3.080 s → 1.193 s,
61.3% less elapsed time**, comparing baseline `c16d1cb` with `13cef8f`.
This fresh comparison alternates five baseline/final pairs, with no concurrent
builds or tests. The reported medians use trials 1–4, after the cold trial.
It measures 300,000 exploratory steps, repeat verification, coverage analysis,
and gates. Baseline uses its original detailed trace and build behavior;
final uses the optimized cached build and opt-in compact trace. All ten runs
passed their gates and produced identical final checkpoints and semantic
coverage. Traces match exactly within each trace mode.

The first final run, including a cold replay-cache build, took **2.157 s**;
the first baseline run took **3.158 s**. These single cold observations are
reported separately from repeated warm medians. The final four runs confirmed
cache hits. Raw trials, hashes, compiler identity, and input provenance are in
`analysis/iterations/final-comparison.json`; reproduce with
`analysis/iterations/final-benchmark.py` after preparing its baseline archive.
Builds used `DEVELOPER_DIR=/Library/Developer/CommandLineTools` consistently.

For faster routine replay, add `--trace-mode compact --verify-repeat` to the
usual replay command. Detailed tracing remains the default for instruction
debugging; optimized builds and their verified cache are automatic. These gains
are workload-specific and must not be multiplied or added into a whole-player
speedup. The baseline already contains the initial optimizations in `c16d1cb`,
so this comparison does not quantify improvements preceding that commit.

Final validation: **504 tests passed, 31 skipped in 50.76 s**. The rebuilt SH4
QEMU and Blackfin simulator completed a 35-second connected NXS smoke run:
GUI exit 0, no launcher timeout, framebuffer present, and no input artifact
changes. Blackfin reported 957,743,104 instructions, 2,099 frames processed,
12 published frames, and zero dropped milliseconds at the last sample. MAIN
remained running until normal launcher teardown. This bounded smoke does not
establish complete boot or audio correctness, and supplies no boot-speed claim.
Test output and smoke provenance are saved in `analysis/iterations/final-tests.txt`
and `final-smoke.json`; firmware and full run artifacts remain ignored.

Scheduler, spin-loop clock, and asynchronous DSP changes remain deferred because
they alter timing contracts and require separate correctness evidence. The
current improvements preserve those contracts while shortening build and replay
iteration time.
