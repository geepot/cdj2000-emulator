# Native motion blocker: active-low REV contact

The all-zero panel default engaged reverse. At the beginning of the track,
the genuine DSP boundary logic correctly blocked further reverse movement.
Driving the actual REV contact high cleared the gate and produced both native
GUI counter decreases and DSP stream-head advancement. Details and limits
follow below.

## Observed result

`runs/agent-play-sdram-fixed` loaded the generated 10-second 44.1-kHz stereo
PCM fixture and accepted PLAY without a DSP fault after the SDRAM alias fix.
Its transport probe nevertheless saw 116 fresh status records over 30 seconds
with the remaining counter fixed at 1500 frames. This is not playback success.

The paused, read-only HPI capture in that run's `live-dsp-full/` directory
contains the complete L2 image and first SDRAM PCM record. `capture.json`
records the pause, saved HPIA, reads, restoration, and resume. The stream's head
is 0 while its tail/count reached 639/640; the earlier fault capture had
head/tail/count 0/199/200. MAIN is supplying records while DSP consumption has
not advanced the head. The first record contains nonzero float samples, but
this investigation does not establish their audio correctness.

## Executed firmware gate

The production alias-fixed replay of the genuine fault checkpoint executes
these functions. Bounded register observers, their source, and input hashes
are preserved in `runs/agent-play-sdram-fixed/motion-diagnostic/`. They do not
alter firmware, CPU registers, or memory. Replay observations are not fresh
native integration evidence.

With B14 = `0x11806900`:

- Mode at `0x1182dde8`, ISR state `[B14+824]`, and host answer
  `0x11837bf8` are all 3.
- Mode 3 dispatches to `0xc0004758` through the table at `0x11805e68`.
- Mode 3 inherits halfword `[B14+444] == 1` in the observed states. Its
  optional initialization path `0xc00047c4..0xc00047e8` would call
  `0xc0033314(0)` if `0x1182de7c == 1`, but this input is zero in the live
  capture. That optional path must not be credited as the observed setter.
- `0xc0004828` calls the processing function `0xc0008804`, which calls
  `0xc00102c8` on the observed path.
- `0xc00105a0..0xc00105d4` combines `[B14+444] != 0` with
  `0x1182de44 == -1`. In the captured state, branch `0xc00105d4` takes the
  path to `0xc0010684`, skipping phase advance and consumption.
- The processing function returns a consumed count of 0 (observed at
  `0xc0008c24` and `0xc000483c`). The stream head stays fixed.
- `0xc0003568..0xc00035a0` separately uses the same gate to mute output.

Both the genuine fault checkpoint and the next run's boot checkpoint contain
identical instruction bytes at `0xc00102c8`, `0xc000ff34`, and
`0xc0004758`. These are coherent executed code regions in those inputs.
Classification as sample data without identifying a different capture or
address mapping is not evidence of an overlay.

## Important interpretation limits

`0x1182de44 == -1` is not evidence of an invalid PCM record. The producer at
`0xc004813c..0xc0048220` derives this auxiliary slot selector from host word
`0x11837bc8`; zero means no selected auxiliary slot and yields -1. The host
word is zero in the native capture.

`0x1182de50 == 0` is not evidence of a failed per-block motion calculation.
This is a host motion/seek delta, acknowledged and cleared by `0xc003a9b4`.
The mode-3 routine revisits its boundary flag in the nonzero-delta paths.

The Q20 rate is `0x100000` and the native float rate fields are 1.0. An
instrumented replay shows the earlier rate ramp from -1 toward +1 executing
its floating-point arithmetic coherently. No ISA or zero-ADC slew defect
has been established.

The legacy behavioral DSP model describes request 3 as PLAY and request 2
as pause, but this comment is not an independently verified NXS DSP mode
mapping. Native modes 2/5 take `0xc0003858`, including readiness checks in
`0xc0000430` that can clear the boundary flag. The next native capture identifies a stronger lead: see below.

No firmware gate, DSP state, or counter was patched to bypass this condition.

## Loaded-state comparison, next native run

`runs/agent-play-checkpoints/dsp-checkpoints/00000000000000000004.cdjdsp`
was captured after loading and before the first PLAY contact. It has native
mode 2, direction `0x1182ddf0 = 1`, host control `0x11837bc4 = 0x81000000`,
and boundary flags 444/446 both set at stream head 0. The direction input
causes `0xc000a038` to select a negative rate. The next first-PLAY transition
in the earlier run instead produced mode 3 and direction 0. This suggests
that native mode 2 is playing, mode 3 is paused, and the fixture initially
auto-plays backwards against its start boundary.

`panel_control.py` documents raw byte 15 bit 1 (REV) as active low for the
legacy MAIN decoder, and the NXS contact-name map inherits this position.
The emulator's default all-zero panel frame therefore warrants an NXS
polarity check and a real contact-level test; a default of zero is not
automatically a neutral panel. This is a control-input lead, not a DSP
state-bypass proposal.

Correction to an intermediate hypothesis: host control bit 24 maps to
`0x1182de80`, while bit 25 maps to `0x1182de7c`. The latter is zero in both
the full live capture and the loaded checkpoint. Mode 3 therefore inherits
the already-set boundary gate; it did not set it through the optional
`0xc00047e8` path.

## Native contact test confirms the cause

In the same run, the real panel contact byte 15 bit 1 was driven high (the
physical REV switch's neutral level). Fresh native status records then
decreased from 1500 to 1496 remaining frames across five samples, with two
decreases. No DSP state or memory was changed by the debugger.

Checkpoint 7 (`b452ca6f002daa510b47a0da9f9e02751675bcb52dd24d09c2d2f07909208785`)
records the matching DSP transition:

| State | Checkpoint 6, REV low | Checkpoint 7, REV high |
|---|---:|---:|
| Native mode | 2 | 2 |
| Direction (`0x1182ddf0`) | 1, negative | 0, positive |
| Host control (`0x11837bc4`) | `0x81000000` | 0 |
| Boundary flags 444 / 446 | 1 / 1 | 0 / 0 |
| Stream head | 0 | 4 |
| Stream tail / count | 319 / 320 | 399 / 396 |

The all-zero default panel frame selected reverse. The loaded deck was
already in mode 2, attempting to play backwards against the track start.
The first PLAY contact switched to mode 3, the stationary pause behavior;
a second contact restored mode 2. For these observed NXS states, status
word 17 bit 9 is set in mode 2 and clear in mode 3. This replaces the
earlier interpretation imported from the legacy behavioral DSP model.

The native counter change and DSP head change establish actual transport
progression under functional McASP scheduling. They do not establish host
audio quality, real-time rate, or complete architectural timing. Subsequent
pause stayed fixed at 1492 over 31 fresh samples; resume also stayed fixed at
1492 over 118 fresh samples. Thus this was brief movement, not sustained
playback success. The independent EDMA completion loss below explains the
next observed stall.

The SDRAM alias fix remains independently relevant: the paused-mode call
at `0xc0004818` requests counter196 minus 26 for pre-roll, independent of
the reverse input. Mode 3 initializes that counter to zero, so entering
pause at the track start can encounter the same speculative predecessor
metadata read. The D-half alias fidelity limit remains documented.


## Sustained-motion stall: EDMA completion loss

The subsequent stall is traced to an unrelated completion being cleared by
the EDMA ICR read/OR/write helper. See
[native-play-edma-completion.md](native-play-edma-completion.md) for native
log evidence, executed firmware gate, tests, and the write-only readback
compatibility limit.
