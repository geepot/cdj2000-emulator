# Stock PCM execution evidence — 2026-09-09

This is a **replay of previously captured genuine stock execution**, not a
fresh successful playback run, sixteen-channel audio or device timing proof.

## Verified initial stereo unpack

Input: `runs/nxs-native-load-mvd-1/dsp-checkpoints/00000000000000002776.cdjdsp`
and that run's complete `dsp-events.jsonl`. Checkpoint 2776 precedes the real
LPCM command at event 204662. The original run loads a synthetic 10-second
stereo S16LE 44100 Hz WAV through the native SD/MAIN/Blackfin/DSP path.
See NXS_LINK_LOADING.md for the original load evidence and limitations.

The observer runs strict C674x execution with original ordered MAIN events,
reads RAM only, and neither changes registers nor produces peripheral replies.
`--connected-stops 16` stops at the sixteenth verified original DSP boundary,
not an arbitrary mid-activation limit. Checkpoint, transcript, source and
binary hashes are recorded in the replay manifest. The unconsumed transcript
suffix is not validated by this bounded experiment.

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-native-load-mvd-1/dsp-checkpoints/00000000000000002776.cdjdsp \
  runs/NEW_PCM_OBSERVATION --steps 30000000 --connected-stops 16 \
  --events runs/nxs-native-load-mvd-1/dsp-events.jsonl \
  --observe-pcm --verify-repeat
.venv/bin/python -m tools.cdj_dsp.pcm_evidence runs/NEW_PCM_OBSERVATION \
  runs/nxs-test-media-1/tracks/TESTTONE.WAV
```

Recorded result: `runs/nxs-pcm-observe-4`, 16 verified connected stops,
exact trace/coverage/final-state/memory repeat. Trace SHA256:
`3bb564c9c509eb3f5b7ba3de199d6843579cbfc09743d765372b70b3db509877`.
WAV SHA256: `1f276d8145fd7b0a5eada120340bf0c47aef7c01886757d9a597bc92ade37a3a`.
Protected SD image SHA256:
`dea1b2bf2edbf65ebee0709ce47d720bda77c6e3bf160c1fa34b5eddb3958b76`.

At `c003c698`: A4=11800200, B4=0 (bank), A6=0..3 (block), B6=2,
B14=11806900. State raw base `[1182dda0+6 words]` is 118381e0;
channel/depth/rate words at 1182de1c/20/24 are 2/2/0x2c.

| Block | Selected kernel | Input A4 | Output B4 | Kernel A6 |
| --- | --- | --- | --- | ---: |
| 0 | c003c398 | 118381e0 | 11800200 | 1176 |
| 1 | c003c398 | 11838b10 | 11800200 | 1176 |
| 2 | c003c398 | 11839440 | 11800200 | 1176 |
| 3 | c003c398 | 11839d70 | 11800200 | 1176 |

Return PC/B3 is c0049e6c. At return, output planes are 11800200 and
11800fc8 (separation 0xdc8). Each holds 588 **float32** samples, exactly
source S16 / 32768. They are not Q31. All 2352 source bytes per block match
the WAV; all 588 samples in both planes per block match, totaling 2352
stereo frames. Output buffers are reused across these calls.

The original tone has identical L/R channels: this establishes conversion
and per-plane values but cannot distinguish swapped/duplicated channels.
The checker's synthetic regression uses distinct channels and rejects
duplication; that host test does not add genuine distinct-channel evidence.

Observer records include the requested 1182dda0..1182de28 state words,
B14 RAM, input/output pointers and RAM spans. A null span means unavailable
RAM, not zero data. PC observations can include fall-through; they are not
automatically calls. Idle cycles are excluded. At most 64 observations are
recorded. Return observations are paired with the observed c003c698 entry.

Unproven: bank 1, full bank/block range, metadata modes, ownership/release
protocol, input refill lifetime, downstream processing, mono/48k/S24 cases,
continuous playback, McASP output, true stereo separation and DSP headroom.
No changes to MAIN parser gates, DSP ABI or firmware payload were made.

Validation: 492 passed / 29 optional skips with the TI toolchain and
`CDJ_ETH_QEMU_TEST=1`; focused replay/checkpoint tests 19 passed, evidence
checker tests 2 passed. The boundary regression confirms an explicit stop
limit saves a verified checkpoint without consuming a following reset event.

## Fresh attempt and first boundary

`runs/nxs-pcm-native-1` uses stock MAIN, the same protected SD fixture, strict
DSP, stopped McASP clock, fresh-only link transport, and an earlier physical
card insertion (`--sd-insert-seconds 1`). Command:

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_PCM_NATIVE \
  --seconds 900 --frame-interval 30 --port 6080 \
  --sd runs/nxs-test-media-1/test-track.img --sd-insert-seconds 1 \
  --fresh-link --trace-link-tx --trace-media --qemu-sync-profile
```

Physical SD selection pulses used `panel_control --port 6084 press 19:08
--hold-ms 200` (three retries used `--repeat 3 --gap 1`). After more than
eight minutes, the screen still said NO CARD; no successful native LOAD or
PLAY was obtained. The diagnostic was interrupted through its launcher,
which finalized artifacts; an empty result object is not a successful boot
or playback result. No unknown instruction was bypassed and no ready-state
was forced.

The SD reset model explicitly delays insertion by 20 virtual seconds by
default because the firmware poller arms its mount gate while the slot is
empty. One-second insertion invalidated that established test setup. This
is a plausible explanation, not a newly verified hardware fault. The next
fresh control must restore the documented default insertion delay before
changing SD behavior. Its immediate blocker is card detection/mounting,
not an observed DSP instruction fault. The verified replay above supplies
the narrower unpack contract without claiming this fresh attempt succeeded.
