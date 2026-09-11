# NXS SD readiness investigation

Read-only firmware analysis, 2026-09-09. MAIN image SHA256
`02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85`.
No RAM values are changed or fabricated by this investigation.

Follow-up: [NXS_BROWSE_BLOCKER.md](NXS_BROWSE_BLOCKER.md) records a fresh
control where the latch and filesystem are populated before the browser can
list the card. Mode 2 / table entry 1 are intermediate observations; mode 3 /
entry 2 and real list replies follow later. Do not equate this low-level
card-detection predicate with completed browser readiness.

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


## Measured, 2026-09-11: the gate polls, readiness is false, card detect is fine

The capture this document asked for has been taken, with
`tools/cdj_main/sd_readiness.py` (QEMU monitor `xp`, no guest writes, no
injected media-ready value and no card-detect polarity change). Stock MAIN,
`cdj2000nxs-main`, the FAT32 test image from `tools.cdj_main.test_media`
carrying one WAV. Steady from t=27 s to the end of a 111 s run:

| signal | address | value | readiness arm |
|---|---|---|---|
| mode state | `0x04cf2180` | `0` | `== 3`? **false** |
| table entry | `0x04cf2994` | `0` | `!= 0`? **false** |
| media-mode fallback | `0x04cf222c` | `1` | media-mode `== 0`? **false** |
| latch | `0x049832ec` | `0` | gate has never fired |
| device pointer | `0x049832f0` | `0x04951c74` | set |
| mount callback | `device + 0x1c` | **`0`** | never installed |
| status byte | `device + 0x66` | `0` | bit 0x40 clear |

**Card detect is NOT the problem, and neither is the source key.** INFO1 bit 5
reads card-present throughout (active low, as this document states). Adding the
SD source-key press (`CDJ_PANEL_KEYS=22:19:04`) changes nothing in the table
above.

**The gate is not stuck; it runs and declines.** Breakpoints on the predicate
`0x04238922` and the gate `0x04238b64` both fire, together, every ~22.2 s
(48.7 / 70.9 / 93.2 / 115.3 / 137.7 s in a 150 s run), with the gate entered
from `0x043694aa`. So the polling task is alive and the predicate is evaluated
on schedule. It returns false because all three of its arms are false, which is
why no SD command is ever issued and INFO1 transitions alone never led to a
mount.

**So the open question is narrower than it was.** It is no longer "why does the
card not appear" but "what drives `0x04cf2180` from 0 to 3, or makes
`0x04cf222c` zero, or fills the table entry at `0x04cf2994`". That state word is
`0x04cf0c64 + 0x151c` and the base named by literal `0x042a0424`, so the next
step is the state machine around those, not the card path.

A bug found on the way and fixed separately (`f23e190`): `nxs_vm`'s private
source-key table had SD and DISC swapped, so `--sd` pressed DISC. That is the
same reversal `panel_control.BUTTON_NAMES` warns about. It was necessary to fix
and did not change any value above.


### The readiness arm that is actually live

Three of the four globals are inert after boot. `0x04cf2180` is written exactly
twice, both at t=0.9 s during init (`0x04388bea`, a block initialiser called
from `0x0429fdcc`, and `0x0429fe76` writing zero), and never again - so the
mode-state arm cannot become true on its own.

`0x04cf222c` is the live one: written **793 times in a 110 s run**, from t=4.5 s
at roughly 7 Hz, by the setter at `0x042a033c` (twelve bytes below the
media-mode function `0x042a0348` this document names), called from
`0x042f5abe`. The watchpoint sees the argument take **both** values - 529 hits
with r4 = 1 and 264 with r4 = 0 - so the media mode genuinely toggles during a
run and simply reads 1 whenever the gate samples it. Since `0x04cf2180` is 0
(not 4 or 5), media-mode returns this word verbatim, so **readiness becomes
true exactly when `0x04cf222c` is 0 at the moment the 22.2 s poll lands.**

**A WRONG INFERENCE, RECORDED SO IT IS NOT REPEATED.** The caller computes the
setter's argument from bit 1 of the flags byte at `r14+76`, with r14 =
`0x051e2184` (loaded at `0x042f5882`), i.e. the byte at `0x051e21d0`:

```
0x042f5ab6:  mov.b  @(r0,r14),r0   ; r0 = 76
0x042f5ab8:  tst    #2,r0          ; T = 1 when bit 1 is CLEAR
0x042f5aba:  subc   r4,r4          ; r4 = -T
0x042f5abe:  jsr    @r1            ; 0x042a033c
0x042f5ac0:  add    #1,r4          ; r4 = 1 if bit 1 SET, 0 if CLEAR
```

That reads as "bit 1 set is why media mode is 1", and it is **contradicted by
measurement**: the byte at `0x051e21d0` holds `0x00` for the whole run, so bit 1
is CLEAR and this path would pass 0. Either another call site reaches the same
setter, or r14 differs there. Do not act on the bit-1 story; the measured facts
are the 793 writes, the two argument values, and the sampled value of 1.

### Hypotheses eliminated by measurement, so they need not be retried

* **Card detect.** INFO1 bit 5 reads present throughout, active low.
* **Source key.** `CDJ_PANEL_KEYS=22:19:04` changes no value in the table.
* **Card contents.** A card carrying `PIONEER/rekordbox/export.pdb` and a WAV
  under `Contents/` produces identical readiness to a bare `tracks/*.WAV`
  card - readiness is decided before the filesystem is consulted.
* **Board count.** A two-board `nxs_vm` run reaches the same "Wait" player
  screen as MAIN alone; the library never appears in either.

Reproduce disassembly using the available local binutils build:

```
/tmp/cdj-binutils-sh-build/binutils/objdump -D -EL -b binary -m sh4 \
  --adjust-vma=0x04000000 --start-address=0x04238922 \
  --stop-address=0x0423895e firmware/nxs/main-unpacked.bin
```

This temporary tool path is host-local, not a dependency supplied by Git.
