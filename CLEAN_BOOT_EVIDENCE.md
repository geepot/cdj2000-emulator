# NXS clean startup evidence

2026-09-09. This addresses reaching the genuine normal player/UTILITY screens
without the E-7010 DSP or E-7206 authentication banners. It does not establish
audio, storage/media loading, every control, complete ISA coverage, cryptographic
authentication, or cycle accuracy.

## Changes and independent sources

- NXS now maps SH7764 IIC at P4 `0xffe70000` and Area7 `0x1ff70000`.
- Single-buffer address/data/STOP sequencing follows Renesas SH7764 manual
  R01UH0360EJ0300 sections 16.3.5/6 and 16.6.1/2 (printed 586-588).
  MDE means TX buffer-to-shifter load; MDT means completed transmission.
  RX holds SCL until MDR is cleared. Reads never advance virtual time;
  MMIO catches up already-due timer events before observing device state.
- Apple independently documents read-only 2.0C identity registers 0/1 as
  `05`/`01`: [HomeKitADK register map](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPMFiHWAuth%2BTypes.h).
  Its [POSIX driver](https://github.com/apple/HomeKitADK/blob/master/PAL/POSIX/HAPPlatformMFiHWAuth.c)
  documents address selection and separate register-ID write/read transactions.
  The NXS firmware requests address byte `0x20`/`0x21` (7-bit `0x10`).
- Only these two one-byte identity registers are modeled. Unsupported access
  fails closed. No certificate, key, signature, self-test pass, fabricated
  authentication result, firmware patch or error suppression is supplied.

Timing is functional/event-level: nine SCL periods per address/byte and one
per STOP, using schematic Pck 53.950MHz. The parts-list alternative 53.930MHz
and exact START/STOP, pin-level, IRQ, double-buffer and arbitration behavior
remain unresolved/unmodeled. The implementation does not inherit TMU clock
acceleration. This is not a cycle-accuracy claim.

## First corrected connected run

```
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-iic-identity-connected-3 \
  --seconds 120 --frame-interval 5 --port 6080 \
  --qemu build/qemu/build/qemu-system-sh4
```

Fresh MAIN/Blackfin startup, strict legacy DSP, no functional DSP switches.
QEMU SHA256 `39c45273f8ab32056fae75b58210e5c07f0163caaea181ec82b6cb8faae7157f`.
Other binary/firmware hashes are unchanged from DSP_BOOT_MILESTONE_AUDIT.md;
all five are captured in run.json and unchanged at exit.

MAIN log records actual identity reads `0 -> 5`, `1 -> 1`. DSP handshake
ready/clear/ack is 104211/104213/104215. Transcript SHA256:
`558e412b33e71dff8d27b6848ce12e0fbb6687473a6f9494b8dcb8bf1afe0db1`.
There are 448 budget boundaries, 13 HINT yields and no DSP fault; terminal
state 448,099,500 packets / 819,938,861 cycles. Packet counts are not accuracy.

Normal player observed at 40.0311s, hash
`9ea6b13c57b8b37eb842d2811e0519f0499a7de86e9d8025bd48dbbbd9f0978b`.
MENU down/up Unix times 1788973115.539775 / 1788973125.674906 produce UTILITY
at 80.0311s, retained through 115.0877s, hash
`cf4226e6abcd0c51462a472cec404485cfe64c7317bbf7aab2291c080fcf2e68`.
Both states were visually inspected without either error banner. Five-second
sampling leaves unobserved intervals; no continuous pixel-level assertion.
GUI exit 0, no timeout. Host regression tests ran concurrently, so this is
not a performance benchmark. Full suite: 408 passed, 27 optional skips.

Earlier attempts are retained honestly: run 1 predates RX/TX refinements and
still displays E-7206; trace-2 stops unsupported during a retry caused by
late MDE/timer delivery. Neither is successful-boot evidence.

## Repeat gate

`runs/nxs-iic-identity-connected-4` completes 120 seconds, GUI exit 0, no
timeout; all five input hashes match run 3 and remain unchanged. Normal player
first observed at 40.0865s. MENU down/up at Unix 1788973265.001927 /
1788973275.2038631 opens UTILITY; encoder +1 at 1788973300.219261 moves the
selection to EJECT/LOAD LOCK by 110.08s. Final inspected frame has no banner,
SHA256 `59644051a33599fbf99fd0f2206c6f49c28b4590d575cc24ca2fc0a11a06bc2c`.
DSP records 446 budget boundaries and 13 HINT yields, no faults; terminal
446,099,500 packets / 816,358,705 cycles. Ready/clear/ack sequence unchanged.
Transcript SHA256:
`e0d11549f5d1cbb8f0b95c9d09fe11e95ad31f6898fba6e765d3ec7540acfc54`.
This verifies reproducible clean startup and basic late interaction, not a
fully functioning player. Independent wrapper review found no fabricated
startup success; it identified an unverified FSB-change-during-active-RX edge
outside this firmware path. Do not claim general IIC receive support until
that edge is guarded or reference-tested. No media/audio claim is established.
