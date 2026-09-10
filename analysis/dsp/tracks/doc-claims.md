# Track 6 — documentation claims versus current implementation

Audit target: `/Users/gpotvin/Git/cdj2000-emulator`, branch `codex/macos-nxs`.
The task named HEAD `3da5ff2`; the actual HEAD when this audit ran was
**`2eb2476` ("Document SH4 and Blackfin coverage boundaries")**, one commit
later, which adds `SH4_BLACKFIN_COVERAGE.md`. That doc is included below.

Working tree was **not clean**: `emulator/qemu/cdj2000_usbh.c` modified, and
untracked `tests/test_nxs_usbh.py`, `tests/test_dsp_isa_audit.py`,
`tests/test_pcm_bank_evidence.py`, `tools/cdj_dsp/{isa_probe,isa_inventory,refdocs,pcm_bank_evidence}.py`,
`tests/cstub/c674x-isa-probe.c`, `analysis/dsp/`. This is concurrent work from
other tracks. Nothing was modified, staged or committed by this track.

## Scope

Covered: the DSP-relevant **claims** in `README.md`, `HANDOFF.md`, `BUILD.md`,
`DSP_BOOT_MILESTONE_AUDIT.md`, `DSP_INTERRUPT_ENTRY.md`,
`PCM_EXECUTION_EVIDENCE.md`, `ITERATION_ANALYSIS.md`, `PERFORMANCE.md`,
`CLEAN_BOOT_EVIDENCE.md`, plus the DSP-identity passage in `RUNNING.md` and the
new `SH4_BLACKFIN_COVERAGE.md`. For each claim: is it still true, superseded,
contradicted by source, or unverifiable at HEAD. Also: reference-citation
accuracy (page/figure numbers against the indexed PDFs), doc-quoted test
counts against an actual suite run, existence of cited evidence artifacts, and
whether any doc commits one of the forbidden equivalences.

NOT covered (owned by other tracks): the ISA itself, pipeline/interrupt/loop
semantics, peripheral fidelity, PCM/audio correctness, artifact-integrity
(gate/manifest/hash) verification beyond "does the cited path exist", and the
non-DSP docs (`ETHERNET_LOCAL.md`, `NXS_*.md`, `INPUT_MANIFEST.md`,
`RUNNING.md` as a whole, `FIRMWARE.md`, `THIRD_PARTY.md`). I did not re-derive
the ISA inventory or probe; I consumed `analysis/dsp/isa_probe.json` as given.
I did not attempt a clean-tree regression run, because that would require
stashing or checking out, which this track is forbidden to do.

## Overall characterisation (read this before the findings)

**The documentation is, with few exceptions, unusually honest and well
qualified.** The dated evidence documents (`DSP_BOOT_MILESTONE_AUDIT.md`,
`DSP_INTERRUPT_ENTRY.md`, `PCM_EXECUTION_EVIDENCE.md`, `CLEAN_BOOT_EVIDENCE.md`,
`PERFORMANCE.md`, `ITERATION_ANALYSIS.md`) systematically state what their
evidence does *not* establish, name their approximations, retain failed
attempts, and refuse the exact equivalences this audit forbids. Examples:

- `HANDOFF.md:1215` — "A completed packet proves only that the current core
  accepted the observed encoding, not architectural correctness or that its
  predicate body executed."
- `HANDOFF.md:1460` — "A passing gate establishes equivalence, not correctness
  or boot."
- `DSP_BOOT_MILESTONE_AUDIT.md:86` — "These counts are execution evidence, not
  correctness percentages."
- `CLEAN_BOOT_EVIDENCE.md:73` — "Packet counts are not accuracy."
- `PERFORMANCE.md:30` — "This measures packet fetch only, not instruction
  execution, real firmware, boot time, or overall emulator speed. No end-to-end
  speedup is claimed."
- `ITERATION_ANALYSIS.md:469` — "Tests/build and smoke overlapped, so neither
  their duration nor GUI counters are performance measurements."
- `HANDOFF.md:1665` — the inventory "is a discovery report, not mnemonic
  disassembly, code/data separation, reachability, or proof of ISA support.
  … Broad counts include data and cannot be interpreted as instruction
  coverage percentages."

I found **no document that claims ISA coverage, instruction-family completeness,
cycle accuracy, full DSP parity, or working audio**. Every occurrence of those
phrases in the corpus is a negation or an eligibility gate. The single machine-
readable self-description, `tools/cdj_main/nxs_vm.py:546`, says "NXS UHPI plus
partial C674x interpreter; incomplete ISA, ROM handoff abstraction" — accurate.

The real documentation defects are of three kinds, in descending severity:

