# CDJ-2000 input manifest

Every input the emulated MAIN board decodes, what was done with it, and what was
measured. The board decodes **48**: 40 button bits and 8 analogue fields.

This file is not decoration. Three parts of the code read it:

* `tools/cdj_gui/view_ui.py` reads the verdict tables, so the operator window
  can say on its own face which of its controls has ever been shown to do
  something. One table in the repository, not a copy per tool.
* `tests/test_panel_control.py` requires the bit rows here and
  `panel_control.BUTTON_BITS` to be the same 40 bits.
* `tests/test_panel_names_match_the_firmware.py` compares the name table below
  against MAIN's own service-mode name table, read out of the firmware image.
  That test exists because the four SOURCE keys were labelled backwards here
  for weeks -- `SD` sent the USB bit -- and every tool agreed with every other
  tool while all of them disagreed with the board.

## How to read a row

A row is a measurement, not an intention. Each of the tables below belongs to
one **world**: one run, one firmware image, one screen, one set of switches. A
verdict only means anything inside its world, because what an input does depends
on what is on the screen when you press it. Later tables supersede earlier ones
for the inputs they cover.

The verdict is always the last column:

| verdict | means |
|---|---|
| `changed` | two frames either side of the press differ, outside the animation mask |
| `no-op` | the frames are identical, and the window was measured -- a proven no-op |
| `not measured` | no frame pair covers the press; the run says nothing about it |

`no-op` and `not measured` are opposite findings and were indistinguishable
until the sampler began recording every tick whether or not it wrote a file.

## Names

Button bits are written `<payload byte>.<bit>`, exactly as `CDJ_PANEL_KEYS`
spells them. Analogue fields are `field0` .. `field7`. The physical names come
from MAIN's own name table, never from a photograph.

## The firmware names the bits itself, and four of our names were backwards — 2026-08-07

Read out of `firmware/main-unpacked.bin`, no emulator, checked by
`python -m pytest tests/test_service_mode_key_names.py -q` (**8 passed**). Every
address below is in that test; if the image is swapped or the map is edited by
hand, the test fails rather than the prose.

MAIN carries a **SERVICE MODE** page whose `BUTTON` row prints *the name of the
key that is down*. That page is not decoration: it is the firmware's own
statement of which panel bit is which control, and until now this file has had
38 anonymous bits called `19.0`, `20.1` and so on.

### The chain, five hops, all static

| where | what it does |
|---|---|
| `0x28e1ae` | payload bytes 15..21 → status bits at `0x04fe29f4 + 72..87` |
| `0x2a1022` | `PnlCom_RcvTASK` copies **36 B from `0x04fe2a20`** and **8 B from `0x04fe2a44`** into a message, id **10001**, to `SRVMOD_MBX` |
| `0x2a097c` | `word0 = [0x04fe2a48]` (masked `0xFFE00000`), `word1 = [0x04fe2a3c]` (**bit 30 inverted**), plus the previous copy → a *changed* mask |
| `0x2a09f2` | matches the changed bits against two `{mask, code}` tables — `0x000a0e14` (11 entries) for word0, `0x000a0d24` (29) for word1 — and returns `0x80000000 \| code` on a press, plain `code` on a release |
| `0x29f9b0` | prints `name[code]`; `name[]` is the pointer table at image `0x340288`, runtime **`0x07db345c`** |

`0x2a15b4` (the BUTTON page) calls `0x29f9b0(key)` on a press and
`0x29f9b0(0)` on a release, and `name[0]` is the empty string — so the row shows
a name exactly while a key is held. `shll2` discards bit 31, which is why the
press flag never disturbs the index.

**The name table was invisible to every reference search, and here is why.**
Not one 32-bit word in the whole 3.9 MB image has low 24 bits `0x340288`.
MAIN's `.data` is stored in the image at `0x34xxxx` and linked at `0x07dbxxxx`;
the constant `0x07A731D4` is fixed by five independent items (`0x07db3444` → a
pointer to `"show"`, `0x07db342c` → `0xffffffff`, `0x07db3440` → 1, and the two
mask-table pointers at `0x07db351c`/`0x07db3520`). Searching the image for the
image address of a `.data` object finds nothing, every time.

### The catalogue — 38 of the 40 decoded bits have a name

The gap between `AUTO LOOP 8` and `BROWSE` was five entries, not a hole:
`SLIP`, `LINK`, `USB`, `SD`, `DISC`.

| payload | status | code | SERVICE MODE name | note |
|---|---|---|---|---|
| 15.0 | 75.7 | 1 | `LOCK` | `EjectLock` in the `0x05c648` table |
| 15.1 | 75.6 | 2 | `REV` | **active low** — `0x2a097c` inverts bit 30 |
| 15.5 | 75.5 | 3 | `JOG TOUCH SW` | |
| 15.6 | 75.4 | — | — | decoded, no code, no name |
| 15.7 | 75.3 | — | — | decoded, no code, no name |
| 16.0 | 75.2 | 4 | `PLAY` | |
| 16.1 | 75.1 | 5 | `CUE` | |
| 16.2 | 74.4 | 10 | `RELOOP` | |
| 16.3 | 74.1 | 13 | `OUT` | |
| 16.4 | 74.2 | 12 | `IN` | |
| 16.5 | 74.6 | 8 | `HOT CUE A` | |
| 16.6 | 74.7 | 7 | `HOT CUE B` | |
| 16.7 | 75.0 | 6 | `HOT CUE C` | |
| 17.0 | 86.5 | 47 | `ENCODER PUSH` | the rotary's push |
| 17.1 | 74.3 | 11 | `4-BEAT LOOP` | |
| 17.2 | 72.1 | 28 | `SD OPEN` | **inverted** at `0x28e3c2` |
| 18.0 | 74.5 | 9 | `REC MODE` | |
| 18.1 | 74.0 | 15 | `PREVIOUS \|<<` | |
| 18.2 | 73.7 | 14 | `NEXT >>\|` | |
| 18.3 | 73.6 | 17 | `REV <<` | |
| 18.4 | 73.5 | 16 | `FWD >>` | |
| 18.6 | 73.4 | 18 | `JOG MODE` | |
| **18.7** | 73.3 | 19 | **`TEMPO RANGE`** | half of the service-mode boot grip |
| 19.0 | 87.7 | 37 | `LINK` | **see the conflict below** |
| 19.1 | 87.6 | 38 | `USB` | **see the conflict below** |
| 19.2 | 87.5 | 39 | `SD` | **see the conflict below** |
| 19.3 | 87.4 | 40 | `DISC` | **see the conflict below** |
| 19.4 | 73.0 | 22 | `TIME/ACUE` | |
| 19.6 | 73.2 | 20 | `MASTER TEMPO` | |
| 19.7 | 73.1 | 21 | `TEMPO RESET` | |
| 20.0 | 87.3 | 41 | `BROWSE` | |
| 20.1 | 87.2 | 42 | `TAG LIST` | |
| 20.2 | 87.1 | 43 | `INFORMATION` | |
| **20.3** | 87.0 | 44 | **`MENU`** | driven since 2026-08-07; `BUTTON_BITS` called it "not decoded" |
| 20.4 | 86.7 | 45 | `RETURN` | decoded only while `[0x04c06fa4+0x1674] > 9` |
| 20.5 | 86.6 | 46 | `TAG TRACK` | same gate |
| 21.0 | 72.4 | 27 | `< CALL` | |
| 21.1 | 72.3 | 26 | `CALL >` | |
| 21.2 | 72.5 | 25 | `DELETE` | |
| **21.3** | 72.6 | 24 | **`MEMORY`** | driven since 2026-08-07; `BUTTON_BITS` stopped at 21.2 |

Two codes in the table are reached from something that is **not** a panel
payload bit and are therefore not rows above: `23 EJECT` (status 72.7, no writer
in `0x28e1ae`) and `29 USB STOP` (status 72.0, written from GPIO
`[0xfff10060]` bit 1 with a three-sample debounce at `0x28e3de`).

