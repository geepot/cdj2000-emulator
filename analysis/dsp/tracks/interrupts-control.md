# Track 3 — Interrupts, control registers, privilege, exceptions

Audit date: 2026-09-10. Repository `/Users/gpotvin/Git/cdj2000-emulator`,
branch `codex/macos-nxs`.

**HEAD discrepancy:** the task statement named HEAD `3da5ff2`. The actual HEAD
is `2eb2476` ("Document SH4 and Blackfin coverage boundaries"), one commit
past `3da5ff2`. That commit touches only `SH4_BLACKFIN_COVERAGE.md`, so nothing
in this track's source changed. Worktree also carries unrelated uncommitted
work (`emulator/qemu/cdj2000_usbh.c` modified, untracked PCM-bank files); none
of it is in this track's source set and none of it was touched.

This audit is **assessment only**. No emulator source, test or tool was
modified. The only files created are this report and scratch artefacts under
the session scratchpad, plus two git-ignored reference downloads described
below.

---

## 1. Scope

### Covered

* The full C674x control register file as enumerated by **SPRUFE8B Table 2-6**
  (printed page 34 (PDF page 34)) and **Table 3-27** (printed page 378
  (PDF page 378)), register by register, read and write separately, with
  field-level fidelity judged against each per-register figure/field table.
* Interrupt entry and return: IFR/ISR/ICR/IER/ISTP/IRP/NRP/SSR/ITSR/NTSR/TSR,
  priority and selection, GIE/PGIE/SGIE, DINT/RINT, `B IRP`, `B NRP`, reset,
  NMI, and the nine-cycle strict entry interval claimed by
  `DSP_INTERRUPT_ENTRY.md`.
* Privilege (supervisor vs user mode, CXM, restricted registers and
  instructions) and the exception model (EFR/ECR/IERR/NTSR/XEN/GEE,
  internal/external/software exceptions, SWE/SWENR).
* The device-level path where it meets the CPU: `cdj_c6747_intc.c` against the
  C674x megamodule INTC register set, and SPRUH91D Table 2-1 DSP Interrupt Map.

### Not covered (deferred or other tracks)

* Field semantics of AMR (circular addressing), ILC/RILC (SPLOOP), and
  FADCR/FAUCR/FMCR (floating point). This track records only whether MVC can
  reach them and with which mask; their *behavioural* fidelity belongs to the
  addressing, software-loop and floating-point tracks. Rows are marked
  `not-assessed` for validation where that is the case.
* SPLOOP interrupt windows beyond the interrupt-recognition gate itself
  (section 7.13 boundary selection, epilog drain, SPMASK pipe-up). The
  recognition gate in `cdj_c674x_interrupt` is assessed; the loop buffer is
  the software-loop track's.
* EDMA, HPI, timer, McASP, SPI, GPIO, PSC internals. Only their *event
  generation into INTC* is counted here.
* Memory protection (MPU/MPPA), emulation/debug (DIER, EMU pins), power-down
  (PDC, CSR.PWRD) beyond recording them absent.
* Checkpoint/replay schema and determinism machinery as such; replay artefacts
  were consulted only as evidence about interrupt behaviour.

---

## 2. References used

| Doc | Revision | sha256 | Provenance |
| --- | --- | --- | --- |
| SPRUFE8B | TMS320C674x DSP CPU and Instruction Set Reference Guide, July 2010 | `34bc3631…bb7b1d37` | `build/references/provenance.json`, verified with `refdocs --check` |
| SPRUH91D | TMS320C6745/C6747 DSP TRM, March 2013 rev Sept 2016 | `8f00cb85…9fd76e6c` | same |
| **SPRUFK5A** | **TMS320C674x DSP Megamodule Reference Guide, August 2010** | **`a0008b6f5614b760d130297c05f7105db7872622687a67af6c39566b12291832`** | **downloaded by this audit** from `https://www.ti.com/lit/ug/sprufk5a/sprufk5a.pdf`, kept in git-ignored `build/references/sprufk5a.pdf`, indexed to `build/references/sprufk5a.txt` |

SPRUFK5A had to be added because **SPRUH91D does not define the DSP interrupt
controller registers at all** — section 2.2.2.1 (printed page 70) says the
INTC "is fully described in the TMS320C674x DSP Megamodule Reference Guide
(SPRUFK5)" and gives only the event map (Table 2-1, printed pages 70–74).
`cdj_c6747_intc.c` already cites "SPRUFK5A 7.2.2 and 7.4.2", so before this
audit the INTC model's only manual citation pointed at a document that was not
in the provenance set.

SPRUFK5A page offset: 219 PDF pages, 210 footers detected, 209 of which have
printed page == PDF page index. The single mismatch is a footer-regex false
positive (a register bit-pattern `11000` matched on PDF page 67). **Printed
page equals PDF page index for SPRUFK5A**, as for the other two documents.
`build/references/sprufk5a.txt` is not added to `provenance.json` — the
coordinator should decide whether to register it in `tools/cdj_dsp/refdocs.py`.

---

