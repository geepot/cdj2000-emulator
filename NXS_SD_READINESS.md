# NXS SD readiness investigation

Read-only firmware analysis, 2026-09-09. MAIN image SHA256
`02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85`.
No RAM values are changed or fabricated by this investigation.

`0x04238922` returns true if any of these conditions hold:

- u32 at `0x04cf2180` equals 3;
- u32 at `0x04cf2994` is nonzero;
- the media-mode function `0x042a0348` returns zero.

The media-mode function returns 1 when u32 `0x04cf2180` is 4 or 5;
otherwise it returns u32 `0x04cf222c`. The first state address is both
`0x04cf0c64 + 0x151c` and the base named by literal `0x042a0424`.
The table read is `0x042a0efa(0,2)`, yielding
`0x04cf2854 + 2*0xa0 = 0x04cf2994`.

SH disassembly confirms this logic. Ghidra's FPSCR argument in the decompiled
call to media-mode is an analysis artifact; the callee uses the global above.

`0x04238b64` calls readiness, then reads SDHI INFO1 at `0xffe4001c`.
Bit5 is active-low card detect. If present AND ready, and the latch at
`0x049832ec` is not already 1, it sets that latch, sets bit0x40 of byte
`device + 0x66`, and invokes the optional callback at `device + 0x1c` with
argument4. The device pointer is u32 `0x049832f0`. If the condition becomes
false while the latch was1, it clears the latch/bit and calls callback5.

Therefore the observed empty-to-present INFO1 transition does not alone prove
that the mount callback ran. Capture all three readiness globals, the latch,
device pointer, callback and status byte from stopped RAM. If readiness is
true and the latch is1, follow callback4 and the mount task instead of changing
card-detect polarity or injecting a media-ready value.

Observed delayed-SD run reports INFO1 `0x20 -> 0` with no SD commands. Actual
RAM capture and downstream callback investigation are pending; no root cause
or successful media loading is asserted here.

Reproduce disassembly using the available local binutils build:

```
/tmp/cdj-binutils-sh-build/binutils/objdump -D -EL -b binary -m sh4 \
  --adjust-vma=0x04000000 --start-address=0x04238922 \
  --stop-address=0x0423895e firmware/nxs/main-unpacked.bin
```

This temporary tool path is host-local, not a dependency supplied by Git.