1. **One contradicted capability-shaped claim in the most-read file**
   (`README.md`) about the DSP's identity, which the repository's own
   `RUNNING.md` refutes.
2. **Stale present-tense LIMITATION lists** in `BUILD.md`'s journal sections
   that a reader will take as current and conclude features are missing that
   are in fact implemented.
3. **Structural evidence weakness**: the DSP evidence corpus is anchored almost
   entirely to `runs/` directories that are gitignored and, for a third of the
   cited ones, absent even locally.

## Coverage table

One row per requirement group. "Classification" uses the four requested values.

| ID | Claim group | Where | Classification | Settled by |
|---|---|---|---|---|
| DOC-README-DSP-IDENTITY | "The DSP (a Pioneer custom LSI with no public instruction set)" | `README.md:58` | **contradicted-by-source** (and by own docs) | `RUNNING.md:603-612` identifies the part as `D710E001BZDHA275`, a TI Aureus DA710 with a TMS320C67x+ core; `emulator/qemu/cdj_c674x.c` is a 3,000-line TI-documented C674x interpreter |
| DOC-README-NO-AUDIO | "**No audio.** … modelled from MAIN's side only: request words, buffer levels, position. There is no signal path" | `README.md:58-60` | **still-true for the legacy machine, superseded as a whole-project statement** | `emulator/qemu/cdj2000_dsp_model.c:11` confirms no audio path in the legacy model; but `PCM_EXECUTION_EVIDENCE.md` documents genuine DSP code executing an S16→float32 unpack on the NXS path |
| DOC-README-DSP-MODEL-WORKS | "The DSP model answers the load handshake, keeps the position report, locates, loops on the beat, and raises events on its interrupt line." | `README.md:45` | still-true (legacy behavioural model) | `emulator/qemu/cdj2000_dsp_model.c`; unchanged by the NXS C674x work, which uses `cdj2000_nxs_hpi.c` instead |
| DOC-BUILD-C674X-CORE | BUILD.md's "Partial C674x execution core" present-tense capability/limitation list | `BUILD.md:509-548` | **superseded** in detail, still-true in frame | See DOC-BUILD-CIRCULAR / DOC-BUILD-DEVICE rows; closing sentence "This is not a complete ISA, pipeline, privilege or interrupt model" (`BUILD.md:548`) remains true per `analysis/dsp/isa_probe.json` (113 rows all-probed-forms-rejected, 98 rows reporting "instruction not implemented") |
| DOC-BUILD-CIRCULAR | "Circular addressing, RAM arbitration for simultaneous overlapping accesses, and register-result collisions stop explicitly." | `BUILD.md:540-542` | **superseded** | `emulator/qemu/cdj_c674x.c:234-247,808-834,1630-1702,2452` implement AMR-driven circular arithmetic; `tests/test_c674x_circular.py` exists; only the AMR-use interlock and reserved modes still stop |
| DOC-BUILD-DEVICE-TXN | "The memory callback currently supports checked L2 RAM writes, not device transactions." | `BUILD.md:524-525` | **superseded** | `emulator/qemu/cdj2000_nxs_hpi.c` routes `dsp_read`/`dsp_write` to the `cdj_c6747_*` peripheral models; HEAD commit `13cef8f` is specifically "Reject non-device DSP writes before transactional state copies" |
| DOC-BUILD-SPLOOP-LIMITS | "SPMASK, reload/nested loops, SPLOOPD/W, protected/control instructions in the body, interrupt draining and restart remain unsupported and must not be counted as complete loop emulation." | `BUILD.md:638-640` | **superseded** (5 of 6 items) | SPMASK/SPMASKR decode+classify at `cdj_c674x.c:635-648,660-662,1062-1064,2579-2589`; SPLOOPD/SPLOOPW at `cdj_c674x.c:644-645,2619,2751-2828`; protected loads at `cdj_c674x.c:720,1079,2663`; interrupt draining at `cdj_c674x.c:42,871-927`. Only SPLOOPD **reload** still stops (`cdj_c674x.c:2826`) |
| DOC-BUILD-LOOPDRAIN-8 | "Returned SPMASK reversal, SPLOOPW return, full retained-buffer state, and interrupt-time loop drain still fail closed or remain unimplemented." | `BUILD.md:1894-1897` | **partly superseded** | Interrupt-time loop drain is implemented (`cdj_c674x.c:42,871-927`) and documented in `DSP_INTERRUPT_ENTRY.md`, which also covers "SPLOOP return behavior"; returned SPMASK still fails closed (`cdj_c674x.c:911,2589`) |
| DOC-BUILD-SPLOOPW-STOP | BUILD.md's replay walkthrough states execution "stops on unsupported instruction `0x4683e000` … `[B1] SPLOOPW 14`" and cites `runs/nxs-c674x-pinmux-bank` | `BUILD.md:554-560,655-690` | **superseded and unverifiable** | SPLOOPW is implemented at HEAD; and the cited input `runs/nxs-c674x-pinmux-bank/dsp-l2.bin` does not exist, so the documented replay entry-point command cannot be run |
| DOC-TESTCOUNTS | Quoted suite totals: 403/27 → 407/27 (`DSP_BOOT_MILESTONE_AUDIT.md:171,177`), 408/27 and 420/27 (`CLEAN_BOOT_EVIDENCE.md:83,25`), 427/29 (`DSP_INTERRUPT_ENTRY.md:30`, `HANDOFF.md:58`), 492/29 (`PCM_EXECUTION_EVIDENCE.md:71`), 497/31, 504/31, 506/31 (`ITERATION_ANALYSIS.md:22,354,467`) | **all unverifiable as completeness measures; the headline 506 is not reproducible from HEAD alone** | Measured this track: tracked-suite-only = **506 passed, 29 skipped**; whole dirty tree = **1 failed, 512 passed, 29 skipped**. The three untracked test files add 7 tests (`pcm_bank_evidence` 2, `nxs_usbh` 1, `dsp_isa_audit` 4). 535 tracked + 2 = the doc's 537 collected, so `ITERATION_ANALYSIS.md`'s 506 includes 2 tests from an **uncommitted** file |
| DOC-ISA-COVERAGE-CLAIM | Does anything claim ISA coverage or completeness? | repo-wide | **still-true: no such claim exists** | `grep -rniE "complete (isa|instruction set)\|full (isa\|instruction set)\|isa coverage\|instruction coverage\|all instructions"` over `tools/ emulator/ tests/ patches/` yields exactly one hit, `tools/cdj_main/nxs_vm.py:546`, which says "incomplete ISA". All doc-level hits are negations. The claim set therefore survives `isa_probe.json` |
| DOC-MNEMONIC-68 | "`runs/dsp-semantic-inventory-2.json` resolves all 5,396 confirmed addresses to 68 mnemonic names and 262 mnemonic/unit/width groups" | `HANDOFF.md:504-506` (also 559, 582, 627, 790; `BUILD.md:1916`) | **still-true and honest** | Every citation frames the figure as resolution of *observed addresses* by GNU libopcodes. Same paragraph: "does not infer semantic test coverage"; `:518` "these counts do not assert that the opcodes are unimplemented"; `:586` "Instruction-family completeness remains unproven". Both `dsp-semantic-inventory-2.json` and `-3.json` carry `validation_eligible: false`. No doc reads 68 as validated families |
| DOC-EQUIVALENCE | Does any doc commit a forbidden equivalence? | corpus-wide | **still-true: none found; the docs explicitly forbid them** | See the quotes in "Overall characterisation". The nearest positive claims are narrowly scoped: `HANDOFF.md:799` "This proves deterministic firmware-driven control and transport, not full DSP parity, full boot, nonzero audio, PCM correctness, or working host audio", and `HANDOFF.md:1257` "CFGCHIP1 now proves HPI is enabled in byte-address mode" — both bounded |
| DOC-REF-CITATIONS | SPRUFE8B page/figure citations in the DSP docs | `DSP_INTERRUPT_ENTRY.md:8`, `DSP_BOOT_MILESTONE_AUDIT.md:54`, `HANDOFF.md:609-612,578`, `BUILD.md:630` | **still-true (accurate), one off-by-one-page edge** | Verified against `build/references/sprufe8b.txt`: Figure 5-4 caption on printed page **641** (PDF 641) ✓ matches "Figure 5-4 (page 641)"; the 9-cycle interrupt overhead statement is on printed 648, consistent with "nine empty issue cycles". Figure H-7 printed 765; Table 3-29 printed 482; Figure G-3 printed 760 — all exist as cited. SADD printed 422-424 ✓, SSHL 493-494 ✓, SSUB printed **499-501** where `HANDOFF.md:609` says "pp499-500" (the instruction spans one more page) |
| DOC-EVIDENCE-ARTIFACTS | Every DSP evidence claim is anchored to a `runs/<dir>` artifact | `HANDOFF.md`, `BUILD.md`, `DSP_*`, `PCM_*`, `CLEAN_BOOT_*`, `ITERATION_*` | **structurally unverifiable** | `/runs/` is in `.gitignore:5` and `git ls-files runs/` returns **0**. The DSP docs cite **202 distinct `runs/` directories**; **66 of them (33%) do not exist in this working tree**, including `runs/nxs-c674x-pinmux-bank`, `runs/dsp-replay-pinmux`, `runs/dsp-replay-before-sploopw`, `runs/nxs-c674x-compact`, `runs/nxs-linked-two-channels`. `HANDOFF.md:1720` partially acknowledges this ("keep … useful ignored run directories privately, or regenerate them") |
| DOC-DSP-PART-IDENTITY | Which TI part is actually modelled | `RUNNING.md:603-612` (DA710 / C67x+ / ROM at L2 0x10000000) vs `BUILD.md:793,841,960`, `HANDOFF.md` (C6747 datasheet, SPRUH91D, SPRUFK5A, global L2 `0x11800000`) | **reconcilable but nowhere reconciled** | The two load addresses differ consistently with two different parts: DA710's L2 at `0x10000000` (legacy CDJ-2000 DSP behind the `0xAC0C0000` window) and C6747's global L2 at `0x11800000` (NXS, per `BUILD.md:487-489`). No document states this; "DA710"/"Aureus"/"C67x+" appear **only** in `RUNNING.md` and nowhere in the emulator source |
| DOC-INTERRUPT-ENTRY | `DSP_INTERRUPT_ENTRY.md` claims (nine-cycle interval, unchanged IRP/priority/IFR semantics, retained approximations, 427/29 suite, QEMU SHA, divergence of the old transcript) | whole file | **still-true and exemplary**; only the quoted suite total has drifted | Manual citation verified (row above); the file explicitly labels the atomic control-register transition an abstraction, says the exploratory breadth mode keeps its prior approximation, and states outright that the diverging old transcript "is expected semantic divergence, not a passed connected equivalence gate" |
| DOC-PCM-EVIDENCE | `PCM_EXECUTION_EVIDENCE.md` claims | whole file | **still-true as written; the suite total has drifted; claim substance is Track-PCM's to verify** | The document leads with "This is a **replay of previously captured genuine stock execution**, not a fresh successful playback run", enumerates nine unproven items (bank 1, modes, lifetime, downstream, McASP, continuous playback, true stereo separation, headroom), and flags its own checker's synthetic regression as not genuine evidence. The queued ownership section explicitly labels its offsets "hypotheses … not already observed runtime evidence" |
| DOC-BOOT-MILESTONE | `DSP_BOOT_MILESTONE_AUDIT.md` claims | whole file | **still-true; source-hash table and suite totals are stale by construction** | Its provenance table pins `cdj_c674x.c` to SHA `6131477b…` at revision `bd042d92`; that file has changed many times since (HEAD is `2eb2476`), so the table documents a past audit, not HEAD. The doc says so implicitly ("Audit date: 2026-09-09") but never says the hashes are historical. Conclusions are correctly bounded to the E-7010 goal |
| DOC-CLEANBOOT | `CLEAN_BOOT_EVIDENCE.md` claims | whole file | **still-true; two typographic defects; suite totals stale** | Unusually careful: it *retracts* its own headline ("Therefore those runs establish successful instances, not robust startup with media"), names the unguarded FSB-during-active-RX edge and forbids a general IIC-receive claim. Defects: missing spaces — "completes120s" (`:16`), "Full suite420passed/27optional skips" (`:25`), "section16.4.8" (`:11`) |
| DOC-PERFORMANCE | `PERFORMANCE.md` claims (RAM-first dispatch, packet-local fetch header, 3,024-vs-6,592-byte copy, loop-origin filtering, benchmark numbers) | whole file | **still-true** | Claims match `offsetof(CdjC674x, loop)` copy semantics and the loop scheduler's modulo-II origin filtering. Every measurement is scoped ("This measures packet fetch only…", "They do not establish a whole-emulator or firmware speedup"). "Persistent instruction predecoding remains unimplemented" is consistent with the fetch-header cache being discarded on return |
| DOC-ITERATION | `ITERATION_ANALYSIS.md` claims | whole file | **still-true, except the 506/31 total (see DOC-TESTCOUNTS)** | Exemplary on measurement discipline: refuses to compound speedups ("cross-run differences must not be interpreted as a reliable regression or compounded improvement"), reports cold and warm trials separately, reports overlapping ranges as overlapping, and prefers the adjacent comparison over the flattering combined one |
| DOC-HANDOFF-CURRENT | `HANDOFF.md` "Current checkpoint" (lines 9-60) | `HANDOFF.md:9-60` | **still-true at the time of writing; now one commit behind** | It summarises the PCM replay, DHCP milestone, Ethernet and interrupt-entry work accurately and is internally consistent with the dated docs. It does not mention the optimization iterations 01-09 (`ITERATION_ANALYSIS.md`) or `SH4_BLACKFIN_COVERAGE.md`, so the "current" pointer is behind HEAD |
| DOC-HANDOFF-JOURNAL | `HANDOFF.md`/`BUILD.md` as append-only journals containing superseded present-tense statements | both files, ~1,700 and ~2,045 lines | **structural risk, not individually dishonest** | Both are newest-first logs with no per-entry "superseded" marker and no date on most entries. A grep-driven reader (which is how the files are meant to be read — they are 108K each) cannot tell a 2026-09-02 limitation from a 2026-09-10 one. This is the mechanism behind every superseded row above |
| DOC-SH4BF-COVERAGE | `SH4_BLACKFIN_COVERAGE.md` (added in HEAD commit `2eb2476`) | whole file | **still-true; explicitly out of this track's subject** | States up front it does not claim SH-4/Blackfin ISA coverage and that those cores come from QEMU and GNU sim. Names its own weakest link (`tests/test_blackfin_parallel.py` skipped because `bfin-elf-as` is absent) and labels recorded skips as "not passing coverage". Says it audits "the current tree at `3da5ff2`" while being committed as `2eb2476` |