**So the board decodes 40 button bits, not 38** — and as of 2026-08-07 all 40
are driven. `20.3` and `21.3` are decoded at `0x28e59a` and `0x28e61e` into
status `87.0` and `72.6`, and they were held out of `panel_control.BUTTON_BITS`
for exactly one day and one reason: `plan coverage` was in flight as
`r160`, and adding two windows to a 1 520 s run while it
is running is the quiet edit this file exists to prevent. That run finished, the
pair is in, **the coverage denominator is 48 and `view_ui --coverage` prints
`48 of 48`**, and `plan coverage` is **49 windows / 1 570 s** (+50 s).
`tests/test_panel_control.py` still carries `DECODED_BUT_NOT_DRIVEN`, now empty:
it is where the next "the firmware decodes it and nothing drives it" gets
written down with a date, instead of living in a comment.

## Which world a number describes — read this before using a number

The same key on the same emulator gives different deltas depending on what is on
the screen, and this file has measurements from two machines that are not
comparable:

| | **r026 world** | **r096 world** | **r113 world** |
|---|---|---|---|
| driven by | `CDJ_PANEL_KEYS`, 15 presses, 15 s apart | the control channel, 22 presses, 25 s apart | the control channel, 21 of 22 presses, 25 s apart |
| browse pane | empty, header `NO USB` | `SD` header, six categories (`BFIN_REQUEST_KIND=2`) | same, plus `BFIN_LINK_HOLD_ANNOUNCE=1` |
| jog area | the spinning "Wait" platter | plain platter, no badge | plain platter, no badge |
| the drawing task | alive | **stuck** in the fault loop `r098` named, from t317.8 | alive: `r112` counts 34 595 completed frames to t740 (~46/s) |
| screen settles at | — | t317.8 (a dead task, not a still screen) | **t194.8** — and its control run `r112` settled at t121.3 |
| mask | `runs/anim-mask.bin` (r048, 4.73 % animated) | `runs/r095/anim-mask-kind2.bin` (8.69 % animated, noise floor 0) | **none, and none is possible** — see below |
| QEMU binary | `33b2fe57…` | the 12:20 rebuild, `grep -c "no longer a socket"` = 2 | the 15:xx rebuild with `BFIN_LINK_HOLD_ANNOUNCE` |

**They disagree, and not subtly.** `r026` measured 372 bytes for bit 18.1 and
364 for 18.2; in `r096` both are **0**, with a mask that covers 91.3 % of the
frame and a measured noise floor of 0. Neither is wrong — they are answers about
different screens. Two numbers from two worlds in one column would be worse than
a missing number, so the tables below are kept apart and each says which world
it belongs to.

**And the r113 world has no mask to offer, in either direction.** `r095`'s mask
describes a screen whose drawing task had died, so on a live screen it lets real
animation through as evidence. The control run for the live world, `r112`, has
no steady phase to fit one from: it stopped changing at t121.3, and
`frame_delta mask --from 150` finds zero frames. That was read as good news —
"nothing to mask, so nothing can be invented" — and it is the reasoning the next
section takes apart.

## How an input is delivered

Two ways, and they differ in what they can express rather than in what they
reach.

### Before the boot — `CDJ_PANEL_KEYS`

A semicolon-separated list of `<virtual seconds>:<payload byte>:<hex mask>[:<hold seconds>]`,
at most **16 entries** (`PANEL_KEYS_MAX`), with `CDJ_PANEL_HOLD_MS` setting how
long each stays down, unless the entry names its own hold time. `0:21:04:20` is
DELETE held from the first frame for 20 s, the way a person holds a key while
switching the deck on; `boot_vm --hold-key delete:20` (also `cosim_scenario
--hold-key`) writes that entry. Everything in the table below was driven this way.

Its two limits are why the rest of this section exists: sixteen presses fixed
before the machine starts, and **only an OR of button bits** — it cannot ramp an
analogue value over time, so it can never move an encoder.

### While it runs — the control channel

`emulator/qemu/cdj2000_input.c` opens a line-oriented TCP server on
`127.0.0.1` when **`CDJ_INPUT_PORT`** names a port, and merges what it is told
into the payload in `cdj_panel_frame()`, after the `CDJ_PANEL_KEYS` loop and
before the checksum. `tools/cdj_main/panel_control.py` speaks it:

    CDJ_INPUT_PORT=5984 python -m tools.cdj_main.boot_vm --sd card.img …
    python -m tools.cdj_main.panel_control --port 5984 press sd
    python -m tools.cdj_main.panel_control --port 5984 rotary 4 +12
    python -m tools.cdj_main.panel_control --port 5984 session --wait 120 \
        150:"press 18.1" 175:"press 18.2"

**With the variable unset nothing binds and nothing is merged**, so a run
without it is a control run in the strict sense. That is not a claim: the file
is compiled against stub headers in `tests/cstub/`, given a real socket and a
steppable virtual clock, and every panel exchange is read back
(`tests/test_input_channel.py`). Measured there, on the host:

| property | measured |
|---|---|
| a press is one pulse, down then up | one stretch of set frames, then clear for the rest |
| the default hold | 4 exchanges at 100 ms of virtual time = 300 ms |
| a hold shorter than one exchange | still 2 exchanges — otherwise it would never reach the wire |
| two presses in a row | never overlap; ≥ 1 quiet exchange between them |
| `CDJ_INPUT_PORT` unset | every payload byte 0, in every exchange |
| analogue set | byte 2 = `0x5a`, bytes 4..5 = `12 34`, i.e. the values the live probe saw at `0x04fe2a20`/`+8` |
| rotary +5 | 1, 2, 3, 4, 5 — one count per exchange, then still |
| rotary −5 from 3 | 2, 1, 0, `0xffff`, `0xfffe` — wraps like a counter |

### The frame either way

The panel frame is 24 bytes. **Payload bytes 2..14 are values and bytes 15..21
are bits** — the decoder runs from `0x28e1ae` and the whole span lands in the
panel status block at `0x04fe29f4 + 72..87` and the analogue block at
`0x04fe2a20`:

| payload | what the decoder does with it |
|---|---|
| 2, 3 | 8-bit levels → `0x04fe2a20 + 0`, `+4` |
| 4..13 | five 16-bit big-endian levels → `+8`, `+12`, `+16`, `+20`, `+24` |
| 14 | a value: `[0x04fe2a44] = byte14 + W[0x04fe2af8]` — **the encoder** |
| 15 | bits 0 1 5 6 7 |
| 16 | bits 0..7 |
| 17 | bits 0 1 2 |
| 18..21 | the 22 bits `0x28e44a` spreads — the ones this file used to list |

Bytes 12/13 deserve their own line: the pair is masked with `0x1ff` and its bit
15 tested against `0x8000`, the flag landing in `0x04fe2a3c`. A 0..511 position
with a touch flag is a **jog wheel**, not a select encoder — which is why field 6
was never going to be the rotary either.

Byte 22 is the sum of 0..21 with an
end-around carry folded *inside* the loop and byte 23 the `0x8f` marker, which
is why an all-zero payload still validates.

## The buttons in the r026 world — all 22 decoded bits

**This table describes the r026 world** (see above): `CDJ_PANEL_KEYS`, an empty
browse pane, the "Wait" platter, the r048 mask. It is kept because it is what
every one of its numbers was measured against, and because the *bit inventory*
— which of the 22 bits exist and which four are the SOURCE keys — is a property
of the firmware and carries over. **The deltas do not carry over.**

Physical names are only filled in where they are *measured*, not guessed. The
four SOURCE keys are known from `0x28ddc8`, which turns a rising edge into the
one-hot source flag at `0x04c084d0 + n*4`.

"Stable delta" is the number of bytes that differ between the frame sampled
just before the press and the first one at least six seconds after it, counted
**only over pixels that never move in the control run** (r048: no buttons, same
340 s, same settings, same card). 4.73 % of the frame is animated; the other
349 835 bytes are the evidence surface. A zero there is a real no-op, not a
measurement that drowned in the spinner.

