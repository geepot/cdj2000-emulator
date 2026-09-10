# Track 1 — instruction encodings and numerical semantics

Repository `/Users/gpotvin/Git/cdj2000-emulator`, branch `codex/macos-nxs`, HEAD `3da5ff2`.
Assessment only. No emulator source, existing test or tool was modified. One new
file was added: `tests/cstub/c674x-audit-sp-rounding.c` (characterization test,
described in section 8).

References, both verified by sha256 against `build/references/provenance.json`:

* SPRUFE8B, *TMS320C674x DSP CPU and Instruction Set Reference Guide*, July 2010,
  sha256 `34bc3631…bb7b1d37`. Printed page == PDF page throughout; every citation
  below is "printed page N (PDF page N)".
* SPRUH91D, *TMS320C6745/C6747 DSP Technical Reference Manual*, March 2013 rev
  Sept 2016, sha256 `8f00cb85…9fd76e6c`. Not load-bearing for this track: the
  C674x ISA is defined entirely in SPRUFE8B; SPRUH91D adds no instruction
  encodings.

Input artefacts re-verified as current before use — the sha256 values recorded in
`analysis/dsp/isa_probe.json` `provenance` match the present
`emulator/qemu/cdj_c674x.c` (`34e6a1c1…1442232f`), `cdj_c674x_loop.c`,
`tests/cstub/c674x-isa-probe.c`, `analysis/dsp/isa_manual_inventory.json` and
`tools/cdj_dsp/isa_probe.py`. The probe result is therefore not stale.

## 1. Scope

Covered:

* The full-width (32-bit) opcode space actually dispatched by
  `cdj_c674x_execute` (`emulator/qemu/cdj_c674x.c:1042-2380`), arm by arm,
  against the SPRUFE8B opfield tables.
* The complete 16-bit compact opcode space, measured exhaustively rather than
  inferred (section 5).
* Predication (`creg`/`z`) and cross-path (`x`) decoding against SPRUFE8B
  Table 3-9 and the Appendix C-H opcode maps (section 6).
* Integer, 40-bit long, saturating, packed and single/double-precision
  floating-point semantics, including FADCR/FAUCR/FMCR field placement,
  rounding modes, exceptional values and delayed-result latency (sections 3, 4, 7).
* The four specific probe findings the task asked to confirm or refute (section 4).

Not covered (deliberately, other tracks own these):

* Software-loop buffer behaviour beyond compact decode of SPLOOP/SPKERNEL/SPMASK
  words (`cdj_c674x_loop.c`, `loop_step`).
* Interrupt entry/return architecture, control-register architecture beyond the
  FP status registers and CSR.SAT/SSR, peripherals, EDMA, McASP, PCM pipeline,
  replay/coverage gates, firmware reachability, performance iterations.
* Multi-instruction execute-packet resource-conflict rules (SPRUFE8B 3.8) beyond
  noting which checks exist.
* Pipeline timing proof. Where a delay-slot count is quoted it is read from the
  source and compared to the manual's "Delay Slots" line; no cycle-accurate
  hardware comparison was performed.
* SPRUH91D cache/EMIF effects on instruction fetch.

## 2. Requirement groups (the denominator this report quotes)

### 2.1 A defect in the supplied denominator

`analysis/dsp/isa_manual_inventory.json` reports **238** Table A-1 rows with a
C674x check mark. The correct count is **240**. Table A-1 on printed page 712
(PDF page 712) contains two rows the parser silently dropped:

```
MPY32 (32-bit result)    ✓ ✓
MPY32 (64-bit result)    ✓ ✓
```

`tools/cdj_dsp/isa_inventory.py:46` restricts row names to
`[A-Z][A-Z0-9]*(?:\s+(?:displacement|register|IRP|NRP|\(15-bit offset\)))?`, so
the qualifier `(32-bit result)` / `(64-bit result)` fails to match. Table B-1
(printed page 718) uses the same two names, so both parsers drop them
identically and the `parse_diagnostics.in_table_b1_only` cross-check stays
empty — the diagnostic is structurally blind to a name the shared regex
rejects. A scan of printed pages 709-714 for check-marked rows finds exactly
these two and no others.

Both dropped rows are *implemented*: `cdj_c674x.c:1820-1860` handles MPY32
opfield `10000` (32-bit scalar result) and `10100` (64-bit pair result). The
omission understates both denominator and numerator.

All counts below use **240 rows**. MPY32's two rows are folded into
`ISA-MPY32`.

### 2.2 Grouping

25 groups, an exact partition of the 240 rows (verified programmatically: no row
in two groups, no row unassigned). Membership is listed in full because it is
the denominator.

| Group | n | Members |
|---|---|---|
| ISA-INT-ALU32 | 17 | ADD ADDU SUB SUBU AND ANDN OR XOR NEG NOT MV ZERO MVK MVKL MVKH MVKLH ADDK |
| ISA-INT-CMP32 | 5 | CMPEQ CMPGT CMPGTU CMPLT CMPLTU |
| ISA-INT-SAT | 5 | SADD SSUB SSHL SAT ABS |
| ISA-INT-SHIFT | 6 | SHL SHR SHRU SSHVL SSHVR ROTL |
| ISA-BITFIELD | 4 | CLR SET EXT EXTU |
| ISA-BITMANIP | 10 | LMBD NORM BITR BITC4 DEAL SHFL SHFL3 XPND2 XPND4 SUBC |
| ISA-GALOIS | 3 | GMPY GMPY4 XORMPY |
| ISA-PACK | 15 | PACK2 PACKH2 PACKHL2 PACKLH2 PACKH4 PACKL4 SWAP2 SWAP4 UNPKHU4 UNPKLU4 SPACK2 SPACKU4 RPACK2 SHLMB SHRMB |
| ISA-PACK16 | 15 | ADD2 SUB2 SADD2 SSUB2 SADDUS2 SADDSU2 ABS2 AVG2 MAX2 MIN2 CMPEQ2 CMPGT2 CMPLT2 SHR2 SHRU2 |
| ISA-PACK8 | 10 | ADD4 SUB4 SADDU4 SUBABS4 AVGU4 MAXU4 MINU4 CMPEQ4 CMPGTU4 CMPLTU4 |
| ISA-DUALRESULT | 7 | ADDSUB ADDSUB2 SADDSUB SADDSUB2 DPACK2 DPACKX2 DMV |
| ISA-MPY16 | 16 | MPY MPYH MPYHL MPYLH MPYU MPYUS MPYSU MPYHU MPYHSU MPYHUS MPYHLU MPYHSLU MPYHULS MPYLHU MPYLSHU MPYLUHS |
| ISA-MPY16-SAT | 5 | SMPY SMPYH SMPYHL SMPYLH SMPY2 |
| ISA-MPY32 | 16 | MPY32 (32-bit result) MPY32 (64-bit result) MPY32SU MPY32U MPY32US MPYI MPYID SMPY32 MPYHI MPYHIR MPYIH MPYIHR MPYIL MPYILR MPYLI MPYLIR |
| ISA-MPY-PACKED | 22 | MPY2 MPY2IR MPYU4 MPYSU4 MPYUS4 CMPY CMPYR CMPYR1 DOTP2 DOTPN2 DOTPRSU2 DOTPRUS2 DOTPNRSU2 DOTPNRUS2 DOTPSU4 DOTPU4 DOTPUS4 DDOTP4 DDOTPH2 DDOTPH2R DDOTPL2 DDOTPL2R |
| ISA-FP-SP | 9 | ABSSP ADDSP SUBSP MPYSP CMPEQSP CMPGTSP CMPLTSP RCPSP RSQRSP |
| ISA-FP-SP-CONV | 5 | INTSP INTSPU SPINT SPTRUNC SPDP |
| ISA-FP-DP | 11 | ABSDP ADDDP SUBDP MPYDP CMPEQDP CMPGTDP CMPLTDP RCPDP RSQRDP MPYSPDP MPYSP2DP |
| ISA-FP-DP-CONV | 5 | INTDP INTDPU DPINT DPTRUNC DPSP |
| ISA-MEM-SCALAR | 14 | LDB LDBU LDH LDHU LDW LDDW LDNW LDNDW STB STH STW STDW STNW STNDW |
| ISA-MEM-15BIT | 8 | LDB/LDBU/LDH/LDHU/LDW (15-bit offset), STB/STH/STW (15-bit offset) |
| ISA-ADDR | 7 | ADDAB ADDAD ADDAH ADDAW SUBAB SUBAH SUBAW |
| ISA-BRANCH | 10 | B displacement, B register, B IRP, B NRP, BNOP displacement, BNOP register, BDEC, BPOS, CALLP, ADDKPC |
| ISA-CTRL-MISC | 8 | MVC DINT RINT NOP IDLE SWE SWENR MVD |
| ISA-SPLOOP | 7 | SPLOOP SPLOOPD SPLOOPW SPKERNEL SPKERNELR SPMASK SPMASKR |

