# Agent development DSP coverage note

**Date:** 2026-09-15

**Source:** current working tree, including the two new C674x encoding rows

**Generated probe destination:** `analysis/dsp/agent-dev-isa-probe-20260915.json`

Reproduction command: `python -m tools.cdj_dsp.isa_probe
analysis/dsp/agent-dev-isa-probe-20260915.json --ti-bin
/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin`. The recorded assembler is TI
TMS320C6x Assembler Unix v8.5.0 (2026-09-15).

## Current measured scope

Regenerating `analysis/dsp/agent-dev-isa-probe-20260915.json` with
`tools/cdj_dsp/isa_probe.py` and the TI `asm6x -mv6740` assembler gives:

| Measure | Count |
| --- | ---: |
| Manual instruction rows | 240 |
| Candidate encodings | 3,448 |
| Distinct words | 618 |
| Rows with all probed forms accepted | 229 |
| Rows with all probed forms rejected | 6 |
| Rows outside the probe scope | 5 |

The six rejected rows are `GMPY`, `RPACK2`, `SPMASKR`, `SWE`, `SWENR`, and
`XORMPY`. A rejected probe candidate is evidence that this emulator rejected
that assembled word; it is not by itself a complete determination of the
architectural ISA. In particular, the six rows should not be described as six
universally missing instructions without checking their documented forms and
execution context.

## Probe contract and limits

The probe measures decode acceptance, not architectural result correctness. It
uses one representative 32-bit encoding per parsed manual syntax form and
functional unit. It does not enumerate every opcode field or immediate value,
and it does not cover compact 16-bit forms, cross-path variants, predication,
parallel packets, or multi-instruction packet interactions. Operand values are
chosen to keep probe memory callbacks in range; acceptance under either of two
register profiles counts as acceptance. Compact exhaustive sweeps and
reference-backed semantic tests are separate evidence.

Therefore this report makes no claim that the C674x DSP ISA is complete.

## LMBD cst5 omission

The stock PLAY fault word `0x00903d5b` is the LMBD constant-source form:
`cst5=1`, source `A4`, destination `B1`, `.L2X` (`x=1`). The current decoder
has the `0xd58` cst5 dispatch row as well as the `0xd78` register-source row.

The generic probe missed this form because the manual inventory records LMBD as
`LMBD (.L) src1, src2, dst`; its parsed source operand is a register. Although
the substitution table knows how to replace a `cst5` placeholder, LMBD's
parsed syntax contains no such placeholder, so no immediate candidate is
generated. Existing register-form semantic coverage had the same blind spot.

The focused regression [tests/test_c674x_lmbd_cst5.py](../tests/test_c674x_lmbd_cst5.py)
now exercises immediate values 0, 1, 30, and 31, `.L1` and `.L2X`, no-match
behavior, predicate suppression, and the existing register form. This narrow
test catches removal or mis-decoding of the documented immediate encoding
without pretending to implement or exhaustively test the whole ISA.

## Reverse-cross SUB .S omission

Continuing the genuine playback checkpoint after the LMBD correction exposed
`SUB .S2X A1,B1,B0` (`0x00043d73`). The `.L` reverse-cross form already had a
dispatch row; the `.S` form (`0xd70`) did not. Its encoded source-field ordering
differs from the `.L` form, as described on SPRUFE8B printed page 529.
[tests/test_c674x_sub_reverse_s.py](../tests/test_c674x_sub_reverse_s.py) uses
distinct register indices on both banks, wrapping subtraction, and a false
predicate to check the `.S` behavior independently.

A one-million-packet continuation from the original playback checkpoint passes
the deterministic replay gate after both corrections. This is bounded replay
evidence; native playback and peripheral interaction still require separate
validation.
