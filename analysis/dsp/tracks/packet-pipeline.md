# Track 2 — Execute packets, pipeline, hazards, software loops

Reference-backed architectural coverage audit of `emulator/qemu/cdj_c674x.c`,
`cdj_c674x_loop.c/.h` and `cdj_dsp_scheduler.c`.

Repository: `/Users/gpotvin/Git/cdj2000-emulator`, branch `codex/macos-nxs`.
HEAD at audit time was **2eb2476** ("Document SH4 and Blackfin coverage
boundaries"), not the 3da5ff2 named in the task — the coordinator committed
between task issue and this run. Nothing in this track was modified.

Manuals: SPRUFE8B (July 2010) and SPRUH91D. All citations are from SPRUFE8B;
**no SPRUH91D citation was needed** — every requirement in this track is CPU
architecture, not device peripheral behaviour. Printed page == PDF page index
held everywhere I checked, with one caveat recorded in Finding F-11.

---

## 1. Scope

### Covered

- Execute-packet assembly: p-bit scan, the eight-instruction limit, compact
  fetch-packet headers, execute packets spanning fetch packets.
- Branch delay slots, the in-flight branch queue, parallel taken branches,
  branch override of multicycle NOPs, annulled execute packets on interrupt
  entry.
- Multicycle-NOP rules (`NOP n`, `BNOP`, `ADDKPC`, `CALLP`, `IDLE`), protected
  loads (PROT) and their four added cycles.
- Delayed results, the load/store writeback queues, pending memory operations,
  simultaneous overlapping RAM accesses, circular-addressing width retained at
  issue time for queued transfers.
- Stall/interlock modelling (or its absence) and what that does to timing
  claims; `cdj_dsp_scheduler.c` and `tests/test_dsp_scheduler.py`.
- Software loops end to end: `SPLOOP`/`SPLOOPD`/`SPLOOPW` decode and setup,
  buffer loading, issue from the buffer, `SPKERNEL` field reconstruction,
  prolog/kernel/epilog schedule, post-loop fetch-enable delay, ILC/RILC,
  stage-boundary termination, `SPLOOPW` early exit, `SPMASK`/`SPMASKR`,
  reload/`SPKERNELR`, branch interaction, interrupt during loop and resume.

### NOT covered (deliberately out of track, or not reachable)

- Per-instruction arithmetic/FP semantics, the 226-mnemonic ISA denominator,
  compact-format operand decode outside the loop/packet path — Track 1.
- AMR/circular *addressing arithmetic* itself (only its interaction with
  queued transfers is assessed here) — Track 1/3.
- Device peripherals, EDMA, INTC event selection upstream of CPU INT4..15,
  McASP, caches — other tracks. `cdj_c674x_interrupt` is entered at the
  already-selected CPU-interrupt boundary and I did not audit what feeds it.
- Exceptions/NMI (`EXCEP`, NRP, NTSR, IERR) beyond noting that SPLOOP
  exception behaviour (7.13.3) is unmodelled. No exception delivery path
  exists to test against.
- Connected/firmware runs. I ran no QEMU, no replay, no transcript gate. Every
  claim below rests on the manual, source reading, the focused unit tests, and
  four read-only probes I compiled against the unmodified core.
- Cycle-count parity against silicon. Impossible here: no hardware, no TI
  cycle-accurate simulator output in the repository.
- SPLOOP nested/reload execution behaviour: unimplemented and fail-closed, so
  there is nothing to validate beyond the rejection.

---

## 2. Coverage matrix

`impl` = implementation, `val` = validation, `fid` = fidelity. Page numbers are
printed pages (== PDF pages).

| ID | Requirement | Manual | impl | Location | val | fid | Confidence |
|---|---|---|---|---|---|---|---|
| PKT-PBIT | p-bit scan, execute packet assembly, ≤8 instructions | 3.5, p74 | supported | `cdj_c674x.c:990-1040` | reference-backed-tests | strict | high |
| PKT-FPHDR | Compact fetch-packet header, header slot occupies no issue slot, two headers for a spanning packet | 3.10.1/3.10.3, p93/p96 | supported | `cdj_c674x.c:1004-1034` | reference-backed-tests | strict | high |
| PKT-UNIT | Each instruction in an execute packet must use a different functional unit | 3.5, p74 | unsupported | `cdj_c674x.c:1054-2439` (no check) | untested | approximation | high |
| PKT-EPRES | 3.10.4 spanning-execute-packet branch-target and eight-instruction header restrictions | 3.10.4, p96 | unsupported | `cdj_c674x.c:990-1040` (no check) | untested | approximation | high |
| PKT-BRDELAY | Five branch delay slots, six in-flight branch positions, one taken branch per cycle | 7.14 p699; B p163-167 | supported | `cdj_c674x.c:977-988,2534-2551` | reference-backed-tests | strict | high |
| PKT-BRNOP | A branch whose delay slots expire overrides an in-progress multicycle NOP | Fig 4-32, p624 | supported | `cdj_c674x.c:2534-2551` | reference-backed-tests | strict | high |
| PKT-MCNOP | Multicycle-NOP constraints: `NOP n`, `BNOP`, `ADDKPC`, `CALLP`, `IDLE`; equal-count `NOP n` exception | 3.8.10/3.8.11.5, p82-83; 4.4.2, p623 | partial | `cdj_c674x.c:590-597,1066-1072,2343-2375` | reference-backed-tests | approximation | high |
| PKT-PROT | PROT header bit adds four NOP cycles after each LD in the fetch packet | 3.10.1, p93 | partial | `cdj_c674x.c:720-741,1079-1084,2663-2668` | reference-backed-tests | approximation | high |
| PKT-ANNUL | Annulled execute packets and the nine-cycle interrupt-entry interval | Fig 5-4 p641; 5.5.1 p648 | supported | `cdj_c674x.c:134-153,939-972` | reference-backed-tests | approximation | high |
| PKT-DELAY | Delayed results: E1 address / E3 memory / E5 register, writeback queue, parallel-write conflict detection | Tab 4-44, p625 | supported | `cdj_c674x.c:2444-2533` | reference-backed-tests | strict | medium |
| PKT-MEMQ | Pending memory operations; simultaneous overlapping RAM load/store | 4.4.3, p624-625 | partial | `cdj_c674x.c:2442-2456` | reference-backed-tests | approximation | high |
| PKT-CIRCQ | Queued transfer keeps its issue-time circular width; AMR change after issue must not retarget it | 2.8.3/3.9.2; Tab 4-44 p625 | supported | `cdj_c674x.c:808-837` | reference-backed-tests | strict | high |
| PKT-STALL | Pipeline interlocks eliminated; memory stalls, cross-path stall, AMR-use stall | Ch4 intro p575; 4.4.3 p625; 3.8.9 p82; 7.15.2 p700 | partial | `cdj_c674x.c:235-245,1629-1637,2474-2552` | untested (for stalls) | approximation | high |
| PKT-SCHED | `cdj_dsp_scheduler.c` is a host time-slicing policy, not a pipeline/clock model | none (no manual requirement) | supported (as designed) | `cdj_dsp_scheduler.c:1-100` | reference-backed-tests (of its own contract) | n/a → strict to its own spec | high |
| LOOP-SETUP | `SPLOOP`/`SPLOOPD`/`SPLOOPW` decode, ii extraction (full + compact), first-in-packet rule, no multicycle NOP in the setup packet, ILC readiness | 7.5.1 p670; p484-486; Fig H-5/H-6 p765-766 | supported | `cdj_c674x.c:2810-2916` | reference-backed-tests | strict | high |
| LOOP-BUF | Issue from the loop **buffer**, not re-fetch; 48-cycle body; 14-execute-packet storage | 7.7/7.7.1/7.7.3, p676-679; 7.4.1 p669 | partial | `cdj_c674x_loop.c:26-95`; `cdj_c674x.c:2558-2744` | reference-backed-tests | approximation | high |
| LOOP-SPKFIELD | `SPKERNEL` fstg/fcyc: Figure H-7 compact scatter then Table 3-29 stage-bit reversal, Table 3-28 allocation | Fig H-7 p766; Tab 3-28/3-29 p482 | supported | `cdj_c674x.c:2604-2619` | reference-backed-tests | strict | high |
| LOOP-SCHED | Prolog/kernel/epilog generation, post-SPKERNEL program-memory fetch enable delay, delay capped at epilog end | 7.7.1 p677; 7.12 p697; SPKERNEL NOTE p482 | supported | `cdj_c674x_loop.c:35-55,63-95` | reference-backed-tests | strict | high |
| LOOP-ILC | Initial and stage-boundary termination tests, conditional ILC decrement, ILC frozen while interrupt draining, `SPLOOPD` first-three-cycle suppression | 7.9.1/7.9.2 p684; 7.9.3 p685 | supported | `cdj_c674x.c:2758-2769,2899-2914` | reference-backed-tests | approximation | medium |
| LOOP-WHILE | `SPLOOPW` predicate termination three cycles early, no epilog, abrupt exit, ILC/RILC untouched | 7.6.3 p676; 7.10 p690-692 | supported | `cdj_c674x.c:2745-2786,2865-2875` | reference-backed-tests | strict | high |
| LOOP-SPMASK | `SPMASK` must start its packet; masks L1/L2/S1/S2/D1/D2 (+M1/M2 full-width); suppresses buffer ops; masked ops not stored; NOP outside the loop mechanism | p487-488; 7.15 p699; Fig H-8 p766 | partial | `cdj_c674x.c:660-674,1062-1064,2579-2676,2715-2740` | reference-backed-tests | approximation | high |
| LOOP-RELOAD | Reload / nested loops: predicated `SPLOOP`, `SPKERNELR`, `SPMASKR`, RILC→ILC, reload counter, reload-vs-drain exception | 7.7.3.6 p679; 7.8.3 p682; 7.9.6 p686; p483,489 | unsupported | `cdj_c674x.c:2824-2826`; `cdj_c674x_loop.h:9-12` | reference-backed-tests (of the rejection only) | n/a | high |
| LOOP-INT | Interrupt during a loop: eligibility conditions, epilog drain, IRP = SPLOOP packet address, ITSR.SPLX=1, vector after drain, request withdrawn while draining | 7.7.3.1 p678; 7.13.1/7.13.4/7.13.6 p697-698 | partial | `cdj_c674x.c:881-937` | reference-backed-tests | approximation | medium |
| LOOP-RESUME | Return/pipe-up: `B IRP` with TSR.SPLX=1, `SPLOOPD`→`SPLOOP`, parallel ops as NOPs, `BNOP label,n`→`NOP n+1`, masked PM ops as NOP, buffer ops normal | 7.13.2/7.13.5 p698; 7.7.3.2 p679 | partial | `cdj_c674x.c:2583-2708,2837-2896` | reference-backed-tests | approximation | medium |
| LOOP-BRANCH | A taken branch idles an active loop buffer after the last delay slot; false branch leaves the loop alone; branch while reloading keeps the buffer | 7.14 p699; 7.7.3.2 p678 | partial | `cdj_c674x.c:2536-2542` | reference-backed-tests | strict (for the non-reload case) | high |
| LOOP-RESCONF | Hardware exception on an unmasked PM/buffer resource conflict; missed-stall internal exception setting IERR.LBX/MSX | 7.15.1/7.15.2, p700 | unsupported | `cdj_c674x.c:2736-2740` (capacity only) | untested | approximation | high |
| LOOP-EXC | Exception during an active loop: recognized immediately, buffer idle, **no** epilog, NTSR.SPLX=1 | 7.13.3, p698 | unsupported | none | not-assessed | unknown | medium |

---

## 3. Findings

### F-1 (missing-validation, high) — functional-unit conflicts inside an execute packet are never detected

SPRUFE8B 3.5 (printed page 74): *"Each instruction in an execute packet must
use a different functional unit."* `instruction_unit()` exists at
`cdj_c674x.c:679` but is called from exactly two places, both in the SPMASK
path (`:746`, `:2647`). `cdj_c674x_execute` checks parallel **register** write
conflicts, parallel **control** write conflicts, parallel taken branches and
nonaligned-memory pairing — never unit occupancy.

Probe (`probe2.c`, compiled against the unmodified core):

```
MVK.S1||MVK.S1 -> ACCEPTED fault=- A3=11 A4=22 cycles=1
8x MVK.S1       -> ACCEPTED fault=- A1=100 A8=107 cycles=1
```

Eight `MVK .S1` operations issue in a single cycle on one functional unit.

Impact: correct TI-generated firmware never emits such a packet, so this is
unlikely to change audio output. What it does cost is a fail-closed net: a
mis-decoded instruction word, a corrupted instruction stream, or a bug in our
own compact-format lowering can produce an architecturally impossible packet
that executes silently instead of faulting. Every other illegal-packet class in
this core fails closed; this one does not.

### F-2 (suspected-bug, high) — a PROT fetch packet with two parallel loads is rejected

SPRUFE8B printed page 93: *"When PROT is 1, four cycles of NOP are added after
each LD instruction within the fetch packet whether the LD is in 16-bit compact
format or 32-bit format."* Two parallel `LDW` on `.D1`/`.D2` inside one PROT
fetch packet is ordinary, common TI codegen, and the expansion is still four
cycles because the two loads occupy the same cycle.

`cdj_c674x.c:1079-1084` sets `elapsed = 5` on the first protected load and then
rejects any second multicycle contributor:

```c
if (protected_load(insn)) {
    if (elapsed > 1)
        return stop(cpu, pc, insn->word, "multiple multicycle instructions");
    elapsed = 5;
}
```

Probe (`probe3.c`):

```
prot=0 two parallel LDW -> accepted fault=- cycles=1 loads=2
prot=1 two parallel LDW -> REJECTED fault=multiple multicycle instructions cycles=0 loads=0
```

The loop path has the same shape at `:2663-2666` (`if (finish || out.loop_wait)
... "invalid protected loop load packet"`), so a dual-load protected packet
inside an SPLOOP body is rejected too.

Impact: fail-closed, so no wrong results — but any firmware path that uses a
PROT fetch packet with dual `.D` loads stops the DSP. `BUILD.md:996` records
that protected loads inside software-pipelined loops are exactly what the NXS
firmware does at `0x11802ea8`, which makes this a live risk for firmware
breadth rather than a theoretical one.

### F-3 (suspected-bug, low) — two `NOP n` with the same count are rejected, but the manual permits them

SPRUFE8B 3.8.11.5 (printed page 83): *"A NOP n (with n > 1) instruction cannot
be placed in parallel with other multicycle NOP counts (ADDKPC, BNOP, CALLP)
**with the exception of another NOP n where the NOP count is the same**."*

`cdj_c674x.c:1069-1070` has no equal-count carve-out. Probe (`probe.c`):

```
(a)  NOP4||NOP4 -> REJECTED fault=multiple multicycle instructions cycles=0
(a2) NOP4||NOP2 -> REJECTED fault=multiple multicycle instructions
```

(a2) is correct; (a) over-rejects. Fail-closed, and the TI assembler has no
reason to emit it, so impact is near zero — but it is a documented exception
that the core does not implement.

### F-4 (missing-implementation, medium) — the 14-execute-packet loop-buffer storage limit is not enforced

SPRUFE8B 7.4.1 (printed page 669) and 7.7 (printed page 676): *"The loop buffer
has storage for up to 14 execute packets"*; separately *"The loop buffer can
accommodate a SPLOOP body of up to 48 cycles."* These are two different limits.
The core enforces 48 cycles (`cdj_c674x_loop.c:29`) and a 112-operation tag cap
(`cdj_c674x.c:2689`), but not the 14-packet storage bound.

Probe (`probe4.c`), `SPLOOP 16` with one stored operation per body cycle:

```
stored body packets=14 -> accepted loop_tags=14 loop_packets=15 length=15
stored body packets=20 -> accepted loop_tags=20 loop_packets=21 length=21
```

112 tags == 14 packets × 8 slots, so the tag cap coincidentally matches the
*operation* capacity of a 14-packet buffer, but it permits 48 sparse packets.

Note the distinction the existing test at `tests/cstub/c674x.c:513-523`
(`assert(c.loop_packets == 21 ...)`) is making: **21 source fetches with zero
stored tags** is architecturally fine, because NOP cycles store nothing. That
test is correct and does not cover F-4. My probe stores 20 real packets.

### F-5 (stale-documentation, high) — `BUILD.md` understates SPLOOP coverage by a wide margin

`BUILD.md:630-641` ("Unconditional SPLOOP execution") states:

- *"Loading is limited to 14 original packets, 48 cycles and eight simultaneous
  instructions."* — wrong on the first count in both directions: source packets
  are not capped at 14 (the code comment at `cdj_c674x.c:2572-2574` says so
  explicitly and the test asserts 21), and stored packets are not capped at 14
  either (F-4).
- *"SPMASK, reload/nested loops, SPLOOPD/W, protected/control instructions in
  the body, interrupt draining and restart remain unsupported."* — of the six
  items listed, **five are now implemented and tested**: SPMASK
  (`tests/cstub/c674x.c:542-612`), SPLOOPD and SPLOOPW (`:459-523`, `:2806-2926`),
  protected loads in the body (`:149-196`), interrupt draining and restart
  (`:2759-2871`). Only reload/nested loops remain unsupported.
- *"The core decodes full-width unconditional SPLOOP and SPKERNEL"* — compact
  `SPLOOP`/`SPLOOPD` (`cdj_c674x.c:2814-2818`), compact `SPKERNEL` (`:2600`)
  and predicated `SPLOOPW` (`:2819`) are all decoded.
- *"the 160-test host suite pass (42 platform/dependency tests skipped)"* —
  the current baseline quoted in the audit task is 506 passed / 31 skipped.

This section reads as a description of a much earlier state of the core.
Because it is phrased as a limitation list, it causes the *opposite* of the
usual documentation risk: a reader will under-credit implemented, tested
behaviour and may redo work.

### F-6 (stale-documentation, medium) — `cdj_c674x.h` claims a seven-instruction interpreter

`emulator/qemu/cdj_c674x.h:7-8`:

```c
/* Partial interpreter. Encodings/semantics: TI SPRUFE8B, instruction entries
 * MVK, MVKH, MVC, AND, B, ADDKPC and NOP; no third-party decoder code. */
```

The file it heads is a ~2900-line core implementing single-precision FP
add/subtract/multiply/convert, the full compact `.D` memory families,
saturating packs, compares, the SPLOOP family, interrupt entry and more. This
is the public header of the module, so it is the first thing a reader sees.
(Noted here because it describes the packet/pipeline core; the instruction
inventory itself belongs to Track 1.)

### F-7 (evidence-weakness, medium) — the only emulator-independent SPKERNEL oracle is skipped by default

`tests/test_c674x_spkernel_fields.py:27-42` is the sole test in this track whose
expected values come from outside our own code: it runs TI `cl6x`/`dis6x` over
`tests/ti/spkernel-oracle.asm` and asserts `9c67`/`dc66`/`1f66` ↔ stages
`3`/`6`/`24`. It is gated on `C6X_TI_BIN`:

```
$ DEVELOPER_DIR=... .venv/bin/python -m pytest -q -rs tests/test_c674x_spkernel_fields.py
.s
SKIPPED [1] tests/test_c674x_spkernel_fields.py:30: set C6X_TI_BIN to independently check TI assembler encodings
1 passed, 1 skipped
```

A skipped prerequisite is not a pass. Any environment without a user-installed
TI CGT 8.5.0 validates the SPKERNEL field reconstruction only against the
repository's own second implementation of the same rule (see §4.2).

### F-8 (missing-implementation, medium) — SPLOOP resource-conflict and missed-stall exceptions are unmodelled

SPRUFE8B 7.15.1 (printed page 700) requires a **hardware exception** when a
program-memory instruction has a resource conflict with an unmasked loop-buffer
instruction, and 7.15.2 requires an **internal exception setting IERR.LBX and
IERR.MSX** when a stall that was needed for correctness was skipped because the
instruction came from the loop buffer. Neither exists. `loop_step` checks only
issue capacity (`cdj_c674x.c:2736-2740`, `:2740`). This is the loop-specific
consequence of F-1: because units are never compared, the architectural
signal that TI designed precisely to catch this class of bug can never fire.

### F-9 (fidelity-gap, medium) — no stall model at all; all timing is fixed issue accounting

SPRUFE8B chapter 4 opening (printed page 575) states interlocks are
*eliminated*, so for register delay slots the absence of interlocks is
**correct**, not a gap. The gaps are the three places the manual does require
a stall:

1. **Memory stalls** (4.4.3, printed pages 624-625, Figure 4-34). Not modelled.
   The manual says *"The results of the program execution are identical whether
   a stall occurs or not"*, so functional fidelity is preserved; **cycle counts
   are not**. `cpu->cycles` advances by `elapsed` (`cdj_c674x.c:2474`), which is
   the multicycle-NOP count and nothing else.
2. **AMR-use stall** (3.8.9, printed page 82). Explicitly fail-closed:
   `address_width()` returns false while `control_ready[0] > cycles` and the
   caller stops with `"AMR use interlock not implemented"` (`:1629-1637`,
   `:1691-1700`). Honest and correct behaviour for an unimplemented rule.
3. **Cross-path stall** (3.8.4; 7.15.2 printed page 700). Not modelled and not
   fail-closed — a cross-path read of a register written in the previous cycle
   executes with no stall and no exception.

Consequence for any timing claim in this repository: `cpu->cycles` is an *issue
cycle* count with multicycle-NOP expansion, not a hardware cycle count. It
cannot be compared to silicon. It is suitable only for deterministic
self-comparison (replay equivalence), which is what the existing artifacts use
it for.

### F-10 (evidence-weakness, low) — `test_dsp_scheduler.py` is not evidence for anything in this track

`cdj_dsp_scheduler.h:13-15` says so itself: *"DEFERRED_V1 is a diagnostic host
scheduling policy, not a DSP clock model."* It is a 1,000,000-step activation
counter sliced at 4,096 steps, with a checkpoint ABI. `test_dsp_scheduler.py`
validates that state machine and its 32-byte ABI — correctly, and it passes —
but it touches no pipeline, packet, hazard or loop behaviour. It should not be
cited in support of any timing or hazard claim.

### F-11 (evidence-weakness, low) — one cited page has no detected printed-page footer

`build/references/sprufe8b.txt` marks PDF page 766 as `@@ PDFPAGE 766 PRINTED -`
— the footer regex missed it (771 PDF pages, 739 footers detected). The page
body's own footer line reads `766 No Unit Specified Instructions and Opcode
Maps`, so **printed page 766 == PDF page 766** and the global offset-0 fact
holds. Figures H-5 through H-8, including the H-7 Uspk format this track
depends on, live on that page. Flagging it so nobody reads the `-` as a
contradiction of the offset-0 verification.

### F-12 (missing-validation, low) — two assembler-enforced packet restrictions are silently permitted

- SPRUFE8B 3.10.4 (printed page 96): an execute packet spanning two fetch
  packets may not be a branch target when either fetch packet is header-based,
  and an eight-instruction execute packet may not involve a header-based fetch
  packet. `cdj_c674x_fetch` tracks both headers correctly (3.10.3) but enforces
  neither restriction.
- SPKERNEL page (printed page 481): *"The SPKERNEL instruction cannot be placed
  in the execute packet immediately following an execute packet containing any
  instruction that initiates multicycle NOPs."* The core rejects SPKERNEL
  *sharing* such a packet (`cdj_c674x.c:2640`, `:2664`; test
  `tests/cstub/c674x.c:197-208`) but not SPKERNEL *following* one: after the NOP
  cycles drain `loop_wait` to zero, the next source fetch accepts SPKERNEL
  normally.

Both are assembler-enforced in TI's flow and are fail-open here.

### F-13 (fidelity-gap, low) — `ii` range above 14 is an undocumented extrapolation

`cdj_c674x_loop_init` accepts `ii` 1..16 and rejects 17+ (`cdj_c674x_loop.c:19`).
The full-width `SPLOOP` opcode carries a 5-bit `ii-1` field (printed page 484),
so 1..32 is encodable. Figure H-6's note (printed page 766) says the compact
form *"Supports ii of 1-16"*. But Table 3-28 (printed page 482) defines the
`stg`/`cyc` bit allocation only for `ii` 1-14. For `ii` 15-16 both the
implementation (`cdj_c674x.c:2611-2615`, `cbits = ceil(log2(ii)) = 4`) and the
test's own helper (`tests/cstub/c674x-spkernel-fields.c:25-29`, `ii <= 8 ? 3 : 4`)
extrapolate the 9-14 row. The extrapolation is self-consistent (4 cycle bits
cover cycles 0-15, exactly what `ii=16` needs) but has no manual authority, and
the test exercises `ii=16` as though it did.

---

## 4. Detailed assessment of the high-value areas

### 4.1 Buffer versus re-fetch — the core models the buffer

Verified by reading `loop_step` (`cdj_c674x.c:2558-2792`) end to end:

- **Loading** (`!loop.sealed`, `loop_wait == 0`): one `cdj_c674x_fetch` per
  cycle; each non-control operation is decoded once, stored in
  `cpu->loop_instructions[tag]` with its **original** pc/word/compact/header,
  and its tag is handed to `cdj_c674x_loop_load` at the current cycle index.
- **Kernel** (`sealed && !post`): **there is no fetch call on this path at
  all.** Issue comes from `cdj_c674x_loop_issue_filtered` and the packet is
  rebuilt from `out.loop_instructions[tags[i]]` (`:2741-2742`).
- **Epilog** (`sealed && post`): one fetch per cycle for post-SPKERNEL
  program-memory instructions, merged with the draining buffered operations
  into one composite packet executed under a single architectural commit.

The schedule itself is a timeline in `CdjC674xLoop`: `tags[48][8]` indexed by
load cycle, and `issue` selects every origin congruent to `cycle % ii`
(`cdj_c674x_loop.c:75-88`). That is a direct model of LBC indexing per 7.7.3.3
(printed page 678), and it is what makes the epilog fall out for free: an
origin stops issuing once `age / ii >= iterations`.

Evidence that the stored-instruction identity is real, not cosmetic:
`tests/cstub/c674x.c:2899-2906` executes a composite packet whose second
instruction is invalid and asserts `c.fault_pc == 0x1010` — the **original**
program PC of the buffered operation, not the replay PC — with full rollback.

Two caveats, both flagged above:

- `cpu->loop_packets` is a **source-fetch counter**, incremented at
  `cdj_c674x.c:2575`. The test `assert(c.loop_packets == 21)` at
  `tests/cstub/c674x.c:523` counts source fetches, **not** buffered issues. Do
  not read it as a buffer-occupancy measurement.
- On interrupt return the body is re-fetched from program memory and each
  instruction is matched against retained metadata (`loop_retained_tag`,
  `:751-768`). That matches the architecture — 7.7.3.1 (printed page 678) says
  the loop "is piped back up by executing a prolog", and a prolog loads from
  program memory — but the identity check itself is an emulator-specific
  strictness, not a hardware mechanism. If the program image changed under the
  ISR, hardware would simply load the new image; we stop with
  `"SPLOOP retained instruction mismatch"`.

### 4.2 The `DSP_BOOT_MILESTONE_AUDIT.md` SPKERNEL claim — verified, with two caveats

The claim (`DSP_BOOT_MILESTONE_AUDIT.md:53-60`) is that compact SPKERNEL fields
are reconstructed per Figure H-7 *before* Table 3-29 stage reversal, so
`0xdc66` is stage 6 at `ii=1` and not stage 3, validated by a 64-field × 9-`ii`
C test and an independent TI fixture checking `9c67`/`dc66`/`1f66` as stages
3/6/24.

**I verified every part of this independently of the code and the tests.**

Figure H-7 (printed page 766) places the compact `ii/stg` field as: bits 15:14 →
field[4:3], bits 9:7 → field[2:0], bit 0 → field[5]. The implementation at
`cdj_c674x.c:2608-2610` is

```c
unsigned field = compact_kernel ?
    ((w & 1) << 5) | ((w >> 7) & 7) | (((w >> 14) & 3) << 3) : (w >> 22) & 63;
```

which is that scatter exactly, with the full-width form reading bits 27:22
directly (SPKERNEL opcode, printed page 481). Table 3-29 (printed page 482)
maps `stg/cyc[5] → stage[0]`, `[4] → stage[1]`, … i.e. the stage bits are
reversed; `cdj_c674x.c:2613-2614` applies `stage |= ((field >> j) & 1) << (5-j)`
for `j` from 5 down to `cbits`, and `cbits = ceil(log2(ii))` reproduces Table
3-28's allocation for every row (`ii`=1→0 cycle bits, 2→1, 3-4→2, 5-8→3,
9-14→4).

Hand-decoding the three oracle words:

| word | fixed bits match H-7 | field | stage at `ii=1` |
|---|---|---|---|
| `0x9c67` | yes | `(1<<5)｜0｜(2<<3)` = 48 = `110000` | reverse → `11` = **3** |
| `0xdc66` | yes | `(0<<5)｜0｜(3<<3)` = 24 = `011000` | reverse → `110` = **6** |
| `0x1f66` | yes | `(0<<5)｜6｜(0<<3)` = 6 = `000110` | reverse → `11000` = **24** |

So stages 3/6/24 — matching TI `cl6x`/`dis6x`, matching the implementation, and
matching the documented claim. The "not stage 3" warning is about the plausible
mis-scatter that reads bits 15:14 as field[1:0] and skips the reversal, which
yields 3 for `0xdc66`.

**Are the expected values independent of the emulator?** Mostly yes, on two
separate axes:

- `tests/cstub/c674x-spkernel-fields.c` re-implements the Figure H-7 scatter
  (`compact_kernel`, line 19) and the Table 3-29 reversal (`stage_value`, line
  31) *in the test*, and computes the expected `post_cycle` arithmetically
  (`128*ii + stage*ii + cycle`, clamped to the loading end and the drain end,
  line 86). These are hand-derived from the manual, not recorded from our
  output. But they are a second implementation by the same author of the same
  rule, so they catch a coding slip, not a misreading.
- The test also asserts the compact decode and the **full-width** decode
  produce byte-identical `CdjC674xLoop` state (`!memcmp`, line 84). The
  full-width field is read directly out of bits 27:22 with no scatter, so this
  cross-check is genuinely structural: a wrong compact scatter cannot survive
  it.
- `test_ti_compact_spkernel_oracle` is the one fully external oracle. It is
  **skipped without `C6X_TI_BIN`** (F-7), and it checks the assembler's
  encodings rather than feeding them to the emulator — the bridge is the C
  test's `assert(compact_kernel(24) == 0xdc66)` plus `assert(stage_value(24,0) == 6)`
  at lines 114-115.

**Two caveats the documentation does not mention.** First, F-7: the external
oracle is optional. Second, and more substantive: in `compare_field` every body
cycle is a zeroed word, which decodes as `NOP 1`, so the loop buffer holds
**zero** operations and the test asserts `!an && !bn` (line 100). The 64-field ×
9-`ii` sweep therefore validates *schedule arithmetic* — `post_cycle`,
`end_cycle`, the `post`/`drained` flags across every cycle to drain — and not
buffered issue of real instructions. Calling it "all 64 fields … and schedule
equivalence" is accurate; reading it as SPKERNEL *execution* coverage would not
be. Execution coverage lives elsewhere (§4.3), and `ii=16` in that sweep is the
undocumented extrapolation of F-13.

### 4.3 Loop schedule and execution evidence that *is* manual-derived

`tests/cstub/c674x-loop.c:84-99` encodes SPRUFE8B **Table 7-1** (printed page
674) as a 14-cycle expected issue mask for Example 7-4 (`SPLOOP 1`, LDW, NOP 4,
MV, `SPKERNEL 6,0 || STW`, ILC=8), with bit 1 = LDW, 2 = MV, 4 = STW:

```
{1,1,1,1,1,3,7,7,6,6,6,6,6,4}
```

I checked this cell by cell against Table 7-1: prolog cycles 1-5 issue LDW only;
cycle 6 adds MV (3); cycles 7-8 are the two kernel cycles (7); cycles 9-13 drop
LDW (6); cycle 14 is STW alone (4). The test then asserts `!post && !drained`
for all 14 and `post && drained && n == 0` on cycle 15 — which is the manual's
own statement that `SPKERNEL 6,0` delays post-loop code until the 6-cycle epilog
completes (7.6.1, printed page 674). **This is a genuine manual-derived oracle.**

End-to-end execution through the real core is at `tests/cstub/c674x.c:2963-2985`:
the same program is assembled into RAM (including `SPKERNEL 6,0` as field 24,
`(24u << 22) | 0x34001`) and driven through `cdj_c674x_step`, asserting eight
words copied, both pointers advanced by `iterations * 4`, `ILC == 0`, and — for
`iterations == 0` — that no memory operation occurred. The copied-data
expectation comes from Example 7-3's C semantics, not from our output. This is
the strongest single piece of loop evidence in the repository.

By contrast, `compare_schedulers` (`tests/cstub/c674x-loop.c:46-75`) compares
`cdj_c674x_loop_issue_filtered` against a `reference_issue` linear-scan
reimplementation over `ii` 1-16 × length 0-48 × 4 modes × 4 iteration counts ×
13 cycle values × 2 filters. That is a **refactor-equivalence / regression**
oracle — it proves the modulo-skip optimisation matches the linear scan it
replaced — and is not independent of the emulator's own interpretation. Valuable,
but not reference-backed.

### 4.4 Reload, early exit, ILC/RILC, stage-boundary termination, interrupt/resume

| Sub-requirement | Manual | State | Test evidence |
|---|---|---|---|
| Initial termination test + ILC decrement at setup | 7.9.1 p684 | implemented | `c674x.c:2965-2985` (ILC=8 and ILC=0 paths) |
| Stage-boundary termination + conditional ILC decrement | 7.9.2 p684 | implemented, but **recomputed** not decremented | `c674x.c:2965-2985`, `:2759-2804` |
| `SPLOOPD` first-three-cycle suppression, `ceil(4/ii)` minimum | 7.9.2/7.9.3 p684-685 | implemented | `c674x.c:459-501` over `ii` 1-16 |
| ILC frozen while interrupt draining; holds remaining iterations after | 7.9.2 p684; 7.13.1 p697 | implemented | `c674x.c:2781-2804` asserts `frozen_ilc` every drain step, then `frozen_ilc - 1` after resume |
| `SPLOOPW` early exit, no epilog, 3-cycle-stale predicate | 7.6.3 p676; 7.10 p690-691 | implemented | `c674x.c:2907-2936` over `ii` 1-14 × both polarities; late-update test at `:2927-2936` |
| `SPLOOPW` leaves ILC/RILC untouched | 7.10 p690 | implemented | `c674x.c:2912,2923` asserts ILC=91, RILC=27 unchanged |
| `SPLOOPW` termination while interrupt draining → resume after SPKERNEL | 7.10.3 p691 | implemented | `c674x.c:2846-2871` |
| Interrupt eligibility: boundary, termination false, not loading/draining, ≥4 cycles in, `ILC >= ceil(dynlen/ii)` | 7.13.1 p697 | implemented | `c674x.c:2728-2742`, `:2759-2804` |
| Interrupt drain → IRP = SPLOOP packet address, ITSR.SPLX=1, TSR.SPLX=0 | 7.7.3.1 p678; 7.13.1 p697 | implemented | `c674x.c:2792-2798` |
| Request withdrawn while draining → continue after SPKERNEL, no vector | 7.13.6 p698 | implemented | `cdj_c674x.c:927-937`; `c674x.c:2869-2871` |
| Pipe-up: `SPLOOPD`→`SPLOOP`, parallel ops NOP, `BNOP label,n`→`NOP n+1` | 7.13.2/7.13.5 p698 | implemented | `c674x.c:2498-2524` (functional mode), `:2529-2568` |
| Pipe-up with SPMASK in the body | 7.13.2 p698 | **strict mode fails closed** | `c674x.c:2744-2757` asserts the `"SPLOOP interrupt SPMASK resume not implemented"` stop |
| Interrupts disabled 2 cycles before `SPLOOP(D/W)` | 7.13.1 p697 | **not modelled**, conservatively subsumed by the 4-cycle opening | none |
| Reload: predicated `SPLOOP`, `SPKERNELR`, `SPMASKR`, RILC→ILC, reload counter, two LBCs, reload-vs-drain exception | 7.7.3.6 p679; 7.9.6 p686 | **unsupported** | `c674x.c:502-512` asserts `"SPLOOPD reload not implemented"`; `isa_probe.json` reports `SPMASKR` all-probed-forms-rejected |
| Exception during loop: immediate idle, no epilog, NTSR.SPLX=1 | 7.13.3 p698 | **unsupported / not assessed** | none |

Two notes on the "implemented" column.

*ILC is recomputed, not decremented,* for the non-delayed `SPLOOP` case:
`cdj_c674x.c:2769` sets `control[13] = iterations - launched` (floored at 0)
every cycle. It is observationally equivalent to a per-boundary decrement for a
loop nobody interferes with, and writes during the body are separately rejected
(`"unmasked loop MVC not permitted"`, `:2681`), so I could not construct a
divergence. It is still a different mechanism from the manual's, and it is why
I rated LOOP-ILC fidelity `approximation` rather than `strict`.

*Interrupt-during-loop resume is functional-mode-dependent for SPMASK bodies.*
Strict mode fails closed; functional mode reconstructs the pipe-up from the
unchanged program image (`:2650-2656`). Functional timing is opt-in via a device
property and warns `"run is not cycle-validation evidence"`
(`cdj2000_nxs_hpi.c:923,934`). Strict default confirmed.

### 4.5 The nine-cycle interrupt-entry abstraction

`DSP_INTERRUPT_ENTRY.md:8-14` claims Figure 5-4 (page 641) puts the first
annulled E1 at cycle 6 and the ISR's E1 at cycle 15, so "current PC is the first
annulled execute packet" implies nine empty issue cycles.

**Verified.** Figure 5-4 (printed page 641) shows packets `n+5` … `n+11` as
"Annulled Instructions", `n+5` reaching E1 in cycle 6, and the ISFP row reaching
E1 in cycle 15. Section 5.5.1 (printed page 648) states independently:
*"Overhead for all CPU interrupts is 9 cycles … no new instructions are entering
the E1 pipeline phase during CPU cycles 6 through 14."* The implementation is
`cpu->idle_cycles = 9` (`cdj_c674x.c:141`), and
`tests/cstub/c674x.c:37-47` pins it: `assert(c->idle_cycles == 9)`, then nine
steps each asserting the PC is unchanged, `cycles` advances by one, and
`idle_cycles` counts down. The expected value 9 is hand-derived from the figure
and corroborated by 5.5.1. **Reference-backed, and pinned by a test.**

Is it strict? The *interval* is strict. The *alignment* is an approximation, and
I rated PKT-ANNUL fidelity `approximation` for two reasons:

1. There is no PG/PS/PW/PR/DP/DC front end. Figure 5-4 detects the interrupt in
   cycle 4 and sets IFm in cycle 6; the emulator annuls whatever packet the PC
   names at the moment `cdj_c674x_interrupt` is called. Relative to hardware the
   entry can be a couple of cycles early or late depending on when the device
   loop presents the request. The code comment (`:939-943`) states this honestly.
2. E-stages beyond E5 are not modelled at all. Figure 5-4 shows older packets
   maturing through E10 during cycles 6-14; the emulator retires queued loads,
   stores and delayed results during the nine idle cycles, which covers every
   latency it models, but nothing longer.

Also note `interrupt_pipe_down` has a second branch (`:144-152`) used only in
functional mode, which drains to the latest queued `due` instead of a fixed 9.
That is a minimum-drain approximation and is correctly labelled as such.

### 4.6 SPMASK

Decode verified against the opcode figure on printed page 487: bits 25:18 are
`{M2,M1,D2,D1,S2,S1,L2,L1}`, and `spmask_decode` (`cdj_c674x.c:669-671`) takes
`(w >> 18) & 255` with the bit order the adjacent comment documents
(`L1,L2,S1,S2,D1,D2,M1,M2`). `instruction_unit` returns `1<<side` for L,
`4<<side` for S, `16<<side` for D, `64<<side` for M — consistent. The compact
form (Figure H-8, printed page 766) carries only six bits and correctly cannot
mask M1/M2; `:665-667` reproduces that scatter.

Covered behaviours, all with tests:

- *SPMASK must be first in its execute packet* (printed page 487) — enforced in
  three places (`:1063`, `:2595`, `:2725`); `tests/cstub/c674x.c:574-588`
  asserts that the misplaced case is atomic (`cycles == 0`, no register write).
- *SPMASK outside the loop mechanism is a NOP* (printed page 487; 7.15 printed
  page 699) — `:1062-1064`; same test, both full-width and compact.
- *Masked program-memory ops execute but are not stored in the buffer*
  (7.7.3.3 printed page 678) — `:2673-2676` routes them to `direct`;
  `tests/cstub/c674x.c:542-559` asserts `loop_tags == 1` with the masked op
  executed.
- *Masked buffer ops are suppressed* (7.15 printed page 699) — `loop_allow`
  filter applied *before* the eight-operation capacity check
  (`cdj_c674x_loop.h:35-37`, tested at `c674x-loop.c:150-153`: 16 candidates,
  8 issued).
- *All six compact mask bits, both sides* — `tests/cstub/c674x.c:589-612`.
- *Masking during the epilog* — `:560-573`.
- `loop_allow` sets `unknown` when `instruction_unit` returns 0 and the caller
  stops with `"buffered SPMASK unit not implemented"` (`:2739`) — it never
  assumes an unclassified operation is safe to mask.

The gap is `SPMASKR` (reload), rejected, and the 7.13.2 reversed-masking case
on interrupt return, which is strict-mode fail-closed (§4.4).

---

## 5. Stale-documentation findings

| File:line | Claim | Why stale | Current truth |
|---|---|---|---|
| `BUILD.md:636` | "Loading is limited to 14 original packets, 48 cycles and eight simultaneous instructions." | The 14-packet bound was deliberately removed; `cdj_c674x.c:2572-2574` says so and `tests/cstub/c674x.c:523` asserts 21 source packets. | 48 cycles, 112 tags, 8 simultaneous are enforced. Neither 14 source packets nor 14 *stored* packets are enforced (F-4). |
| `BUILD.md:638-641` | "SPMASK, reload/nested loops, SPLOOPD/W, protected/control instructions in the body, interrupt draining and restart remain unsupported" | Five of six are implemented and tested. | Only reload/nested loops remain unsupported. SPMASK, SPLOOPD, SPLOOPW, protected loads in the body, interrupt draining and restart all have tests. |
| `BUILD.md:624-625` | "The core decodes full-width unconditional SPLOOP and SPKERNEL" | Compact and predicated forms are decoded. | Compact `SPLOOP`/`SPLOOPD` (`:2814-2818`), compact `SPKERNEL` (`:2600`), predicated `SPLOOPW` (`:2819`), and compact `SPLOOPD` reload forms rejected explicitly (`:2824-2826`). |
| `BUILD.md:648-649` | "the 160-test host suite pass (42 platform/dependency tests skipped)" | Superseded baseline. | Task-quoted baseline is 506 passed / 31 skipped. |
| `emulator/qemu/cdj_c674x.h:7-8` | "instruction entries MVK, MVKH, MVC, AND, B, ADDKPC and NOP" | ~2900 lines implementing FP arithmetic, compact `.D` memory families, packs, compares, the SPLOOP family and interrupt entry. | A large partial ISA; the header undersells it by two orders of magnitude. |
| `emulator/qemu/cdj_c674x_loop.h:10-11` | "retained-buffer reload is not implemented" | True of this module, misleading for the subsystem. | `cdj_c674x.c` does implement retained-metadata capture and interrupt-return re-loading (`:94-106`, `:751-787`, `:2682-2708`). What is unimplemented is *nested-loop reload* (`SPKERNELR`/`SPMASKR`/RILC). The two senses of "reload" should be distinguished. |

---

## 6. Open questions

1. Is F-2 (PROT + two parallel loads rejected) actually reachable in the NXS
   firmware? `BUILD.md:996` puts a protected `LDW` inside a software-pipelined
   loop at `0x11802ea8`, but I did not disassemble the surrounding fetch packet
   to see whether a second `.D` load is parallel to it. Resolving this needs a
   firmware image and is outside a read-only architecture audit.
2. For `ii` 15-16, what *is* TI's `stg`/`cyc` bit allocation? Table 3-28 stops
   at 14 while Figure H-6 advertises `ii` 1-16. `cl6x` could settle it —
   assemble `SPLOOP 15` / `SPKERNEL n,0` and read the listing — but that needs a
   new fixture and belongs with the existing oracle test rather than in this
   audit.
3. Can any legal instruction stream distinguish the ILC *recompute* at
   `cdj_c674x.c:2769` from the manual's per-boundary *decrement*? I could not
   construct one given that body MVC writes to ILC are rejected, but I did not
   prove equivalence.
4. Does the full-width `SPLOOP` field really permit `ii` up to 32, or is 16 the
   architectural maximum with bits 27:28 of the field reserved? The instruction
   page (printed page 484) states no range; only the compact figure does.
5. What cycle cost should a memory stall carry, if the project ever wants
   cycle-comparable timing? SPRUFE8B gives the stall *mechanism* (4.4.3) but the
   latencies are device-specific; that is SPRUH91D/EMIF/cache territory and
   belongs to the peripheral tracks.
6. `stores[24]` / `loads[40]` / `branch_queue[5]` are implementation bounds with
   fail-closed overflow (`"load queue full"`). Five is architecturally right for
   branches; 24/40 are generous relative to anything the E1-E5 window can hold,
   but I did not derive a proof that they cannot be exceeded by legal code.

---

## 7. Commands run

All from `/Users/gpotvin/Git/cdj2000-emulator`. No emulator source, test or
tool file was modified; the only files I created are this report and four probe
sources under the session scratchpad.

```
git log --oneline -1
  -> 2eb2476 Document SH4 and Blackfin coverage boundaries   (HEAD had moved past 3da5ff2)

.venv/bin/python -m tools.cdj_dsp.refdocs --check
  -> sprufe8b: 771 pdf pages, 739 footers
     spruh91d: 1473 pdf pages, 1435 footers
     provenance -> build/references/provenance.json        (provenance gate present, verified)

DEVELOPER_DIR=/Library/Developer/CommandLineTools \
C6X_TI_BIN=/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin \
  .venv/bin/python -m pytest -q tests/test_c674x_spkernel_fields.py \
      tests/test_dsp_scheduler.py tests/test_c674x.py
  -> 20 passed in 3.22s   (no skips)

DEVELOPER_DIR=... C6X_TI_BIN=... .venv/bin/python -m pytest -v -rs \
      tests/test_c674x_spkernel_fields.py tests/test_dsp_scheduler.py
  -> test_c674x_spkernel_fields PASSED
     test_ti_compact_spkernel_oracle PASSED      (TI cl6x/dis6x oracle really ran)
     test_dsp_scheduler_state_machine PASSED
     3 passed in 0.56s

DEVELOPER_DIR=... .venv/bin/python -m pytest -q -rs tests/test_c674x_spkernel_fields.py
  -> 1 passed, 1 skipped
     SKIPPED: set C6X_TI_BIN to independently check TI assembler encodings   (Finding F-7)

DEVELOPER_DIR=... .venv/bin/python -m pytest -q tests/test_c674x_circular.py \
      tests/test_c674x_saturation.py
  -> 2 passed in 0.76s   (queued-transfer circular-width evidence for PKT-CIRCQ)

# Read-only probes, compiled against the unmodified core.
# Sources: <scratchpad>/probe.c, probe2.c, probe3.c, probe4.c
DEVELOPER_DIR=... cc -std=c11 -Wall -Wextra -Werror -I emulator/qemu \
      <probe>.c emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c -o <probe>

probe   -> (a)  NOP4||NOP4 REJECTED "multiple multicycle instructions"      (F-3)
           (a2) NOP4||NOP2 REJECTED "multiple multicycle instructions"      (correct)
probe2  -> MVK.S1||MVK.S1 ACCEPTED, A3=11 A4=22, cycles=1                   (F-1)
           8x MVK.S1      ACCEPTED, A1=100 A8=107, cycles=1                 (F-1)
probe3  -> prot=0 two parallel LDW accepted, cycles=1, loads=2
           prot=1 two parallel LDW REJECTED "multiple multicycle instructions" (F-2)
probe4  -> 14 stored body packets accepted, loop_tags=14, loop_packets=15
           20 stored body packets accepted, loop_tags=20, loop_packets=21    (F-4)
```

Manual pages read in full for this track: SPRUFE8B printed 74, 82-83, 93, 96,
274, 388-389, 481-490, 575, 623-626, 641-642, 648, 669-670, 673-674, 676-681,
684-685, 690-691, 697-700, 765-767.

Scratchpad:
`/private/tmp/claude-501/-Users-gpotvin-Git-cdj2000-emulator/60f50c0f-0b26-4acb-93da-0bac7a675217/scratchpad`