| payload | status bit | physical name | driven | window | stable delta | verdict |
|---|---|---|---|---|---|---|
| 18.0 | 74.5 | — | r026 | 8.1 s | 0 | no-op |
| **18.1** | 74.0 | — | r026 | 7.2 s | **372** | **changes the display** |
| **18.2** | 73.7 | — | r026 | 9.1 s | **364** | **changes the display** |
| 18.3 | 73.6 | — | r026 | 9.1 s | 0 | no-op |
| 18.4 | 73.5 | — | r026 | 9.2 s | 0 | no-op |
| 18.6 | 73.4 | — | r026 | 8.1 s | 0 | no-op |
| 18.7 | 73.3 | — | r026 | 9.0 s | 0 | no-op |
| **19.0** | 87.7 | **SOURCE LINK** | r026 | 9.1 s | **906** | **changes the display** |
| 19.1 | 87.6 | **SOURCE USB** | every run | — | — | selects the medium: `0x04c084d4` goes to 1, drive letter `b:` reaches type 3 |
| 19.2 | 87.5 | **SOURCE SD** | r026 | 28.3 s | 83 908 | changed, **window too wide to attribute** |
| 19.3 | 87.4 | **SOURCE DISC** | r026 | 21.2 s | 4 384 | changed, **window too wide to attribute** |
| **19.4** | 73.0 | — | r026 | 7.1 s | **749** | **changes the display** |
| 19.6 | 73.2 | — | r026 | 19.2 s | 4 376 | changed, **window too wide to attribute** |
| 19.7 | 73.1 | — | r026 | 22.2 s | 4 632 | changed, **window too wide to attribute** |
| **20.0** | 87.3 | — | r026 | 8.1 s | **885** | **changes the display** |
| **20.1** | 87.2 | — | r026 | 7.1 s | **749** | **changes the display** |
| 20.2 | 87.1 | — | r076 | 27.7 s | 1 570 | changed, **window too wide to attribute** |
| 20.4 | 86.7 | — | r076 | 30.7 s | 1 570 | changed, **window too wide to attribute** |
| 20.5 | 86.6 | — | r076 | 9.2 s | 0 | no-op |
| 21.0 | 72.4 | — | r076 | 9.3 s | 0 | no-op |
| 21.1 | 72.3 | — | r076 | 9.2 s | 0 | no-op |
| 21.2 | 72.5 | — | r076 | 9.2 s | 0 | no-op |

**Every one of the 22 decoded bits has now been driven at least once.** The
tally: six change the display, nine are no-ops, one is the SD source key, and
six are still unattributed because the sampling window around them was too
wide.

Re-running the four `19.x` rows in r076 did not settle them: all four landed
inside a **50-second hole** in the sample stream, sharing one before/after pair.
That hole is itself informative — the frame sampler only writes a file when the
frame changed, so fifty quiet seconds mean the display was static across all
four presses, and the 14 062-byte difference at the end of the window belongs
to whatever happened at `t0243.8`. Attributing them needs one run per key, or a
sampler that writes a frame on a fixed cadence regardless of change.

The four "window too wide" rows are honest failures of the *sampling*, not of
the input: the frame sampler only writes a file when the frame actually
changed, so a quiet stretch leaves a 20-second hole and the pair either side of
it can contain anything. They did change the display; the pair just cannot
prove which press did it. Re-running those four keys alone, far apart, would
settle them.

`CDJ_PANEL_KEYS` accepts at most 16 entries (`PANEL_KEYS_MAX`), one of which is
the source press `--sd` adds — `19.1`, which is the **USB** key, not SD — so
six of the 22 bits could not be driven in the same run.

Bits 18.5, 19.5, 20.6 and 20.7 are **not decoded by `0x28e44a` at all**, so they
are not inputs on this board and are deliberately absent from the table. (`20.3`
stood in that list too and did not belong there: `0x28e59a` decodes it into
status `87.0`, and MAIN calls it `MENU`.)

## The buttons in the r096 world — all 22 driven over the socket

`r096` is the first run in which every decoded bit was pressed **while the
machine ran**: 745 s, 22 presses 25 s apart starting at t150, `--no-peer`,
`BFIN_REQUEST_KIND=2`, transcript in `runs/r096/session.txt`, index in
`runs/r096/frames-index.tsv`. The channel carried all 22 with no drop. The
mask is `runs/r095/anim-mask-kind2.bin` from a control run of the same
length and settings: 335 295 evidence bytes of 367 200 (91.3 %), **noise floor
0** over 15 held-out frames, so any non-zero delta below is a movement.

Re-judged with the window counted in content changes (see below), where the
first pass counted it in file timestamps:

| payload | window | delta | box | verdict |
|---|---|---|---|---|
| 18.0 | 6.3 s | 0 | — | measured, nothing moved |
| 18.1 | 8.2 s | 0 | — | measured, nothing moved |
| 18.2 | 8.2 s | 0 | — | measured, nothing moved |
| **18.3** | **7.8 s** | **498** | 66,156..80,182 | changes the display: the TRACK digit goes 0 → 1 |
| 18.4 | 65.8 s | 949 | 444,203..478,223 | changed at t317.8, window too wide to attribute |
| 18.6 | — | 0 | — | no-op, but the screen had stopped † |
| 18.7 | 15.8 s | 949 | 444,203..478,223 | changed at t317.8, window too wide to attribute |
| 19.0 | — | — | — | SOURCE LINK; NOT MEASURED, sampler read race |
| 19.1 | — | — | — | SOURCE USB; selects the medium, 0x04c084d4 → 1 |
| 19.2 | — | 0 | — | SOURCE SD; no-op, but the screen had stopped † |
| 19.3 | — | 0 | — | SOURCE DISC; no-op, but the screen had stopped † |
| 19.4 | — | 0 | — | no-op, but the screen had stopped † |
| 19.6 | — | — | — | NOT MEASURED, sampler read race |
| 19.7 | — | 0 | — | no-op, but the screen had stopped † |
| 20.0 | — | — | — | NOT MEASURED, sampler read race |
| 20.1 | — | — | — | NOT MEASURED, sampler read race |
| 20.2 | — | — | — | NOT MEASURED, sampler read race |
| 20.4 | — | 0 | — | no-op, but the screen had stopped † |
| 20.5 | — | 0 | — | no-op, but the screen had stopped † |
| 21.0 | — | 0 | — | no-op, but the screen had stopped † |
| 21.1 | — | — | — | NOT MEASURED, sampler read race |
| 21.2 | — | 0 | — | no-op, but the screen had stopped † |

One attribution, two unattributed, three measured pairs with no movement, nine
proven no-ops, seven holes. The target is six attributions.

**† and this dagger is the important part.** The frame content was byte-for-byte
identical from **t317.8 to the end of the run**, 611 `same` ticks, while MAIN
kept taking 14 112 requests. Every row marked † lies after that, so what is
proven there is that *the screen was frozen*, not that the key does nothing.
An expected no-op is allowed, but a no-op is only a statement about the key
if the display could still have moved. **Those nine rows are recorded, not
counted.** The control run `r095`, same settings and no input at all, froze at
t236.0 — so the freeze is not the input's doing.

**And it is not idleness either — `r098` named it.** A data CPLB miss for
`0x01100000` from the CRC byte loop at `0xb7cb50`, repeated a thousand times
inside a tenth of a second, because `0xb99192` is the firmware's EVT3 vector: it
repairs nothing and `RTX` returns to the faulting instruction. The drawing task
never advances while the interrupt-driven ones do, which is exactly the picture
`r095` and `r096` show. The opt-in repair
is `BFIN_LINK_HOLD_ANNOUNCE=1`. So the fifteen late bits of `r096` are **not
measured**, and calling any of them a no-op would be reading a dead task.

### What 18.3 is, from looking rather than counting