## 3. Control register inventory — explicit counts

**Denominator.** SPRUFE8B Table 2-6 (printed page 34) and Table 3-27 (printed
page 378) both enumerate **30 register acronyms**. Table 3-27 maps them onto
**28 distinct `crlo` encodings**, because two encodings carry two acronyms
each: `crlo=00010` is IFR on read / ISR on write, and `crlo=11101` is EFR on
read / ECR on write. Grouping is therefore by *acronym*, with the read and
write directions of each acronym assessed separately. `crhi` aliases (CSR also
at `crhi=00001`, IFR at `00010`, PCE1 at `10000`, and the `0xxxx` wildcards)
are **not** counted as separate registers.

**Method.** Source reading of `control_read()`, `control_read_supported()`,
`control_write_supported()` and the control-write switch in
`emulator/qemu/cdj_c674x.c`, plus a behavioural probe compiled against the real
core (`scratchpad/crprobe.c`) that issues `MVC crlo,B5` and `MVC B4,crlo` for
all 32 `crlo` values through `cdj_c674x_step` and records accept/reject and the
value actually stored. The probe is the authority for the counts below; the
source reading explains them. Unsupported IDs **fail closed** with
`control register read not implemented` / `control register write not
implemented` and do not advance architectural time.

| Outcome | Count | Acronyms |
| --- | --- | --- |
| Reachable by MVC (read, write, or both) | **17 / 30** | AMR, CSR, IFR, ISR, ICR, IER, ISTP, IRP, NRP, ILC, RILC, FADCR, FAUCR, FMCR, SSR, TSR, ITSR |
| — of those, readable | 15 | the 17 minus ISR and ICR (both write-only, correctly) |
| — of those, writable | 16 | the 17 minus IFR (read-only, correctly — writes go via ISR/ICR) |
| Absent entirely (both directions rejected) | **13 / 30** | DIER, DNUM, ECR, EFR, GFPGFR, GPLYA, GPLYB, IERR, NTSR, PCE1, REP, TSCH, TSCL |

Of the 17 reachable registers, classified by field-level fidelity against the
per-register figures:

| Fidelity class | Count | Registers |
| --- | --- | --- |
| **Modelled** — every defined field present with manual read/write rules | 7 | IFR, ISR, ICR, IER, ISTP, SSR, ITSR |
| **Partially modelled** — reachable, some defined fields missing or frozen | 4 | CSR (PWRD/PCC/DCC not stored), TSR (only SGIE+GIE writable), IRP (storage only, correct), NRP (storage only, no consumer) |
| **Reachable, field fidelity owned by another track** | 6 | AMR, ILC, RILC, FADCR, FAUCR, FMCR |

`SSR` and `CSR.SAT` have their own reference-backed evidence in
`tests/cstub/c674x-saturation.c` (compiled by `tests/test_c674x_saturation.py`,
1 passed): `MVC reg,SSR` with 0 clears and with `UINT_MAX` sets `0x3f`
(Figure 2-25, bits 5-0, printed page 54); `MVC reg,CSR` with `0x200` cannot set
SAT (Table 2-9, "can be cleared only by the MVC instruction", printed page 39);
and a same-cycle `MVC` to CSR or SSR alongside a pending functional-unit
saturation leaves `SSR == 1` and `CSR.SAT` set, which is Table 2-22's
"setting of the flag from a functional unit takes precedence over a write to
the bit from an MVC instruction" (printed page 54). Those expected values are
derived from the manual, not recorded output.

**No reachable register reads as a hard-wired constant.** The closest case is
`IER` bit 0, which is forced to 1 — and Figure 2-8 (printed page 42) specifies
that bit as `R-1`, so the constant is correct, not a stub. Registers that
would read as a constant are instead rejected outright, which is the right
choice: `control_read()`'s `default: return 0` is unreachable because
`control_read_supported()` gates every call.

---

## 4. Interrupt entry — verification of `DSP_INTERRUPT_ENTRY.md`

### 4.1 The figure and the page — verified

The doc claims "TI SPRUFE8B Figure 5-4 (page 641) places the first annulled E1
at cycle 6 and the interrupt handler's E1 at cycle 15."

* Figure 5-4, "Nonreset Interrupt Detection and Processing: Pipeline
  Operation", is on **printed page 641 (PDF page 641)**. Correct.
* In the figure, execute packet `n+5` reaches `E1` at cycle 6, and the `ISFP`
  row reaches `E1` at cycle 15 (`PG`=9 … `DP`=13, `DC`=14, `E1`=15).
* Section 5.4.4, printed page 643 (PDF page 643), states: "During CPU cycles
  6-14 … The next execute packets (from n + 5 on) are annulled. … **The address
  of the first annulled execute packet (n + 5) is loaded in IRP.** A branch to
  the address formed from ISTP … is forced into the E1 phase of the pipeline
  during cycle 9. IFm is cleared during cycle 8."
* 15 − 6 = 9. The doc's arithmetic and its identification of the first annulled
  packet with the interpreter's current PC are both correct.

### 4.2 The nine-cycle implementation — verified

