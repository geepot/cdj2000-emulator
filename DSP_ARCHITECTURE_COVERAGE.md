# C674x / C6747 architectural coverage audit

Audit date 2026-09-10. This is an **assessment**, not an implementation change.
No emulator source was modified to produce it.

Repository `/Users/gpotvin/Git/cdj2000-emulator`, branch `codex/macos-nxs`.

**Provenance of what was audited.** The audit was commissioned against HEAD
`3da5ff2`. During the audit another session committed seven times on the same
branch (`2eb2476`…`828898d`: SH-4 ATA/USB-host, Blackfin SPORT/DMA, docs).
`git diff --name-only 3da5ff2..828898d` touches **no** C674x, C6747, `cdj_dsp` or
`cdj2000_dsp` source, so every finding below about DSP source holds at both
commits. Two tracks read the tree at `3da5ff2` and four at `2eb2476`; the
difference is documentation, and documentation findings are dated accordingly.
`ITERATION_ANALYSIS.md` and `RUNNING.md` changed inside that window.

## 0. Status since this audit

The findings below are dated 2026-09-10 and describe commit `3da5ff2`. They are
**left as written**, because an audit report whose baseline is edited away stops
being evidence. This section records what has since been fixed; each entry names
the section it supersedes.

| Landed | Commit | Supersedes |
|---|---|---|
| The blanket reserved-predicate gate is narrowed, so the 25 nonconditional rows are reachable and report "instruction not implemented"; ADDAB/ADDAH/ADDAW long-immediate implemented | `d7937e7` | §5.1 |
| FADCR/FAUCR/FMCR reserved bits masked on read and write | `323f7e0` | §5.6 |
| Compact Figure F-29 `Sx2op` decoded | `e3314af` | §5.3 |
| PROT dual-load, equal-count `NOP n`, and IDLE | `c48f886` | §5.4 (a), (b), (d) |
| SMPY/SMPYH/SMPYHL/SMPYLH/SMPY2 and compact Figure E-5 `M3` | `44c2575` | §5.2, §5.3 |

Measured after those five commits, by the same tools:

| Measure | At audit | Now |
|---|---|---|
| Rows with at least one probed encoding accepted (excl. pseudo-ops) | 117 | **123** |
| Rows where every probed encoding is rejected | 113 | **107** |
| …rejected only as "reserved predicate" | 22 | **0** |
| …rejected only as "reserved NOP count" (IDLE) | 1 | **0** |
| Compact words rejected "not implemented" | 11,440 | **6,944** |
| Control registers reading back unmasked | 6, 7, 13, 14, 18, 19, 20 | **6, 7, 13, 14** (all correct: full 32-bit R/W) |
| Packet-rollback failures | 0 | **0** |

Still deliberately **not** done, and why:

- **The 22 nonconditional instructions are reachable but unimplemented.** That was
  the point of `d7937e7`: an honest diagnostic first, semantics later.
- **Compact Figure F-32 `Sx1b` with `s = 0` stays fail-closed.** The manual
  contradicts itself (Figure F-32 leaves `s` free and Table B-1 does not footnote
  "BNOP register" as S2-only; but printed page 168 heads the entry `unit = .S2`
  and `cl6x` refuses it). The audit's own rule is that an unresolved question
  stays unknown, and accepting an encoding hardware may reject is the worse error.
  Opening it later is one bit in two places.
- **Overlapping parallel load/store to one address stays fail-closed.** SPRUFE8B
  does not define the ordering; inventing one would be worse than a halt.
- **Figure D-6 `Ltbd` remains unexplained** and referenced by no instruction.

Two ambiguities were found while implementing and are now open questions:

- **Does a PROT packet with N parallel loads expand once or once per load?**
  Printed page 93's text is per-LD and does not address two LDs in one execute
  packet. Charging once per packet is an inference from parallel loads sharing a
  single issue cycle; a literal reading gives 4 + 4. Rejecting the packet, the old
  behaviour, was wrong either way.
- **Figure E-5 `M3` and Figure G-4 `LSDx1` overlap** on 112 words and the manual
  does not say which wins. TI's disassembler decodes them as `M3`, which is what
  the implementation follows, and the now-unreachable `LSDx1` guard is kept rather
  than deleted so that reordering cannot silently open the space.

One test is labelled **metamorphic** rather than reference-backed: the protected-
loop equivalence test pins page 93's equivalence between a protected load and an
explicit `LD; NOP 4`, but the absolute values it compares are our own trace of the
second program, so a shared error in `NOP 4` handling would not be caught.

## 1. References

Both manuals are proprietary TI documents. They are downloaded into git-ignored
`build/references/` and **not committed**; only citations are.