## Findings

**F1 (most dangerous — contradicted capability/identity claim in the front
door).** `README.md:58` states "The DSP (a Pioneer custom LSI with no public
instruction set)". The repository's own `RUNNING.md:603-604` says the opposite:
"The DSP itself is not a Pioneer custom part: `D710E001BZDHA275` is a TI Aureus
DA710 with a TMS320C67x+ core", and goes on to say the downloaded program
"disassembles cleanly as little-endian C67x+". Independently, HEAD contains a
3,000-line TI-manual-derived C674x interpreter (`emulator/qemu/cdj_c674x.c`)
and thirteen `cdj_c6747_*` peripheral models. A reader who reads only README
concludes the DSP is unknowable; the whole Track-1..5 body of work exists
because it is not. This is the one claim in the corpus that is actively wrong
rather than merely old.

**F2 (stale limitation, highest-traffic kind).** `BUILD.md:638-640` lists
"SPMASK, reload/nested loops, SPLOOPD/W, protected/control instructions in the
body, interrupt draining and restart" as unsupported. Five of those six are
implemented at HEAD (evidence in the DOC-BUILD-SPLOOP-LIMITS row). Only SPLOOPD
reload still stops. A reader planning work would re-implement SPMASK.

**F3 (stale limitation).** `BUILD.md:540-542` says circular addressing "stop[s]
explicitly" and `BUILD.md:524-525` says the memory callback does "not [support]
device transactions". Both are implemented (`cdj_c674x.c` circular arithmetic;
`cdj2000_nxs_hpi.c` peripheral dispatch, and HEAD commit `13cef8f` is about
device-write rejection ordering). Same paragraph also still says "Unconditional
SPLOOP execution is supported below; other loop forms and control-register reads
remain incomplete", superseded by the SPLOOPD/SPLOOPW work.