Three cross-cutting *encoding-scope* rows are also carried in the matrix. They
are dimensions of the rows above, not additional Table A-1 rows, and are
excluded from the 240: `ISA-ENC-COMPACT16`, `ISA-ENC-PREDICATE`,
`ISA-ENC-CROSSPATH`, `ISA-ENC-LONG40`.

### 2.3 Headline counts (not percentages of "the ISA")

By group, using the probe's decode-acceptance status plus source reading:

* Groups where every member is dispatched by `cdj_c674x_execute` for at least
  one documented encoding: ISA-INT-ALU32 (17), ISA-BITFIELD (4), ISA-MPY16 (16),
  ISA-MEM-SCALAR (14), ISA-MEM-15BIT (8) — 59 rows.
* Groups entirely absent from the decoder: ISA-BITMANIP (10), ISA-GALOIS (3),
  ISA-PACK16 (15), ISA-PACK8 (10), ISA-DUALRESULT (7), ISA-MPY16-SAT (5),
  ISA-MPY-PACKED (22), ISA-FP-DP (11), ISA-FP-DP-CONV (5) — **88 rows with no
  implementation at all**.
* Mixed groups: ISA-INT-CMP32, ISA-INT-SAT, ISA-INT-SHIFT, ISA-PACK,
  ISA-MPY32, ISA-FP-SP, ISA-FP-SP-CONV, ISA-ADDR, ISA-BRANCH, ISA-CTRL-MISC,
  ISA-SPLOOP.

A caveat on the probe's 109 "all-probed-forms-accepted" rows: five of them
(MV, NEG, NOT, ZERO, SWAP2) are **assembler pseudo-operations**. SPRUFE8B
printed pages 374, 387, 392, 572 and 553 state that the assembler emits
ADD/OR, SUB, XOR, MVK and PACKLH2 respectively. Their acceptance is the
acceptance of those base instructions, not independent coverage.

## 3. What the full-width decoder actually dispatches

Read arm by arm from `cdj_c674x.c:1042-2380`. The dispatch chain's terminal
`else` is `return stop(..., "instruction not implemented")` at
`cdj_c674x.c:2377`, so anything not listed here is absent by construction.

| Family | Source | Notes |
|---|---|---|
| NOP / PROT multicycle | 1065-1086 | `nop_cycles` 589-598; count > 9 rejected |
| compact lowering | 1087-1505 | section 5 |
| CALLP (full + compact Scs10) | 1163-1190 | six-cycle, B3/A3 return |
| DINT / RINT | 1505-1530 | TSR.GIE/SGIE pair |
| SADD/SSUB signed-40 | 1539-1573 | 40-bit saturate, CSR.SAT + SSR in E2 |
| SADD/SSUB/SSHL scalar 32 | 1574-1602 | opfields 278/258/820/1f8/3f8/1d8/8e0/8a0 |
| scalar memory (incl. nonaligned, doubleword, 15-bit offset) | 1603-1676 | E1 address / E3 RAM / E5 destination |
| ADDK | 1677-1681 | |
| MVK / MVKH / MVKLH | 1682-1686 | |
| ADDAB/H/W/D, SUBAB/H/W (.D, non-cross) | 1686-1707 | opfields 0x30-0x3d only |
| ADD/SUB .D non-cross and cross | 1708-1725 | |
| MVK .D, MVK .L | 1723-1726 | |
| ADD/ADDU/SUB/SUBU 40-bit long .L | 1726-1818 | opfields 20,21,23,24,27,29,2b,2f,37,3f |
| MPY32 / MPY32U / MPY32SU / MPY32US | 1819-1871 | E4, pair result |
| 16x16 non-saturating MPY family (all 16 permutations + 2 const forms) | 1872-1960 | E2 delayed scalar |
| MPYIH/MPYHI/MPYIL/MPYLI (+R) | 1961-1999 | E4 |
| MVD | 2000-2018 | E4 |
| INTSP / INTSPU | 2019-2046 | FADCR rounding, INEX |
| SPINT / SPTRUNC | 2047-2072 | SPTRUNC forces mode 1 |
| ABSSP | 2073-2098 | FAUCR |
| CMPEQSP / CMPGTSP / CMPLTSP | 2099-2115 | FAUCR, UNORD |
| ADDSP / SUBSP (.L and .S, both reverse layouts) | 2116-2151 | FADCR |
| MPYSP | 2152-2177 | FMCR |
| PACK2/PACKH2/PACKHL2/PACKLH2/PACKL4/PACKH4 | 2178-2203 | |
| ANDN (.L/.S/.D) | 2204-2207 | |
| AND/ADD/SUB/OR/XOR const and register forms, reverse-cross .L SUB | 2208-2224 | |
| CMPEQ/CMPGT/CMPGTU/CMPLT/CMPLTU (32-bit .L only) | 2225-2253 | |
| CLR/SET/EXT/EXTU | 2254-2275 | |
| SHL/SHR/SHRU (.S, 32-bit only) | 2276-2282 | |
| MVC both directions | 2283-2296 | gated by `control_{read,write}_supported` |
| B IRP | 2297-2314 | |
| B displacement, BDEC, BNOP displacement, B register (2 forms), ADDKPC | 2315-2376 | |