`interrupt_pipe_down()` (`emulator/qemu/cdj_c674x.c:134-152`) sets
`cpu->idle_cycles = 9` in strict mode. `cdj_c674x_step_capture_direct`
(`:2801-2808`) consumes one `idle_cycles` per step by executing an empty packet
— which still retires queued loads/stores and calls `cycle_tick` — and only
fetches once the counter reaches zero. So exactly nine empty issue cycles
separate the redirect from the first ISR packet. Breadth/exploratory mode
(`cdj_c674x_loop_functional_timing()`) instead drains to the latest queued
result, which the doc correctly labels its "prior minimum-drain approximation".

### 4.3 The claimed tests — all present, assertions read

`tests/cstub/c674x.c`, helper `finish_interrupt_entry` at line 37 and the
interrupt block at lines 2445–2760. Every item in the doc's verification list
exists with real assertions:

| Doc claim | Where | What the assertions actually check |
| --- | --- | --- |
| empty entry | `finish_interrupt_entry`, line 37 | `idle_cycles==9`, then nine steps each asserting `pc` unchanged, `cycles==cycles+i+1`, `idle_cycles==8-i` |
| writebacks due in each of the nine slots | line 2636, `for (due = 0; due <= 9; ++due)` | per slot: `load_count == (due > i)` and `r[1][0] == (due && due<=i ? 0xfeedface : 0)`; tenth step then yields `0x1180face` vs `0x11800000` |
| store retirement | line 2664 | store `due=3` retires during entry, `memory[48]==0x12345678`, `store_count==0`, no handler packet issued |
| board clock ticks | same loop, line 2648 | `ticks == i` for every one of the nine slots, `ticks == 10` after the ISR packet |
| pending IRQ latching without restarting entry | same loop, line 2647-2657 | `cdj_c674x_interrupt(&c, 1u<<7)` called inside every slot; `control[6]` (IRP) stays `0x1000`, `idle_cycles` keeps counting down, and afterwards `control[2] == (1u<<7)` |
| `B IRP` | line 2486 | after entry, `cycles==10`, `pc==0x1084`, `branch_due==15`, `control[26]==0x45f` restored from ITSR, `CSR & 3 == 3`; then five steps to `pc==0x1040` |
| SPLOOP return behaviour | lines 2497, 2759, 2807, 2846 | SPLX retention through `B IRP`, boundary vectoring with `IRP` = setup packet, predicate-loop and SPLOOPW epilog cases |
| fail-closed collision | line 2674 | a load `due=10` colliding with ISR E1 still faults `delayed-result write conflict` at `cycles==9`, `load_count==1`, `r[1][0]==0` |

**Are the expected values independent of the emulator?** Yes, for this block.
They are hand-computed from the manual and from fixed encodings: vector
`0x1000 + 4*32 = 0x1080` from ISTP semantics (Table 2-15, printed page 45);
`ITSR == 0x45f` is exactly `(TSR & 0xc6de) | CSR.GIE` for the seeded TSR, the
mask being the set of bits TSR Figure 2-28 defines (printed page 57);
`TSR == (1<<15)|(1<<9)|(1<<4)|(1<<2)` is exactly Table 5-3's "set IB, set INT,
GEE unchanged, DBGM unlisted/unchanged, clear GIE/SGIE/XEN/CXM/EXC"
(printed page 643); `0x0008c06a` is a hand-assembled `MVKH .S2 0x1180,B0`.
I verified each of those arithmetically against the manual tables. They are
not recorded emulator output.

### 4.4 Strict model or abstraction?

**An abstraction with a manual-derived interval, not a strict pipeline model.**
The doc's own admission ("does not model all pin synchronization, fetch stages
or per-cycle TSR field transitions") is confirmed, and is if anything
understated. Specifically, at the interpreter's one-packet-per-step
abstraction the following parts of Figure 5-4 are collapsed into one atomic
transition at entry rather than distributed over cycles 6–14:

* `IFm` is cleared at entry; the manual clears it **during cycle 8**
  (5.4.4). An ISR that reads IFR cannot observe the two-cycle window.
* `PGIE ← GIE`, `GIE ← 0`, `TSR → ITSR` and `TSR ←` defaults all happen at
  entry; Figure 5-4 spreads the TSR field transitions across cycles 6–9.
* The forced ISTP branch entering E1 at cycle 9 has no representation; only its
  net effect (PC redirect + nine empty cycles) exists.
* `PCXM`/`DCXM`, the pipelined execution-mode registers, do not exist.
* There is no fetch pipeline, so `PG`/`PS`/`PW`/`PR`/`DP`/`DC` stages, the
  annulment of `n+6…n+11`, and speculative fetch of the wrong path are all
  absent. The identity "current PC == first annulled packet" is what makes the
  abstraction sound.

### 4.5 Additional fidelity gaps found in entry, beyond what the doc admits

