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


## Measured, 2026-09-11 (later): the retraction above was a tool bug

Two of this document's earlier claims do not survive measurement, and the fix
for both is that the instruments were wrong, not the firmware.

**The flag byte was read big-endian on a little-endian target.**
`sd_readiness.py` extracted byte `n` of a word as `word >> (8 * (3 - n))`.
This SH4 is little-endian, so that returned the wrong byte, and the byte it
returned happened to be `0x00`. Read correctly the word at `0x051e21d0` is
`0x00000002`: **bit 1 is SET**. So the "WRONG INFERENCE" section above is
itself withdrawn - the bit-1 story it retracts is correct, and media mode is 1
because bit 1 of that byte is set. Fixed in `sd_readiness.py`
(`byte_of`, with `tests/test_sd_readiness_bytes.py`); the corrected sampler
reports `flags=0x02 bit1=1` on a MAIN-alone run, agreeing with the watchpoint,
and the status byte at `device + 0x66` reads `0x01` rather than `0x00`.

**The "both argument values" split was the after-access artifact.** With a
write watchpoint on `0x04cf222c`, every stop in a MAIN-alone run reports
`pc = 0x042f5ac2` - the return address of the setter, whose store sits in the
`rts` delay slot - and the word reads **1 at every one of them**. The earlier
"529 hits with r4 = 1 and 264 with r4 = 0" was the register-after-access trap
this document warns about, not a toggling media mode.

### The setter has two call sites, and neither runs after boot

Cross-references on SH4 are literal-pool loads, since the architecture has no
absolute call: `tools/cdj_main/xrefs.py` finds them in the unpacked image, so
no Ghidra project is needed. The setter `0x042a033c` has exactly two:

* `0x042f5abe`, the site already known, which passes bit 1 of `0x051e21d0`;
* `0x04101408`, inside `set_flag(index, value)` at `0x04100fda` - a 66-arm
  dispatcher over the jump table at `0x0410105c`. **Index 30** is the arm at
  `0x041013ee`, which sets or clears that same bit 1 and calls the setter.

A breakpoint on `0x041013ee` does not fire once in ninety seconds of a
MAIN-alone run, so flag 30 is latched during boot and the ~7 Hz writes are all
the `0x042f5abe` site restating the bit.

Disassembly confirms the two accessors name one word:
`0x042a033c` writes `0x04cf0c64 + 0x15c8` and `0x042a0348` returns
`0x04cf2180 + 0xac`, and both are `0x04cf222c`.

### Board count is NOT eliminated - it is the difference

The claim above that a two-board run "reaches the same Wait player screen" is
withdrawn. In a two-board `nxs_vm` run the same globals read:

| signal | address | MAIN alone | two-board |
|---|---|---|---|
| flag byte | `0x051e21d0` | `0x02` | `0x00` |
| media-mode word | `0x04cf222c` | `1` | `0` |
| readiness | - | false | **true** |
| mount latch | `0x049832ec` | `0` | **`1`** |
| mode state | `0x04cf2180` | `0` | `2` |

So with the GUI board attached the gate fires within the first minute, and the
open question is no longer why readiness is false.

**What is still not measured** is the mechanism of that difference. On MAIN
alone the byte is already `0x02` at t = 5 s, and a breakpoint on the
`set_flag(30)` arm never fires after that, so the bit is set in early boot.
A frozen-start (`gdbprobe --attach-at 0`) write watchpoint on the byte does
reach that window, but it drowns: 398 of 400 stops land inside 0.1 s at
t = 6.7 s, all of them writes of `0` from the routine whose store returns to
`0x042f5826`, with `set_flag` not involved. Catching the `0 -> 2` transition
needs a watchpoint that can filter on the value written, which this probe does
not have. It is no longer on the critical path, since the two-board run
mounts. The earlier two-board
comparison was made on a run far shorter than the media manager needs;
`NXS_BROWSE_BLOCKER.md` already records mount, listing and a native load on
this path at MAIN virtual times of 172-459 s.

### Instruments added for this

* `tools/cdj_main/xrefs.py` - literal-pool cross-references in the unpacked
  image. This is the "who calls this" answer that `xp /Ni` cannot give.
* Disassembly without the binutils build: Capstone's SH mode reads this image
  directly, but it needs `CS_MODE_SH4 | CS_MODE_SHFPU | CS_MODE_LITTLE_ENDIAN`
  - without SHFPU it silently fails on `sts fpscr,Rn`, which is common here.