498 bytes in a 14x26 box at `66,156..80,182`, which is the second digit of the
`TRACK` field in the player row. The crop shows a seven-segment `0` becoming a
seven-segment `1`. That is the same change the r024/r026 comparison table lower
down recorded as "`TRACK` `00` → `01`" without being able to say which bit did
it; here it is one press, 7.8 s later, on a screen that had stood still for the
18.3 s before it.

**The display answers slowly, and that is a fact the next plan needs.** 7.8 s
between the press and the repaint is most of the ten-second attribution limit.
18.4 and 18.7 fail on exactly this: their one shared change lands at t317.8,
65.8 s and 15.8 s after their presses, and no reading of the index can give them
one each.

### What the answer time does to `--settle`, and it is the opposite of intuition

`--settle` is a floor on how late the second frame may be taken; it is there so
a frame caught mid-redraw is not mistaken for the settled result. The obvious
move on learning the display takes 7.8 s is to raise it past that. **That
destroys the measurement**, and the reason is the sampler: it writes nothing
while the screen stands, so when the display answers, the answering frame is the
*only* frame there is. A settle longer than the answer steps over it and takes
the **next input's** repaint instead.

Measured on a synthetic two-key run, 25 s apart, each answered after 5 s:

| `--settle` | pair | delta | window |
|---|---|---|---|
| 6 s | `t0100.0 -> t0130.0` | 24 | 30.0 s — **the next key's repaint** |
| 3 s | `t0100.0 -> t0105.0` | 12 | 5.0 s — the answer |

The two errors are not symmetric. Too high loses the row and replaces it with a
plausible wrong one; too low can at worst catch a frame mid-redraw, which the
index shows as consecutive `new` ticks. **So the margin goes downwards:**
`panel_control` carries `PLAN_RESPONSE = 7.8` with `PLAN_RESPONSE_SOURCE =
"r096, bit 18.3"` and sets `--settle` to half of it. `frame_delta` reports
`SETTLE SKIPPED` and refuses to count the row when it happens anyway — but only
when the frame taken instead falls outside the attribution limit, because a
change inside the settle is ordinary while the screen is busy and r096's first
three windows each have one.

**And the run does not get longer for it.** The binding constraint is
attribution, not spacing: an answer has to land inside 10 s of its press, and
25 s of spacing already keeps the next press 15 s clear of that window. A wider
gap buys nothing an answer slower than 10 s could use. `keys` stays at **745 s**.
What is thin is the headroom — 2.2 s — and `18.7` at 15.8 s is what running out
of it looks like: no spacing rescues that row, only a faster display or a method
with a different limit.

**One sample is one sample.** r096's other windows cannot corroborate the 7.8 s:
during the busy stretch the screen repainted on its own every second or two, so
"the next change after the settle" there is the animation's cadence and not an
answer. Every compared row now prints how long the display took, so the next run
yields one sample per repainting key instead of one for the whole project;
That is asked for explicitly.

### There are two samples now, and the settle went down rather than up

`r113` printed three answer times and two of them were withdrawn — 3.3 s and
3.7 s, both measured while the screen was repainting on its own. What is left is

    7.8 s   r096, bit 18.3   TRACK digit 0 -> 1
    9.5 s   r113, bit 18.7   the tempo-range corner

and by the rule `B-008` set out in advance — *median above 7.8 s, or scattering
around 10 s, means `PLAN_RESPONSE` is wrong and the attribution limit itself
becomes the question* — **the rule fires.** The median is 8.65 s and the slower
sample is 0.5 s under the limit. Two samples are still thin, and they are all
there is: seventeen bits repaint nothing at all, so a third sample cannot come
from the buttons.

`PLAN_RESPONSE` therefore takes the **slower** figure, 9.5 s, because that is
what a plan has to survive. `--settle` does **not** follow it to half:

- Half of 9.5 is 4 s, and 4 s is above something `r113` actually recorded — a
  change 0.2 s after the `18.4` press. That change is unattributable for another
  reason entirely, but the settle must not be what decides it.
- Placing a change relative to its input is now the index's job, properly:
  `CHANGE NOT PROVEN AFTER THE INPUT` refuses exactly the case a large settle was
  loosely guarding against.

So `PLAN_SETTLE` is **1 s** — enough to clear the sampler's own tick and nothing
more.

**What is not being done, and deliberately:** the 10-second attribution limit is
not being raised to make room for 9.5 s. Raising it alone widens every window
and is precisely how a row gets a number it did not earn. If a third answer
crosses it, the limit and the spacing move together or not at all, and that is a
decision about the method rather than a constant to tune.

## The buttons in the r113 world — the compositor is alive, and one bit moves it

`r113` is the first key run on a screen proven to be drawing. `BFIN_GUI_FRAME_TRACE`
counts completed frames where the sampler can only compare file contents, and
its control run `r112` — same switches, same 745 s, channel open and no
session — completed **34 595 frames by t740, about 46 a second, with the picture
unchanged after t121.3**. So the seventeen no-ops below are statements about
their keys and not about a dead task, which is the condition attached
to accepting a no-op. Every dagger in the r096 table above is retired by that
same measurement.

**The run is not a complete measurement and must not be read as one.** It was
terminated at t662 of 745 s at the user's request: 21 of the 22 presses landed,
`21.2` never did, and `runs/r113/run.txt` carries the `INVALID` note. The
rows below are what a cut-short run produced, not a pass.

Alignment is **measured**, not given: `+2.0 s` from `runs/r113/session.txt`
against the sampler's own directory. The published table used `--shift 1.8` by
hand, which moved two rows by a fifth of a second and, for `18.1`, across the
line that decides whether a change can be placed after its input at all.

| payload | window | delta | box | verdict |
|---|---|---|---|---|
| 18.0 | 3.3 s | 759 | 105,70..148,94 | **withdrawn** — SCREEN BUSY: 9 changes in the 10 s before the press |
| 18.1 | 3.7 s | 796 | 104,65..148,86 | **withdrawn** — the change is not proven to follow the press, and 796 < the 1148 an input-free window of the same run reaches |
| 18.2 | — | 0 | — | no-op, proven: 9 ticks, the frame never changed |
| 18.3 | — | 0 | — | no-op, proven: 10 ticks |
| 18.4 | — | 1573 | 48,156..478,223 | **withdrawn** — last sighting t251.0, press t252.0, change t252.0: the change may predate the press |
| 18.6 | — | 0 | — | no-op, proven: 10 ticks |
| **18.7** | **9.5 s** | **949** | 444,203..478,223 | **changes the display**: the tempo-range corner, self-baseline 0 over 4 input-free windows |
| 19.0 | — | 0 | — | SOURCE LINK; no-op, proven: 10 ticks |
| 19.1 | — | 0 | — | SOURCE USB; no-op, proven — the medium was already selected at t40 |
| 19.2 | — | 0 | — | SOURCE SD; no-op, proven: 10 ticks |
| 19.3 | — | 0 | — | SOURCE DISC; no-op, proven: 10 ticks |
| 19.4 | — | 0 | — | no-op, proven: 10 ticks |
| 19.6 | — | 0 | — | no-op, proven: 10 ticks |
| 19.7 | — | 0 | — | no-op, proven: 10 ticks |
| 20.0 | — | 0 | — | no-op, proven: 10 ticks |
| 20.1 | — | 0 | — | no-op, proven: 9 ticks |
| 20.2 | — | 0 | — | no-op, proven: 10 ticks |
| 20.4 | — | 0 | — | no-op, proven: 10 ticks |
| 20.5 | — | 0 | — | no-op, proven: 10 ticks |
| 21.0 | — | 0 | — | no-op, proven: 9 ticks |
| 21.1 | — | 0 | — | no-op, proven: 10 ticks |
| 21.2 | — | — | — | NOT MEASURED — never pressed, the run was cut short |

One attribution, three withdrawn, seventeen proven no-ops, one never driven.
The target is six attributions.

### The three guards this cost, now in `frame_delta windows`