1. **Recognition conditions 5.4.2 are only partly enforced.** The manual
   requires that "the five previous execute packets (n through n+4) do not
   contain a branch (**even if the branch is not taken**) and are not in the
   delay slots of a branch." `cdj_c674x_interrupt` defers only while
   `cpu->branch_due || cpu->branch_count` — i.e. only for *taken* branches. The
   source comment admits it ("False conditional branches do not yet have
   pipeline state"). Effect: an interrupt can be recognised in a window the
   hardware would have blocked, with a correspondingly earlier IRP.
2. **`MVC`-clear-GIE is given DINT's semantics.** Section 5.2 (printed page
   634) and the DINT description (printed page 233) explicitly distinguish the
   two: the CPU *may* take an interrupt in the cycle immediately following an
   `MVC` that clears GIE (Example 5-2, interrupt between instructions 3 and 4,
   with PGIE=0), but *may not* after `DINT`. The run loop calls
   `cdj_c674x_interrupt` **before** each step, so by the time the check runs,
   the `MVC`'s E1 has already cleared GIE and the interrupt is not taken. This
   is conservative (one cycle late, never early) but it is not strict.
3. **Reset is not an interrupt.** `cdj_c674x_reset()` zeroes the CPU and sets
   `pc = entry` from its caller. SPRUFE8B 5.3.4.1 (printed page 638) requires
   "program execution begins at the address specified by the ISTB field in
   ISTP". The only caller that matters, `cdj2000_nxs_hpi.c:814`, passes
   `ldl_le_p(s->l2)` — the first word of loaded L2 — not ISTB. There is no
   10-cycle RESET hold, no reset ISFP fetch, and no RESET path through
   `cdj_c674x_interrupt` (which rejects any bit outside `0x0000fff0`).

Everything else in 5.3.4.1 is right: AMR/ISR/ICR/IFR = 0, IER = 1,
CSR bits 15-0 = `0x100` (little endian), TSR = ITSR = 0, and
ISTP = `0x00700000`, which matches SPRUH91D HOST1CFG `DSP_ISTP_RST_VAL`
reset value `0x1C00 << 10` (printed page 180 (PDF page 180)). Note the ISTB
reset value is **hard-coded** in `cdj_c674x_reset`; there is no SYSCFG
HOST1CFG register model feeding it, so firmware cannot relocate the reset
vector through the device path.

---

## 5. What is NOT there

