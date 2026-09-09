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
