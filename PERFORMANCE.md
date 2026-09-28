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