| Feature | Classification | Manual reference | Firmware behaviour that would need it |
| --- | --- | --- | --- |
| **Privilege / supervisor vs user mode** | unsupported | SPRUFE8B ch. 8, printed pages 703–706; TSR.CXM Figure 2-28/Table 2-23, printed page 57 | Any OS-style split (CXM transitions on interrupt/exception, `B NRP` restoring mode, restricted-register exception with IERR.RAX/PRX, restricted `B IRP`/`B NRP`/`IDLE`). The CXM field exists as storage in `control[26]` bits 7-6 but is never set non-zero, is never read by any access check, and `MVC` cannot write it (correct per the manual, but also means nothing can). There is no `cdj_c674x` API to enter user mode. A firmware that executed entirely in supervisor mode — which the C6747 does after reset (8.2.1) — is unaffected; anything that switched to user mode would silently keep full privilege. |
| **Exception model (EFR/ECR/IERR)** | unsupported | SPRUFE8B 2.9.3/2.9.4/2.9.7, printed pages 48, 49, 51; ch. 6, printed pages 653–668 | EFR (`crlo=11101` read) and ECR (same write) and IERR (`crlo=11111`) all reject. No EXCEP input, no internal-exception detection, no NXF/EXF/IXF/SXF flags, no exception vector at ISTP+0x20, no IERR cause reporting. Firmware with an exception service routine, or that reads EFR to classify a fault, would fault at the first `MVC`. TSR.XEN and TSR.GEE are read-only storage: `MVC reg,TSR` drops them (see below), so exceptions cannot even be *enabled*. |
| **NMI** | unsupported | SPRUFE8B 5.1.1.2, printed page 629; 5.4.5 and Figure 5-6, printed pages 643-644; Table 5-4, printed page 646 | `cdj_c674x_interrupt` faults `invalid CPU interrupt request mask` for any bit outside `0x0000fff0`, so bit 1 (NMI) cannot even be requested. IFR's read mask `0xfff2` includes NMIF, but nothing can set it: `cdj_c674x_interrupt` ANDs with `0xfff0` and ISR writes mask `0xfff0` (the latter matches the manual — "You cannot set any bit in ISR to affect NMI or reset", printed page 44). IER.NMIE is modelled as set-only and correctly gates maskable interrupts, but is never cleared by an NMI and never set by `B NRP`. ISTP.HPEINT can therefore never report NMI, which the manual permits it to (printed page 45). |
| **`B NRP`** | unsupported | SPRUFE8B 5.3.4.2, printed page 639; MVC-adjacent opcode `0x001c00e2` | Recognised only in `interrupt_gate_parallel_conflict` (so `DINT \|\| B NRP` is rejected atomically); execution falls through to `instruction not implemented`. Confirmed independently by `analysis/dsp/isa_probe.json`: `B NRP` is `all-probed-forms-rejected`. |
| **NTSR** | unsupported | SPRUFE8B 2.9.10 Figure 2-23/Table 2-21, printed page 53 | `crlo=11100` rejects both directions. `control[28]` exists as a zeroed slot. Needed by any NMI or exception return, and by NTSR.HWE/NTSR.SPLX inspection after an NMI terminated a SPLOOP. |
| **TSR supervisor-writable fields** | partial | SPRUFE8B Table 2-23, printed pages 57–58; 8.2.4.2.2, printed page 705 | `MVC reg,TSR` keeps only bits 1-0 (SGIE, GIE); writes to DBGM (4), XEN (3), GEE (set-only, 2) and clears of EXC (10) are **silently dropped**, not rejected. The source comment says "Privilege and hardware-owned TSR fields need a later execution-mode model." Because GEE and XEN are the exception enables, this is the reason exceptions cannot be turned on at all. Silently dropping is the one place in this track where the core is not fail-closed. |
| **SWE / SWENR** | unsupported | SPRUFE8B 6.5.3.1/6.5.3.2, printed page 666; instruction entries printed pages 557–558 | Both appear in `interrupt_gate_parallel_conflict` (conservative: the manual's DINT/RINT restriction lists do not include them, so this only over-rejects) but neither executes. `analysis/dsp/isa_probe.json`: both `all-probed-forms-rejected`. A firmware OS-entry path (`SWENR` vectoring through REP, `SWE` through the NMI ISFP, EFR.SXF set) would fault. REP itself (`crlo=01111`) also rejects. |
| **TSCL / TSCH** | unsupported | SPRUFE8B 2.9.14, printed pages 55–56 | Both directions reject, including the write-to-TSCL that *enables* counting. The CPU already keeps a 64-bit `cpu->cycles`, so this is plumbing rather than semantics — but a firmware using TSC as a timebase faults today. |
| **PCE1, DNUM, GFPGFR, GPLYA/GPLYB, DIER** | unsupported | printed pages 46, 48, 40, 50, 47 | All reject. PCE1 matters for any fault/trace handler that reads the E1 program counter; GFPGFR/GPLYA/GPLYB matter only to Galois-field instructions (GMPY — ISA track); DIER/DNUM are debug/multicore and irrelevant to a single-core C6747. |
| **Nested maskable interrupt** | unsupported, fail-closed | SPRUFE8B 5.6.2 | `cdj_c674x_interrupt` faults `nested maskable interrupt not implemented` when `TSR.INT` (bit 9) is already set. This is correct fail-closed behaviour and is tested (line 2732). Any ISR that re-enables GIE before returning would stop the core rather than nest. |

---

## 6. Interrupt priority, IST and sticky IFR

* **ISTP vector computation.** `cpu->pc = (control[5] & 0xfffffc00) + interrupt*32`.
  Matches Table 2-15 (ISTB = bits 31-10, 1 KB alignment) and Figure 5-2's
  32-byte ISFP stride (printed pages 45 and 631). `MVC reg,ISTP` writes only
  `value & 0xfffffc00`, so HPEINT and the five zero LSBs are not storable —
  correct, both are read-only in Figure 2-12.
* **HPEINT synthesis.** `control_read(5)` recomputes HPEINT on every read as
  the lowest set bit of `IFR & IER & 0xfff2`, placed at bits 9-5, with 0 when
  nothing is pending. That matches Table 2-15 exactly, including the clause
  "The corresponding interrupt need not be enabled by NMIE (unless it is NMI)
  or by GIE" — the derivation deliberately does *not* consult GIE or NMIE.
  Tested at line 2346 with pending {4,7} enabled {4,7} → `0x00800080`
  (ISTB `0x00800000` + `4<<5`), which is Example 5-1's construction
  (printed page 633) applied to a different pair. **Gap:** NMI can never
  contribute, per §5.
* **Priority.** Table 5-1 (printed page 629) orders RESET > NMI > INT4 > … >
  INT15, i.e. ascending bit number among maskables. Both the acceptance
  selector and the HPEINT derivation scan from the lowest bit number. Correct.
  Tested at line 2464: pending {4,7}, IER enables only 7 → vectors to INT7
  (`0x10e0`) and leaves IF4 set.
* **INT4..INT15 restriction.** Enforced at the API boundary:
  `pending & ~0x0000fff0` is a hard fault. Tested at line 2729 with bit 3.
  The INTC side can never produce an out-of-range mask —
  `request_selected_event` only ever sets bits 4..15.
* **NMI and reset.** Neither is handled; see §5.
* **Sticky IFR under mask.** `control[2] = (control[2] | pending) & 0xfff0` runs
  *before* the three enable tests, so requests latch while masked and survive
  until acceptance or ICR clears them. Matches 5.4.1 ("IFm remains set until
  either you clear it by writing a 1 to bit m of ICR, or the processing of INTm
  occurs", printed page 639). Tested at line 2445 (latch with GIE=0 and
  NMIE=0, then recognise once all three enables are true) and inside the
  nine-slot loop (latching during entry without restarting it). Acceptance
  clears only the selected bit (`control[2] &= ~(1u << interrupt)`), verified at
  line 2481.
* **ISR/ICR one-delay-slot update of IFR.** Implemented by queueing a
  `CDJ_C674X_DELAYED_IFR_SET`/`_CLEAR` entry at `cycles + 2` in the existing
  delayed-result queue, with simultaneous set winning over clear. Matches the
  MVC note ("the results cannot be read … until two cycles after the write")
  and the ICR note ("Any write to ICR is ignored by a simultaneous write to the
  same bit in ISR"), printed pages 41, 44 and 377. Tested at lines 2358–2384
  with three successive `MVC IFR` reads observing old, new, new.

---

## 7. The C6747 device-level path (`cdj_c6747_intc.c`)

Register addresses and reset values check out against SPRUFK5A Table 7-3
(printed page 169) and the per-register figures:

| Group | Emulator | Manual | Verdict |
| --- | --- | --- | --- |
| EVTFLAG0-3 @ `0x01800000` | read-only, sticky | Table 7-3; "EVTFLAGx registers are read-only and must be cleared through … EVTCLR" | matches |
| EVTSET0-3 @ `+0x20` | write-only, bank 0 masks bits 3-0 | Figures 7-19..7-22 | matches |
| EVTCLR0-3 @ `+0x40` | write-only | Figures 7-23..7-26 | matches |
| EVTMASK0-3 @ `+0x80`, reset 0 (bank 0 forced `0xf`) | read/write | Figure 7-6 `R/W-0`; "event mask bits for events 0 through 3 are reserved, and are always masked" (printed page 160) | matches |
| MEVTFLAG0-3 @ `+0xa0` | derived `flag & ~mask` | Figure 7-7 `R-0`, "content … identical to … event flag registers for the events that are enabled" | matches; derived, not a stale latch |
| EXPMASK0-3 @ `+0xc0`, reset `0xffffffff` | read/write | Figures 7-41..7-44 `R/W-FFFFh` both halves (printed page 181) | matches |
| MEXPFLAG0-3 @ `+0xe0` | derived | Figures 7-45..7-48 | matches |
| INTMUX1-3 @ `+0x104`, reset `0x07060504`/`0x0b0a0908`/`0x0f0e0d0c`, write mask `0x7f7f7f7f` | read/write | Figures 7-35..7-37, `INTSEL4..15` reset `4h..Fh`, bits 31/23/15/7 reserved, INTSEL is 7 bits (printed page 178) | matches exactly |
| AEGMUX0/1 @ `0x01810140/4` | **absent** | Table 7-3 | unsupported |
| INTXSTAT @ `+0x180` | **absent** | Figure 7-38, printed page 179 | unsupported |
| INTXCLR @ `+0x184` | **absent** | Figure 7-39, printed page 180 | unsupported |
| INTDMASK @ `+0x188` | **absent** | Figure 7-40, printed page 180 | unsupported |

So **8 of 12 register groups (31 of 38 registers) are modelled**, and the 4
absent groups (5 registers) return `false` from `cdj_c6747_intc_read/write`,
which fails closed to the bus. Event numbering is validated: events 0-3 are
rejected as inputs (`event < 4` → `false`), consistent with Table 2-1 calling
them "C674x Interrupt Control 0..3" and SPRUFK5A calling them combiner
outputs; events 4..127 accepted, 128+ rejected.

### Two structural gaps

1. **The exception combiner is register storage with no output.** EXPMASK and
   MEXPFLAG behave correctly as registers, but the `EXCEP` signal they drive
   (SPRUFK5A Figure 7-14, printed page 167) has no destination — the CPU has no
   exception entry point at all. Combined with §5, this means exception
   plumbing exists only at the MMIO surface.
2. **Only 2 of the 124 system events are ever generated.** Grepping the whole
   tree, `cdj_c6747_intc_deliver_event` is called with exactly two literals:
   event **8** (`TPCC0_INT1`, EDMA3CC region-1 completion) in
   `cdj2000_nxs_hpi.c:423` and `tools/cdj_dsp/replay.c:563`, and event **34**
   (`UHPI_DSPINT`) in `cdj2000_nxs_hpi.c:862` and `replay.c:969,1210`. Timer64P0
   (`T64P0_TINT12`, event 4 — the reset-default source of INT4), McASP
   (event 61), I2C, SPI, UART, GPIO banks, PSC and EMIF error events are never
   raised: `cdj_c6747_timer.c`, `cdj_c6747_mcasp.c`, `cdj_c6747_gpio.c` and
   `cdj_c6747_i2c.c` contain no interrupt or IRQ code at all. The INTMUX
   selection logic is generic over all 128 events, so the *mapping* is
   implemented; the *sources* are not. Any firmware path that waits on a timer
   tick or a McASP interrupt cannot be driven.
3. **The C6747 NMI source is absent.** SPRUH91D 2.2.2.1.2 (printed page 73)
   says DSP NMI is asserted by writing 1 to `CHIPSIG4` in SYSCFG `CHIPSIG` and
   cleared via `CHIPSIG_CLR`. `emulator/qemu/cdj_c6747_syscfg.h` defines only
   `KICK0`, `KICK1`, `PINMUX0` and `CFGCHIP0`; there is no CHIPSIG model, so
   `SYSCFG_CHIPINT2` (event 5) and `SYSCFG_CHIPINT3` (event 67) are also
   unreachable.

`tests/cstub/c6747-intc.c` is a genuine register-semantics test with
manual-derived expected values (reset constants `0xf`, `0xffffffff`,
`0x07060504`, `0x0f0e0d0c`; `INTMUX` readback `0x7f7f7f7f`; derived
`MEVTFLAG`/`MEXPFLAG`; check-phase atomicity; command registers rejecting
reads; status registers rejecting writes; `size != 4` rejected). It also pins
the model's *delivery* choice — direct events pulse on each arrival, combined
events pulse only on inactive→active — which is a design decision rather than
a quoted manual rule; SPRUFK5A describes the dataflow but not this edge
semantics explicitly.

---

## 8. Evidence quality notes

* **`analysis/dsp/isa_probe.json` says nothing about per-register MVC coverage.**
  `MVC` is one row, status `all-probed-forms-accepted`, and its `candidates`
  list contains exactly two encodings: `MVC .S2 B4, AMR` (`001003A2`) and
  `MVC .S2 AMR, B5` (`028003E2`). The probe therefore establishes that two of
  56 possible `crlo`×direction combinations assemble and decode. The
  register-by-register picture in §3 comes from source reading plus the
  behavioural probe in this audit, not from the ISA probe. Anyone reading the
  probe's 109 "all-probed-forms-accepted" rows should not infer control
  register coverage from the `MVC` row.
* **The interrupt-named replay runs are equivalence evidence, not correctness
  evidence, and some are duplicates.** `runs/dsp-interrupt-strict-replay-1` and
  `runs/dsp-interrupt-connected-strict-replay-1` have *identical*
  `trace_sha256` (`e87f6d32…87c1eab3`) and `coverage_sha256`, so they are the
  same replay under two names. Both report `passed: true` while
  `stop.fault == "parallel register write conflict"`; "passed" there means
  repeat-equivalence held, and `scope` says so explicitly ("trace equivalence
  only; not architectural correctness or boot").
* **The two diagnostic runs cited by `DSP_INTERRUPT_ENTRY.md` are one
  observation, not two.** I verified the doc's claimed hash: both
  `runs/dsp-interrupt-entry-fixed-events-3/trace.jsonl` and `…-4/trace.jsonl`
  hash to `a8564c5813ca3a2e9d158873ecf9aa4a42f32af981ee9f9e14db29ee1c266084`,
  exactly as the doc states. Because the two hashes are equal, the pair is
  repeat-determinism evidence for a single trace. Neither directory contains a
  `gate.json` or `coverage.json`, consistent with the doc's own statement that
  this is "not a passed connected equivalence gate".
* **No run artefact attests the nine-cycle interval.** The diagnostic traces
  are memory-write transcripts (`{"event":"write","address":…}`) plus one
  `checkpoint_restore` record; they contain no interrupt-entry,
  control-register or cycle-accounting rows. The only evidence for the
  interval is the architecture test in `tests/cstub/c674x.c`.
* **`runs/dsp-interrupt-entry-fixed-1/gate.json`** passes with
  `architectural_validation_eligible: true` and an empty `approximations` list,
  but it is a 100-step no-events standalone replay — which the doc itself says
  "does not inject the original interrupt and is not evidence for this fix".
  A passing gate with that name is easy to misread.
* **`DSP_BOOT_MILESTONE_AUDIT.md` makes no claim in this track's area.** A
  full-text search for interrupt/IFR/ISTP/IRP/GIE/INTC/exception/privilege/
  supervisor/NMI returns nothing. It was listed as a doc to verify; there is
  nothing here to verify against it.

---

## 9. Stale documentation findings

None found. Both docs named for this track are accurate where they make claims:

* `DSP_INTERRUPT_ENTRY.md` — figure, page, cycle numbers, the nine-cycle
  interval, the test list, the two trace hashes, and every self-imposed
  limitation all check out. The one thing to note is not staleness but
  **incompleteness**: its admitted non-modelling list ("pin synchronization,
  fetch stages, per-cycle TSR field transitions") omits that `IFm` is cleared
  at entry rather than in cycle 8, that not-taken branches do not block
  recognition (5.4.2), and that `MVC`-clear-GIE is given DINT timing. Those are
  recorded as findings in §4.5 rather than as stale-doc findings.
* Source comments in `cdj_c674x.c` are accurate where I checked them
  (`Table 5-3`, `5.4.1`, `5.4.2`, `7.13.1`, `pp.155-156`, `2.9.13`,
  `SPRUFE8B 7.7.3.2`). The comment "PCC/DCC are documented as ignored on
  C674x" is true of their *effect* (Table 2-9) but is used to justify not
  *storing* them, which Table 2-9's `R/SW` legend does require — a fidelity
  gap, not a stale doc.
* `cdj_c6747_intc.c`'s citation "SPRUFK5A 7.2.2 and 7.4.2" is correct, and
  SPRUFK5A's §7.2.2 (event combiner) and §7.4.2 (CPU servicing of interrupt
  events) say what the comment claims. It just pointed at a document that was
  not in `build/references` until this audit.

---

## 10. Open questions

1. Should `build/references/sprufk5a.txt` / `.pdf` be registered in
   `tools/cdj_dsp/refdocs.py` and `provenance.json`? The INTC model cannot be
   audited against SPRUH91D alone, so every future INTC claim needs it.
2. Is `MVC reg,TSR` silently dropping supervisor-writable bits (DBGM, XEN,
   GEE, EXC-clear) the intended policy, or should it fail closed like an
   unsupported register? Everything else in this track fails closed; this is
   the exception, and it is the specific reason exceptions cannot be enabled.
3. Does the real firmware ever write `CSR.PWRD`, `PCC` or `DCC` and read them
   back? If so, the non-round-tripping write mask is observable. I found no
   evidence either way in this track's scope.
4. Does the firmware use `TSCL`/`TSCH` as a timebase? If it does, the absent
   registers are a hard blocker and the fix is plumbing to `cpu->cycles`, not
   new semantics.
5. Is the C6747's dropped-interrupt mechanism (INTXSTAT/INTDMASK, and the
   `INTERR` event it raises) ever exercised by firmware? The emulator has no
   drop detection, so a dropped CPU interrupt is silently lost rather than
   reported.
6. The INTC delivery model pulses a CPU request on every direct event arrival
   but only on inactive→active transitions for combined events. I could not
   find an explicit statement of this edge semantics in SPRUFK5A §7.2/§7.4;
   it may need a primary-source citation or an errata check.
7. The DINT/RINT parallel-conflict list rejects `NOP n` only for `n > 1`.
   The manual writes "NOP n" without qualification; whether plain `NOP`
   (`n == 1`) is legal in parallel with DINT is not settled by the text I read.

---

## 11. Commands run

| Command | Result |
| --- | --- |
| `git log --oneline -5 && git status --short && git branch --show-current` | HEAD `2eb2476` on `codex/macos-nxs`, not the stated `3da5ff2`; unrelated dirty files noted |
| `.venv/bin/python -m tools.cdj_dsp.refdocs --check` | sprufe8b 771 pdf pages / 739 footers, spruh91d 1473 / 1435; hashes verified, provenance written |
| `curl -sSL https://www.ti.com/lit/ug/sprufk5a/sprufk5a.pdf` | 951.5 KB, 219 pages, sha256 `a0008b6f…12291832`; copied to `build/references/` (git-ignored, confirmed via `git check-ignore`) |
| `pdftotext -layout` + page-marker indexer (scratchpad script) | `build/references/sprufk5a.txt`, 219 pages, 210 footers, 209 with printed == PDF index; the one mismatch is a regex false positive |
| `cc -std=c11 -Wall -Wextra -Werror` on `scratchpad/crprobe.c` + `cdj_c674x.c` + `cdj_c674x_loop.c`, with `DEVELOPER_DIR=/Library/Developer/CommandLineTools` | compiled clean; probe enumerated all 32 `crlo` read/write outcomes — the table behind §3 |
| `DEVELOPER_DIR=… .venv/bin/python -m pytest tests/test_c674x.py -q` | **17 passed** in 4.27 s (includes `tests/cstub/c674x.c` interrupt block and `tests/cstub/c6747-intc.c`) |
| `DEVELOPER_DIR=… .venv/bin/python -m pytest tests/test_c674x_saturation.py -q` | **1 passed** in 0.25 s (SSR / CSR.SAT MVC semantics) |
| `DEVELOPER_DIR=… .venv/bin/python -m pytest tests/test_dsp_event_replay.py tests/test_dsp_checkpoint_replay.py -q` | **16 passed** in 9.54 s |
| `shasum -a 256 runs/dsp-interrupt-entry-fixed-events-{3,4}/trace.jsonl` | both `a8564c58…c1e266084`, matching `DSP_INTERRUPT_ENTRY.md` exactly, and identical to each other |
| gate inspection of `runs/dsp-interrupt-{strict,connected-strict}-replay-1`, `runs/dsp-interrupt-entry-fixed-1`, `runs/dsp-post-interrupt-pipedown-5m-replay-2` | identical trace/coverage hashes for the first two; first two `passed: true` with a `parallel register write conflict` stop; `-pipedown-5m-2` is `architectural_validation_eligible: false` with six listed approximations including "interrupt entry retires already-issued results with minimum empty cycles; exact interrupt pipeline latency is not modeled" |
| tree-wide grep for `cdj_c674x_interrupt`, `cdj_c6747_intc_deliver_event`, `cdj_c674x_reset`, `CHIPSIG` | delivery only ever for events 8 and 34; reset only from `cdj2000_nxs_hpi.c:814` with the first L2 word; no CHIPSIG model |
