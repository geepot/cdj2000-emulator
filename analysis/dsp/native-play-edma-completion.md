# Native playback stall: EDMA completion acknowledgement

The device model exposed pending completion bits through the write-only
interrupt-clear register. Firmware's read/OR/write acknowledgement then
cleared other transfers' completions, permanently blocking PCM-ring refill.
Returning zero for ICR reads preserves the requested single-bit clear.

The evidence below is from the native run `runs/agent-play-checkpoints`,
which used functional McASP scheduling. It proves the lost acknowledgement
and the resulting refill gate. A fresh native run must still establish
sustained playback and pause/resume after the fix.

## Evidence and correction


Checkpoint 9 has mode 2, forward direction, cleared boundary flags, a stream
head of 4, tail of 749, and count of 746. Its four-record internal PCM ring
at `0x1182a7b0` is empty. The executed record lookup returns record 4
successfully; no linked-list self-loop was established.

A bounded, unmodified-state replay of checkpoint 9 follows
`0xc0007754 -> 0xc00071c4`. Before submitting the next PCM copy, firmware
checks software completion bit 1 in `[B14+530]`, then calls
`0xc004e228(1)` when it is absent. That function polls TCC 9 through
`0xc004e1c0(9)`. It returns zero because EDMA IPR is zero. The actual copy
function `0xc0006138` is skipped, the ring remains empty, and processing
returns zero consumption through its underflow path.

The native log records the completion being lost earlier:

- `main-stderr.log:223924` triggers the final QDMA1 transfer, copying from
  `0xc0094b10` to `0x11827908`, with TCC 9 enabled.
- A host QDMA0 transfer completes with TCC 8.
- Line 223945 writes `0x300` to global ICR `0x01c01070`, clearing both
  completions. No later QDMA1 trigger appears in that run.
- The same pattern later loses QDMA2 completion: line 254717 writes
  `0x500`, clearing TCC 8 and TCC 10 together.

Firmware `0xc004e1c0` polls the selected IPR bit, reads ICR, ORs the selected
mask, and writes ICR. The model incorrectly returned all pending IPR bits
on the ICR read. The read/OR/write therefore acknowledged unrelated
transfers, leaving their software completion flags unset indefinitely.

SPRUH91D section 16.4.2.6.5, Figure 16-74 and Table 16-53 label ICR
write-only (W-0), and define each written 1 as clearing only its corresponding
IPR bit. They do not specify a read value. The corrected model returns zero
for global and shadow ICR reads as an explicit compatibility choice for the
firmware's helper; this is not a claim that the manual specifies read-zero.
Other register aliases remain unchanged.

The regression performs two actual copies with different TCCs, verifies
the copied data and both completions, then executes the read/OR/write
acknowledgement and checks that the unrelated completion survives. Global
and shadow ICR paths are covered. `tests/test_c674x.py`: 26 passed.

The native log excerpts, source-log hash, checkpoint hash and bounded
observer artifacts are in
`runs/agent-play-checkpoints/motion-diagnostic/edma-completion-loss.*`.
Checkpoint 9 already contains the lost completion and cannot recover by
changing future ICR reads alone. Fresh native boot/load validation is
required; no completion was fabricated to repair the checkpoint.
