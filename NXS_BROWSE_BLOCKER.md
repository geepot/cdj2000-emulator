# NXS browse blocker follow-up (2026-09-10)

The stock path passes the reported browse blocker in
`runs/nxs-browse-blocker-control-2`. It first returns NO CARD, then lists
TESTTONE.WAV through genuine MAIN/GUI traffic. No firmware memory values,
media-ready flags, link replies or LOAD requests were fabricated.

## What the blocker note misidentifies

The research handover `docs/dante/emulator-browse-blocker.md` records MAIN
media-manager mode `0x04cf2180 = 2` and table entry `0x04cf2994 = 1`. Those
are also intermediate states in this control. A set mount latch, populated
volume geometry and completed FAT reads do not establish that the higher-level
media manager has finished making the card available to the browser.

The control later reaches mode **3**, table entry **2**, and latch **1**.
`no-card-branch.json` captures those values on the browser's common readiness
path after it has started listing the card. These are observations of this
exact stock image, not instructions to overwrite the globals.

Halfword **26 stays 0x1000 while browsing works**. The legacy CDJ-2000 bit
layout and addresses `0x0489bd68/6c/70/74` must not be used as an NXS readiness
oracle. This is independently corroborated by the previously verified
`runs/nxs-native-load-mvd-1/main-link.bin`: records 2130 and 2148 (one-based)
list SD/FOLDER and TESTTONE.WAV while the preceding status still has
halfword 26 = 0x1000. The absence of a watchpoint hit on that field does not
locate the NXS browse blocker.

## Fresh control evidence

All record numbers below are one-based SPRX receive records; MAIN times come
from `main.log` and are **virtual seconds**, not the GUI's wall clock.

| Observation | Receive record | MAIN time | Prior status halfword 26 |
| --- | ---: | ---: | --- |
| NO CARD | 1002 | 171.9085 | 0x1000 |
| NO CARD after FAT scanning | 1528 | 260.6782 | 0x1000 |
| SD / FOLDER | 1994 | 346.3442 | 0x1000 |
| TESTTONE.WAV | 2013 | 349.3175 | 0x1000 |
| Repeated SD / FOLDER | 2276 | 393.0451 | 0x1000 |
| Repeated TESTTONE.WAV | 2291 | 395.5563 | 0x1000 |

The transition to a real list happened before the successful debugger
breakpoint capture. It did not require a lid toggle or a new firmware build.
The timing is a measurement of this instrumented run, not a recommended
fixed sleep. The older verified load took substantially longer on its build.

Subsequent direct encoder contacts produced genuine requests:

- ENTER at MAIN t=429.0458: `0000 0001 0003 0007 0001 0000`.
- LOAD at MAIN t=458.8549: `0000 0007 0001 0000 0000 0000`.

The browser screenshots are `early-no-card.png`, `after-mount.png`, and
`after-enter.png`. LOAD request acceptance alone is not load completion or
audio playback; consult the final observation below for that distinction.

### Final observation: native load completed

`loaded-track.png` shows TESTTONE.WAV, TRACK 01, REMAIN 00:10.000.
MAIN clears the loading flag (status halfword 19 changes from 0x1200 to
0x1000). Receive records 3209 and 3228 contain the same command-5 track
duration as the previously verified load:
`0005 0001 00ff 0000 0a00 0000 0000 ae1b`.
The completed duration remains on screen at the final observation, without
a DSP fault. This verifies native stock track-load completion, not audible
playback. No audio capture was requested.

## Reproduce the path

From this repository, choose a new output directory and unused ports:

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_RUN \
  --seconds 1200 --sd runs/nxs-test-media-1/test-track.img \
  --fresh-link --trace-link-tx --trace-media --source-key none \
  --frame-interval 15 --port 7290 --qemu-sync-profile
```

The default insertion remains 20 virtual seconds. Use the actual NXS source
contact (decimal byte 19, hexadecimal mask 08), rather than the legacy named
`sd` contact in `panel_control`:

```sh
.venv/bin/python -m tools.cdj_main.panel_control --port 7294 \
  press 19:08 --hold-ms 100
```

Allow the media manager to finish. If a premature selection gave NO CARD,
reselect SD after readiness progresses. Decode **list replies**, including
64-byte payloads, rather than treating every 64-byte record as status:

```sh
.venv/bin/python -m tools.cdj_gui.decode_link_dump \
  runs/NEW_RUN/main-link.bin
```

Once the folder/track is visible, briefly click the encoder to enter the
folder, inspect the resulting selection, then click it again to load. This
control used `PanelControl.hold('17:01', True)`, 100 ms host sleep, and
`hold('17:01', False)` in a `finally` block. A long hold is not a short browser
click; queued panel presses can also have different delivered durations.

Do not add `--browse-aids`, `CDJ_SD_MEDIA_STATE`, or injected browse requests
to this control. The first two originated in legacy CDJ-2000 investigations;
native NXS ENTER/LOAD have already been demonstrated.

## Actual NXS browser check

In the stock unpacked image, the NO CARD UTF-16 string is at `0xa406f1f4`,
referenced by literal `0x04168e94` and loaded at `0x04168d96`.
`0x04168d32` tests r9 before the empty-media branches. Its common media
context check starts at `0x04168af6`, following a pointer from the browser
context's per-source slot and then examining the relevant media entry.
This is a useful upstream investigation point if a later run stays blocked
after the higher-level media manager has progressed. It is not the status
transmit buffer and not the legacy four-word table.

## Provenance and limitations

The run manifest records the complete environment and input hashes at launch:

- MAIN: `02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85`.
- SD: `dea1b2bf2edbf65ebee0709ce47d720bda77c6e3bf160c1fa34b5eddb3958b76`.
- QEMU: `2a2254bf841c988d145e5c9b7b76c405d76d2f2c9a41ce562d1a8dd7094e1753`.
- GUI simulator: `72f0d12063f29805e4e5cf31c73f1cd8ebad9a122c365c2ecf0490afd6d23458`.

Strict default DSP settings, legacy DSP scheduling, snapshot-only media
writes, no functional timing/audio switches. The run included read-only
monitor inspections and temporary debugger pauses; an initial panel connection
attempt made while MAIN was stopped timed out and MAIN was resumed. Thus host
elapsed times are perturbed. The breakpoint was removed and MAIN resumed
after the successful observation. This task did not rebuild an emulator.
However, finalization detected a concurrent change to the QEMU executable on
disk: its exit hash is
`ceda8c3f2aee29f41513db98110afc0c68bd35c008bccedac242bd51ee26fa04`.
All other input hashes remained unchanged. This is therefore an observed
successful session, not a stable-artifact build comparison or validation of
that replacement executable. Documentation/help changes made during the run
do not describe a newly executed hardware implementation. Focused decoder
and launcher evidence tests pass (52 tests).

This establishes a way past the stock browse blocker. It does not validate
the research repository's patched AmbiX image or its sixteen-channel fixture,
explain every media-manager delay, or establish audio output.