* `tools/cdj_main/gdbprobe.py` - breakpoints and watchpoints over QEMU's own
  gdbstub, spoken directly, because this host has no gdb. Its report carries
  the same warning: at a watchpoint stop the registers are the state AFTER the
  access, so read the watched word rather than trusting a register.

## Resolved, 2026-09-11: the card mounts and a track loads, on the two-board path

`runs/nxs-sd-repro-1` is a fresh reproduction of the whole path on stock
firmware, with no injected media-ready value, no card-detect polarity change
and no browse aids:

```sh
python -m tools.cdj_main.nxs_vm runs/nxs-sd-repro-1 --seconds 1400 \
  --sd runs/nxs-test-media-1/test-track.img --fresh-link --trace-link-tx \
  --trace-media --source-key none --frame-interval 15 --port 7290 \
  --qemu-sync-profile
python -m tools.cdj_main.panel_control --port 7294 press 19:08 --hold-ms 100
#  ... wait for the FAT scan to settle, then press SOURCE again ...
python -m tools.cdj_main.panel_control --port 7294 press 17:01 --hold-ms 100
python -m tools.cdj_main.panel_control --port 7294 press 17:01 --hold-ms 100
```

Observed, in order. Only the two link-rx rows carry a MAIN virtual time, since
those are the lines that timestamp themselves; the rest are ordered
observations, not clock readings:

| what | evidence | when |
|---|---|---|
| SD commands issued | CMD0/8/55/41/2/3/9/10/7/16, then 1046 CMD17 and 417 CMD12 | from boot |
| readiness true, gate fired | `latch = 1`, `0x04cf222c = 0`, flag byte `0x00` | already true at the first sample, ~110 s wall after boot |
| browser answers | list reply `NO CARD`, after the first SOURCE press | before the FAT scan settles |
| media manager ready | mode `3`, table entry `2` | after the second SOURCE press |
| library listed | list replies `SD` / `FOLDER`, then `TESTTONE.WAV` | with mode 3 / entry 2 |
| ENTER accepted | link-rx `0000 0001 0003 0007 0001 0000` | **t = 356.26** |
| LOAD accepted | link-rx `0000 0007 0001 0000 0000 0000` | **t = 377.70** |
| load complete | status halfword 19 back to `0x1000` | after the LOAD |
| on screen | `TESTTONE.WAV`, TRACK 01, REMAIN 00:10.000 | - |

Both link-rx signatures are byte-for-byte the ones `NXS_BROWSE_BLOCKER.md`
recorded for its own ENTER and LOAD.

`link_inject` was not needed: both the ENTER and the LOAD came from real
encoder contacts on the panel channel. The two presses of the SOURCE contact
matter - the first lands before the media manager has scanned the card and is
answered `NO CARD`, exactly as `NXS_BROWSE_BLOCKER.md` describes.

### PCM from the track this run loaded

`tools.cdj_dsp.replay` on a checkpoint of this run, by the recipe in
`PCM_EXECUTION_EVIDENCE.md`, reaches the stock stereo unpack and it checks out
against the source WAV:

```sh
python -m tools.cdj_dsp.replay \
  runs/nxs-sd-repro-1/dsp-checkpoints/00000000000000002838.cdjdsp \
  runs/nxs-pcm-observe-repro-2 --steps 30000000 --connected-stops 16 \
  --events runs/nxs-sd-repro-1/dsp-events.jsonl --observe-pcm --verify-repeat
python -m tools.cdj_dsp.pcm_evidence runs/nxs-pcm-observe-repro-2 \
  runs/nxs-test-media-1/tracks/TESTTONE.WAV
```

`passed: true`, 16 verified connected stops, four blocks of 588 frames at
`0x118381e0`, `0x11838b10`, `0x11839440`, `0x11839d70`, each with the input and
both output planes matching the WAV. Trace SHA256
`8f251ab9bcb342b46b8090db52f9879eb8d9c5d4aeca2a1f3eb60a2209c2927e`.

The choice of checkpoint matters and is not obvious: this run issues the LPCM
command (a fixed-write of `2` to `0x11838100`) three times, at events 206954,
317191 and 317400. Only the first leads to the unpack. A replay from the
checkpoint before 317191 reaches its sixteen verified connected stops and
records **no** PCM observation at all, so `pcm_evidence` fails with "missing
complete observed calls" - which is a wrong checkpoint, not a failed unpack.

As `PCM_EXECUTION_EVIDENCE.md` states, this is observed execution replay. It is
not physical audio output and not a claim about playback timing.

**Why the MAIN-alone investigation stalled.** Readiness is false on MAIN alone
and true with the GUI board attached, so the three-minute MAIN-only boot was
measuring a machine that cannot mount rather than a firmware that declines.