**F4 (stale, and it breaks a documented command).** `BUILD.md:652-690` presents
`tools.cdj_dsp.replay runs/nxs-c674x-pinmux-bank/dsp-l2.bin …` as "the
repeatable diagnostic entry point". That input directory does not exist, so the
documented entry point cannot be run. The surrounding narrative ("stops on
unsupported instruction `0x4683e000` … `SPLOOPW 14`", "stopping at compact
`SPMASK S1`") describes behaviour two implementation generations old.

**F5 (test-count drift, and the headline number is not reproducible from
HEAD).** The corpus quotes 403/27, 407/27, 408/27, 420/27, 427/29, 430/29,
436/30, 465/29, 489/29, 492/29, 497/31, 504/31 and 506/31 across eight
documents. Measured here: the **tracked** suite gives 506 passed / 29 skipped
(TI tools + `CDJ_ETH_QEMU_TEST=1`); the dirty tree gives 1 failed / 512 passed /
29 skipped. Reconciling collection counts (535 tracked + 2 = the doc run's 537),
`ITERATION_ANALYSIS.md:467`'s "506 passed, 31 skipped" **includes 2 tests from
the untracked, uncommitted `tests/test_pcm_bank_evidence.py`** — a number a
fresh clone cannot reproduce. None of these figures is an architectural
completeness measure and no document claims they are; the risk is that the
rising sequence reads as rising coverage.

**F6 (structural evidence weakness).** The DSP evidence corpus cites **202
distinct `runs/` directories**; `/runs/` is gitignored with zero tracked files,
and **66 of the 202 are absent even in this working tree**. Every strict gate,
transcript SHA-256 and connected-run claim in `HANDOFF.md`, `BUILD.md`,
`DSP_BOOT_MILESTONE_AUDIT.md`, `DSP_INTERRUPT_ENTRY.md`,
`PCM_EXECUTION_EVIDENCE.md` and `CLEAN_BOOT_EVIDENCE.md` therefore rests on
artifacts a reviewer cannot obtain. The quoted hashes are the only residue and
are self-referential without the files. `ITERATION_ANALYSIS.md:54` identifies
the cause (52 GB of runs) and proposes retention/export tooling; that tooling
does not exist.

**F7 (unreconciled device identity).** `RUNNING.md` is the only place in the
repository that names the DSP part, and it names a **DA710 / C67x+ with L2 at
`0x10000000`**, while the NXS model is built against **C6747 / C674x with global
L2 at `0x11800000`** (SPRUFE8B, SPRUH91D, SPRUFK5A, `cdj_c6747_*`). The two are
consistent only under the reading that the legacy CDJ-2000 and the NXS carry
different DSPs — which is plausible and is what the differing L2 base addresses
suggest, but which **no document states**. Until it is stated, a reader cannot
tell whether the C6747 peripheral set is the right device model or an
unvalidated substitution.

**F8 (provenance tables pinned to dead hashes).** `DSP_BOOT_MILESTONE_AUDIT.md`
pins `emulator/qemu/cdj_c674x.c` to SHA-256 `6131477b…` at revision
`bd042d92`. The file has changed repeatedly since. The table is valid as a
historical audit record but is not labelled as historical, so it reads as a
current integrity statement.

**F9 (no ISA-coverage overclaim exists — a positive finding).** Requirement 3 of
this track looked for a place claiming ISA coverage or completeness that
`isa_probe.json` would refute. There is none. The only machine-readable
self-description says "incomplete ISA". The "68 mnemonics / 5,396 addresses"
figure is cited five times and is honestly framed every time as GNU-libopcodes
resolution of *observed* addresses, accompanied in the same paragraphs by
"does not infer semantic test coverage", "these counts do not assert that the
opcodes are unimplemented", and "Instruction-family completeness remains
unproven". Both inventory artifacts carry `validation_eligible: false`. The
claim set survives `isa_probe.json` (109 of 238 manual rows
all-probed-forms-accepted, 98 rows reporting "instruction not implemented")
without amendment.

**F10 (journal format is the root cause).** `HANDOFF.md` (1,743 lines) and
`BUILD.md` (2,045 lines) are append-only, newest-first logs whose entries use
present tense ("currently supports", "remain unsupported") and mostly carry no
date. The docs instruct readers to grep them. Grep returns the stale entry and
the current entry with equal authority. F2, F3, F4 and F8 are all instances of
this one mechanism; fixing them individually without addressing the format will
reproduce them.

## Stale-documentation findings (concrete edit list for the coordinator)

Ordered by severity. **I did not make any of these edits.**

1. `README.md:58` — claim: "The DSP (a Pioneer custom LSI with no public
   instruction set)". Why stale: contradicted by `RUNNING.md:603-604` and by
   the existence of `emulator/qemu/cdj_c674x.c`. Current truth: the part is a
   TI DSP with a published instruction set (`D710E001BZDHA275` / Aureus DA710,
   C67x+ core per `RUNNING.md`; the NXS path is modelled as C6747/C674x against
   SPRUFE8B). Suggested: drop "with no public instruction set", and add one
   sentence that the NXS research branch runs a partial C674x interpreter whose
   coverage is incomplete, pointing at `BUILD.md` and `HANDOFF.md`.
2. `README.md:58-60` — claim: "**No audio.** … modelled from MAIN's side only
   … There is no signal path". Why stale: true of the legacy machine only.
   Current truth: on the NXS path genuine DSP code has been observed performing
   an S16→float32 PCM unpack (`PCM_EXECUTION_EVIDENCE.md`); there is still no
   end-to-end audible output, no McASP output validation and no continuous
   playback. Suggested: scope the bullet to the legacy CDJ-2000 model and state
   the NXS position in its own clause, keeping "PLAY changes nothing audible".
3. `BUILD.md:638-640` — claim: "SPMASK, reload/nested loops, SPLOOPD/W,
   protected/control instructions in the body, interrupt draining and restart
   remain unsupported". Why stale: five of six implemented. Current truth: only
   SPLOOPD reload stops (`cdj_c674x.c:2826`); returned SPMASK reversal and
   SPLOOPW return still fail closed (`cdj_c674x.c:911,2589`). Suggested: mark
   the section as a dated batch record and add a one-line forward pointer.
4. `BUILD.md:540-542` — claim: "Circular addressing … stop[s] explicitly".
   Current truth: implemented; only the AMR-use interlock and reserved modes
   stop (`cdj_c674x.c:1630-1702`).
5. `BUILD.md:524-525` — claim: "The memory callback currently supports checked
   L2 RAM writes, not device transactions." Current truth: device transactions
   are dispatched to the `cdj_c6747_*` models via `cdj2000_nxs_hpi.c`.
6. `BUILD.md:535-536` — claim: "Unconditional SPLOOP execution is supported
   below; other loop forms and control-register reads remain incomplete."
   Current truth: SPLOOPD and SPLOOPW are supported; conditional SPLOOP forms
   and reload are not.
7. `BUILD.md:652-660` — claim: the `runs/nxs-c674x-pinmux-bank/dsp-l2.bin`
   replay command is "the repeatable diagnostic entry point". Why stale: the
   cited input does not exist and the described stop no longer occurs.
   Current truth: needs a currently-present checkpoint and the
   `--trace-mode compact` guidance from `ITERATION_ANALYSIS.md:347`.
8. `BUILD.md:1894-1897` — claim: "interrupt-time loop drain still fail[s]
   closed or remain[s] unimplemented". Current truth: implemented per
   `DSP_INTERRUPT_ENTRY.md` and `cdj_c674x.c:42,871-927`; returned SPMASK
   reversal and SPLOOPW return are the parts that still fail closed.
9. `ITERATION_ANALYSIS.md:467` — claim: "**506 passed, 31 skipped in 67.76 s**".
   Why stale/misleading: 2 of those tests come from the untracked
   `tests/test_pcm_bank_evidence.py`, which is not in HEAD. Current truth: the
   committed suite gives 506 passed / 29 skipped with `C6X_TI_BIN` and
   `CDJ_ETH_QEMU_TEST=1` set, and the split moves with those gates. Suggested:
   state the env gates and note the untracked contribution, as the same
   document already does for the baseline archives.
10. `DSP_BOOT_MILESTONE_AUDIT.md:12-30` — claim: the source SHA-256 table.
    Why stale: pinned to `bd042d92`; `cdj_c674x.c` no longer hashes to
    `6131477b…`. Current truth: historical record. Suggested: label the table
    "as measured at `bd042d92`".
11. `DSP_BOOT_MILESTONE_AUDIT.md:171,177`, `DSP_INTERRUPT_ENTRY.md:30`,
    `CLEAN_BOOT_EVIDENCE.md:25,83`, `PCM_EXECUTION_EVIDENCE.md:71` — claim:
    various suite totals. Current truth: superseded by 506/29 (tracked). These
    are correct as dated records; the fix is one sentence in each, or once in
    `HANDOFF.md`, saying the number is a regression snapshot at that date and
    not a coverage measure.
12. `HANDOFF.md:609` — claim: "SSUB pp499-500". Current truth: the SSUB pages
    in SPRUFE8B run 499-501 (printed = PDF throughout, verified). Trivial.
13. `CLEAN_BOOT_EVIDENCE.md:11,16,25` — typographic: "section16.4.8",
    "completes120s", "Full suite420passed/27optional skips". Not claims, but
    they make the quoted numbers hard to grep.
14. `RUNNING.md:603-612` versus `BUILD.md`/`HANDOFF.md` — add one sentence
    reconciling DA710/C67x+ (legacy, L2 `0x10000000`) with C6747/C674x (NXS,
    global L2 `0x11800000`), or say explicitly that the reconciliation is open.
15. `HANDOFF.md` and `BUILD.md` structure — add a dated heading to each batch
    section and a standing note at the top that entries below the current
    checkpoint are historical and may describe superseded limitations. This is
    the single highest-leverage edit: it retires F2/F3/F4/F8 as a class.

## Open questions I could not resolve

1. Whether the legacy CDJ-2000 DSP and the NXS DSP are genuinely different TI
   parts (DA710 vs C6747). The differing L2 base addresses strongly suggest yes,
   but no document or source comment states it, and I had no datasheet for
   `D710E001BZDHA275` in `build/references/`.
2. Whether `ITERATION_ANALYSIS.md`'s 506/31 run had `C6X_TI_BIN` set. The
   document does not say, and `analysis/iterations/09-tests.txt` is `-q` output
   with no test names, so the pass/skip split cannot be attributed per test.
   My reconciliation of 535+2=537 is arithmetic, not a per-test match.
3. Whether the 66 missing `runs/` directories were deleted, renamed, or never
   existed on this machine. `ITERATION_ANALYSIS.md:54` says runs are never
   deleted automatically, which makes deletion a manual act nobody recorded.
4. Whether `README.md`'s "the audio DSP … report[s] up, so no caution banner
   stands in the way" (`:37`) is still true for the legacy machine — that is a
   legacy-boot question outside this track.
5. Whether the one failing test in the dirty tree
   (`tests/test_nxs_usbh.py::test_usbh_remote_wakeup_interrupt`) fails at clean
   HEAD. It is an untracked test exercising an uncommitted
   `cdj2000_usbh.c` change against a prebuilt `build/qemu/build/qemu-system-sh4`
   that predates that change, so the most likely cause is a stale binary, not a
   HEAD defect. I could not confirm without stashing, which this track may not do.

## Commands run

| Command | Result |
|---|---|
| `git log --oneline -8`; `git branch --show-current` | HEAD `2eb2476`, one commit past the task's `3da5ff2`; branch `codex/macos-nxs` |
| `git status --short` | `emulator/qemu/cdj2000_usbh.c` modified; 8 untracked paths from concurrent tracks |
| `wc -l` on the nine DSP docs | 5,007 lines total; HANDOFF 1,743, BUILD 2,045 |
| `cat` of `DSP_INTERRUPT_ENTRY.md`, `PERFORMANCE.md`, `CLEAN_BOOT_EVIDENCE.md`, `PCM_EXECUTION_EVIDENCE.md`, `DSP_BOOT_MILESTONE_AUDIT.md`, `README.md`, `SH4_BLACKFIN_COVERAGE.md` | read in full |
| targeted `sed -n` reads of `HANDOFF.md` and `BUILD.md` (lines 1-60, 189, 495-700, 780-805, 1205-1220, 1455-1470, 1660-1675; BUILD 1-135, 244-135 headings, 390-700, 1875-1940) | as cited above |
| `grep -n "68 mnemonic\|5396\|semantic-inventory"` across docs | 5 citations, all honestly framed |
| `grep -rniE "complete (isa\|instruction set)\|isa coverage\|instruction coverage\|all instructions" tools/ emulator/ tests/ patches/` | 1 hit, `tools/cdj_main/nxs_vm.py:546` ("incomplete ISA") |
| `grep -rn "architectural validation\|validation_eligible"` across docs | 10 hits, all negations or eligibility gates |
| `python3` over `build/references/sprufe8b.txt` resolving `@@ PDFPAGE` markers for Figure 5-4, Figure H-7, Table 3-29, Figure G-3, SADD/SSHL/SSUB | printed 641, 765, 482, 760, 422-424, 493-494, 499-501; printed == PDF page throughout |
| `python3 -c` on `runs/dsp-semantic-inventory-{2,3}.json` | both `validation_eligible: False`, 262 groups |
| `python3 -c` on `analysis/dsp/isa_probe.json` | 238 rows, 109 accepted, 11 partial, 113 rejected, 5 out-of-scope, 98 not-implemented |
| path-existence sweep over 172 backticked repo paths in the nine docs | 6 missing, all `runs/` |
| `runs/` citation sweep over the DSP docs | 202 distinct directories cited; 66 missing locally; `git ls-files runs/` = 0 |
| `grep` of `emulator/qemu/cdj_c674x.c`, `cdj_c674x_loop.c`, `cdj2000_nxs_hpi.c`, `cdj2000_dsp*.c` for circular/AMR/SPMASK/SPLOOP/drain/device dispatch | as cited in the coverage table |
| `DEVELOPER_DIR=… C6X_TI_BIN=… CDJ_ETH_QEMU_TEST=1 .venv/bin/python -m pytest -q` | **1 failed, 512 passed, 29 skipped in 54.70 s**; failure is untracked `tests/test_nxs_usbh.py::test_usbh_remote_wakeup_interrupt` |
| same, with `--ignore` of the three untracked test files | **506 passed, 29 skipped in 50.08 s** |
| `pytest -q --collect-only` on the three untracked test files | 7 tests (2 + 1 + 4) |

## Cross-track notes

- The dirty-tree failure `tests/test_nxs_usbh.py::test_usbh_remote_wakeup_interrupt`
  belongs to whichever track owns the uncommitted `cdj2000_usbh.c` remote-wakeup
  change; it asserts against a prebuilt QEMU binary that predates the change.
- `PCM_EXECUTION_EVIDENCE.md`'s substantive claims (float32 vs Q31, the
  `c003c398` kernel table, the `0xdc8` plane separation) are for the PCM track
  to verify; I assessed only their framing.
- `BUILD.md:1890-1896`'s "A two-cycle delay avoids the conflict but is not
  supported by the ISA documentation. Keep it functional-only" is a live
  fidelity approximation for the pipeline/loop track, not a doc defect.