None of them needs a second machine; all three read `index.tsv` of the run being
judged, so a run before `fbde2eb` (r026 among them) evaluates exactly as before
and the six calibration numbers are untouched.

| guard | fires when | why a row cannot survive it |
|---|---|---|
| `SCREEN BUSY` | two or more content changes in the 10 s **before** the input | the display was already repainting, so the compare measures the repainting |
| `BELOW SELF-BASELINE` | the delta does not beat the worst of up to four input-free windows from this run's own gaps | the screen reaches that number with nothing sent |
| `CHANGE NOT PROVEN AFTER THE INPUT` | the earlier frame's last sighting is before the input | the change is bracketed in a stretch starting before its supposed cause |

All three exit non-zero, like `NOT MEASURED` and `WINDOW TOO WIDE`: a row that
cannot be scored must not leave a run looking like it passed. `--baseline 0`
turns the second one off, which is a decision and reads like one in the command
line.

**The general rule, and it is the one to carry forward:** a mask from another
run is only evidence if that run reached the state being judged, and there is no
way to check that from the mask itself. The control the run carries with it can
be checked, because it is the same frames.

## The buttons in the r116 world — the complete run, and it is all no-ops

`r116` (815 s, `plan keys --start 220`) is the first key run that is complete and
valid: 813 sampler ticks, 22 of 22 presses acknowledged, 3 read errors in 813
and none in a window, and `frame_delta windows --align --settle 1` **exits 0** —
every row a result, no holes, no `WINDOW TOO WIDE`, and none of the three new
guards fires. The presses reached MAIN independently of the picture: the monitor
reads `source LINK 0x04c084dc = 1` at the end, which only `19.3` can have done.

**All 22 rows are `no-op, proven`, 9–10 ticks each.** And `r116` does **not**
reproduce `r113`'s `18.7`: here that bit is a proven no-op over ten ticks and the
`444,203..478,223` corner still reads `±10` after 820 s. Two runs paired that box
with `18.7`; a third of the same binary contradicts them. **The pairing is
unconfirmed, and the coverage target therefore stands at zero attributed changes,
not one.** The r113 table above keeps its row because that is what r113 measured;
this is what happened when it was asked again.

The control is inside the run: 44 content changes, all before t123.6, then 766
`same` ticks to t820.8, so nothing over the measured stretch (t221.9..t756.9)
moves by itself and there is nothing to mask. `--start 220` is what made the
difference — the mount animation ended at t123.6 here, t172.4 in `r115`, t194.8
in `r113` and t121.3 in `r112`. **73 seconds of spread on the same binary and the
same card**, which is also why a control run cannot bound another run's
animation and why the instrument that works is the measured run's own index.

| payload | driven in | verdict |
|---|---|---|
| 18.0 | r116 | no-op, proven |
| 18.1 | r116 | no-op, proven |
| 18.2 | r116 | no-op, proven |
| 18.3 | r116 | no-op, proven |
| 18.4 | r116 | no-op, proven |
| 18.6 | r116 | no-op, proven |
| 18.7 | r116 | no-op, proven — does **not** reproduce r113's 949 B |
| 19.0 | r116 | SOURCE LINK; no-op, proven ‡ |
| 19.1 | r116 | SOURCE USB; no-op, proven ‡ |
| 19.2 | r116 | SOURCE SD; no-op, proven ‡ |
| 19.3 | r116 | SOURCE DISC; no-op, proven ‡ — but it *did* reach MAIN: `0x04c084dc` = 1 |
| 19.4 | r116 | no-op, proven |
| 19.6 | r116 | no-op, proven |
| 19.7 | r116 | no-op, proven |
| 20.0 | r116 | no-op, proven |
| 20.1 | r116 | no-op, proven |
| 20.2 | r116 | no-op, proven |
| 20.4 | r116 | no-op, proven |
| 20.5 | r116 | no-op, proven |
| 21.0 | r116 | no-op, proven |
| 21.1 | r116 | no-op, proven |
| 21.2 | r116 | no-op, proven |
| 15.0 | — | not driven in this run — see the r117 table below |
| 15.1 | — | not driven in this run — see the r117 table below |
| 15.5 | — | not driven in this run — see the r117 table below |
| 15.6 | — | not driven in this run — see the r117 table below |
| 15.7 | — | not driven in this run — see the r117 table below |
| 16.0 | — | not driven in this run — see the r117 table below |
| 16.1 | — | not driven in this run — see the r117 table below |
| 16.2 | — | not driven in this run — see the r117 table below |
| 16.3 | — | not driven in this run — see the r117 table below |
| 16.4 | — | not driven in this run — see the r117 table below |
| 16.5 | — | not driven in this run — see the r117 table below |
| 16.6 | — | not driven in this run — see the r117 table below |
| 16.7 | — | not driven in this run — see the r117 table below |
| 17.0 | — | not driven in this run — see the r117 table below |
| 17.1 | — | not driven in this run — see the r117 table below |
| 17.2 | — | not driven in this run — see the r117 table below |

**‡ The four SOURCE rows are statements about a switch, not about their keys.**
`BFIN_REQUEST_KIND=2` (`bfin_sport_force_kind` in `dv-bfin_ppi.c`) overwrites
word 4 of *every* outgoing type-1 request, so the browse pane asks for SD no
matter which source key the panel reports. A source key cannot change what is
displayed while that switch is on. Those four have to be measured again without
it before their zeros mean anything.

### Sixteen inputs were never drivable, and that is where the space is

The table above ends with sixteen rows no run has ever sent, and they are not an
oversight of the runs — they were absent from the *inventory*. `BUTTON_BITS` was
taken from `0x28e44a`, which is where payload byte 18 begins. The decoder starts
at `0x28e1ae`, and before byte 18 it reads three more bytes as bit sources:

    0x28e280   byte 15, bits 0 1 5 6 7          ->  +75 bits 7 6 5 4 3, +79 bit 6
    0x28e2fc   byte 16, bits 0..7               ->  +75 bits 2 1 0, +74 bits 4 1 2 6 7
    0x28e39a   byte 17, bits 0 1 2              ->  +86 bit 5, +74 bits 3 0, +72 bit 1

**So the board decodes 38 button bits and eight analogue values, not 22 and
seven.** Seventeen inputs — sixteen bits and payload byte 14 — had no verb and
no plan entry, and that matters precisely because `r115` drove all seven old
analogue fields and `r116` all 22 old bits and both returned **zero**
attributable changes. The missing display changes cannot be in what has already
been proven empty; they can be here.

It was found from both ends on the same afternoon: reading the decoder forwards
from its entry, and reading MAIN's own 66-arm panel simulator at `0x1010a4`
backwards from its jump table. The simulator is worth keeping in mind as a
second opinion on *meaning* rather than just on inventory — it is the firmware's
own statement of what each control does, arm by arm.

## The buttons in the r117 world — the sixteen that had never been sent

`r117` (865 s, `plan keys` over payload bytes 15..17 plus byte 14 as a ramp,
20 888 requests, 24 of 24 commands acknowledged, sampler 862 ticks to t870.7, no
`INCOMPLETE`) is the run that closes the inventory. **All 24 windows are proven
no-ops and `frame_delta windows` exits 0.** With `r116` before it, every one of
the 38 decoded button bits now has a result on the same binary.

The byte-14 ramp arrived: the monitor reads `0x04fe2a44 = 0x000000ff` at the end
of the run, so the encoder field walked 1, 3, 7, 15, 31, 63, 127, 255 and the
prefix halfword at `0x04fe2af8` is 0. A zero here is a statement about the
encoder, not about the channel.