| Document | Revision | SHA-256 | Pages |
|---|---|---|---|
| [SPRUFE8B](https://www.ti.com/lit/ug/sprufe8b/sprufe8b.pdf) TMS320C674x DSP CPU and Instruction Set Reference Guide | July 2010 | `34bc36312d37b092be9741986e70088f0fcefa00d8401a078f2b2981bb7b1d37` | 771 |
| [SPRUH91D](https://www.ti.com/lit/ug/spruh91d/spruh91d.pdf) TMS320C6745/C6747 DSP Technical Reference Manual | March 2013, rev. September 2016 | `8f00cb85ee803c034794096b2e663293c677fdc09bd8843415520e229fd76e6c` | 1473 |

Reproduce and verify with `python -m tools.cdj_dsp.refdocs --check`, which also
writes `build/references/provenance.json`.

**Printed page equals PDF page index for both documents** — offset 0 across 739
and 1435 detected footers respectively. Citations therefore read "printed page N
(PDF page N)". No page was found where they differ.

Two further TI documents (SPRS377F, SPRUFK5A) are cited by
`analysis/dsp/tracks/memory-peripherals.md` but are **not** in
`build/references/` and were not hash-verified. Rows resting on them are marked
accordingly and must not be read as reference-verified here.

## 2. How to read the matrix

Three dimensions are tracked **separately** for all 192 rows. Collapsing them is
how "the tests pass" becomes "the architecture is implemented".

- **implementation** — `supported` / `partial` / `unsupported` / `unknown`
- **validation** — `reference-backed-tests` / `firmware-observation-only` /
  `untested` / `not-assessed`
- **fidelity** — `strict` / `approximation` / `unknown`

`reference-backed-tests` requires that the test's assertions *and their expected
values* were read, and that the expected values are independent of the emulator
(hand-computed from the manual, or produced by the TI assembler/disassembler).
A matching test name is not evidence. `tools/cdj_dsp/coverage_matrix.py`
enforces three things about such a claim: the row must name at least one test,
each test must state where its expected values came from, and each test must be a
**committed, rerunnable** file. It rejected five rows naming no test at all, and a
further sixteen citing scratchpad probes that no longer exist or citing emulator
source as if it were a test. All were repaired or downgraded; see section 7.

Each row also carries the **verifier verdict** from an independent adversarial
pass: `upheld` / `downgraded` / `overturned` / `unresolved` / `not-challenged`.
**`not-challenged` means no verifier examined that row. It is not a
confirmation.**

Full detail — manual section and printed page, implementation location, test
names and expected-value provenance, firmware evidence, exclusions, open
questions and next acceptance test for every row — is in
`analysis/dsp/coverage_inventory.json`. The table in §11 is a summary of it.

## 3. Denominators

No single number describes "how much of the ISA works", so none is given. These
are the denominators actually used, each with its grouping and exclusions stated.

**Instruction names.** SPRUFE8B Appendix A Table A-1 (printed pages 709-714) has
**240 rows carrying a C674x check mark**, over **227 distinct base mnemonics**.
Generated by `tools/cdj_dsp/isa_inventory.py` from Table A-1, Table B-1 (unit
mapping, printed pages 715-720) and the §3.12 running headers.
**A row is an instruction name — not an encoding, not a semantic variant, not a
unit.** One row can cover many opfields, operand types and units: `ADD` alone has
9 documented opfields and executes on `.L`, `.S` and `.D`.

> An earlier revision of that tool reported 238. It silently dropped
> `MPY32 (32-bit result)` and `MPY32 (64-bit result)` because its row-name regex
> rejected that qualifier shape, and its Table A-1↔B-1 cross-check was
> structurally blind to the omission because both parsers dropped the rows
> identically. Found by the instruction track, verified against printed page 712,
> fixed, and the tool now reports any check-marked line it cannot parse
> (`unparsed_checkmark_lines`, currently 0).

**Documented instruction formats.** Appendices C–H define **29 32-bit** and
**48 compact 16-bit** instruction-format figures. Derived independently twice
(coordinator count of format figures split at each appendix's own 32-bit/16-bit
section boundary; instruction-track cross-reference of every §3.12 "Compact
Instruction Format" table) and the two agree at 48.

**Compact encoding space.** All **65,536** 16-bit words, swept under 13 named
header configurations × 3 named register profiles by
`python -m tools.cdj_dsp.audit_sweeps`.

**Control registers.** All 32 control-register ids, driven through real `MVC`
instructions in both directions.

**Peripherals.** Counted per peripheral with the register denominator stated
(e.g. "7 of 15 EMIFB registers"). **No peripheral percentage is given** and the
deferred/unassessed list in §9 is explicit. This is not a complete audit of every
SPRUH91D peripheral.

## 4. What we can trust today

Measured, reproducible, and independent of the emulator:

- **Single-precision floating-point arithmetic is correctly rounded.**
  `tests/cstub/c674x-audit-sp-rounding.c` checks MPYSP, ADDSP and SUBSP over
  240,000 random in-range cases in all four FADCR/FMCR rounding modes against the
  host FPU under `fesetround` as an independent correctly-rounding oracle, and
  asserts the INEX status bit is set exactly when the rounding was inexact.
  **0 failures.** Now in the suite via `tests/test_dsp_isa_audit.py`.
  Not covered: denormal and overflow/underflow boundaries (hand cases only),
  `ABSSP`, the SP compares, the `.S`-unit ADDSP/SUBSP opfields, cross paths.
- **Packet rejection is atomic.** Four cases — GPR write, control-register write,
  store, and parallel-write conflict, each paired with a rejected instruction —
  leave CPU state and memory bit-identical. 0 failures. This is the requirement
  the three most recent DSP commits were about and which **no audit track opened
  a row for**; it is now measured and asserted.
- **Scalar saturating arithmetic.** `tests/cstub/c674x-saturation.c` covers every
  scalar and signed-40-bit `SADD`/`SSUB` operand form and `SSHL` over 12 shift
  counts, both sides, both cross paths, immediate and register `src1`, predicate
  true and false, asserting the *delayed* `CSR.SAT` and per-unit `SSR` flag two
  packets later. Expected values hand-derived.
- **Compact SPKERNEL field reconstruction**, verified by two independent routes:
  `tests/cstub/c674x-spkernel-fields.c` (all 64 fields at nine II values) and an
  independent TI assembler/disassembler fixture (`tests/ti/spkernel-oracle.asm`)
  checking `9c67`/`dc66`/`1f66` as stages 3/6/24. **Caveat: the TI oracle is
  skip-gated** — it runs only with `C6X_TI_BIN` set, and is the one skip in the
  ISA suite by default. Confirmed: it passes when enabled.
- **The nine-cycle strict interrupt-entry interval** is the single claim two
  independent tracks reached separately and agreed on, against SPRUFE8B
  Figure 5-4 (printed page 641).
- **Addressing and circular addressing** is the best-evidenced area. SPRUFE8B
  worked Examples 3-4/3-5/3-6 were executed against the manual's own printed
  result values — the only place in the audit where a manual worked example with
  printed expected values was run.
- **Deterministic replay is genuinely deterministic.** It is regression evidence
  of exact repeatability, and nothing more: the comparison is our output against
  our earlier output.

## 5. The most consequential gaps

### 5.1 One check makes 25 instruction rows unreachable

`cdj_c674x.c:1531` rejects `creg == 0 && z == 1` as "reserved predicate" **before
any opcode classification**:

```c
unsigned creg = w >> 29, z = (w >> 28) & 1;
if (creg == 7 || (!creg && z)) return stop(cpu, pc, insn->word, "reserved predicate");
```

SPRUFE8B Table 3-9 (printed page 77) does mark that combination Reserved — but
only for instructions that *have* a `creg` field. TI encodes the C64x+
unconditional extensions with a fixed `0001` in bits 31-28, which aliases exactly
into that hole. Verified in three independent opcode figures:

| Instruction | Printed page | Bits 31-28 | Manual note |
|---|---|---|---|
| `SWE` | 557 | `0 0 0 1` literal | internal exception / Supervisor entry |
| `CMPY` | 215 | `0 0 0 1` literal | "This instruction executes unconditionally." |
| `ADDSUB` | 132 | `0 0 0 1` literal | — |
| `ADDAB` | 115 | `0 0 0 1` literal | "This instruction is executed unconditionally, it cannot be predicated." |

**22 of the 240 rows are rejected *solely* for this reason**, plus the three
`ADDAB`/`ADDAH`/`ADDAW` long-immediate forms: `ADDSUB ADDSUB2 CMPY CMPYR CMPYR1
DDOTP4 DDOTPH2 DDOTPH2R DDOTPL2 DDOTPL2R DPACK2 DPACKX2 GMPY MPY2IR RPACK2
SADDSUB SADDSUB2 SHFL3 SMPY32 SWE SWENR XORMPY` + `ADDAB ADDAH ADDAW` = **25
rows**.

Two consequences. First, the diagnostic is misleading: these are unimplemented
instructions reported as a reserved encoding. Second and more important, **this
gate is a prerequisite dependency** — no one can implement any of those 25
instructions without fixing it first, because the decoder cannot reach their
opcodes. The tracks found the `ADDAB` instance; the class was found by the
coordinator and is the highest-leverage decoder finding in the audit.

Fail-closed, so nothing produces wrong results. Note `ADDAB` is instructive about
why that matters: had the predicate check not fired, `0x1280043C` would have
fallen into the 15-bit-offset memory arm and executed as **`STB` with a 15-bit
offset**.

### 5.2 The instruction gap has shape

Measured by `analysis/dsp/isa_probe.json` (TI `asm6x` assembles one
representative encoding per manual syntax form, per unit, per operand binding —
3,448 candidates, 618 distinct words — each run through `cdj_c674x_fetch` +
`cdj_c674x_execute` under two register profiles):

| Measure | Count | of |
|---|---|---|
| Rows with at least one probed encoding accepted | 122 | 240 |
| …excluding assembler pseudo-operations | 117 | 240 |
| Rows where every probed encoding is rejected | 113 | 240 |
| …rejected only as "instruction not implemented" | 90 | 240 |
| …rejected only as "reserved predicate" (§5.1) | 22 | 240 |
| …rejected only as "reserved NOP count" (`IDLE`) | 1 | 240 |
| Rows outside the probe's scope (SPLOOP family) | 5 | 240 |

`MV`, `NEG`, `NOT`, `ZERO` and `SWAP2` are **assembler pseudo-operations**
(printed pages 374, 387, 392, 572, 553): the assembler emits `ADD`/`OR`, `SUB`,
`XOR`, `MVK` and `PACKLH2`. Their acceptance is the base instruction's, not
independent coverage — hence the second row above.

Wholly absent families, from source reading: **all packed 2×16 and 4×8
arithmetic and compares, all double-precision floating point and its
conversions, all dot-product and complex-multiply, saturating 16-bit multiply,
bit-manipulation, Galois field**. Single-precision FP is implemented and now
well-validated; double-precision is absent entirely.

> An earlier figure of "88 rows with no implementation" counted only the nine
> entirely-absent groups and left 93 rows with no stated status. The table above
> replaces it. The 113/240 figure is measured, not summed from prose.

**What these counts do not say.** A row counted as having an accepted encoding may
be implemented on fewer units or fewer operand forms than the manual documents.
All five integer compares and all three shifts are 32-bit-only, their 40-bit
long-operand forms absent. **No per-unit denominator was produced by any track**,
so "covered" here means "dispatched for at least one documented encoding".

### 5.3 Compact space: four closed formats

All 65,536 16-bit words, reproducibly (`analysis/dsp/audit_sweeps.json`):
**53,856 accepted, 11,440 rejected "compact instruction not implemented",
176 reserved LSDx1, 64 invalid register pair.** The instruction track's
independent scratch sweep produced the same four numbers.

Only the **11,440** is quoted as coverage. The accepted total moves with the
probe's register profiles and memory window and is not a coverage measure — an
earlier run of this sweep reported 10,464 additional "unaligned or unmapped"
rejections that were purely an artefact of pointer registers holding 0, which is
why the third `all-pointers` register profile exists.

Four formats are closed:

| Figure | Format | Printed page | State |
|---|---|---|---|
| E-5 | `M3` — the only compact `.M` format, 8 instructions | 744 | all 4,096 words rejected, while `instruction_unit()` still classifies them as `.M` |
| F-29 | `Sx2op` — compact in-place `.S` ADD/SUB | 755 | all 512 words rejected |
| F-32 | `Sx1b` — `BNOP src2, N3` | 756 | the 128 `s = 0` words rejected; `src2` is always a B register so `s = 0` is legal, and the data path is already correct |
| D-6 | `Ltbd` | 737 | unhandled — **and referenced by no instruction anywhere in SPRUFE8B**; left open, see §9 |

### 5.4 Two fail-closed halts that ordinary TI codegen triggers

- **Overlapping parallel load/store.** Reproduced independently by the
  coordinator, using TI-assembled encodings `0x05100264` (`LDW .D1 *+A4[0], A10`)
  and `0x051002F6` (`STW .D2 B10, *+B4[0]`) as one parallel packet: with
  `A4 != B4` the packet executes; with `A4 == B4` it halts at
  `cdj_c674x.c:2454` with "simultaneous overlapping RAM accesses not
  implemented". A plain aligned `.D1`/`.D2` pair — no PROT, no exotic encoding.
- **PROT fetch packet with two parallel loads.** Reproduced independently by the
  coordinator: with a mixed fetch-packet header (top nibble `0xE`) and PROT
  (header bit 20), two parallel 32-bit `LDW` are rejected as "multiple multicycle
  instructions". SPRUFE8B printed page 93 says verbatim: *"When PROT is 1, four
  cycles of NOP are added after each LD instruction within the fetch packet
  whether the LD is in 16-bit compact format or 32-bit format."* Two loads in one
  cycle is still one 5-cycle packet, not a fault. `BUILD.md:996` records the NXS
  firmware executing a protected `LDW` inside a software-pipelined loop at
  `0x11802ea8`, which makes this a live firmware-breadth risk. The SPLOOP path has
  the same shape.

Both fail closed — they halt the DSP rather than produce wrong audio.

### 5.5 Firmware cannot be driven: events, timers, exceptions

- **Only 2 of the 124 C6747 system events are ever generated**, so most
  interrupt-driven firmware paths cannot be exercised at all.
- **Timer64P is register-complete and behaviour-empty**: no counter increments,
  so no period interrupt, no EDMA event, no watchdog.
- **Privilege and exceptions are structurally absent**: `TSR.CXM` is storage that
  is never non-zero, no access check consults it, NMI is impossible on both CPU
  and device side, and `SWE`/`SWENR` (the OS-entry mechanism, §5.1) do not
  execute, leaving `REP` unreachable.
- **There is no cache model.** `cdj_c6747_cache.c` is register storage plus no-op
  coherence commands — not an untimed cache, no cache.

### 5.6 A verified fidelity defect in a seam

`FADCR`, `FAUCR` and `FMCR` reserve bits 31-27 and 15-11. SPRUFE8B Tables
2-25/2-26/2-27 (printed pages 59, 60, 62) each say verbatim: *"Reserved. The
reserved bit location is always read as 0. A value written to this field has no
effect."* `control_read()` at `cdj_c674x.c:203` returns `cpu->control[id]`
unmasked for ids 18/19/20, where AMR, CSR, IFR, SSR, TSR and ITSR all mask.
Measured: writing `0xffffffff` and reading back gives `ffffffff`; it should give
`07ff07ff`. Two tracks each assumed the other owned it.

`IRP`, `NRP`, `ILC` and `RILC` also read back `ffffffff`, and that is **correct** —
SPRUFE8B Figures 2-21 and 2-24 and the IRP/NRP figures show full 32-bit R/W
registers with no reserved field. Checked rather than assumed.

### 5.7 Control-register reachability, reproducibly

`MVC` driven in both directions for all 32 ids, with encodings taken from TI's
assembler (`MVC .S2 B4, AMR` → `0x001003a2`; `MVC .S2 AMR, B5` → `0x028003e2`):

- **Readable (15):** AMR, CSR, IFR, IER, ISTP, IRP, NRP, ILC, RILC, FADCR, FAUCR,
  FMCR, SSR, TSR, ITSR
- **Writable (16):** those plus ISR — which is write-only, as SPRUFE8B specifies
- **Absent (16):** ids 8-12, 15-17, 22-25, 28-31 — both directions reject

This replaces an earlier enumeration that rested on a scratch program containing
no assertions at all. It is now asserted in `tests/test_dsp_isa_audit.py`.

## 6. Historical versus current evidence

| Claim | Status |
|---|---|
| "5,396 instruction addresses, 68 mnemonics" (`runs/dsp-semantic-inventory-3.json`) | **Exploratory disassembly of observed addresses.** The artifact sets `validation_eligible: false` and its own caveats say so. It is not 68 validated instruction families, and must never be cited as ISA coverage. |
| Strict PCM evidence: 16 connected stops, 4 blocks, 2,352 stereo frames S16→float32 matching expected samples | Holds as a **narrow unpack contract**. Establishes nothing about continuous playback, other PCM modes, downstream processing, McASP output, headroom or genuine stereo separation — the source tone has identical L/R channels. `PCM_EXECUTION_EVIDENCE.md` states these limits carefully and is one of the more honest documents in the repo. |
| Nine optimization iterations, exact repeatability | **Regression evidence** of determinism under exploratory timing/audio modes. Not strict architectural proof. |
| "506 passed, 31 skipped" | Not reproducible as stated; see §8. Regression coverage, never architectural completeness. |
| DSP_BOOT_MILESTONE_AUDIT.md source-hash table | Several hashes no longer match current files, which is expected for changed code. It dates those claims to older source; it does not invalidate them. |

## 7. Limitations of this audit

Stated plainly, because the matrix would otherwise imply coverage it does not have.

**Audited by nobody.** Each of these was excluded by name by two or more tracks,
each assuming another owned it:

- `emulator/qemu/cdj_dsp_checkpoint.c` (506 lines). Every resumed run stands on
  it, including `runs/nxs-pcm-observe-4`'s 2.7-billion-packet ancestry — the
  repository's strongest PCM evidence. A dropped field corrupts state silently.
- **The PCM/audio path itself.** The headline audio claim (`c003c398` kernel,
  `0xdc8` plane separation, float32 not Q31) was verified by no track. Two tracks
  deferred it to an "audio-path track" that was never commissioned.
- `tools/cdj_dsp/replay.c` (1,331 lines), which holds the real C6747 memory map
  and is the source of the 2 delivered events.
- **Per-functional-unit coverage.** No track produced a per-unit denominator
  (§5.2).
- **Register banks.** A/B symmetry is structurally implied by the `s` bit and was
  never stated as a claim or tested as one.
- **Firmware reachability.** 100 `dsp-l2.bin` images sit in `runs/`, and no track
  checked whether the firmware actually contains the instructions we do not
  implement. Four tracks deferred it to each other. This is the cheapest
  unanswered question in the audit and it would reprioritise much of §10.

**Not measured, only read.** Every delay-slot and latency claim in this audit was
established by reading source against the manual's "Delay Slots" text. **No cycle
oracle was applied by anyone.** There is no stall model at all — `cpu->cycles` is
an issue count — so every timing statement must be read as issue cycles, and no
timing claim in this report is independently validated.

**Verification coverage.** Six independent verifiers attacked the six tracks:
58 rows upheld, **75 downgraded, 5 overturned**, 54 not challenged. Only three
rows make a positive claim and were never challenged: `PER-GPIO`, `PER-I2C`,
`PER-PLLC`. The completeness critic warned that three tracks appeared unverified;
that was an artefact of the digest it was given being truncated — all six
verifiers ran, and their corrections are merged into the inventory.

**Evidence that had already evaporated.** Sixteen rows cited a "test" that was
either a scratchpad probe from an earlier audit pass — gone the moment that
session ended — or an emulator source location rather than a test. The
control-register inventory was the worst case: its sole authority was a scratch
program containing no assertions at all. That one now cites the committed sweep
and its assertions (section 5.7); the other fifteen lost the citation, and the
rows left with no test at all were downgraded to `untested`.
`coverage_matrix.py` now rejects both citation shapes, so the class cannot
recur. Separately, `PKT-PROT` cited a scratch probe that recorded the PROT
dual-load divergence; a test asserting that refusal would be asserting the
defect, so the divergence is recorded in section 5.4 instead and the row keeps
its two genuine tests.

**Circular validation.** 11 rows cite at least one test whose expected values are
derived from the emulator rather than independently: `DOC-BUILD-CIRCULAR`,
`EP-CIRC-LOOPORACLE`, `EP-TEST-CLASSIFY`, `IC-DEV-EVENT-SOURCES`,
`IC-DEV-NMI-SOURCE`, `IC-EXC-SWE`, `IC-INT-NMI`, `IC-PRIV-MODE`, `LOOP-SCHED`,
`LOOP-SPKFIELD`, `PER-SPI`. Four of the software-loop area's five cited tests are
self-referential, and its one fully external oracle is skip-gated.

**Peripheral tests of unconfirmed provenance.** Only 7 of 31 `tests/cstub` files
cite a TI document. `c6747-mcasp.c` (195 asserts), `c6747-edma.c` (123),
`c6747-syscfg.c` (89), `c6747-pll.c` (86) and `c6747-intc.c` (48) carry no
recorded reference — **541 asserts whose provenance is unconfirmed**. Three McASP
field claims were spot-verified against SPRUH91D. Three of 195 is not a
reference-backed row, and the affected rows are marked `untested` or carry the
caveat rather than `reference-backed-tests`.

**Deferred / unassessed peripherals.** Derived from the SPRUH91D chapter list and
checked for actual presence on C6747: **EMIFA (absent and unmapped), USB, MMC/SD,
uPP, LCDC, VPIF, RTC, eCAP, eQEP, UART, McBSP, DDR2/mDDR, ECC, MPU.** EMIFB models
SDRAM configuration registers only — no refresh, timing or arbitration. I2C has
no protocol at all, only GPIO-mode pin registers. **This is not a complete audit
of every SPRUH91D peripheral and must not be read as one.**

## 8. Stale documentation findings

43 items are recorded across the track reports. The ones that matter:

1. **`README.md:57` is actively false.** It calls the DSP "a Pioneer custom LSI
   with no public instruction set", refuted by the repository's own
   `RUNNING.md:603-604` and by a ~3,000-line TI-derived C674x interpreter. This
   is the one affirmatively wrong claim in the corpus.
2. **`BUILD.md` understates SPLOOP coverage by a wide margin** — the software-loop
   implementation is far more complete than the document describes.
3. **`cdj_c674x.h` claims a seven-instruction interpreter.** The header comment
   lists "MVK, MVKH, MVC, AND, B, ADDKPC and NOP"; 122 rows accept at least one
   encoding. A reader trusting the header would badly misjudge the core.
4. **Test-count drift**: docs quote 403/27, then 407/27, then 427/29, then
   506/31. None is an architectural-completeness measure and the report says so
   wherever they appear.
5. `HANDOFF.md` and `BUILD.md` (~108K each) interleave current and historical
   state with no marker. Dated headings plus a "below here is historical" banner
   would retire a whole class of stale-limitation findings at once.

Documentation is, on the whole, **careful and well-qualified** — heavily hedged,
explicit about what evidence does not establish. The corpus's failure mode is
staleness and volume, not overclaiming. `PCM_EXECUTION_EVIDENCE.md` and
`DSP_BOOT_MILESTONE_AUDIT.md` are models of honest scoping.

## 9. Open questions, left open

These stay `unknown`. Correctness was not settled by majority vote anywhere in
this audit, and where a track and its verifier genuinely disagreed the row is
marked `unresolved` rather than split.

- **Figure D-6 `Ltbd`** (printed page 737) has a format diagram, no mnemonic
  table, and is referenced by no instruction anywhere in SPRUFE8B. Resolving it
  needs a second primary source; GNU binutils `tic6x-insn-formats.h` is already a
  build input of `coverage.py` and is the obvious next place to look.
- **`CMPGTU`/`CMPLTU` immediate decode** zero-extends the full 5-bit `src1` field
  although the manual gives the operand type as `ucst4`. SPRUFE8B does not state
  what hardware does when bit 4 is set.
- **Is the silent drop of supervisor-writable `TSR` bits intended policy?** This
  is the one non-fail-closed path found in the entire CPU model (§10, task 5).
- **SPLOOP `ii` above 14.** Partly settled: the TI assembler refuses to emit it
  (`[E1400] SPLOOP II N out of range`, 1..14 accepted), so no TI-generated
  firmware can contain it. What remains genuinely unknown is what *hardware* does
  with such an encoding, and SPRUFE8B does not say. Three of our tests assert
  `ii = 16` behaviour with no architectural referent, so those assertions pin our
  own choice rather than the manual's.

## 10. Prioritised backlog

Missing implementation and missing validation are separated. Ranking weighs
firmware-development and correct-audio value, dependencies, risk and effort.

### Top five, with scope and acceptance criteria

**1. Measure firmware reachability of the unimplemented set.** *(validation;
hours; no dependencies)*
Scope: scan the 100 `dsp-l2.bin` images in `runs/` for the 113 unimplemented
rows, the four closed compact formats, the `ADDAB`/`ADDAH`/`ADDAW` long-immediate
pattern, `IDLE`, and the 25 `creg`-hole encodings; add a reachability column to
`analysis/dsp/coverage_inventory.json`.
Acceptance: a committed tool regenerates the reachability column from the images;
every row in the inventory carries `reached` / `not-reached` / `not-scanned`; no
firmware bytes are committed.
Why first: it reprioritises everything below it, collapses several deferred open
questions, and is the difference between "latent defect" and "active blocker".
Proposed by no track — all four technical tracks deferred it to each other.

**2. Fix the two fail-closed halts ordinary TI codegen triggers.**
*(implementation; ~1 day)*
Scope: (a) overlapping parallel load/store with equal base registers;
(b) count protected loads once per packet rather than once per instruction, in
both `cdj_c674x_execute` and the SPLOOP path; (c) free rider: accept `Sx1b`
`s = 0` (one bit at `cdj_c674x.c:1355`); (d) `NOP n || NOP n` with equal counts,
which SPRUFE8B printed page 83 explicitly permits.
Acceptance: a test asserting each case executes with the manual's cycle count —
5 cycles for a PROT dual-load packet, citing printed page 93 — plus the existing
suite unchanged. No strict check weakened: each fix must narrow the rejection
condition, never remove it.

**3. Remove the blanket reserved-predicate gate.** *(implementation; ~0.5 day;
unblocks 25 rows)*
Scope: classify bits 31-28 = `0001` as an opcode field before applying the
`creg`/`z` reserved test at `cdj_c674x.c:1531`, so the 25 unconditional C64x+
encodings reach a decoder arm. Implement none of them yet — they must reject with
"instruction not implemented", which is the honest diagnostic.
Acceptance: all 25 encodings report "instruction not implemented" rather than
"reserved predicate"; `0x1280043C` is **not** executed as `STB` (§5.1); genuinely
predicable instructions still reject `creg=0/z=1`; `analysis/dsp/isa_probe.json`
regenerates with `probe_rows_rejected_only_as_reserved_predicate` at 0.
This is a prerequisite for implementing any of those 25 instructions.

**4. One table test over the multiply and pack families.** *(validation; ~1 day)*
Scope: 16 `MPY` permutations, 4 `MPY32` variants, 8 `MPYHI`/`MPYIL` variants and
6 pack opfields, expected values hand-computed. Operands `src1 = 0x8001FFFF`,
`src2 = 0x7FFF8000` — every halfword-selection and signedness reading yields a
distinct product, so a wrong permutation cannot pass.
Acceptance: every opfield asserted with a hand-derived expected value and the
documented E2/E4 latency; the row `ISA-MPY16` moves from `untested` to
`reference-backed-tests` with expected values marked independent.
Why high: these are the instructions an audio kernel uses most, they are
currently **untested** (the audit downgraded them), and a halfword or signedness
error among 16 near-identical permutations yields plausible-but-wrong samples
rather than a fault — the hardest class of bug to see from audio output.
`mpyli`, `mpyus`, `mpysu`, `pack2` and `packl4` are reached by real firmware with
no assertion covering them.

**5. Audit `cdj_dsp_checkpoint.c` field-by-field, and gate eligibility on
completeness.** *(validation + small implementation; ~1 day)*
Scope: compare every `CdjC674x` field against what the serializer writes and
reads, including loop-buffer metadata, FP status registers, the delayed
load/store queues and retained circular widths; make
`architectural_validation_eligible` require `complete == true`; write
`manifest.json` *after* execution, not before.
Acceptance: a round-trip test that populates every field with a distinct value
and asserts bit-identical restore; the two aborted runs currently on disk claiming
`architectural_validation_eligible` no longer do.
Why: 506 unaudited lines carry every resumed run, including the repository's
strongest PCM evidence. A dropped field corrupts state with no fault.
Excluded by name by three tracks.

### Next, not top five

*Implementation.* Double-precision floating point (11 rows + 5 conversions) —
large and currently unreachable by firmware as far as anyone has checked; packed
2×16 / 4×8 arithmetic (25 rows); compact `M3` and `Sx2op` formats; `IDLE`;
40-bit long-operand compares and shifts; `TSCL`/`TSCH` (the core already keeps a
64-bit cycle counter); Timer64P counter behaviour; the C6747 event sources beyond
the 2 currently generated.

*Validation.* Mask `FADCR`/`FAUCR`/`FMCR` reserved bits (§5.6) — a one-line fix
with a test already written to fail when it lands; establish provenance for the
541 peripheral asserts; un-gate the TI SPKERNEL oracle or make its absence a
failure rather than a skip; replace the four self-referential software-loop tests
with manual-derived expectations; McASP serializer/clock/format validation
against SPRUH91D, which the PCM evidence explicitly does not cover.

*Deliberately demoted.* The 14-execute-packet loop-buffer limit, the `dynlen`
floor, the spanning-packet branch-target restriction and functional-unit
occupancy checking all fail **open** on packets hardware cannot hold or the
assembler will not emit — near-zero firmware value, worth one paragraph rather
than engineering time. Anything cycle-shaped is unmeasurable until a cycle oracle
exists.

## 11. Validation record

Toolchain: `DEVELOPER_DIR=/Library/Developer/CommandLineTools`,
`C6X_TI_BIN=/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin`
(TI C6x Assembler Unix v8.5.0), `cc` from CommandLineTools, Python
`.venv/bin/python` 3.12.

```sh
# reference provenance
python -m tools.cdj_dsp.refdocs --check

# manual-derived denominator and the decode-acceptance probe
python -m tools.cdj_dsp.isa_inventory analysis/dsp/isa_manual_inventory.json
python -m tools.cdj_dsp.isa_probe analysis/dsp/isa_probe.json --ti-bin "$C6X_TI_BIN"

# compact sweep, control-register enumeration, packet rollback
python -m tools.cdj_dsp.audit_sweeps analysis/dsp/audit_sweeps.json

# inventory validation and summary counts
python -m tools.cdj_dsp.coverage_matrix
python -m tools.cdj_dsp.coverage_matrix --markdown

# audit tooling tests
python -m pytest -q tests/test_dsp_isa_audit.py          # 8 passed

# full suite
python -m pytest -q                                       # see below
```

**Full regression suite: 524 passed, 28 skipped, exit 0**, in 52.56s, with
`C6X_TI_BIN` **and** `CDJ_ETH_QEMU_TEST=1` set, on the working tree described at
the top of this document — which includes this audit's new tests and another
session's new Blackfin/ATA tests. No failures.

This does **not** match any number quoted in the committed documentation, and the
two audit tracks that ran the suite reported 505/30 and 506/29 respectively. The
discrepancies are fully explained by which environment gates were set and how
many test files were present at the time; the gates are named here precisely so
the number is reproducible. **None of these numbers measures architectural
completeness.**

The two §5.4 halts were each reproduced from a standalone probe compiled against
unmodified core sources, using encodings taken from TI's assembler. The PROT case
needs a mixed fetch-packet header (top nibble `0xE`) with PROT at header bit 20 —
two earlier attempts with the wrong header bit did not reproduce it, which is
recorded here because a failed reproduction is not evidence of absence.

Focused runs: `tests/test_c674x.py tests/test_c674x_saturation.py
tests/test_c674x_circular.py tests/test_c674x_spkernel_fields.py
tests/test_dsp_isa_audit.py` → 24 passed, 1 skipped; the single skip is
`set C6X_TI_BIN to independently check TI assembler encodings`, and it passes
when that variable is set (2 passed). A skipped prerequisite is not a pass.

### Artifacts

| Path | Contents |
|---|---|
| `analysis/dsp/coverage_inventory.json` | all 192 rows, three dimensions each, verifier verdict, coordinator overrides, generated counts |
| `analysis/dsp/isa_manual_inventory.json` | the 240-row manual denominator with per-row manual citations |
| `analysis/dsp/isa_probe.json` | decode-acceptance measurement, 3,448 candidate encodings with provenance hashes |
| `analysis/dsp/audit_sweeps.json` | compact sweep, control-register enumeration, packet-atomicity result |
| `analysis/dsp/tracks/*.md` | the six full track reports |

Downloaded manuals, firmware images and large traces are **not** committed.

## 12. Coverage matrix

All 192 rows, summarised. Manual section/page, implementation location, test
names with expected-value provenance, firmware evidence, exclusions, open
questions and next acceptance test are in
`analysis/dsp/coverage_inventory.json`; regenerate this table with
`python -m tools.cdj_dsp.coverage_matrix --markdown`.

| ID | Requirement | Manual | Impl | Validation | Fidelity | Verdict |
|---|---|---|---|---|---|---|
| `DOC-BOOT-MILESTONE` | DSP_BOOT_MILESTONE_AUDIT.md's claims: the E-7010 milestone conclusion, the source-hash provenance table, the S | SPRUFE8B Figure H-7 (Uspk format) and Table 3-29 (stg/cyc bit allocati | partial | untested | approximation | upheld |
| `DOC-BUILD-C674X-CORE` | BUILD.md section "Partial C674x execution core": its present-tense enumeration of supported instruction forms  | SPRUFE8B Appendix A Table A-1 / section 3.12 (the denominator the clai | partial | untested | approximation | upheld |
| `DOC-BUILD-CIRCULAR` | BUILD.md limitation claim: "Circular addressing, RAM arbitration for simultaneous overlapping accesses, and re | SPRUFE8B 2.9.1 AMR / circular addressing pn/a — not page-verified by t | unsupported | untested | unknown | not-challenged |
| `DOC-BUILD-DEVICE-TXN` | BUILD.md limitation claim: "The memory callback currently supports checked L2 RAM writes, not device transacti | other n/a — repository documentation claim about its own callback cont | unsupported | untested | unknown | not-challenged |
| `DOC-BUILD-LOOPDRAIN-8` | BUILD.md schema-8 limitation claim: "Returned SPMASK reversal, SPLOOPW return, full retained-buffer state, and | SPRUFE8B 7.10.3 (loop termination during interrupt drain); Figure 5-4  | partial | untested | unknown | upheld |
| `DOC-BUILD-SPLOOP-LIMITS` | BUILD.md limitation claim: "SPMASK, reload/nested loops, SPLOOPD/W, protected/control instructions in the body | SPRUFE8B Chapter 7 (software pipelined loop buffer), 7.10.3, 7.15 pn/a | partial | untested | unknown | downgraded |
| `DOC-BUILD-SPLOOP-SCOPE` | BUILD.md limitation claim: "Unconditional SPLOOP execution is supported below; other loop forms and control-re | SPRUFE8B Chapter 7 (software pipelined loop buffer), 7.14 pn/a — secti | partial | untested | unknown | downgraded |
| `DOC-BUILD-SPLOOPW-STOP` | BUILD.md's documented replay entry point and the execution narrative around it: the tools.cdj_dsp.replay comma | other n/a — repository tooling claim pn/a | unsupported | untested | unknown | not-challenged |
| `DOC-CLEANBOOT` | CLEAN_BOOT_EVIDENCE.md's claims: the clean-startup evidence, its own retraction, the IIC modelling sources, an | other Renesas SH7764 manual R01UH0360EJ0300 sections 16.3.5/6 and 16.6 | partial | untested | approximation | downgraded |
| `DOC-DSP-PART-IDENTITY` | Which TI device the DSP model actually represents, as claimed across the documentation | SPRUH91D C6745/C6747 memory map (global L2 at 0x11800000) versus RUNNI | unknown | untested | unknown | not-challenged |
| `DOC-EQUIVALENCE` | Whether any doc statement commits one of the forbidden equivalences (disassembly with implementation, acceptan | other n/a — repository documentation claim pn/a | partial | untested | approximation | downgraded |
| `DOC-EVIDENCE-ARTIFACTS` | Whether the artifacts the DSP documentation cites as evidence are obtainable, i.e. whether any documented DSP  | other n/a — repository evidence-provenance property pn/a | unsupported | firmware-observation-only | unknown | not-challenged |
| `DOC-HANDOFF-CURRENT` | HANDOFF.md's 'Current checkpoint' section as a statement of where the project currently stands | other n/a — repository status claim pn/a | partial | untested | approximation | upheld |
| `DOC-HANDOFF-JOURNAL-FORMAT` | Whether HANDOFF.md and BUILD.md, as append-only newest-first journals that readers are instructed to grep, can | other n/a — repository documentation-structure property pn/a | unsupported | untested | unknown | not-challenged |
| `DOC-INTERRUPT-ENTRY` | DSP_INTERRUPT_ENTRY.md's claims: the nine-cycle interrupt-entry interval, unchanged IRP/priority/sticky-IFR/co | SPRUFE8B Figure 5-4, nonreset interrupt detection and processing; inte | partial | untested | approximation | downgraded |
| `DOC-ISA-COVERAGE-CLAIM` | Whether any location in the repository claims DSP ISA coverage, instruction-family completeness, or architectu | SPRUFE8B Appendix A Table A-1 (the 238-row C674x denominator) p709-714 | supported | not-assessed | approximation | downgraded |
| `DOC-ITERATION` | ITERATION_ANALYSIS.md's claims: the nine optimization iterations, their speedup figures, and the final validat | other n/a — repository measurement claim pn/a | partial | firmware-observation-only | approximation | downgraded |
| `DOC-MNEMONIC-68` | Every citation of the '68 mnemonic names / 262 groups / 5,396 confirmed addresses' figure from runs/dsp-semant | other n/a — repository inventory artifact claim pn/a | supported | firmware-observation-only | strict | downgraded |
| `DOC-PCM-EVIDENCE` | PCM_EXECUTION_EVIDENCE.md's framing: that it is a replay of captured execution, its enumerated unproven list,  | other n/a — repository evidence claim pn/a | supported | not-assessed | approximation | upheld |
| `DOC-PERFORMANCE` | PERFORMANCE.md's claims: RAM-first read dispatch preserving MMIO order, packet-local fetch-header reuse with n | other n/a — repository implementation and measurement claim pn/a | supported | untested | approximation | downgraded |
| `DOC-README-DSP-IDENTITY` | README.md's characterisation of the audio DSP as an unknowable part: "The DSP (a Pioneer custom LSI with no pu | other n/a — repository documentation claim, no manual anchor pn/a | unsupported | untested | unknown | not-challenged |
| `DOC-README-DSP-MODEL-WORKS` | README.md capability claim: "The DSP model answers the load handshake, keeps the position report, locates, loo | other n/a — repository documentation claim pn/a | partial | untested | approximation | downgraded |
| `DOC-README-NO-AUDIO` | README.md "What does not" bullet: "**No audio.** The DSP ... is modelled from MAIN's side only: request words, | other n/a — repository documentation claim pn/a | partial | untested | approximation | upheld |
| `DOC-REF-CITATIONS` | Accuracy of the SPRUFE8B page, figure and table citations made by the DSP documentation | SPRUFE8B Figure 5-4 (interrupt pipeline); Figure H-7 (Uspk format); Ta | partial | untested | approximation | downgraded |
| `DOC-SH4BF-COVERAGE` | SH4_BLACKFIN_COVERAGE.md, added in HEAD commit 2eb2476, as a coverage claim | other n/a — board-side coverage claim, explicitly disclaims SH-4/Black | partial | untested | approximation | downgraded |
| `DOC-TESTCOUNTS` | Every suite pass/skip total quoted in the DSP documentation, and whether the latest headline figure is reprodu | other n/a — repository regression claim pn/a | partial | firmware-observation-only | approximation | downgraded |
| `EP-ART-CIRC` | runs/dsp-circular-transcript-replay-1/{gate,manifest,coverage}.json: assertions, mode, pinned hashes, gate com | other n/a - artifact integrity pn/a | supported | untested | approximation | downgraded |
| `EP-ART-DOCCITED` | Gate artifacts cited by DSP_BOOT_MILESTONE_AUDIT.md and DSP_INTERRUPT_ENTRY.md exist and pass every check they | SPRUFE8B 5.2 Interrupt Detection and Processing, Figure 5-4 (citation  | partial | untested | strict | downgraded |
| `EP-ART-FINAL4` | build/performance/09-combined/final-4/{gate,manifest,coverage}.json: assertions, mode, pinned hashes, chain co | other n/a - artifact integrity pn/a | supported | untested | approximation | downgraded |
| `EP-ART-PCM4` | runs/nxs-pcm-observe-4/{gate,manifest,coverage}.json: assertions, mode, pinned hashes, meaning of architectura | other n/a - artifact integrity pn/a | supported | untested | approximation | downgraded |
| `EP-ART-SEMINV` | runs/dsp-semantic-inventory-3.json: what it asserts, its mode, pinned hashes, validation_eligible and caveats | other n/a - artifact integrity, no manual requirement pn/a | supported | untested | strict | downgraded |
| `EP-CHAIN-EXTERNAL` | Every recorded run's input checkpoint and producing-run gate are locally verifiable | other n/a - provenance pn/a | partial | untested | unknown | overturned |
| `EP-CHAIN-REPLAYPY` | The program that authors every gate and decides architectural eligibility is hash-pinned in the artifacts it w | other n/a - provenance pn/a | unsupported | untested | unknown | not-challenged |
| `EP-CIRC-LOOPORACLE` | The loop-scheduler optimization regression compares against an independent reference | SPRUFE8B 7.13.1 (single cited rule inside the test oracle) pnot cited  | partial | untested | approximation | upheld |
| `EP-CIRC-REPLAY` | Expected values, transcripts and gate comparisons must derive from TI documentation or an independent oracle r | other n/a - methodology pn/a | unsupported | untested | approximation | not-challenged |
| `EP-DOC-STALE` | Committed documentation accurately describes the current tree and its own evidence | other n/a - documentation audit pn/a | partial | untested | approximation | downgraded |
| `EP-GATE-ABORTED` | An artifact claiming architectural_validation_eligible must correspond to a completed run | other n/a - artifact schema pn/a | unsupported | untested | unknown | not-challenged |
| `EP-GATE-SHALLOW` | gate.json is sufficient provenance for citing a result | other n/a - artifact schema pn/a | partial | untested | approximation | upheld |
| `EP-HASH-AUDIT` | The six source hashes recorded in DSP_BOOT_MILESTONE_AUDIT.md match the current files | other n/a - provenance verification pn/a | partial | untested | approximation | downgraded |
| `EP-PCM-SCOPE` | Exact scope of the 16 connected stops / four blocks / 2352 stereo frames S16->float32 PCM result | other n/a - firmware algorithm evidence, not an ISA requirement pn/a | supported | reference-backed-tests | strict | upheld |
| `EP-SEM-COUNTS` | What confirmed_instructions, source_fetches and source_predicate_outcomes actually count; whether coverage dis | other n/a - tooling semantics pn/a | supported | reference-backed-tests | strict | upheld |
| `EP-SEM-FETCHSUM` | packet_fetch_observations / source_fetch_observations arithmetic is a true fetch count | other n/a - tooling semantics pn/a | partial | untested | approximation | upheld |
| `EP-SEM-MODE` | What the strict versus exploratory mode switches change, and what architectural_validation_eligible means | other n/a - tooling semantics pn/a | partial | untested | approximation | downgraded |
| `EP-SEM-REPEAT` | What --verify-repeat compares and what gate passed actually means | other n/a - tooling semantics pn/a | supported | reference-backed-tests | approximation | upheld |
| `EP-TEST-CLASSIFY` | Classify the regression suite into reference-backed architecture tests, transcript/replay regressions and host | SPRUFE8B Citation spot-check: SADD p422, SSHL p493, SSUB p499, SPKERNE | supported | reference-backed-tests | approximation | downgraded |
| `EP-TEST-NOARTIFACT` | The regression suite re-verifies the integrity of the recorded evidence artifacts | other n/a - CI coverage of artifacts pn/a | unsupported | untested | unknown | not-challenged |
| `IC-CR-AMR` | AMR reachable by MVC with the six reserved MSBs not written and the documented access latency | SPRUFE8B 2.8.3 Addressing Mode Register (AMR), Figure 2-3 / Table 2-7  | partial | not-assessed | unknown | downgraded |
| `IC-CR-CSR` | CSR: CPUID/REVID read-only, PWRD, SAT clear-only, EN read-only, PCC/DCC, PGIE, GIE | SPRUFE8B 2.8.4 Control Status Register (CSR), Figure 2-4 / Figure 2-5  | partial | reference-backed-tests | approximation | downgraded |
| `IC-CR-FPCFG` | FADCR, FAUCR, FMCR reachable by MVC | SPRUFE8B 2.10.1/2.10.2/2.10.3 Floating-point configuration registers,  | partial | not-assessed | unknown | downgraded |
| `IC-CR-IER` | IER: bit 0 read-only 1 (reset enable), bit 1 NMIE set-only, bits 3-2 reserved, bits 15-4 read/write | SPRUFE8B 2.8.7 Interrupt Enable Register (IER), Figure 2-8 / Table 2-1 | supported | reference-backed-tests | approximation | upheld |
| `IC-CR-IFR` | IFR read-only, bits 15-4 maskable flags, bit 1 NMIF, bit 0 reset flag always 0 | SPRUFE8B 2.8.8 Interrupt Flag Register (IFR), Figure 2-9 / Table 2-13  | partial | reference-backed-tests | approximation | upheld |
| `IC-CR-ILC-RILC` | ILC and RILC reachable by MVC with the documented access latency | SPRUFE8B 2.9.8 ILC, 2.9.12 RILC p52 and 54 (PDF pages 52 and 54) | supported | not-assessed | unknown | upheld |
| `IC-CR-INVENTORY` | Whole control register file: how many of the manual's registers are reachable by MVC, and with what field fide | SPRUFE8B Table 2-6 (2.8 Control Register File) and Table 3-27 (MVC reg | partial | reference-backed-tests | approximation | downgraded |
| `IC-CR-IRP` | IRP is a 32-bit read/write register holding the address of the first execute packet not executed because of a  | SPRUFE8B 2.8.9 Interrupt Return Pointer Register (IRP), Figure 2-10; 5 | supported | reference-backed-tests | strict | upheld |
| `IC-CR-ISR-ICR` | ISR and ICR are write-only, affect only IFR bits 15-4, cannot touch NMI or reset, and update IFR one delay slo | SPRUFE8B 2.8.6 Interrupt Clear Register (ICR) and 2.8.10 Interrupt Set | supported | reference-backed-tests | approximation | downgraded |
| `IC-CR-ISTP` | ISTP: ISTB bits 31-10 read/write, HPEINT bits 9-5 read-only and derived as the highest-priority enabled pendin | SPRUFE8B 2.8.11 Interrupt Service Table Pointer Register (ISTP), Figur | partial | reference-backed-tests | approximation | upheld |
| `IC-CR-ITSR` | ITSR holds the saved TSR across a maskable interrupt, is fully read/write in supervisor mode, and its GIE bit  | SPRUFE8B 2.9.9 Interrupt Task State Register (ITSR), Figure 2-22 / Tab | supported | reference-backed-tests | strict | upheld |
| `IC-CR-MISC-ABSENT` | DNUM, REP, DIER, GFPGFR, GPLYA, GPLYB reachable by MVC | SPRUFE8B 2.9.2 DNUM, 2.9.11 REP, 2.9.1 DIER, 2.8.5 GFPGFR, 2.9.5 GPLYA | unsupported | untested | unknown | not-challenged |
| `IC-CR-NRP` | NRP is a 32-bit read/write register holding the NMI/exception return address consumed by B NRP | SPRUFE8B 2.8.12 Nonmaskable Interrupt (NMI) Return Pointer Register (N | partial | untested | approximation | downgraded |
| `IC-CR-NTSR` | NTSR holds the saved TSR across an NMI or exception, including the HWE bit | SPRUFE8B 2.9.10 NMI/Exception Task State Register (NTSR), Figure 2-23  | unsupported | untested | unknown | not-challenged |
| `IC-CR-PCE1` | PCE1 is the read-only E1-phase program counter | SPRUFE8B 2.8.13 E1 Phase Program Counter (PCE1) p46 (PDF page 46) | unsupported | untested | unknown | not-challenged |
| `IC-CR-SSR` | SSR bits 5-0 (M2, M1, S2, S1, L2, L1) read/write, set by functional-unit saturation one cycle after the result | SPRUFE8B 2.9.13 Saturation Status Register (SSR), Figure 2-25 / Table  | supported | reference-backed-tests | strict | upheld |
| `IC-CR-TSC` | TSCL and TSCH: read-only 64-bit free-running CPU cycle counter, enabled by writing to TSCL (value ignored) | SPRUFE8B 2.9.14 Time Stamp Counter Registers (TSCL and TSCH), Figures  | unsupported | untested | unknown | not-challenged |
| `IC-CR-TSR` | TSR: IB, SPLX, EXC, INT, CXM, DBGM, XEN, GEE, SGIE, GIE with per-bit write rules (supervisor-writable DBGM/XEN | SPRUFE8B 2.9.15 Task State Register (TSR), Figure 2-28 / Table 2-23; 8 | partial | reference-backed-tests | approximation | downgraded |
| `IC-DEV-EVENT-SOURCES` | The 124 C6747 system events of SPRUH91D Table 2-1 are actually generated by their peripherals and reach the IN | SPRUH91D 2.2.2.1 Interrupt Controller (INTC) with Table 2-1 DSP Interr | partial | firmware-observation-only | approximation | downgraded |
| `IC-DEV-INTC-COMBINER` | Event combiner: four groups of events OR-ed into EVT0..EVT3, gated by EVTMASK, with MEVTFLAG as the masked vie | other SPRUFK5A 7.2.2 Event Combiner with Figures 7-5/7-6/7-7; 7.5.2.1/ | supported | reference-backed-tests | approximation | upheld |
| `IC-DEV-INTC-EXCCOMBINER` | Exception combiner: EXPMASK gates which events drive the CPU EXCEP input; MEXPFLAG gives the masked view | other SPRUFK5A 7.5.4.1 EXPMASK with Figures 7-41..7-44 and Table 7-13; | partial | reference-backed-tests | approximation | upheld |
| `IC-DEV-INTC-REGS` | C674x megamodule INTC register file: addresses, access types, reset values and field masks | other SPRUFK5A (TMS320C674x DSP Megamodule Reference Guide, August 201 | partial | reference-backed-tests | approximation | downgraded |
| `IC-DEV-INTC-SELECT` | Interrupt selector: INTMUX1-3 map any of the 128 events onto CPUINT4..15 and present them to the CPU IFR | other SPRUFK5A 7.5.3.1 Interrupt Mux Registers with Table 7-9; 7.4.1 C | partial | reference-backed-tests | approximation | downgraded |
| `IC-DEV-NMI-SOURCE` | C6747 DSP NMI source: SYSCFG CHIPSIG.CHIPSIG4 asserts and CHIPSIG_CLR.CHIPSIG4 clears the DSP NMI; CHIPSIG2/3  | SPRUH91D 2.2.2.1.2 NMI Interrupt; SYSCFG chapter CHIPSIG / CHIPSIG_CLR | unsupported | untested | unknown | not-challenged |
| `IC-EXC-MODEL` | Exception model: EFR/ECR flags, IERR cause reporting, TSR.GEE and TSR.XEN enables, EXCEP external input, inter | SPRUFE8B Chapter 6 CPU Exceptions (6.1-6.5), Table 6-1 Exception-Relat | unsupported | untested | unknown | not-challenged |
| `IC-EXC-SWE` | Software exception: SWE sets EFR.SXF and vectors through the NMI ISFP; SWENR sets EFR.SXF, vectors through REP | SPRUFE8B 6.5.3 Software Exception (6.5.3.1 SWE, 6.5.3.2 SWENR); instru | unsupported | untested | unknown | downgraded |
| `IC-INT-CONDITIONS` | Conditions for processing a nonreset interrupt: GIE, NMIE, IER bit, no higher-priority IFR bit, and no branch  | SPRUFE8B 5.4.2 Conditions for Processing a Nonreset Interrupt; 5.2 Glo | partial | reference-backed-tests | approximation | downgraded |
| `IC-INT-ENTRY` | Nonreset maskable interrupt entry: nine empty issue cycles between the first annulled E1 and the handler's E1, | SPRUFE8B 5.4.1 / 5.4.2 / 5.4.4 with Figure 5-4 (Nonreset Interrupt Det | supported | reference-backed-tests | approximation | downgraded |
| `IC-INT-GATE-DINT-RINT` | DINT and RINT: unconditional no-unit instructions manipulating TSR.SGIE and the shared GIE bit, with a defined | SPRUFE8B DINT — Disable Interrupts and Save Previous Enable State; RIN | supported | reference-backed-tests | approximation | upheld |
| `IC-INT-IFR-STICKY` | IFR flags are sticky: a request latches while masked and survives until acceptance or an ICR write clears it | SPRUFE8B 5.4.1 Setting the Nonreset Interrupt Flag p639 (PDF page 639) | supported | reference-backed-tests | approximation | downgraded |
| `IC-INT-NESTED` | Nested maskable interrupt handling (an interrupt taken while TSR.INT is already set) | SPRUFE8B 5.6.2 Nesting Interrupts; TSR.INT in Table 2-23 p57 (TSR.INT) | unsupported | untested | unknown | downgraded |
| `IC-INT-NMI` | Nonmaskable interrupt: NMIF, NMIE life cycle, detection conditions, NTSR save, NMI ISFP vector, B NRP return,  | SPRUFE8B 5.1.1.2 Nonmaskable Interrupt (NMI); 5.4.5 with Figure 5-6; 5 | unsupported | untested | unknown | not-challenged |
| `IC-INT-PRIORITY` | Interrupt priority RESET > NMI > INT4 > ... > INT15, selection over IFR intersect IER, acceptance clears only  | SPRUFE8B 5.1.1 with Table 5-1 Interrupt Priorities; 5.1.2.1 Figure 5-2 | partial | reference-backed-tests | approximation | upheld |
| `IC-INT-RESET` | Reset interrupt: CPU state after RESET and execution beginning at the ISTB field of ISTP | SPRUFE8B 5.1.1.1 Reset (RESET); 5.3.4.1 CPU State After RESET p629 and | partial | reference-backed-tests | approximation | upheld |
| `IC-INT-RETURN-BIRP` | B IRP: five delay slots, ITSR restored into TSR in E1 with ITSR.GIE moved to GIE, PGIE unchanged, branch to th | SPRUFE8B 5.3.4.3 Returning From Maskable Interrupts with Example 5-13; | supported | reference-backed-tests | approximation | downgraded |
| `IC-PRIV-MODE` | Two-level privilege: supervisor and user mode, CXM transitions, restricted control registers, restricted instr | SPRUFE8B Chapter 8 CPU Privilege (8.1-8.4), in particular 8.2.4.1 rest | unsupported | untested | unknown | not-challenged |
| `ISA-ADDR` | Address arithmetic in byte/halfword/word/doubleword addressing modes: ADDAB ADDAD ADDAH ADDAW SUBAB SUBAH SUBA | SPRUFE8B 3.12 ADDAB/ADDAD/ADDAH/ADDAW/SUBAB/SUBAH/SUBAW; 2.8.3 AMR; Ap | partial | reference-backed-tests | approximation | upheld |
| `ISA-BITFIELD` | Constant and register bit-field operations: CLR SET EXT EXTU (4 rows) | SPRUFE8B 3.12 CLR/SET/EXT/EXTU; .S constant field format p174-176 (CLR | supported | reference-backed-tests | strict | upheld |
| `ISA-BITMANIP` | Bit manipulation and division support: LMBD NORM BITR BITC4 DEAL SHFL SHFL3 XPND2 XPND4 SUBC (10 rows) | SPRUFE8B 3.12 individual instruction entries p304-305 (LMBD), 390-391  | unsupported | not-assessed | unknown | not-challenged |
| `ISA-BRANCH` | Branches and PC arithmetic: B displacement, B register, B IRP, B NRP, BNOP displacement, BNOP register, BDEC,  | SPRUFE8B 3.12 branch entries; 5.3.4.3 (B IRP); Appendix F.3 Figures F- | partial | reference-backed-tests | unknown | upheld |
| `ISA-CTRL-MISC` | Control-register move, interrupt gating, NOP/IDLE, software exceptions and the delayed .M move: MVC DINT RINT  | SPRUFE8B 3.12 entries; 2.9 control registers; Appendix H.1/H.2 (Figure | partial | reference-backed-tests | approximation | upheld |
| `ISA-DUALRESULT` | Dual-result compound .L/.S operations: ADDSUB ADDSUB2 SADDSUB SADDSUB2 DPACK2 DPACKX2 DMV (7 rows) | SPRUFE8B 3.12 parallel/compound entries p132 (ADDSUB), 133-134 (ADDSUB | unsupported | not-assessed | unknown | not-challenged |
| `ISA-ENC-COMPACT16` | ENCODING SCOPE (not a Table A-1 row): the complete 16-bit compact instruction space - 48 formats in SPRUFE8B A | SPRUFE8B 3.10 compact instructions; C.4, D.4, E.4, F.4, G.3, H.4 16-bi | partial | firmware-observation-only | approximation | downgraded |
| `ISA-ENC-CROSSPATH` | ENCODING SCOPE (not a Table A-1 row): the x bit and 1X/2X cross-path operand selection, including the reverse- | SPRUFE8B 2.3 functional units and cross paths; Appendix C.3/D.3/E.3/F. | supported | reference-backed-tests | unknown | upheld |
| `ISA-ENC-LONG40` | ENCODING SCOPE (not a Table A-1 row): the 40-bit long-operand encodings that appear as extra opfields inside e | SPRUFE8B 2.2 general-purpose register file (40-bit register pairs); th | partial | reference-backed-tests | strict | upheld |
| `ISA-ENC-PREDICATE` | ENCODING SCOPE (not a Table A-1 row): creg/z predication in the full-width formats, the two reserved combinati | SPRUFE8B 3.6 conditional operations, Table 3-9 'Registers That Can Be  | partial | reference-backed-tests | approximation | downgraded |
| `ISA-FP-DP` | Double-precision floating-point arithmetic and compares: ABSDP ADDDP SUBDP MPYDP CMPEQDP CMPGTDP CMPLTDP RCPDP | SPRUFE8B 3.12 DP entries p105-106 (ABSDP), 125-126 (ADDDP), 541-543 (S | unsupported | not-assessed | unknown | not-challenged |
| `ISA-FP-DP-CONV` | Double-precision conversions: INTDP INTDPU DPINT DPTRUNC DPSP (5 rows) | SPRUFE8B 3.12 conversion entries p275 (INTDP), 276 (INTDPU), 258-259 ( | unsupported | not-assessed | unknown | not-challenged |
| `ISA-FP-SP` | Single-precision floating-point arithmetic and compares: ABSSP ADDSP SUBSP MPYSP CMPEQSP CMPGTSP CMPLTSP RCPSP | SPRUFE8B 3.12 SP entries; 2.10.1 FADCR Figure 2-29/Table 2-25; 2.10.2  | partial | reference-backed-tests | strict | upheld |
| `ISA-FP-SP-CONV` | Single-precision conversions: INTSP INTSPU SPINT SPTRUNC SPDP (5 rows) | SPRUFE8B 3.12 conversion entries; 2.10.1 FADCR Table 2-25 p277 (INTSP) | partial | reference-backed-tests | strict | upheld |
| `ISA-GALOIS` | Galois field multiply: GMPY GMPY4 XORMPY (3 rows) | SPRUFE8B 3.12 GMPY/GMPY4/XORMPY p270-271 (GMPY), 272-273 (GMPY4), 566- | unsupported | not-assessed | unknown | not-challenged |
| `ISA-INT-ALU32` | 32-bit integer add/sub/logic/move on .L/.S/.D: ADD ADDU SUB SUBU AND ANDN OR XOR NEG NOT MV ZERO MVK MVKL MVKH | SPRUFE8B 3.12 instruction entries; Appendix C.3/D.3/F.3 32-bit opcode  | supported | reference-backed-tests | strict | upheld |
| `ISA-INT-CMP32` | Integer compares producing 0/1: CMPEQ CMPGT CMPGTU CMPLT CMPLTU (5 rows) | SPRUFE8B 3.12 CMPEQ/CMPGT/CMPGTU/CMPLT/CMPLTU opcode maps p177-178, 18 | partial | untested | approximation | upheld |
| `ISA-INT-SAT` | Scalar saturating integer arithmetic and saturate: SADD SSUB SSHL SAT ABS (5 rows) | SPRUFE8B 3.12 SADD/SSUB/SSHL/SAT/ABS; CSR Table 2-9; SSR 2.9.13 p422-4 | partial | reference-backed-tests | strict | upheld |
| `ISA-INT-SHIFT` | Non-saturating and variable-saturating shifts and rotate: SHL SHR SHRU SSHVL SSHVR ROTL (6 rows) | SPRUFE8B 3.12 SHL/SHR/SHRU opcode maps; SSHVL/SSHVR; ROTL p447-448 (SH | partial | untested | approximation | upheld |
| `ISA-MEM-15BIT` | 15-bit-offset loads and stores from B14/B15: LDB LDBU LDH LDHU LDW STB STH STW, each '(15-bit offset)' (8 rows | SPRUFE8B 3.9.3 long-immediate addressing; Appendix C.3 Figure C-5 p724 | supported | reference-backed-tests | strict | upheld |
| `ISA-MEM-SCALAR` | Scalar loads and stores including aligned/nonaligned and doubleword: LDB LDBU LDH LDHU LDW LDDW LDNW LDNDW STB | SPRUFE8B 3.12 load/store entries; 3.9 addressing modes; 2.8.3 AMR circ | supported | reference-backed-tests | approximation | upheld |
| `ISA-MPY-PACKED` | Packed and complex multiplies and dot products: MPY2 MPY2IR MPYU4 MPYSU4 MPYUS4 CMPY CMPYR CMPYR1 DOTP2 DOTPN2 | SPRUFE8B 3.12 packed-multiply and dot-product entries p316-327 (MPY2/M | unsupported | not-assessed | unknown | not-challenged |
| `ISA-MPY16` | Non-saturating 16x16 multiplies, all signedness and halfword-selection permutations: MPY MPYH MPYHL MPYLH MPYU | SPRUFE8B 3.12 MPY family; Appendix E.3 .M 32-bit opcode map (MPY Instr | supported | untested | strict | overturned |
| `ISA-MPY16-SAT` | Saturating/left-shifted 16x16 multiplies: SMPY SMPYH SMPYHL SMPYLH SMPY2 (5 rows) | SPRUFE8B 3.12 SMPY family; Appendix E.4 Figure E-5 (compact M3 with he | unsupported | not-assessed | unknown | not-challenged |
| `ISA-MPY32` | 32-bit multiplies: MPY32 (32-bit result) MPY32 (64-bit result) MPY32SU MPY32U MPY32US MPYI MPYID SMPY32 MPYHI  | SPRUFE8B Table A-1 printed page 712 and Table B-1 printed page 718 (bo | partial | untested | strict | overturned |
| `ISA-PACK` | Pack, unpack, swap and saturating-pack: PACK2 PACKH2 PACKHL2 PACKLH2 PACKH4 PACKL4 SWAP2 SWAP4 UNPKHU4 UNPKLU4 | SPRUFE8B 3.12 PACK/UNPK/SPACK/RPACK/SHxMB entries p392-408 (PACK famil | partial | untested | strict | overturned |
| `ISA-PACK16` | Packed 2x16 arithmetic, compare, min/max and shift: ADD2 SUB2 SADD2 SSUB2 SADDUS2 SADDSU2 ABS2 AVG2 MAX2 MIN2  | SPRUFE8B 3.12 packed 2x16 entries p112-113 (ADD2), 531-532 (SUB2), 425 | unsupported | not-assessed | unknown | not-challenged |
| `ISA-PACK8` | Packed 4x8 arithmetic, compare and min/max: ADD4 SUB4 SADDU4 SUBABS4 AVGU4 MAXU4 MINU4 CMPEQ4 CMPGTU4 CMPLTU4  | SPRUFE8B 3.12 packed 4x8 entries p139-140 (ADD4), 533-534 (SUB4), 440  | unsupported | not-assessed | unknown | not-challenged |
| `ISA-SPLOOP` | Software-loop buffer instructions: SPLOOP SPLOOPD SPLOOPW SPKERNEL SPKERNELR SPMASK SPMASKR (7 rows) | SPRUFE8B Chapter 7 software pipelined loop buffer; Appendix H.4 Figure | partial | not-assessed | unknown | upheld |
| `DEFER-BOOT` | Boot Considerations: boot ROM execution, boot modes, AIS, the HPI boot path | SPRUH91D Chapter 11 Boot Considerations p239-240 (PDF pages 239-240) | unsupported | firmware-observation-only | approximation | not-challenged |
| `DEFER-EMAC` | EMAC and MDIO module | SPRUH91D Chapter 17 EMAC/MDIO Module p570-693 (PDF pages 570-693) | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-LCDC` | LCD controller (present on C6747, not on C6745) | SPRUH91D Chapter 23 Liquid Crystal Display Controller (LCDC) p931-980  | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-MEGAMOD` | Megamodule internal peripherals other than INTC: IDMA, bandwidth manager (BWM), power-down controller (PDC) | SPRUH91D 4.2 DSP Memories, 'Internal Peripherals' (names INTC, PDC, BW | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-MMCSD` | Multimedia Card (MMC) / Secure Digital (SD) card controller | SPRUH91D Chapter 25 Multimedia Card (MMC)/Secure Digital (SD) Card Con | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-MOTORCTL` | Enhanced Capture (eCAP0-2), Enhanced High-Resolution PWM (eHRPWM0-2), Enhanced Quadrature Encoder Pulse (eQEP) | SPRUH91D Chapter 13 eCAP, Chapter 14 eHRPWM, Chapter 15 eQEP p243-281, | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-PRUSS` | Programmable Real-Time Unit Subsystem: PRU0, PRU1, PRU RAM0/RAM1, PRU Config | SPRUH91D Chapter 12 Programmable Real-Time Unit Subsystem (PRUSS); Tab | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-PWRCLK` | Power Management and Device Clocking as models (beyond PSC module states and PLL0) | SPRUH91D Chapter 9 Power Management; Chapter 6 Device Clocking (6.3 Pe | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-RTC` | Real-Time Clock | SPRUH91D Chapter 26 Real-Time Clock (RTC) p1147-1170 (PDF pages 1147-1 | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-UART` | UART0, UART1, UART2 | SPRUH91D Chapter 29 Universal Asynchronous Receiver/Transmitter (UART) | unsupported | not-assessed | unknown | not-challenged |
| `DEFER-USB` | USB 2.0 controller and USB 1.1 OHCI host controller | SPRUH91D Chapter 31 Universal Serial Bus 2.0 (USB) Controller; Chapter | unsupported | not-assessed | unknown | not-challenged |
| `MEM-ADDR-ALIGN` | Aligned load/store forms require natural alignment (halfword/word/doubleword) | SPRUFE8B LDW/LDDW/STW/STDW instruction pages ('Word addresses must be  | supported | reference-backed-tests | strict | downgraded |
| `MEM-ADDR-B14B15` | *+B14/B15[ucst15] long-displacement form: .D2 only, unsigned scaled 15-bit offset, fixed base, no base update, | SPRUFE8B 3.9.3; LDW/STW 15-bit-offset instruction pages p90, 302, 408  | supported | reference-backed-tests | strict | upheld |
| `MEM-ADDR-MODES` | 12 valid .D-unit address generator options and 4 reserved mode encodings (positive/negative constant or regist | SPRUFE8B 3.9.3 Table 3-11 Address Generator Options for Load/Store p90 | supported | untested | strict | downgraded |
| `MEM-ADDR-NONALIGN` | LDNW/LDNDW/STNW/STNDW may access words/doublewords at any byte boundary | SPRUFE8B 3.8.5; LDNW/LDNDW/STNW/STNDW instruction pages p80, 292, 296, | supported | reference-backed-tests | strict | upheld |
| `MEM-ADDR-SCALE` | offsetR/ucst5 scaled by data size; LDNDW/STNDW sc bit selects scaled (x8) or nonscaled (x1) | SPRUFE8B 3.9.1.1; LDNDW and STNDW instruction pages p87, 292, 504 (PDF | supported | reference-backed-tests | strict | upheld |
| `MEM-AMR-BLOCK-VS-SIZE` | An aligned access wider than the AMR circular block size (buffer must be aligned on a byte boundary equal to t | SPRUFE8B 2.8.3 ('the buffer must be aligned on a byte boundary equal t | partial | untested | approximation | upheld |
| `MEM-AMR-CIRC-ADDA` | Circular addressing on ADDAB/ADDAH/ADDAW/ADDAD and SUBAB/SUBAH/SUBAW; there is no SUBAD | SPRUFE8B 3.9 and 3.9.2.2; ADDAB (opfield 110000/110010) and ADDAD (111 | supported | untested | strict | downgraded |
| `MEM-AMR-CIRC-LDST` | Circular addressing on LD/ST: only bits N..0 of the result update, bits 31..N+1 unchanged; result bounded to 2 | SPRUFE8B 3.9.2.1 and Examples 3-4, 3-6 p88-89 (PDF pages 88-89) | supported | untested | strict | downgraded |
| `MEM-AMR-FIELDS` | AMR layout: 2-bit mode per A4-A7/B4-B7, BK1 bits 25:21, BK0 bits 20:16, block size = 2^(N+1) bytes, mode 3 res | SPRUFE8B 2.8.3 Figure 2-3, Table 2-7, Table 2-8 p36-37 (PDF pages 36-3 | supported | untested | strict | downgraded |
| `MEM-AMR-INTERLOCK` | An MVC write to AMR immediately followed by a LD/ST/ADDA/SUBA that uses A4-A7 or B4-B7 for addressing causes a | SPRUFE8B 3.8.9 Constraints on AMR Writes p82 (PDF page 82) | unsupported | reference-backed-tests | unknown | downgraded |
| `MEM-AMR-NONALIGN` | Nonaligned circular accesses behave as the equivalent byte sequence and wrap at both buffer edges; buffer must | SPRUFE8B 3.9.2.3 Circular Addressing Considerations with Nonaligned Me | supported | reference-backed-tests | strict | downgraded |
| `MEM-AMR-WIDTH-CAPTURE` | The circular width used by a nonaligned transfer is sampled when the address is formed (E1) and reused at E3/E | SPRUFE8B 3.9.2/3.9.2.3 plus the LD/ST pipeline-stage tables p88-89, 30 | supported | untested | approximation | downgraded |
| `MEM-CACHE-BEHAVIOUR` | Cache behaviour: line allocation, fill, writeback, invalidate, L1/L2/external coherence, cache-miss timing and | other SPRUFK5A cache chapters; SPRUH91D 5.2.5 references DSP L1/L2 cac | unsupported | untested | unknown | upheld |
| `MEM-CACHE-REGS` | Cache control registers: L2CFG, L1PCFG, L1PCC, L1DCFG, L1DCC, block-base/word-count operation pairs, global co | other SPRUFK5A (C674x Megamodule Reference Guide) chapters 2-4 registe | partial | untested | approximation | downgraded |
| `MEM-L1L2-SIZES` | L1P 32 kB RAM, L1D 32 kB RAM, L2 256 kB RAM plus 1024 kB ROM; default configuration 32 kB L1P cache, 32 kB L1D | SPRUH91D 4.2 DSP Memories p80 (PDF page 80) | partial | untested | approximation | overturned |
| `MEM-MAP` | C6747 address map visible to the DSP: on-chip L1/L2/ROM, shared RAM, EMIFA, EMIFB, peripheral windows | SPRUH91D 2.3 Memory Map and 4.1 Introduction — both explicitly defer t | partial | firmware-observation-only | approximation | upheld |
| `MEM-ORDER-E1E3E5` | Store writes memory in E3; load forms its address in E1, accesses memory in E3, writes its destination in E5 | SPRUFE8B LD and ST instruction pipeline-stage tables (Read/Written/Uni | supported | reference-backed-tests | strict | upheld |
| `MEM-ORDER-VISIBILITY` | When a queued store becomes visible to a later load, and what happens on a same-cycle overlapping access | SPRUFE8B LD/ST pipeline tables; the manual states no bus-arbitration r | supported | untested | strict | downgraded |
| `MEM-PARALLEL-NONALIGN` | No other memory access may be used in parallel with a nonaligned memory access; the other .D unit may be used  | SPRUFE8B 3.8.5 Constraints on Loads and Stores p80 (PDF page 80) | supported | reference-backed-tests | strict | upheld |
| `MEM-PARALLEL-SIDE` | Two load/store instructions using a data destination/source from the same register file cannot be issued in th | SPRUFE8B 3.8.5 Constraints on Loads and Stores (T1/T2, DA1/DA2, LD1a/L | unsupported | untested | unknown | not-challenged |
| `MEM-READ-CONTRACT` | Bus reads must be side-effect-free and remain mapped between E1 validation and E3 sampling; read-clear registe | other Header contract, emulator/qemu/cdj_c674x.h:73-74 (not a manual r | partial | untested | approximation | downgraded |
| `NOTEXIST-C6747` | Peripherals that do NOT exist on C6745/C6747 and are therefore correctly absent, so they must not be counted a | SPRUH91D Full chapter list (31 chapters: Overview, DSP Subsystem, Syst | unknown | not-assessed | unknown | upheld |
| `PER-EDMA3CC` | EDMA3 channel controller: PaRAM sets, ER/ESR/CER/EER/SER/IER/IPR/EMR, QER/QEER/QSER/QEMR, DRAE/QRAE, DMAQNUM/Q | SPRUH91D Chapter 16 Enhanced Direct Memory Access (EDMA3) Controller,  | partial | reference-backed-tests | approximation | downgraded |
| `PER-EDMA3TC` | EDMA3 transfer controllers TC0 and TC1: configuration, status, error registers, burst size, read/write control | SPRUH91D Chapter 16 (transfer-controller sections); Table 3-1 lists ED | unsupported | not-assessed | unknown | not-challenged |
| `PER-EMIFA` | EMIFA: 8/16-bit asynchronous ASRAM/NOR/NAND for up to 4 devices, 4-bit NAND ECC, 16-bit SDRAM with a 128 MB ad | SPRUH91D Chapter 18 External Memory Interface A; summary at 4.2 p694-7 | unsupported | not-assessed | unknown | not-challenged |
| `PER-EMIFB` | EMIFB base controller: REVID, SDCFG, SDRFC, SDTIM1, SDTIM2, SDCFG2, BPRIO, PC1/PC2/PCC/PCMRS/PCT, IRR/IMR/IMSR | SPRUH91D Chapter 19 External Memory Interface B; 19.4 Registers, Table | partial | reference-backed-tests | approximation | upheld |
| `PER-GPIO` | GPIO banks 0-7 as four register pairs: DIR, OUT_DATA, SET_DATA, CLR_DATA, IN_DATA, SET/CLR_RIS_TRIG, SET/CLR_F | SPRUH91D Chapter 20 General-Purpose Input/Output; 20.3.2-20.3.11 p824- | partial | reference-backed-tests | approximation | not-challenged |
| `PER-HPI` | C6747 HPI peripheral registers: REVID (00h), HPIC (30h), HPIAW/HPIAR, power and emulation management; plus the | SPRUH91D Chapter 21 Host Port Interface; 21.2.2 Memory Map, Figure 21- | partial | reference-backed-tests | approximation | downgraded |
| `PER-I2C` | I2C0/I2C1: full controller (ICOAR, ICIMR, ICSTR, ICCLKL/H, ICCNT, ICDRR/ICDXR, ICSAR, ICMDR master mode, ICIVR | SPRUH91D Chapter 22 Inter-Integrated Circuit; 22.3.16 (GPIO mode), Tab | partial | reference-backed-tests | approximation | not-challenged |
| `PER-INTC-DEV` | DSP interrupt controller device registers: EVTFLAG0-3, EVTSET0-3, EVTCLR0-3, EVTMASK0-3, MEVTFLAG0-3, EXPMASK0 | other SPRUFK5A chapter 7 (7.2.2, 7.4.2) — SPRUH91D 4.2 names the INTC  | partial | untested | approximation | downgraded |
| `PER-MCASP-AFIFO` | McASP audio FIFO: AFIFOREV, WFIFOCTL, WFIFOSTS, RFIFOCTL, RFIFOSTS at offset 1000h | SPRUH91D Chapter 24, Table 24-9 McASP AFIFO Registers; 24.1.44 onwards | unsupported | untested | unknown | not-challenged |
| `PER-MCASP-CLOCK` | McASP transmit clock generation: ACLKX bit clock from CLKXDIV (divide 1-32), AHCLKX from HCLKXDIV (divide 1-40 | SPRUH91D Chapter 24, 24.1.28 AFSXCTL, 24.1.29 ACLKXCTL, 24.1.30 AHCLKX | unsupported | firmware-observation-only | approximation | not-challenged |
| `PER-MCASP-FORMAT` | McASP transmit format unit: XMASK bit mask, XFMT.XROT right rotation, XPAD/XPBIT padding, XRVRS bit reversal,  | SPRUH91D Chapter 24, 24.1.26 (XMASK) and 24.1.27 (XFMT) p1074-1075 (PD | unsupported | untested | unknown | not-challenged |
| `PER-MCASP-OUTPUT` | Serialized audio data reaching the AXR[n] pins and an external audio device | SPRUH91D Chapter 24, 24.0.21 (transmit data path and serializer operat | unsupported | untested | unknown | downgraded |
| `PER-MCASP-PINS` | McASP pin registers PFUNC, PDIR, PDOUT, PDSET, PDCLR for three instances; 16/12/4 serializers for McASP0/1/2;  | SPRUH91D Chapter 24, Table 24-7 (offsets 0h, 10h-20h), 24.1.2-24.1.8 p | partial | untested | approximation | downgraded |
| `PER-MCASP-RXREGS` | McASP receive side: RMASK, RFMT, AFSRCTL, ACLKRCTL, AHCLKRCTL, RTDM, RINTCTL, RSTAT, RSLOT, RCLKCHK, REVTCTL ( | SPRUH91D Chapter 24, Table 24-7 (64h-8Ch, 280h-2BCh) and Table 24-8 (D | unsupported | untested | unknown | not-challenged |
| `PER-MCASP-TXREGS` | McASP transmit-side control registers: GBLCTL/RGBLCTL/XGBLCTL, AMUTE, DLBCTL, DITCTL, XMASK, XFMT, AFSXCTL, AC | SPRUH91D Chapter 24, Tables 24-7 and 24-8; 24.1.9-24.1.42 p1036-1039;  | supported | firmware-observation-only | approximation | downgraded |
| `PER-MPU` | Memory Protection Unit: Supervisor/User privilege levels, fixed and programmable protection ranges, MPPA permi | SPRUH91D Chapter 5 Memory Protection Unit (MPU); 5.2.1 Privilege Level | unsupported | not-assessed | unknown | not-challenged |
| `PER-PLLC` | PLL controller: PLLCTL, OCSEL, PLLM, PREDIV, PLLDIV1-7, OSCDIV, POSTDIV, PLLCMD, PLLSTAT, ALNCTL, DCHANGE, CKE | SPRUH91D Chapter 7 Phase-Locked Loop Controller (PLLC); 7.2 PLL0 Contr | partial | reference-backed-tests | approximation | not-challenged |
| `PER-PSC` | Power and Sleep Controller PSC0/PSC1: MDSTAT, MDCTL, PTCMD, PTSTAT, populated-module and always-enabled topolo | SPRUH91D Chapter 8 Power and Sleep Controller; Tables 8-1/8-2 (topolog | partial | untested | approximation | downgraded |
| `PER-SPI` | SPI0/SPI1 register set: SPIGCR0/1, SPIINT0, SPILVL, SPIFLG, SPIPC0-5, SPIDAT0/1, SPIBUF, SPIEMU, SPIDELAY, SPI | SPRUH91D Chapter 27 Serial Peripheral Interface; 27.3.14 (SPIBUF/RXOVR | supported | untested | approximation | downgraded |
| `PER-SYSCFG` | System Configuration module: KICK0R/KICK1R, MSTPRI0-2, PINMUX0-19, CFGCHIP0-4, plus REVID, DIEIDR, DEVIDR, BOO | SPRUH91D Chapter 10 System Configuration (SYSCFG) Module, Table 10-1;  | partial | reference-backed-tests | approximation | upheld |
| `PER-TIMER64P` | Timer64P0/Timer64P1: REVID, EMUMGT, GPINT/GPEN, GPDAT/GPDIR, TIM12/TIM34, PRD12/PRD34, TCR, TGCR, WDTCR, REL12 | SPRUH91D Chapter 28 64-Bit Timer Plus; 28.2.7 Figure 28-21 and Table 2 | partial | reference-backed-tests | approximation | downgraded |
| `LOOP-BRANCH` | Branch interaction with an active loop buffer: a taken branch (not reloading) idles the buffer after the last  | SPRUFE8B 7.14 Branch Instructions; 7.7.3.2 Loop Buffer Active or Idle  | partial | reference-backed-tests | approximation | downgraded |
| `LOOP-BUF` | Loop buffer operation: instructions are loaded once and thereafter ISSUED FROM THE BUFFER indexed by LBC (cycl | SPRUFE8B 7.4.1 Loop Buffer; 7.7 Loop Buffer; 7.7.1 Software Pipeline E | partial | reference-backed-tests | approximation | upheld |
| `LOOP-EXC` | If an internal or external exception occurs while the loop buffer is active: the exception is recognized immed | SPRUFE8B 7.13.3 Exceptions pprinted page 698 (PDF page 698) | unsupported | not-assessed | unknown | not-challenged |
| `LOOP-ILC` | Initial termination condition test and ILC decrement; stage-boundary termination condition and conditional ILC | SPRUFE8B 7.9.1 Initial Termination Condition Test and ILC Decrement; 7 | supported | reference-backed-tests | approximation | upheld |
| `LOOP-INT` | Interrupting the loop buffer: all of the 7.13.1 eligibility conditions; the loop pipes down by executing an ep | SPRUFE8B 7.7.3.1 Interrupt During SPLOOP Operation; 7.13 and 7.13.1 In | partial | reference-backed-tests | approximation | downgraded |
| `LOOP-RELOAD` | Loop buffer reload for nested loops: predicated SPLOOP, SPKERNELR/SPMASKR triggering RILC->ILC, the reloading  | SPRUFE8B 7.7.3.6 Enabling (Reloading) Instructions in the Loop Buffer; | unsupported | untested | unknown | downgraded |
| `LOOP-RESCONF` | A hardware exception occurs on an unmasked resource conflict between a program-memory instruction and a loop-b | SPRUFE8B 7.15.1 Program Memory and Loop Buffer Resource Conflicts; 7.1 | unsupported | untested | approximation | not-challenged |
| `LOOP-RESUME` | Returning to an SPLOOP(D/W) after an interrupt: B IRP copies ITSR to TSR with SPLX=1, the loop pipes back up b | SPRUFE8B 7.13.2 Returning to an SPLOOP(D/W) After an Interrupt; 7.13.5 | partial | firmware-observation-only | approximation | downgraded |
| `LOOP-SCHED` | Prolog/kernel/epilog generation from one scheduled iteration time-shifted by multiples of ii; post-SPKERNEL pr | SPRUFE8B 7.7.1; 7.8.1 Prolog, Kernel, and Epilog Execution Patterns; 7 | supported | reference-backed-tests | approximation | downgraded |
| `LOOP-SETUP` | SPLOOP/SPLOOPD/SPLOOPW decode and setup: ii extraction (full-width 5-bit field and compact Figure H-5/H-6 scat | SPRUFE8B 7.5.1 SPLOOP/SPLOOPD/SPLOOPW; SPLOOP/SPLOOPD/SPLOOPW instruct | partial | reference-backed-tests | approximation | downgraded |
| `LOOP-SPKFIELD` | SPKERNEL fstg/fcyc reconstruction: Figure H-7 compact scatter (bits 15:14 -> field[4:3], bits 9:7 -> field[2:0 | SPRUFE8B SPKERNEL instruction page; Table 3-28 Field Allocation in stg | supported | reference-backed-tests | strict | upheld |
| `LOOP-SPMASK` | SPMASK/SPMASKR: must be the first instruction in its execute packet; masks L1/L2/S1/S2/D1/D2 (and M1/M2 in the | SPRUFE8B SPMASK and SPMASKR instruction pages; 7.15 Instruction Resour | partial | reference-backed-tests | approximation | upheld |
| `LOOP-WHILE` | SPLOOPW: termination determined by the recorded predicate, evaluated at stage boundaries using its value three | SPRUFE8B 7.6.3 Some Points About the SPLOOPW Example; 7.10 Loop Buffer | partial | reference-backed-tests | approximation | downgraded |
| `PKT-ANNUL` | Annulled execute packets on interrupt entry: the first annulled E1 is cycle 6 and the ISR's E1 is cycle 15, so | SPRUFE8B 5.4.4 and Figure 5-4 Nonreset Interrupt Detection and Process | supported | reference-backed-tests | approximation | upheld |
| `PKT-BRDELAY` | Five branch delay slots, branch taken the cycle after the fifth; six simultaneous in-flight branch positions;  | SPRUFE8B 7.14 Branch Instructions; B instruction entries pprinted page | partial | reference-backed-tests | approximation | downgraded |
| `PKT-BRNOP` | A branch whose delay slots expire while a multicycle NOP is still dispatching overrides the NOP; the target be | SPRUFE8B 4.4.2 Multicycle NOPs, Figure 4-32 Branching and Multicycle N | supported | reference-backed-tests | strict | upheld |
| `PKT-CIRCQ` | A queued transfer must keep the circular-buffer width it had at issue time; a later AMR change must not retarg | SPRUFE8B 2.8.3 AMR; 3.9.2 circular addressing; Table 4-44 load phases  | supported | reference-backed-tests | strict | upheld |
| `PKT-DELAY` | Delayed results: E1 compute address, E2 send address, E3 memory read/write, E4 data at CPU boundary, E5 data t | SPRUFE8B 4.4.3 Memory Considerations, Table 4-44 Program Memory Access | supported | untested | unknown | downgraded |
| `PKT-EPRES` | Execute packets spanning fetch packets may not be a branch target when either fetch packet is header-based; an | SPRUFE8B 3.10.4 Execute Packet Restrictions pprinted page 96 (PDF page | unsupported | untested | approximation | not-challenged |
| `PKT-FPHDR` | Compact fetch-packet header: header word occupies no issue slot, per-slot 16/32-bit selection, p-bits field, a | SPRUFE8B 3.10.1 fetch-packet header; 3.10.3 Processing of Fetch Packet | partial | reference-backed-tests | approximation | downgraded |
| `PKT-MCNOP` | Multicycle-NOP constraints: two multicycle-NOP generators (NOP n>1, IDLE, BNOP target,n, ADDKPC label,reg,n) c | SPRUFE8B 3.8.10 Constraints on Multicycle NOPs; 3.8.11.2/3.8.11.4/3.8. | partial | reference-backed-tests | approximation | downgraded |
| `PKT-MEMQ` | Pending memory operations; behaviour when a queued load and a queued store touch overlapping addresses in the  | SPRUFE8B 4.4.3 Memory Considerations and Figure 4-34 Program and Data  | partial | untested | approximation | downgraded |
| `PKT-PBIT` | p-bit scan left-to-right, execute-packet assembly, maximum eight instructions per execute packet | SPRUFE8B 3.5 Parallel Operations pprinted page 74 (PDF page 74) | supported | reference-backed-tests | strict | downgraded |
| `PKT-PROT` | The PROT fetch-packet header bit adds four cycles of NOP after each LD in the fetch packet, in 16-bit compact  | SPRUFE8B 3.10.1 fetch-packet header, bit 20 (PROT); 3.10.2.2 pprinted  | partial | reference-backed-tests | approximation | upheld |
| `PKT-SCHED` | cdj_dsp_scheduler.c: bounded deferred host scheduling state and its checkpoint ABI. Explicitly NOT a DSP clock | other No manual requirement - this is host policy, not architecture. S | supported | untested | unknown | downgraded |
| `PKT-STALL` | Pipeline interlocks are eliminated by design; the three cases that DO stall are memory stalls (PW/E3), the cro | SPRUFE8B Chapter 4 opening (interlocks eliminated); 4.4.3 and Figure 4 | partial | untested | approximation | downgraded |
| `PKT-UNIT` | Each instruction in an execute packet must use a different functional unit | SPRUFE8B 3.5 Parallel Operations pprinted page 74 (PDF page 74) | unsupported | untested | approximation | not-challenged |
