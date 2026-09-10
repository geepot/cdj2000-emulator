# Connected profile reconnaissance

The 1 ms macOS samples cover one short startup interval. The SH-4 CPU/TCG
thread has 1,935 samples; 1,908 are in the MMIO write path and 1,799 reach
`execute_dsp` at one sampled call site (other call sites also appear). The
Blackfin main thread has 2,387 samples: 550 are in `select` (waiting), 1,256
reach `sim_engine_run`, and 166 are in `mach_absolute_time`. These are stack
samples, so `execute_dsp` children such as `cdj_c674x_execute`/`memmove`/`memset`
overlap their parents and must not be added to the parent counts.

The successful 70-second connected run is
`runs/optimization-11-profile-2`. It produced 451 DSP callbacks totaling
67.804 seconds of execution and 0.933 seconds of reporting. Its final DSP
progress was 438,099,500 packets and 802,035,289 cycles. The final Blackfin
stats line is also recorded there (2,210,136,064 instructions,
27,996,438,000 ticks, 20.78 parked seconds, 144,805,933 spins). The input
hashes, result, and sample denominators are recorded in `profile-summary.json`.
The earlier
`runs/optimization-11-profile` attempt was not execution: QEMU could not bind
the serial TCP socket on 127.0.0.1:6180.

The idle idea remains deferred. `bfin_wall_sync` needs its initial clock read
for guest-time catch-up, a fresh read after event delivery because handlers can
move PC, and a post-wait read to account for the time actually slept. A host
park loop would need explicit event/interrupt/link wake state while preserving
hardware-loop exclusions, PLL `bfin_idle_wake_pending`, and PC-change ordering.
Caching or coalescing these reads is therefore not provably equivalent without
a bounded wake/event harness; changing wait thresholds could introduce late
firmware-visible events.

SH-4 fixed-source DMA and HPI batching are deferred. The sample does not show
them as leading hotspots. Fixed-source ordinary-RAM fill may be revisited with
strict guards for address modes, aliases, overlap, faults, and completion
interrupts. HPI batching can only map/chunk the ordinary-RAM side; each HPI
register access must retain address increment, visibility, event order, and
DSP interrupt behavior. Existing HPI/DMAC tests need multiword coverage before
such a change.