| payload | driven in | verdict |
|---|---|---|
| 15.0 | r117 | no-op, proven |
| 15.1 | r117 | no-op, proven |
| 15.5 | r117 | no-op, proven |
| 15.6 | r117 | no-op, proven |
| 15.7 | r117 | no-op, proven |
| 16.0 | r117 | no-op, proven |
| 16.1 | r117 | no-op, proven |
| 16.2 | r117 | no-op, proven |
| 16.3 | r117 | no-op, proven |
| 16.4 | r117 | no-op, proven |
| 16.5 | r117 | no-op, proven |
| 16.6 | r117 | no-op, proven |
| 16.7 | r117 | no-op, proven |
| 17.0 | r117 | no-op, proven |
| 17.1 | r117 | no-op, proven |
| 17.2 | r117 | no-op, proven |

## The analogue fields in the r115/r117 world — the eight, one row each

These eight were driven and scored like the buttons and then written up **in
prose**, three sections below, while the 38 bits each got a row. That asymmetry
is not cosmetic: it is why `plan keys` could cover "the manifest" and leave the
analogue half out, and why the operator window could offer 38 controls and one
spinbox. A row per input is the shape that makes a missing one visible, so the
same measurements are restated here as one.

Nothing new was measured for this table. `r115` swept fields 0..6 and `r117`
ramped field 7 (payload byte 14) through 1, 3, 7, 15, 31, 63, 127, 255; the
monitor read `0x04fe2a44 = 0xff` at the end of `r117`, so the encoder arrived
and its zero is a statement about the encoder.

| field | payload | driven in | verdict |
|---|---|---|---|
| field0 | 2 | r115 | refused by a guard: `CHANGE NOT PROVEN AFTER THE INPUT`, 813 B against a self-baseline of 808 — not a result either way |
| field1 | 3 | r115 | no-op, proven |
| field2 | 4/5 | r115 | no-op, proven |
| field3 | 6/7 | r115 | no-op, proven |
| field4 | 8/9 | r115 | no-op, proven |
| field5 | 10/11 | r115 | no-op, proven |
| field6 | 12/13 | r115 | no-op, proven — **position only; bit 15 has never been sent** |
| field7 | 14 | r117 | no-op, proven — the encoder; arrived, `0x04fe2a44 = 0xff` |

Seven proven no-ops, one refused by a guard, zero attributed — the same tally
the prose carries. The one word that is new is **position only** in the field6
row, and it is not a re-reading of `r115`: `rotary` walks one count per panel
exchange, so no run driving field 6 with it has ever come within 32 000 counts
of bit 15. See the chapter at the top of this file.

## The buttons in the r118 world — the four SOURCE keys, switch removed

`A-002` was right that `BFIN_REQUEST_KIND=2` makes any statement about a source
key a statement about the switch: `bfin_sport_force_kind` overwrites word 4 of
*every* outgoing type-1 request, so the browse pane asks for SD whatever the
panel reports. `r118` (380 s, **without** the switch, 8 917 requests, four
presses at t222/247/272/297, all acknowledged) re-drove all four.

**The reservation was real and it was not the reason.** All four are proven
no-ops without the switch too, `frame_delta windows` exits 0, and the run's own
self-motion ended at t198.4, so all four windows sit in the quiet. `r120` drove
`19.3` and `19.1` again with `BFIN_MAIN_LINK_DUMP` and got the same two zeros —
plus the capture that explains them.

| payload | driven in | verdict |
|---|---|---|
| 19.0 | r118 | SOURCE LINK; no-op, proven — without `BFIN_REQUEST_KIND` |
| 19.1 | r118, r120 | SOURCE USB; no-op, proven — without `BFIN_REQUEST_KIND` |
| 19.2 | r118 | SOURCE SD; no-op, proven — without `BFIN_REQUEST_KIND` |
| 19.3 | r118, r120 | SOURCE DISC; no-op, proven — reaches MAIN, word 18 follows |

These four are the rows the two gates explain most directly. `r120`'s capture
shows word 18 tracking both presses in the right order and word 26 never leaving
`0x1000`, so the GUI was told which source is selected and, in the same record,
that no source has a medium. `0xb9b5d6` returns 0 for that source, the router's
only arm for 0 is screen 0, and the edge that would have run the router at all
was never sent. A source key on this record has nowhere to go.

## The buttons in the r133/r134 world — screen 5, and an attribution that survives

The first display change in this project that passes every check *including the
trace*. `r133`: `press 19.1` becomes key code 6, the dispatcher hands it to the
media-state router `0xb9b706`, and the router switches to **screen 5**, the
library. `runs/r133/before-t208.png` (wait platter, `NO USB`) against
`after-t377.png` (platter gone, six empty list rows):

    19.1-sd2   83671  t0208.9.ppm -> t0377.5.ppm  window 1.0 s
                      the change is at t377.5, 0.7 s after the input
                      box 2,4..480,133   self-baseline 808 B
                      key dispatcher R2=6 at t373.9

`r134` reproduces it at 0.8 s with `R2=8`. The key map is
`runs/r133/key-table.txt`: the jump table `0xb2f2e0`, index `R2−1`, sends
keys 5/6/7/8 to four byte-identical arms that all call the router.

| payload | driven in | verdict |
|---|---|---|
| 19.1 | r133, r134 | **changes the display**: key 6 → router → screen 5. 83 671 B, 0.7 s, dispatch traced |
| 19.3 | r134 | key 8 → router → screen 5, 83 671 B, 0.8 s, dispatch traced |
| 19.0 | r133 | key 5 → router; its window has no dispatch, so not a result either way |
| 19.2 | r133 | key 7 → router; its window has no dispatch |
| 20.0 | r132, r133 | key 1, own arm `0xb9b9f0`. Its 4 582 B in `r133` has **no dispatch** and is not a result; `r132` dispatched it at t298.4 |
| 17.0 | r132, r133 | key 16, and its arm is a plain `RTS` — **a no-op with a reason**, which is the strongest form of one and the only such row in this file |

**`17.0` is worth copying as a method.** A no-op backed by the instruction the
key runs into needs no window, no mask and no baseline, and it cannot be
undermined by a later finding about the screen. Where a bit's arm can be read,
read it.

## The rotary — driven on the machine in `r117`, and it is a no-op too

**Status: all eight analogue values are now driven and scored.** `r115` swept
fields 0..6 and `r117` ramped field 7 (payload byte 14, the select encoder)
through 1, 3, 7, 15, 31, 63, 127, 255. Eight windows, seven proven no-ops and
one refused by a guard, zero attributed. The encoder arrived — the monitor reads
`0x04fe2a44 = 0xff` at the end of `r117` — so this is a statement about the
encoder and not about the channel.

The target is **every CDJ key and the rotary -- left, right and press**.
Left and right are the same field walked in either direction and the
field answers nothing; the press is one of the 38 bits, all of which are now
proven no-ops. **The rotary is no longer the place the missing attributions can
hide** — that reading was correct while seventeen inputs had never been sent,
and `r117` sent them. See the two gates at the top of this file for where the
path actually stops.

The rest of this section is the reasoning that found the field, kept because it
is how a control was identified without spending a run on a sweep.

The obstacle recorded here for months was the *host* side: `CDJ_PANEL_KEYS` only
ORs button bits, so no schedule could ever move an encoder. That is gone.
`rotary <field> <delta>` sets a target and the applier walks towards it **one
count per panel exchange**, which is what an encoder looks like; `analog <field>
<value>` jumps instead, for the cases where a jump is wanted.

The seven fields, as `0x28e1d6` splits payload bytes 2..13 — two 8-bit fields
and then five 16-bit **big-endian** pairs:

| field | payload bytes | width | lands at |
|---|---|---|---|
| 0 | 2 | 8-bit | `0x04fe2a20` |
| 1 | 3 | 8-bit | `+4` |
| 2 | 4..5 | 16-bit BE | `+8` |
| 3 | 6..7 | 16-bit BE | `+12` |
| 4 | 8..9 | 16-bit BE | `+16` |
| 5 | 10..11 | 16-bit BE | `+20` |
| 6 | 12..13 | 16-bit BE | `+24` |

