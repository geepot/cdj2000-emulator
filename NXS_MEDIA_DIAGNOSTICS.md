# NXS media diagnostics

Track loading and audio remain unverified. The generated FAT32/WAV fixture is
not a rekordbox export, and attaching it is not firmware mount success.

## Delayed SD insertion, 2026-09-09

`runs/nxs-sd-late-insert-1` ran for 180 seconds with:

```
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-sd-late-insert-1 \
  --seconds 180 --frame-interval 5 --port 6980 \
  --sd runs/nxs-test-media-1/test-track.img --sd-insert-seconds 110 --trace-media
```

QEMU SHA-256:
`34e9503f7964b0cd62296d55983af6f8c5a4b2fabbdf0138495eeb2558339378`.
The run manifest reports no changed input hashes, including the protected SD
image. MAIN records genuine IIC identity reads 0/1 returning 5/1. The inspected
150-second frame displays an empty Wait browser without an error banner. No
track metadata or playback is established.

The SD trace contains reset writes at PC `04239e62/64`, followed by INFO1
reads at `04238b74` changing from `0x20` (empty) to `0` (present). No command
register writes occur. Delaying insertion therefore does not solve this stall;
the driver did observe the empty-to-present transition.

Read-only local SH4 analysis of genuine MAIN identifies detection function
`04238b64`: presence requires INFO1 bit 5 clear and helper `04238922` equal
to 1. This confirms the existing active-low interpretation. The readiness
helper also consults firmware state via `042a0efa(0,2)` and `042a0348`; their
runtime values and the mount task remain to be investigated. Do not force
these RAM fields or enable the legacy `CDJ_SD_MEDIA_STATE` write.

Two initial diagnostic socket commands lacked their newline and timed out;
this was caller misuse, not a panel regression. `PanelControl.hold('sd', True)`
was acknowledged normally and opened the browser. The run ended before manual
release, so this is not a complete press/release interaction test. Teardown
destroyed the emulated input state.

## USB comparison

`runs/nxs-usb-track-trace-1` used the same protected fixture and QEMU binary,
`--usb`, `--trace-media`, `--qemu-sync-profile`, and a 180-second duration.
Unlike SD, USB progressed through bus reset (106.877 to 111.749 virtual
seconds), address assignment and bulk transport. INQUIRY returned the QEMU
disk identity; TEST UNIT READY returned a failed CSW followed by REQUEST SENSE
reporting unit attention, ASC 0x29 (reset). This is observed initialization
progress, not proof of a permanent failure or a successful filesystem mount.
No READ(10) command was recorded before this run ended.

Stopped 128-MiB MAIN RAM captures at approximately 90 and 165 seconds have
identical readiness fields: `04cf2180=0`, `04cf222c=1`, `04cf2994=0`.
The SD-present latch is zero (expected for this USB-only run), device pointer
`049832f0` is `04951c74`, callback at device+0x1c is zero and flags at +0x66
are 1. These are snapshot observations, not proof the gate never clears.
Both GUI message pools are fully free in the first snapshot, receiver-ready
is 1, and the previous pool-deadlock signature is absent. USB down and up were
acknowledged and the panel state reported no held buttons.

The lock profiler measures 170.94540 seconds of I/O-thread BQL waiting at
`util/main-loop.c:313`; MAIN waits at interrupt, MMIO-read and MMIO-write
sites total approximately 0.62829 seconds. This is consistent with the known
synchronous DSP blocking but does not establish sole causation. Profiling and
two stop/save/resume RAM captures add observer effects. A longer same-binary
run is needed to distinguish slow initialization from a persistent stall.

## Longer USB run: progress and a late DSP fault

`runs/nxs-usb-track-long-1` extends the same configuration to 600 seconds.
USB retries TEST UNIT READY at 244.001 virtual seconds, then reads capacity
and issues READ(10) for sector zero at 244.819. Numerous FAT sectors follow.
This supersedes any interpretation of the 180-second run as a permanently
stalled USB controller. The generated image has an unknown FSInfo free count;
whether that causes the observed sequential FAT scan is not yet established.

However, around virtual time 368.274 the strict DSP interpreter stops:

- Packet 1,301,099,502, cycle 2,346,859,933, PC `008001e8`.
- Reason: `delayed-result write conflict`.
- Checkpoint `00000000000000001342.cdjdsp`, event sequence 151389.

The previous budget checkpoint is at packet 1,301,099,500, PC `c004cbb8`.
Preserve this transcript/checkpoint pair for deterministic diagnosis. Subsequent
USB progress and faster RTOS ticks occur after the DSP fault; they are not
clean end-to-end execution evidence. The fault's relation to the browser wait
is not established.

The inspected 460-second frame displays LINK after the contact currently
labeled USB (`19/02`) was pressed and released. A subsequent `19/04` probe
shows Wait. This raises a source-selection/protocol question; it does not yet
prove a particular alternative mapping. Both probes use actual panel input.
There is still no verified TESTTONE listing, track load or playback.

Separately, static NXS panel analysis and RAM snapshots identify the SD lid
gate: neutral raw byte 17 bit 2 is decoded as lid open and leaves `04cf222c=1`.
The closed-contact state needs a connected `down 17 04` test before changing
the launch defaults or declaring an SD fix. No guest RAM fields were forced.