Numerical-semantics helpers: `saturate32` 226, `saturating_shift32` 254,
`shift_32` 280, `integer_to_sp` 293, `compare_sp` 330, `add_sub_sp` 365,
`multiply_sp` 477, `sp_to_integer` 539, `register_long40` 273.

Everything else in the 240 rows is genuinely absent. I spot-checked **48**
mnemonics from the probe's 98 "instruction not implemented" set —
ABSDP ADDDP SUBDP MPYDP CMPEQDP RCPDP RSQRDP RCPSP RSQRSP SPDP DPSP INTDP
INTDPU DPINT DPTRUNC ADD2 SUB2 ADD4 DOTP2 MAX2 MIN2 SADD2 UNPKHU4 SPACK2
CMPEQ2 MPY2 SMPY SAT SUBC LMBD NORM BITC4 BITR DEAL SHFL ROTL GMPY XPND2
SWAP4 SSHVL SSHVR SHR2 SHLMB MPYI MPYID ABS ABS2 DMV BPOS SWE SWENR — with
`grep -c '\b<mnemonic>\b' emulator/qemu/cdj_c674x.c`. Only three produced
hits, and all three are false positives: `SAT` (5 hits, all `CSR.SAT`,
`CDJ_C674X_DELAYED_SAT`, "saturation"), `SWE` and `SWENR` (1 hit each, only in
`interrupt_gate_parallel_conflict` at 639-641, which detects a *format-level*
packet conflict and never executes them). So the 98 count is not a probe
artefact: these really are absent, including the entire double-precision family
(ABSDP ADDDP SUBDP MPYDP CMPEQDP CMPGTDP CMPLTDP RCPDP RSQRDP MPYSPDP
MPYSP2DP INTDP INTDPU DPINT DPTRUNC DPSP) and the SP transcendental
approximations RCPSP/RSQRSP and the SP↔DP conversions SPDP/DPSP.

## 4. The four specific probe findings

### 4.a 98 "instruction not implemented" rows — CONFIRMED

See section 3. Of the floating-point families named in the task: ABSDP, ADDDP,
MPYDP, RCPSP, RSQRSP, SPDP, DPSP, INTDP, DPINT, DPTRUNC are all confirmed
absent. SPINT and SPTRUNC are **not** absent (the task list conflated them);
they are implemented at `cdj_c674x.c:2047-2072`.

Of the SP rows that probe as accepted, all are genuinely implemented with
manual-matched rounding and status, not merely accepted:

* **ABSSP** (`2073`): treats NaN→`0x7fffffff` + INVAL (+ INFO-free), SNaN adds
  bit 4, denormal→0 with INEX|DEN2, infinity→INFO. Matches SPRUFE8B ABSSP
  printed pages 107-108 and the FAUCR field map (Table 2-26, printed page 61).
* **ADDSP/SUBSP** (`2116`): FADCR rounding from bits 10-9 / 26-25, which is what
  the NOTE on printed page 59 requires even for the `.S`-unit forms. Explicit
  denormal flush, inf-minus-inf INVAL, overflow/underflow per rounding mode.
* **MPYSP** (`2152`): FMCR, exact 48-bit integer product rounded once.
* **CMPEQSP/CMPGTSP/CMPLTSP** (`2099`): denormals compare as signed zero, NaNs
  unordered with FAUCR UNORD bit 9 and INVAL on the ordered relations.
* **INTSP/INTSPU** (`2019`) and **SPINT/SPTRUNC** (`2047`): four FADCR modes,
  sticky INEX, saturation with INEX|OVER.

FADCR/FAUCR/FMCR bit placement was checked line by line against SPRUFE8B
Figure 2-29 / Table 2-25 (printed pages 59-60), Figure 2-30 / Table 2-26
(printed pages 61-62) and Figure 2-31 / Table 2-27 (printed page 63):
NAN1=0, NAN2=1, DEN1=2, DEN2=3, INVAL=4, INFO=5, OVER=6, INEX=7, UNDER=8,
UNORD=9 (FAUCR only), DIV0=10 (FAUCR only), RMODE=10:9 (FADCR/FMCR only), and
the whole set repeated at +16 for unit 2. The implementation matches exactly.

### 4.b ADDAB/ADDAH/ADDAW B14/B15+ucst15 rejected as "reserved predicate" — **DECODER BUG, CONFIRMED**

SPRUFE8B printed page 115 (PDF page 115) carries a *second* Opcode figure for
ADDAB:

```
31 30 29 28 | 27   23 | 22        8 | 7 | 6 5 4 3 2 | 1 | 0
 0  0  0  1 |   dst   |    ucst15   | y | 0 1 1 1 1 | s | p
```

and the description on the same page says verbatim: *"This instruction is
executed unconditionally, it cannot be predicated."* Bits 31-28 are a **fixed
opcode field `0001`**, not `creg`/`z`. The same second figure appears for ADDAH
(printed page 120) and ADDAW (printed page 123); ADDAD has no such form
(printed pages 117-118), consistent with the probe, where ADDAD is
all-forms-accepted.

The probe's rejected word `0x1280043C` decodes under that figure as
`ADDAB .D1 B14, 4, A5`: bits 31-28 = `0001`, dst = 5, ucst15 = 4, y = 0 (B14),
bits 6-2 = `01111`, s = 0, p = 0. `cdj_c674x.c:1529-1531` computes
`creg = w >> 29` (= 0) and `z = (w >> 28) & 1` (= 1) and rejects `creg == 0 &&
z == 1` as "reserved predicate". That rejection is correct *for the load/store
and .L/.S/.M formats*, where Table 3-9 (printed page 77) does mark `000`/`z=1`
reserved, but it fires before any opcode classification and so it also closes
the ADDAB/ADDAH/ADDAW long-immediate subspace.

The aliasing is worth stating: had the predicate check not fired, these words
would have fallen into the 15-bit-offset memory arm at `cdj_c674x.c:1603`
(`(w & 0x0c) == 12`, op = `(w>>4)&7` = 3) and executed as **STB with a 15-bit
offset**. So the current behaviour is fail-closed rather than wrong, but the
instruction is unimplemented and its diagnostic is misleading. TI deliberately
encodes these three instructions in the reserved-predicate hole of the
load/store format, distinguished by bits 31-28 = `0001`.

Classification: ISA-ADDR — implementation `partial` (register and ucst5 forms on
`.D1/.D2` implemented at `cdj_c674x.c:1686-1707`; the B14/B15+ucst15 form
unimplemented and misdiagnosed).

### 4.c IDLE rejected as "reserved NOP count" — CONFIRMED, misclassified

SPRUFE8B printed page 274 (PDF page 274) gives IDLE's opcode as bits 16-13 =
`1111` with all other bits zero except `p`, i.e. the word `0x0001E000` — exactly
the probe's word. Printed page 388 gives NOP the same format with `src` in bits
16-13 encoding `count - 1`, and states *"The maximum value for count is 9"*.