| input | driven | state |
|---|---|---|
| rotary left | **channel yes, machine yes** (`r117`) | ramps down and wraps (`2, 1, 0, 0xffff, 0xfffe`) — measured on the host. It is **field 7, payload byte 14**, which is why `r115`'s sweep of fields 0..6 could not find it. |
| rotary right | **channel yes, machine yes** (`r117`) | ramped 1→255, arrived (`0x04fe2a44 = 0xff`), **no-op, proven** over 8 windows |
| rotary press | **yes** (`r116`, `r117`) | it is one of the 38 bits, and all 38 are now proven no-ops — so whichever bit it is, its row exists and reads zero |

**`r115` swept all seven old fields and the answer was no** — six proven no-ops
and one caught by the `CHANGE NOT PROVEN AFTER THE INPUT` guard, five bytes above
its own baseline. That is the outcome this section provided for ("a sweep in
which all seven windows show zero is a real result"), and it is a real result:
the select encoder is in none of payload bytes 2..13.

It is in byte 14, and two independent readings say so:

- **The decoder.** `0x28e1d6` does not stop at byte 13. At `0x28e26e` it reads
  byte 14, adds the sign-extended halfword at `0x04fe2af8` and stores the sum at
  `0x04fe2a44`.
- **MAIN's own panel simulator.** `0x1010a4` is a 66-arm dispatcher behind a
  `braf` table at `0x101128`, one control per arm. Arms 51..56 set fields 0..2 to
  0 or 255; 59/60 move field 4 by ±200 with field 5 pinned at 300; 62/63 step
  field 6 by ±10 within [0, 390]. Those are levels and limits. **Arms 64 and 65
  do nothing but `+1` and `-1` on the halfword at `0x04fe2af8`**, and no other
  arm does. An arm pair stepping a signed counter by one in each direction is an
  encoder.

That simulator is worth remembering as an instrument: it is the firmware's own
statement of what each control *means*, and it costs no machine time to read.

## What is measured

Two runs of 15 button presses each (r024, 5 s apart; r026, 15 s apart) against a
**control run with no buttons** (r025), all `--no-peer`, same binary, same card,
frames sampled every second with `boot_vm --frames`:

| | control r025 | keys r024 | keys r026 |
|---|---|---|---|
| `TRACK` | `00` | **`01`** | **`01`** |
| `REMAIN` | shown | **gone** | **gone** |
| tempo range | `±10` | **`±16`** | **`±16`** |
| `MT` (master tempo) | dark | **lit red** | **lit red** |
| staircase graphic | absent | **present** | absent |

Four of those five reproduce across both key runs and appear in neither control
frame, so **button input demonstrably changes the display**. The frame-to-frame
byte delta of the screen's own animation is a median of **808** bytes in every
run, which is the noise floor any attribution has to beat.

## Doing it again — `tools/cdj_main/frame_delta.py`

Every number above was computed by hand, and the review that followed
each need the same arithmetic over seven and six windows. It is one command now:

    python -m tools.cdj_main.frame_delta windows <frames> \
      --mask runs/anim-mask.bin --look <dir> 150:18.1 175:18.2 …
    python -m tools.cdj_main.frame_delta mask <control-frames> mask.bin --from 120
    python -m tools.cdj_main.frame_delta pair before.ppm after.ppm --mask …

**It is calibrated, not merely written.** Given the six pairs
`runs/manifest.jsonl` recorded for r026 and the r048 mask, it returns
372, 364, 906, 749, 885 and 749 — the six rows of the table above, to the byte
(`tests/test_frame_delta.py`). Agreeing roughly would have meant either the old
numbers or the new code was wrong with no way to tell which.

The mask format is confirmed by the same exercise: one byte per channel byte,
`1` where the pixel animates, `0` where it is evidence; `anim-mask.bin` has
349 835 zeros, which is the 95.3 % figure quoted above.

### A proven no-op, and a hole, are not the same row

The sampler writes a file only when the bytes changed — right for disk, fatal
for evidence. A window over a stretch where the screen stood still contains no
file at all, and `no frame on one side` used to be printed for **two opposite
findings**: *this input changed nothing* and *this input was never measured*.
An expected no-op is allowed but must be **proven**, and a no-op
is only provable if a hole can be told from it.

`boot_vm` now writes `index.tsv`, one row per sampler tick with a status, and
`frame_delta windows` reads it:

| the index says | the row says |
|---|---|
| every tick across the window is `new`/`same`, and none is `new` after the input | **`no-op, proven`** with the tick count — stronger evidence than any byte compare |
| a tick reports `error:…` or `empty` | **`NOT MEASURED`**, with the reason |
| no tick covers the window at all | **`NOT MEASURED`** — nobody was looking |
| a change did occur | the ordinary masked byte compare |

`NOT MEASURED` rows are counted and the command exits non-zero, because a row
that cannot be scored must not look like a row that was. That is the same rule
as `WINDOW TOO WIDE`.

Ten seconds is the stretch across which stillness has to hold, because ten
seconds is the attribution limit the method already uses.

Runs before `fbde2eb` have no index — `r026` among them. Those still evaluate,
but the header says the two cases cannot be separated, the same honesty as a
missing `--align`.

### The window is counted in content changes, not file timestamps

The same index answers a second question the first pass did not ask. A file's
name says when a frame *appeared*; it says nothing about how long it stood,
because the sampler writes nothing while it stands. So a pair of files twenty
seconds apart looks like a twenty-second window even when the screen changed
once, in the last second of it.

A `same` tick is not silence. The sampler read the live capture, compared it
against exactly the bytes of the last written file, and found them equal — it
is a **sighting** of that frame at that moment. A run of them therefore moves
the earliest possible time of the next change forward, from the file's
timestamp to the last sighting before the `new` that ends the run. An `error`
tick does not extend a hold but does not end one either: a later `same` sees the
same bytes again.

That is what cost `r096` its best row. Bit 18.3 carried 498 bytes and was
reported over a **19.3-second** window because the previous *file* was 19.3
seconds old — while the index held eleven `same` ticks in between. The screen
had stood still since t215.5, the press was at t227.0, and the change is at
t234.8: a **7.8-second** window, and a result.

Two properties keep this from being a looser rule rather than a sharper one:

- **The span can only shrink.** With no index the last sighting is the file
  itself and the arithmetic is exactly the old one, so `r026` evaluates
  unchanged and the six calibration numbers are untouched.
- **A late change stays late.** `r096`'s 18.4 and 18.7 share one change at
  t317.8. The index pins that moment just as precisely — 65.8 s and 15.8 s after
  their presses — and both stay `WINDOW TOO WIDE`. Knowing *when* something
  happened is not the same as being allowed to blame it on something.

A `WINDOW TOO WIDE` row now also makes the command exit non-zero, like
`NOT MEASURED`: a row that cannot be scored must not leave a run looking like it
passed.

Three things it refuses to let you skip:

- **`--from` is required** when building a mask. The steady phase has to be a
  decision, and the report prints what fraction was excluded and warns past a
  fifth — so a repeat of the whole-run mistake shows up in the first line of
  output rather than in a conclusion.
- **The noise floor is measured, not guessed.** Every third steady frame is held
  back; the mask is fitted on the rest and the worst residual over the held-out
  frames is the floor, written to a `.json` beside the mask and printed at every
  later evaluation. Over the *fitted* frames the residual is 0 by construction,
  which is reported separately as a self check — anything else means the mask
  and the frames do not line up.
- **A window wider than ten seconds is flagged, not counted.** That is what
  turned four r076 rows into "unattributed"; the tool now says so on the line.

### Looking, not only counting

`--look` writes `<name>-crop.png` (before over after, magnified, with a margin)
and `<name>-where.png` (the panel with the changed rectangle ringed). Run over
the frames already in `runs/`, that immediately reads out what the numbers
never said:

| window | delta | box | what the crop shows |
|---|---|---|---|
| 19.0 | 906 | 444,203..478,223 | the tempo range digits, `±10` lit → `±16` lit |
| 20.0 | 885 | 114,70..406,157 | the `MT` box going from dark to lit red |

Both match rows of the r024/r026 comparison table further up, which had recorded
those changes without attributing them to a bit. So they are attributions now,
in the same tight-window masked sense as the rest of the table.

**And one of them raises a question about a name.** Row 19.0 is called
`SOURCE DISC`, from `0x28ddc8` turning a rising edge into the one-hot source
flag. But r026's own read-back ends with `source flags 0x04c084d0 = 0` — the
DISC slot never set — while the change inside 19.0's window is the tempo range.
That is not enough to rename anything: the flag could have been cleared again
with no disc present, and r026's bit order is reconstructed rather than read.
It is a thing to settle in `B-003`, and it is exactly the kind of mismatch that
only shows up when the picture is looked at next to the number.

`acceptance.py`'s point 3 check compares the two named frames byte-wise, which
is a weaker test than the above — the raw pairs also differ in the 4.7 % that
animates. The evidence for the claim is the masked analysis in this file; the
recorded pairs are the frames it was computed from.

## The canonical plan — `panel_control plan`

Everything above was stitched together from several runs under the 16-entry
ceiling of `CDJ_PANEL_KEYS`, at 5–15 second spacing, which is why four rows say
"window too wide" and six bits are unattributed. The control channel has no
ceiling, so point 3 can be driven from one plan instead:

    python -m tools.cdj_main.panel_control plan keys
    python -m tools.cdj_main.panel_control plan rotary-sweep
    python -m tools.cdj_main.panel_control plan rotary --field N

Each prints two commands — the session that drives the machine and the
`frame_delta windows` invocation that judges it — **complete, to be pasted
verbatim**. They come from the same entries, so a name, a time or the transcript
path cannot be right in one and wrong in the other; tests parse both printed
lines back through `shlex` and check that the session's `--transcript` and the
evaluation's `--align` name the same file.

That last part was missing at first and is worth stating as the failure it was:
the lines are meant to be copied, so anything left off them is left off the run.
A session printed without `--transcript` leaves no anchor, and by the time the
unaligned evaluation says so the run is over and only `--shift` by hand remains.

Every plan opens with **`5:"ping"`**. It touches no payload byte, so it cannot
disturb a measurement, and it is driven but not scored — there is no window for
it. It is there because of `r094`, where a `keys` run whose first command was at
t=150 delivered **nothing at all**: the channel sat idle from the moment it
opened and was gone by then. `r091`, which survived, had a command at t=20 and
went quiet afterwards. The probe makes that difference part of the plan, tells
the next run whether the channel was ever alive — which `r094` could not
distinguish from one that died later — and, if an idle channel is the trouble,
is also the way around it. A split plan gets one probe per part; each part is
its own run.

| plan | inputs | run needs | covers |
|---|---|---|---|
| **`coverage`** | **49** | **1 570 s** | **the whole board in one run — the only shape in which every row of this file shares one HEAD and one binary** |
| `keys` | 38 | 1 295 s | every decoded button bit, 25 s apart, first at t300 |
| `rotary-sweep` | 8 | 545 s | every analogue field |
| `rotary --field N` | 2 | 395 s | left and right of one field |

The three below `coverage` are its parts and stay for diagnosis; each prints
an `INCOMPLETE` line saying how much of the board it leaves
out. The numbers moved twice: 22 → 38 inputs when bytes 15..17 were found in
the decoder, and t210 → t300 when four traced runs showed no press had ever
reached the key dispatcher before t150.

The spacing is 25 s because 5–15 s demonstrably was not enough. **The first
input moved from t150 to t210 after `r113`**, and the reason is the one that
cost it two rows: reaching the browse phase is not the same as standing still
afterwards. The GUI reaches it at ~115 s, but `r113`'s screen went on repainting
until **t194.8**, and a window inside that measures the repainting. t210 is that
plus 15 s.

It is not a safe constant and the plan says so: `r112`, same switches, settled at
t121.3, so where the churn ends moves by 70 s between runs. What makes this
survivable is that `frame_delta` now **refuses** a row measured inside the churn
instead of scoring it, so the cost is a lost row rather than a wrong one. A run
that comes back with `SCREEN BUSY` rows should be re-planned with `--start` past
where that run's own index says the churn ended.

`--parts 2` splits `keys` into two 530-second runs; that is **1060 s in total
against 805**, so splitting costs time rather than saving it and is there only
if a thirteen-minute run is impossible for another reason. Shrinking the spacing
to fit is the one thing not on offer.

**The plan names no mask any more.** Both directions have failed: `r093` lost its
answer to a mask from a world with a spinning platter, and `r113` invented two
rows with no mask on a screen that was moving. `r112` cannot supply a
replacement — `frame_delta mask --from 150` finds zero frames in it. What stands
in its place is the self-baseline out of the run's own input-free gaps, which is
in the right world by construction. `--mask` is still there for the day a control
run genuinely in this state exists.

### The two clocks, and why they have to be measured

A session's seconds count from the moment the control channel opens; the frame
sampler's file names count from when `boot_vm` started. **In `r091` those
differed by about 45 seconds** — the channel came up after the 40-second mark.
At 25-second spacing an error that size slides every window nearly two
positions, so each key would be attributed to its neighbour: consistently, and
therefore invisibly. A uniformly wrong table looks exactly like a right one.

So the plan's connect allowance is **45 s, from `r091`**, not the 25 s estimate
that stood here before — and more to the point, the offset does not have to be
assumed at all:

    python -m tools.cdj_main.panel_control … session --transcript <evidence>/session.txt
    python -m tools.cdj_main.frame_delta windows <frames> --align <evidence>/session.txt

The transcript records the epoch at which the channel opened; the frames carry
theirs in their modification times. `--align` subtracts the two and prints the
shift it measured. Without it, `frame_delta` now says on every run that it is
unaligned rather than quietly assuming zero.

`--align` needs the sampler's own directory: copying frames without preserving
modification times destroys the anchor, and the tool warns when the per-frame
estimates disagree. `--shift SECONDS` is the manual fallback for that case.

A plan whose last input would fall past the end of the run **refuses to be
generated**. That is r088's failure turned into a check: it lost a command at
300 s off the end of a 340-second run, and nothing said so until the frames came
back with a window nothing had driven.

The rotary *press* is not a separate drive: if it is a button it is one of the
22, and if it is analogue the sweep has it. Which one it is, is a result of
these runs rather than an input to them.

## The operator window

`tools/cdj_gui/view_ui.py` puts a click surface around the frame:
`BROWSE`/`TAG LIST`/`INFO`/`MENU` above it, `LINK`/`USB`/`SD`/`DISC` to its
left, all 38 decoded bits to its right, and the eight analogue fields below it —
one row each, with a detent pair, a slider and an exact value, plus a `touch`
box on field 6. None of them is drawn *into* the 480x234 panel —
that rectangle is the LCD and nothing else, and `tests/test_panel_layout.py`
asserts the displayed frame is the capture's top-left 480x234 pixels byte for
byte.

    python -m tools.cdj_main.view_vm --sd <card.img>          # channel on 5984
    python -m tools.cdj_main.view_vm --sd <card.img> --no-control
    python -m tools.cdj_gui.view_ui --coverage                # no window, no run

The verdict column is read out of this file at startup and shown beside each
input, so there is one table and not two. It reads only rows under a
`## The buttons in the <run> world` or
`## The analogue fields in the <run> world` heading, the newest section wins,
and **the run name is shown with the verdict** — `18.1  measured, nothing moved
[r096]`. Without that tag the window would present r026's 372 bytes as a fact
about a screen r026 never saw, which is the whole hazard the section at the top
of this file is about. `tests/test_panel_layout.py` pins both the gating and the
tag.

`--coverage` prints every control, the input it claims and the protocol line it
would send, and **exits non-zero if any of the 46 has no control** — which is
the check the window failed silently for as long as it existed. The same
comparison runs as a test, and every one of those lines is driven through the
compiled `cdj2000_input.c` in `tests/test_input_channel.py`, so "clickable" is
measured at the payload rather than at the widget.
