# Strict DSP interrupt-entry interval

The late USB-run fault at checkpoint 1342 was caused by entering INT15 while
an older load into B0 was still pending. The handler's MVKH then collided
with that load. The conflict detector correctly rejected the overlap; the
missing interrupt-entry interval made the overlap occur incorrectly.

TI SPRUFE8B Figure 5-4 (page 641) places the first annulled E1 at cycle 6
and the interrupt handler's E1 at cycle 15. At this interpreter's abstraction
(current PC is the first annulled execute packet), strict acceptance therefore
inserts nine empty issue cycles. Existing cycle execution retires queued loads,
stores and arithmetic results and ticks devices during this interval. No ISR
instruction issues until the next step. This is a fixed interval, not a loop
that waits arbitrarily long to hide a collision.

IRP, priority, sticky IFR and control-state acceptance semantics are unchanged.
The existing atomic control-register transition is still an abstraction: this
patch does not model all pin synchronization, fetch stages or per-cycle TSR
field transitions. Exploratory breadth mode retains its prior minimum-drain
approximation. No new checkpoint fields or schema changes are required.

## Verification

- Architecture tests cover empty entry, writebacks due in each of the nine
  slots, store retirement, board clock ticks, pending IRQ latching without
  restarting entry, B IRP and SPLOOP return behavior.
- A synthetic result due on ISR E1 still produces `delayed-result write
  conflict`; the fail-closed detector is unchanged.
- AddressSanitizer and UndefinedBehaviorSanitizer pass for the core harness.
- Final combined-tree suite: 427 passed, 29 optional skips.
- QEMU rebuild SHA-256:
  `bcf1f4b3e64b82741b3007eccbf32a8c8235c91788ad617f67e43b4e87363367`.
  This binary also includes the independently developed persistent SD-lid fix.

Event-aware diagnostic commands (use new output directories):

```
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-usb-track-long-1/dsp-checkpoints/00000000000000001341.cdjdsp \
  runs/dsp-interrupt-entry-fixed-events-3 --steps 100 \
  --events runs/nxs-usb-track-long-1/dsp-events.jsonl --verify-repeat
```

The old transcript expects a fault after two packets at PC `008001e8`.
With the fix, both diagnostic runs `-3` and `-4` execute 100 steps with no
fault, reaching PC `c0047758`, packet 1301099600, cycle 2346860091. They
correctly exit nonzero at event 151389: the old connected fault outcome no
longer matches. This is expected semantic divergence, not a passed connected
equivalence gate. The mismatch diagnostic now prints actual PC/counters/fault
without weakening any comparison. The separate no-events standalone replay
does not inject the original interrupt and is not evidence for this fix.
Both event-aware diagnostic trace files have SHA-256
`a8564c5813ca3a2e9d158873ecf9aa4a42f32af981ee9f9e14db29ee1c266084`.

## Connected post-rebuild gate

`runs/nxs-dsp-entry-fixed-usb-1` completed 450 seconds using the protected
USB fixture, normal strict timing and the rebuilt QEMU above. It reached
1,663,099,500 packets / 2,994,849,998 cycles, beyond the earlier failure's
1,301,099,502 packets / 2,346,859,933 cycles. Recorded stops consist of
1,663 budget boundaries and 13 HINT yields, with no DSP fault. GUI exit is 0
and all input hashes are unchanged at exit. The USB source contact was pressed
and released, with no ordinary buttons left held.

This is bounded connected regression evidence, not full DSP ISA/timing parity,
track-loading or audio-playback validation.
