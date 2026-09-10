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