`nop_cycles` (`cdj_c674x.c:589-598`) matches `(word & 0xfffe1ffe) == 0` and
returns `((word >> 13) & 15) + 1`, so it returns 16 for IDLE; the caller at
`cdj_c674x.c:1068` rejects `nop > 9` as "reserved NOP count". For `src` =
9..14 (count 10..15) that rejection is correct — those are genuinely reserved.
For `src` = 15 it is not: that is the IDLE instruction, a distinct Table A-1 row
with a defined architectural behaviour ("infinite multicycle NOP that terminates
upon servicing an interrupt, or a branch occurs due to an IDLE instruction being
in the delay slots of a branch"). `cdj_c674x.c:631` already comments
"Includes full-width IDLE", so the code knows the word is IDLE and still reports
a reserved count.

Classification: IDLE — implementation `unsupported`, and a diagnostic-accuracy
defect (a supported-looking rejection reason for an unimplemented instruction).
The CPU's `idle_cycles` field is unrelated: it holds interrupt-drain and
single-cycle-packet padding, not IDLE.

### 4.d 40-bit long forms of CMPEQ/CMPGT/CMPGTU/CMPLT/CMPLTU and SHL/SHR/SHRU — CONFIRMED unimplemented

The `.L` "1 or 2 sources" format places the 7-bit opfield at bits 11-5 with bits
4-3 = `11`, bit 2 = `0`, so `w & 0xffc == (op << 5) | 0x18`. The `.S` shift
format places a 6-bit opfield at bits 11-6 with bit 5 = `1`, bits 4-2 = `000`,
so `w & 0xffc == (op << 6) | 0x20` and the decoder's `0xfbc` mask deliberately
frees bit 6 (the constant-vs-register selector).

| Instruction | Manual page | Opfield | Operands | `w & 0xffc` (`0xfbc` for shifts) | In decoder? |
|---|---|---|---|---|---|
| CMPEQ | 177 | 1010011 | sint, xsint | 0xa78 | yes (2225) |
| CMPEQ | 177 | 1010010 | scst5, xsint | 0xa58 | yes |
| CMPEQ | 177 | **1010001** | xsint, **slong** | 0xa38 | **no** |
| CMPEQ | 177 | **1010000** | scst5, **slong** | 0xa18 | **no** |
| CMPGT | 188 | 1000111 / 1000110 | 32-bit | 0x8f8 / 0x8d8 | yes |
| CMPGT | 188 | **1000101 / 1000100** | **slong** | 0x8b8 / 0x898 | **no** |
| CMPGTU | 197 | 1001111 / 1001110 | 32-bit (src1 = ucst4) | 0x9f8 / 0x9d8 | yes |
| CMPGTU | 197 | **1001101 / 1001100** | **ulong** | 0x9b8 / 0x998 | **no** |
| CMPLT | 202 | 1010111 / 1010110 | 32-bit | 0xaf8 / 0xad8 | yes |
| CMPLT | 202 | **1010101 / 1010100** | **slong** | 0xab8 / 0xa98 | **no** |
| CMPLTU | 211 | 1011111 / 1011110 | 32-bit (src1 = ucst4) | 0xbf8 / 0xbd8 | yes |
| CMPLTU | 211 | **1011101 / 1011100** | **ulong** | 0xbb8 / 0xb98 | **no** |
| SHL | 447 | 110011 / 110010 | xsint → sint | 0xca0 | yes (2276) |
| SHL | 447 | **110001 / 110000** | **slong → slong** | 0xc20 | **no** |
| SHL | 447 | **010011 / 010010** | **xuint → ulong** | 0x4a0 | **no** |
| SHR | 451 | 110111 / 110110 | xsint → sint | 0xda0 | yes |
| SHR | 451 | **110101 / 110100** | **slong → slong** | 0xd20 | **no** |
| SHRU | 457 | 100111 / 100110 | xuint → uint | 0x9a0 | yes |
| SHRU | 457 | **100101 / 100100** | **ulong → ulong** | 0x920 | **no** |

Confirmed exactly as the probe reports: all `slong`/`ulong` operand forms of
these eight instructions are unimplemented while the 32-bit forms are
dispatched. Note the asymmetry with SADD/SSUB, whose 40-bit forms *are*
implemented (`cdj_c674x.c:1539-1573`), and with ADD/ADDU/SUB/SUBU, whose 40-bit
forms are implemented (`1726-1818`).

Two manual-derived fidelity observations on the implemented 32-bit shifts:

* SPRUFE8B printed pages 447/451/457 all say "When a register is used, the six
  LSBs specify the shift amount and valid values are 0-40 … If 39 < src1 < 64,
  src2 is shifted by 40." `shift_32` (`cdj_c674x.c:280`) clamps at 32, not 40.
  For a 32-bit destination the two clamps are indistinguishable (left and
  logical-right give 0; arithmetic-right gives the sign fill for any count ≥ 32),
  so this is **not** a fidelity gap for the implemented forms. It would become
  one if the 40-bit forms were added.
* SPRUFE8B gives CMPGTU/CMPLTU's immediate operand type as **ucst4**, not ucst5
  (printed pages 197, 211). `cdj_c674x.c:2229` uses the full 5-bit `src1` field
  zero-extended, so it would also execute `src1` values 16-31 that the
  assembler will not emit. This is a permissive decode, not a wrong result for
  legal encodings; recorded as an open question since SPRUFE8B does not say what
  hardware does with bit 17 set.

## 5. Compact (16-bit) encodings

The probe does not cover compact forms, so I measured them exhaustively with a
read-only scratch probe
(`/private/tmp/.../scratchpad/compact_probe.c`, compiled against the unmodified
core) that builds a single-instruction compact packet for every one of the
65,536 16-bit words, under 12 header configurations (plain, SAT, BR, SAT|BR, RS,
DSZ 1-7) and two register profiles, and records the first accepting combination
or the last fault string. Acceptance here means the decoder dispatched the word;
it is not a semantic claim.

Result over 65,536 words: 53,856 accepted, 11,440 "compact instruction not
implemented", 176 "reserved compact LSDx1 instruction", 64 "unaligned stack
access or invalid register pair".

### 5.1 Compact denominator from the manual

SPRUFE8B Appendices C.4, D.4, E.4, F.4, G.3 and H.4 define **48** 16-bit opcode
formats: C-8…C-21 (14), D-4…D-11 (8), E-5 (1), F-17…F-32 (16), G-1…G-4 (4),
H-5…H-9 (5). Cross-referencing every instruction's own "Compact Instruction
Format" table in section 3.12 gives **58 of the 240 Table A-1 rows with a
documented compact form** and **47 of the 48 formats referenced by at least one
instruction**. (Note: `isa_manual_inventory.json` counts 41 rows carrying
Table A-1's compact footnote; that footnote sits in the C64x+ column, as the
inventory's own caveat says, and 58 is the figure derived from section 3.12
itself.)

The one unreferenced format is **Figure D-6 "Ltbd"** (printed page 737). It has
no mnemonic table, and a full-text search of SPRUFE8B finds "Ltbd" only in the
list of figures and the figure caption — no instruction lists it. See open
questions.

### 5.2 Per-format coverage (measured)

| Figure | Format | Words matching | Accepted | Verdict |
|---|---|---|---|---|
| C-8 | Doff4 / Doff4DW | 8192 | 8192 | lowered, `cdj_c674x.c:1092` |
| C-10 | Dind / DindDW | 4096 | 4096 | lowered, 1093 |
| C-12 | Dinc / DincDW | 1024 | 1024 | lowered, 1094 |
| C-14 | Ddec / DdecDW | 1024 | 1024 | lowered, 1095 |
| C-16 | Dstk | 1024 | 1024 | 1392 |
| C-17 | Dx2op | 512 | 512 | 1238 |
| C-18 | Dx5 | 512 | 512 | 1220 |
| C-19 | Dx5p | 64 | 64 | 1228 |
| C-20 | Dx1 | 128 | 80 | via LSDx1 handler 1298; rejects are the manual's Reserved rows |
| C-21 | Dpp | 256 | 192 | 1430; 64 rejects are odd register pairs for the 8-byte form |
| D-4 | L3 | 4096 | 4096 | 1490 |
| D-5 | L3i | 4096 | 4096 | 1271 |
| **D-6** | **Ltbd** | **4096** | **0** | **unhandled; no instruction references this format** |
| D-7 | L2c | 4096 | 4096 | 1334 |
| D-8 | Lx5 | 512 | 512 | 1318 |
| D-9 | Lx3c | 256 | 256 | 1321 |
| D-10 | Lx1c | 256 | 256 | 1324 |
| D-11 | Lx1 | 128 | 104 | via LSDx1 1298 |
| **E-5** | **M3** | **4096** | **0** | **compact `.M` multiply family entirely unimplemented** |
| F-17/18 | Sbs7 / Sbu8 | 2048 | 2048 | 1373 |
| F-19 | Scs10 (CALLP) | 2048 | 2048 | 1169 |
| F-20/21 | Sbs7c / Sbu8c | 4096 | 4096 | 1373 |
| F-22 | S3 | 4096 | 4096 | 1204 |
| F-23 | S3i | 4096 | 4096 | 1245 |
| F-24 | Smvk8 | 4096 | 4096 | 1292 |
| F-25 | Ssh5 | 2048 | 2048 | 1254 |
| F-26 | S2sh | 512 | 512 | 1264 |
| F-27 | Sc5 | 2048 | 2048 | 1276 |
| F-28 | S2ext | 512 | 512 | 1276 |
| **F-29** | **Sx2op** | **512** | **0** | **compact in-place `.S` ADD/SUB unimplemented** |
| F-30 | Sx5 | 512 | 512 | 1212 |
| F-31 | Sx1 | 128 | 104 | via LSDx1 1298 + MVC-to-ILC 1363 |
| **F-32** | **Sx1b** | **256** | **128** | **`s = 0` (.S1) half unconditionally rejected — see below** |
| G-1 | LSDmvto | 4096 | 3072 | 1475; 1024 rejects are `unit = 3`, Reserved |
| G-2 | LSDmvfr | 4096 | 3072 | same handler |
| G-3 | LSDx1c | 512 | 384 | 1197; rejects are `unit = 3` |
| G-4 | LSDx1 | 512 | 280 | 1298; rejects are the manual's Reserved rows |
| H-9 | Unop | 8 | 8 | `nop_cycles` 591 |
| H-5/6/7/8 | Uspl/Uspldr/Uspk/Uspm | — | not probed | loop-buffer track; compact SPMASK decode is present at `spmask_decode` 662 |

Three genuine compact gaps:

1. **Figure E-5 "M3"** (printed page 744): the only compact `.M` format, and it
   encodes eight instructions — MPY, MPYH, MPYLH, MPYHL with header SAT = 0 and
   SMPY, SMPYH, SMPYLH, SMPYHL with SAT = 1. All 4096 matching words are
   rejected. Note `instruction_unit` (`cdj_c674x.c:686`) *does* classify
   `(w & 0x1e) == 0x1e` as a `.M` instruction, so the format is recognised for
   unit classification but never executed — exactly the kind of place where
   "disassembly" and "implementation" must not be conflated. MPY/MPYH/MPYLH/MPYHL
   have working 32-bit encodings; SMPY/SMPYH/SMPYLH/SMPYHL have none at either
   width, so the whole ISA-MPY16-SAT group is unsupported.
2. **Figure F-29 "Sx2op"** (printed page 755): compact in-place `.S`
   `ADD src1, src2, dst (src1 = dst)` and `SUB`. All 512 words rejected. The
   adjacent Sx5 (F-30, bit 10 = 1) is handled; Sx2op differs only in bit 10 = 0
   and bit 6 = 0.
3. **Figure F-32 "Sx1b"** (printed page 756): `BNOP (.unit) src2, N3`, with the
   manual's NOTE "src2 from B0-B15". The `s` bit selects `.S1` vs `.S2`; because
   `src2` is always a B register, `s = 0` is architecturally legal.
   `cdj_c674x.c:1355` matches `(w & 0x187f) == 0x006f`, which requires bit 0 = 1,
   so all 128 `s = 0` words are rejected as "compact instruction not
   implemented". The data path it would need (`cpu->r[1][(w >> 7) & 15]`) is
   already correct. Contrast Figure F-31 op `110` (MVC to ILC), where the manual
   explicitly writes "(s = 1)" and the decoder's `s = 1` requirement at
   `cdj_c674x.c:1363` is right.

## 6. Predication and cross paths

### 6.1 creg / z — correct

`cdj_c674x.c:1529-1535`:

```c
unsigned creg = w >> 29, z = (w >> 28) & 1;
if (creg == 7 || (!creg && z)) return stop(cpu, pc, insn->word, "reserved predicate");
if (creg) {
    static const unsigned bank[] = {0,1,1,1,0,0,0};
    static const unsigned index[] = {0,0,1,2,1,2,0};
    enabled = (cpu->r[bank[creg]][index[creg]] != 0) ^ z;
}
```

Compared against SPRUFE8B Table 3-9, "Registers That Can Be Tested by
Conditional Operations", printed page 77 (PDF page 77): `000`/`z=0`
unconditional, `000`/`z=1` Reserved, `001`→B0, `010`→B1, `011`→B2, `100`→A1,
`101`→A2, `110`→A0, `111`→Reserved for any z. The table mapping is exact,
including both reserved cases and the `^ z` polarity (section 3.6: "If z = 1,
the test is for equality with zero").

Validation: the scalar saturating tests issue every form under
`creg = 001` (B0) with `c.r[1][0] = enabled` and assert the destination is
untouched when false —
`tests/cstub/c674x-saturation.c:70` builds the word with `(1u << 29)` and
line 75 asserts
`c.r[side][3] == (enabled ? expected : 0x11223344u)`, with `expected` computed
by an in-test reference model (`clamp`, line 23), not by the emulator. The
floating-point blocks use `creg = 010` (B1) and `creg = 110` (A0) with both `z`
polarities (`tests/cstub/c674x.c:736-737`, `969`, `1505`) and assert no result
**and no FADCR/FMCR warning** is produced by a false predicate
(`tests/cstub/c674x.c:971`: `assert(c.r[0][4] == 99 && !c.load_count &&
!c.control[18]);`). That is genuine predicate-body-versus-source-fetch evidence
for those opcodes. No test exercises `creg = 011` (B2), `101` (A2), or the two
reserved encodings on a *valid* opcode; `tests/test_dsp_isa_audit.py:77` covers
`0xffffffff` (creg = 111) only.

Compact predication: SPRUFE8B 3.6 (printed page 77) states "Compact (16-bit)
instructions on the DSP do not contain a creg field and always execute
unconditionally." The separate compact predicate mechanisms are the 2-bit CC
field of Figure G-3 LSDx1c (A0/!A0/B0/!B0, handled at `cdj_c674x.c:1197`) and
the `z` bit of Figures F-20/F-21 Sbs7c/Sbu8c (handled at `cdj_c674x.c:1373-1381`,
where the comment and code correctly apply the predicate to the branch only and
never to the inserted NOPs — SPRUFE8B printed page 166 NOTE 1). Both are
implemented.

### 6.2 Cross path (x bit) — correct for the formats implemented, and tested

`cdj_c674x.c:1155`: `unsigned cross = side ^ ((w >> 12) & 1);` with
`a = (w >> 13) & 31` (src1, read from `cpu->r[side][a]`) and
`b = (w >> 18) & 31` (src2, read from `cpu->r[cross][b]`). That matches the
`.L`/`.S`/`.M` "1 or 2 sources" opcode maps, where src2 is the `xsint`/`xuint`
operand. The reverse-layout exceptions are handled explicitly and separately:
reverse-cross `.L` SUB at `2222` (`cpu->r[cross][a] - cpu->r[side][b]`),
SSUB opfield `0x3f8` at `1590`, SUBSP opfield `0x2b8` at `2128`, and the
non-cross `.D` ADD/SUB at `1708-1722` whose operand order is src2,src1 because
the D-unit hardware subtracts the src1 field from the src2 field (SPRUFE8B
printed pages 110, 529). Memory formats use the `y` bit at position 7, not bit
12 (`bank = (w >> 7) & 1`, `1623`), matching Figure C-4.

Cross-path operand selection is established against the manual by tests that
place the operand in the opposite register file and assert a hand-computed
result:

* `tests/cstub/c674x-saturation.c:59-75` loops `cross` 0/1 and writes
  `c.r[side ^ cross][2] = values[j]`, asserting `clamp(a ± b)`.
* `tests/cstub/c674x.c:833-847` (INTSP/INTSPU) loops `cross_path` 0/1 with
  `c.r[side ^ cross_path][src] = tc.source` and asserts
  `c.r[side][dst] == tc.expected` where `tc.expected` is an IEEE-754 bit
  pattern written out in the table (e.g. `{0x7fffffff, 0x4f000000, 0, false,
  true}` — 2^31-1 rounded to nearest is 0x4F000000, inexact). Those expected
  values are hand-computed from the format, independent of the emulator.
* Same pattern for MPYSP (`885-905`) and SPINT/SPTRUNC (`940-963`).

The `x = 1` form of the immediate-long `.L` ADD/SUB opfields is deliberately
rejected at `cdj_c674x.c:1755` ("cross-path long operand not supported") with a
correct justification: SPRUFE8B 2.3 gives each cross path a single 32-bit
operand, so a 40-bit src2 cannot arrive over one.

## 7. Numerical-semantics assessment per group, with the assertions read

For every "reference-backed-tests" claim below I opened the test, read the
assertion, and judged the provenance of its expected value.

**ISA-FP-SP / ISA-FP-SP-CONV — reference-backed, expected values independent of
the emulator.** The tables in `tests/cstub/c674x.c` are literal IEEE-754 bit
patterns with the intended value in a comment. Examples actually read:

* `tests/cstub/c674x.c:868` — `{0x7f800000, 0x00000000, 0x7fffffff, 0x010, 0}`
  with the comment `/* infinity * zero */`, asserted by lines 900-902:
  `assert(c.r[side][dst] == tc.expected && !c.load_count);` and
  `assert(c.control[20] == ((tc.rmode << (shift + 9)) | (tc.status << shift)));`.
  `0x010` is FMCR INVAL (bit 4) and nothing else, which is what Table 2-27
  requires for "infinity is a source to a multiply by zero"; the result
  `0x7fffffff` is the documented C674x NaN-substitute. Independent of the
  emulator.
* `tests/cstub/c674x.c:866` — `{0x7f800001, 0x40000000, 0x7fffffff, 0x011, 0}`,
  `/* SNaN */`: status `0x011` = NAN1 | INVAL, again straight from Table 2-27.
* `tests/cstub/c674x.c:929` — `{0x7f800000, 0x7fffffff, 0x0c0, 0}`,
  `/* infinity */` for SPINT: status INEX|OVER, result saturated to INT32_MAX.
* `tests/cstub/c674x.c:820` — `{0x7fffffff, 0x4f000000, 0, false, true}` for
  INTSP, and the three directed-rounding siblings at 821-823 giving
  `0x4effffff` for truncate and round-down. These are exactly the two
  representable neighbours of 2^31-1 in binary32; hand-derivable.
* `tests/cstub/c674x.c:865` `{0x3fc00000, 0x40000000, 0x40400000, 0x000, 0}`
  (`1.5 * 2 = 3`) and `:866` `{0xc0200000, 0x4109999a, 0xc1ac0000, 0x080, 0}`
  annotated `/* TI -2.5 * 8.6 */`, i.e. taken from the manual's own worked
  example rather than from our output.

One weakness: the hand tables are all *special* values plus a handful of normals.
They do not establish correct rounding across the normal range. That is what the
new test in section 8 addresses.

**ISA-INT-SAT — reference-backed, expected values from an in-test reference
model.** `tests/cstub/c674x-saturation.c` computes every expectation from
`clamp()` (line 23) and `shift_expected()` (line 79), written from SPRUFE8B
pp422-424/493-494/499-501, then asserts both the result and the *delayed*
CSR.SAT/SSR effect two packets later (`delayed_flags`, line 29:
`assert((c->control[1] & 0x200) == (saturated ? 0x200u : 0));` and
`assert(c->control[21] == (saturated ? unit_bit : 0));`). It sweeps sides,
cross paths, immediate/register forms, predicate true/false, 9 operand values ×
9, the 40-bit boundary values `±(2^39)` and shift counts
`{0,1,15,30,31,32,33,63,64,95,96,UINT_MAX}`. Expected values are independent of
the emulator. Caveat: `shift_expected` masks the count with 63 exactly as the
implementation does, so the *masking rule itself* is a shared assumption rather
than an independently derived one — though SPRUFE8B printed page 493 does state
the six-LSB rule.

**ISA-MPY16 / ISA-MPY32** — implemented; I read the 16 permutations at
`cdj_c674x.c:1872-1960` against the mnemonic naming (H = upper halfword, L =
lower, U = unsigned, S = signed) and they are consistent, with the delayed
result at E2 for 16x16 and E4 for 32x32 and MPYHI/MPYIL, matching the
"Delay Slots" lines in section 3.12. I did **not** find a dedicated
reference-backed table test for the 16 permutations or for MPY32's four
signedness variants; `tests/cstub/c674x.c:1094-1101` exercises a couple of
`.M` forms inside broader packet tests. Recorded as `untested` for semantics at
the permutation level — a clear next acceptance test.

**ISA-PACK** — PACK2/PACKH2/PACKHL2/PACKLH2/PACKL4/PACKH4 implemented at
`cdj_c674x.c:2178-2203`; SWAP2 is the PACKLH2 pseudo-op. SPACK2/SPACKU4/RPACK2
(saturating packs), SWAP4, UNPKHU4/UNPKLU4, SHLMB/SHRMB unimplemented. I found
no reference-backed semantic test for the implemented pack family.

**ISA-PACK16 / ISA-PACK8 / ISA-MPY-PACKED / ISA-MPY16-SAT / ISA-DUALRESULT /
ISA-BITMANIP / ISA-GALOIS / ISA-FP-DP / ISA-FP-DP-CONV** — no implementation, so
no semantics to validate. 88 rows. For audio work the consequential members are
the packed 2×16 arithmetic and the `DOTP*` family (the natural C674x idiom for
16-bit stereo DSP), the SMPY family (Q15 fractional multiply), and the
double-precision family.

**ISA-CTRL-MISC** — MVC is gated by `control_read_supported` /
`control_write_supported` (`cdj_c674x.c:213-224`): readable are AMR, CSR, IFR,
IER, ISTP, IRP, NRP, ILC, RILC, FADCR, FAUCR, FMCR, SSR, TSR, ITSR; writable
adds ISR/ICR/ISTP. `control_read` applies architectural masks (e.g. SSR masked
to six bits per 2.9.13; CSR read mask `0xffff03ff`). DINT/RINT implemented with
the TSR.GIE/SGIE sharing rule. NOP implemented including the 9-cycle maximum.
IDLE, SWE, SWENR unimplemented. MVD implemented at `2000`.

## 8. New characterization test

`tests/cstub/c674x-audit-sp-rounding.c` (new; nothing else added or changed).

Uncertainty it resolves: the existing SP tests are 18 hand-picked MPYSP cases
and 21 SPINT cases, almost all special values. They do not establish that the
hand-rolled integer MPYSP/ADDSP/SUBSP paths are *correctly rounded* over the
normal range in all four FADCR/FMCR modes.

Oracle: the host FPU under `fesetround`. For SP normal operands the exact
product fits in a double (24 + 24 = 48 ≤ 53 significand bits), and the exact sum
does too when the operand exponents are within 25 of each other (24 + 25 = 49),
so the single `double → float` conversion is one correctly-rounded SP rounding.
The expected value therefore does not come from the emulator. Samples whose SP
result is not normal are skipped — C674x flushes those and the existing
hand-written cases cover that path. The test also asserts the expected FADCR /
FMCR status is exactly INEX (bit 7) when and only when the rounding was inexact.

Compile and run (exact commands and results):

```
$ DEVELOPER_DIR=/Library/Developer/CommandLineTools cc -std=c11 -Wall -Wextra -Werror \
    -I emulator/qemu tests/cstub/c674x-audit-sp-rounding.c \
    emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c -o <scratch>/sp-rounding
(no diagnostics; exit 0)

$ <scratch>/sp-rounding
sp-rounding: checked MPYSP=80000 ADDSP=80000 SUBSP=80000, failures=0
(exit 0)
```

240,000 random in-range cases across four rounding modes, zero mismatches in
both value and INEX. This raises ISA-FP-SP's arithmetic core from
"reference-backed on 18 special cases" to "reference-backed against an
independent correctly-rounding oracle over the normal range". It does **not**
cover: denormal and overflow/underflow boundaries (covered only by the existing
hand cases), ABSSP, the SP compares, `.S`-unit ADDSP/SUBSP opfields
(0xe18/0xe38/0xeb8 — only the `.L` 0x218/0x238 were driven), the reverse
subtract layouts, cross paths, or predication.

No test file was added to `tests/` as a pytest wrapper, so this test is not yet
part of the regression suite; it is an audit artefact. Adding
`tests/test_c674x_audit_sp_rounding.py` mirroring
`tests/test_c674x_saturation.py` would be a one-file follow-up for the
coordinator to decide on.

## 9. Regression baseline recorded

```
$ DEVELOPER_DIR=/Library/Developer/CommandLineTools .venv/bin/python -m pytest \
    tests/test_c674x.py tests/test_c674x_saturation.py tests/test_c674x_circular.py \
    tests/test_c674x_spkernel_fields.py tests/test_dsp_isa_audit.py -q
24 passed, 1 skipped in 6.64s
```

This is regression coverage of the ISA tests that exist. It is not
architectural completeness, and the skip is `test_probe_driver_reads_the_assembler_listing`
or a compiler-dependent case, not a failure.

## 10. Findings

1. **Inventory defect (denominator).** `analysis/dsp/isa_manual_inventory.json`
   omits `MPY32 (32-bit result)` and `MPY32 (64-bit result)`, both
   C674x-checked in Table A-1 (printed page 712) and Table B-1 (printed page
   718). True row count 240, not 238. Cause:
   `tools/cdj_dsp/isa_inventory.py:46` name regex; the `in_table_b1_only`
   cross-check cannot see it because both tables use the same rejected name.
   Both instructions are implemented (`cdj_c674x.c:1820-1860`).
2. **Decoder bug: ADDAB/ADDAH/ADDAW long-immediate form rejected as a reserved
   predicate.** SPRUFE8B printed pages 115, 120, 123 define a second opcode with
   bits 31-28 fixed at `0001` and state the instruction "cannot be predicated".
   `cdj_c674x.c:1529-1531` treats those bits as `creg`/`z`. Fail-closed, but
   unimplemented and misdiagnosed; had the check not fired the words would alias
   onto STB with 15-bit offset.
3. **IDLE misclassified.** `0x0001E000` is IDLE (SPRUFE8B printed page 274),
   not a reserved NOP count. `nop_cycles` (`cdj_c674x.c:589`) returns 16 and
   `cdj_c674x.c:1068` reports "reserved NOP count". Unimplemented instruction
   with a wrong diagnostic; the code already knows it is IDLE
   (`cdj_c674x.c:631`).
4. **Compact `.M` family (Figure E-5 M3) entirely unimplemented** — 4096 words,
   8 instruction rows (MPY/MPYH/MPYLH/MPYHL compact plus SMPY/SMPYH/SMPYLH/
   SMPYHL at either width). `instruction_unit` (`cdj_c674x.c:686`) classifies
   the format as `.M` while execute rejects it: a concrete case where unit
   classification must not be read as implementation.
5. **Compact Sx2op (Figure F-29) unimplemented** — 512 words, compact in-place
   `.S` ADD/SUB.
6. **Compact Sx1b `s = 0` rejected** — `cdj_c674x.c:1355` requires bit 0 = 1,
   but SPRUFE8B printed page 756 makes `s` the unit selector while `src2` is
   always a B register, so `BNOP .S1 B<n>, N3` is legal. 128 of 256 words.
7. **40-bit long-operand forms missing for eight instructions** —
   CMPEQ/CMPGT/CMPGTU/CMPLT/CMPLTU and SHL/SHR/SHRU, confirmed opfield by
   opfield against printed pages 177, 188, 197, 202, 211, 447, 451, 457. The
   40-bit forms of SADD/SSUB and ADD/ADDU/SUB/SUBU *are* implemented, so this is
   an inconsistency in long-operand coverage rather than a blanket absence.
8. **88 of 240 rows have no implementation at all**, concentrated in nine
   groups: all packed 2×16 and 4×8 arithmetic, all dot-product and complex
   multiply, all saturating 16×16 multiply, all double-precision floating point,
   all bit-manipulation (LMBD/NORM/BITR/BITC4/DEAL/SHFL/SHFL3/XPND2/XPND4/SUBC),
   all Galois multiply, and the dual-result `.L` compounds.
9. **SP floating point is the strongest-validated area.** FADCR/FAUCR/FMCR field
   placement matches Tables 2-25/2-26/2-27 exactly; existing tests are
   reference-backed with emulator-independent expected values; the new test adds
   240,000 correctly-rounded cases across all four modes with zero mismatches.
   RCPSP, RSQRSP, SPDP and DPSP remain unimplemented.
10. **CMPGTU/CMPLTU immediate decode is permissive** — SPRUFE8B gives the
    operand type as `ucst4` (printed pages 197, 211) while
    `cdj_c674x.c:2229` zero-extends the whole 5-bit `src1` field. No wrong
    result for legal encodings; open question for bit 17 set.
11. **Evidence weakness: 16×16 and 32×32 multiply permutations are untested.**
    32 implemented opfields (16 MPY permutations, 4 MPY32 variants, 8
    MPYHI/MPYIL variants, MVD, 2 signed-constant MPY forms) have no dedicated
    reference-backed semantic table. High-value next acceptance test.
12. **Evidence weakness: the implemented pack family has no semantic test.**
    PACK2/PACKH2/PACKHL2/PACKLH2/PACKL4/PACKH4 byte/halfword lane selection is
    read-correct against the manual but unvalidated.

## 11. Stale documentation

1. `emulator/qemu/cdj_c674x.h:7-8`:
   `/* Partial interpreter. Encodings/semantics: TI SPRUFE8B, instruction entries
   MVK, MVKH, MVC, AND, B, ADDKPC and NOP; no third-party decoder code. */`
   The interpreter now dispatches roughly 120 of the 240 Table A-1 rows,
   including the entire single-precision floating-point family with FADCR/FAUCR/
   FMCR status, saturating arithmetic with delayed CSR.SAT, 32×32 multiplies,
   the full scalar memory family, and 34 of the 48 compact formats. The
   seven-instruction list is badly stale and is the first thing a reader of the
   header believes.
2. `tests/test_dsp_isa_audit.py:77`: "creg=11111 is a reserved predicate in
   SPRUFE8B Table 3-1". The reserved-predicate table is **Table 3-9**,
   "Registers That Can Be Tested by Conditional Operations", printed page 77.
   Table 3-1 is something else. The assertion is correct; the citation is not.
3. `HANDOFF.md:1146`: "saturating compact operations with delayed CSR.SAT remain
   fail-closed." Superseded by the saturation batch described at
   `HANDOFF.md:604-617` and confirmed by measurement — compact SAT-header
   SADD/SSUB/SSHL words are accepted and produce delayed CSR.SAT/SSR. Mitigating
   factor: the line sits under the heading "### Previous stable-wait checkpoint"
   (`HANDOFF.md:1124`), so the document's own structure labels it history; it is
   nonetheless written in the present tense.
4. `analysis/dsp/isa_manual_inventory.json` `scope` / `counts` assert 238 rows as
   the manual-derived denominator. See finding 1: the correct figure is 240. Any
   downstream statement of the form "N of 238" is off.

## 12. Open questions I could not resolve

1. **Figure D-6 "Ltbd"** (SPRUFE8B printed page 737) defines a 16-bit `.L`
   format with no mnemonic table, and a full-text search finds the name only in
   the list of figures and the caption — no instruction's "Compact Instruction
   Format" table references it. Our decoder rejects all 4096 matching words.
   I cannot tell from SPRUFE8B alone whether this is a documentation artefact, a
   reserved format, or a format whose mnemonic table was dropped in this
   revision. Resolving it needs a second primary source (a later C674x/C66x
   revision, or TI's `dis6x`).
2. **CMPGTU/CMPLTU `ucst4` vs the 5-bit field.** SPRUFE8B does not say what the
   hardware does when `src1[4]` is set in the immediate forms. Our decoder
   zero-extends all five bits. Needs silicon or a TI statement.
3. **SHL/SHR/SHRU clamp at 40 vs 32.** Indistinguishable for the implemented
   32-bit destinations; becomes load-bearing the moment the `slong`/`ulong`
   forms are added. Recorded so the distinction is not lost.
4. **`.S`-unit ADDSP/SUBSP opfields** (0xe18 / 0xe38 / 0xeb8) are implemented but
   my new test drove only the `.L` opfields; the existing tables do cover the
   `.S` encodings (`tests/cstub/c674x.c:1317` carries the opfield in the case
   table), but not with a correctly-rounding oracle.
5. **Delay-slot counts were read, not measured.** Each delayed-result arm queues
   at `cpu->cycles + N` and those N values match the manual's "Delay Slots"
   lines, but no independent cycle oracle was applied. Strict timing validation
   is another track's and another tool's job.

## 13. Commands run

| Command | Result |
|---|---|
| `shasum -a 256` on `cdj_c674x.c`, `cdj_c674x_loop.c`, `c674x-isa-probe.c`, `isa_manual_inventory.json`, `isa_probe.py`, both PDFs | all match `isa_probe.json` `provenance` and `build/references/provenance.json`; probe artefact is current |
| Python scans of `build/references/sprufe8b.txt` for Table A-1/B-1 rows, opfield tables, Appendix C-H figures, Table 3-9, Tables 2-25/26/27 | citations in sections 4-6 |
| `grep` of Table A-1 pages 709-714 for check-marked rows not in the inventory | exactly 2: both MPY32 rows |
| Python extraction of every instruction's "Compact Instruction Format" table | 58 of 240 rows have compact forms; 47 of 48 formats referenced; D-6 referenced by none |
| `cc -std=c11 -O1` build + run of scratch `compact_probe.c` over all 65,536 compact words × 12 headers × 2 register profiles | 53,856 accept / 11,440 not implemented / 176 reserved LSDx1 / 64 invalid pair; per-format table in section 5.2 |
| `grep -c '\b<mnemonic>\b' emulator/qemu/cdj_c674x.c` for 48 not-implemented mnemonics | 45 zero hits; SAT/SWE/SWENR hits are all false positives (CSR.SAT, packet-conflict detection) |
| `cc -std=c11 -Wall -Wextra -Werror` build of `tests/cstub/c674x-audit-sp-rounding.c` | clean, exit 0 |
| run `sp-rounding` | `checked MPYSP=80000 ADDSP=80000 SUBSP=80000, failures=0`, exit 0 |
| `pytest tests/test_c674x.py tests/test_c674x_saturation.py tests/test_c674x_circular.py tests/test_c674x_spkernel_fields.py tests/test_dsp_isa_audit.py -q` | 24 passed, 1 skipped |
