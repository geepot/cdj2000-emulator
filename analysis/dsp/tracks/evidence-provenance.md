# Track 5 — Evidence provenance and artifact integrity

Audit date 2026-09-10. Repository `/Users/gpotvin/Git/cdj2000-emulator`, branch
`codex/macos-nxs`.

**HEAD correction:** the task specified HEAD `3da5ff2`. Actual HEAD at audit time
is `2eb2476` ("Document SH4 and Blackfin coverage boundaries"), one commit later.
The working tree also carries uncommitted changes I did not make and did not
touch: modified `emulator/qemu/cdj2000_usbh.c`, and untracked
`tests/test_nxs_usbh.py`, `tests/test_dsp_isa_audit.py`,
`tests/test_pcm_bank_evidence.py`, `tests/cstub/c674x-isa-probe.c`,
`tools/cdj_dsp/{isa_probe,isa_inventory,refdocs,pcm_bank_evidence}.py`.
All measurements below are against that tree. I modified nothing.

## Scope

Covered: integrity and actual meaning of the recorded evidence artifacts —
gate/manifest/coverage JSON semantics, hash chains, mode provenance, the
counting semantics of `coverage.py`/`replay.py`, the exact scope of the PCM
result, circular-validation mechanisms, and a classification of the current
regression suite.

Not covered (other tracks own these): ISA completeness, pipeline/timing
correctness, interrupt semantics, peripheral fidelity, the `isa_probe` /
`isa_manual_inventory` denominators themselves. I verified a handful of manual
page citations only as a citation-integrity spot check, not as an ISA audit.
I did not attempt to re-run any recorded replay (their input checkpoints are
absent — see EP-CHAIN-EXTERNAL), so I verify recorded artifacts, not
reproducibility.

## Coverage table

| ID | Requirement | Impl | Validation | Fidelity | Confidence |
| --- | --- | --- | --- | --- | --- |
| EP-ART-SEMINV | `runs/dsp-semantic-inventory-3.json` asserts what it claims | supported | reference-backed-tests | strict | high |
| EP-ART-CIRC | `runs/dsp-circular-transcript-replay-1/{gate,manifest,coverage}` | supported | reference-backed-tests | approximation | high |
| EP-ART-PCM4 | `runs/nxs-pcm-observe-4/{gate,manifest,coverage}` | supported | reference-backed-tests | strict | high |
| EP-ART-FINAL4 | `build/performance/09-combined/final-4/{gate,manifest,coverage}` | supported | reference-backed-tests | approximation | high |
| EP-ART-DOCCITED | Gates cited by `DSP_BOOT_MILESTONE_AUDIT.md` / `DSP_INTERRUPT_ENTRY.md` | partial | firmware-observation-only | unknown | high |
| EP-HASH-AUDIT | Documented source hashes vs current files | partial | reference-backed-tests | strict | high |
| EP-SEM-COUNTS | `confirmed_instructions` / `source_fetches` / `source_predicate_outcomes` semantics | supported | reference-backed-tests | strict | high |
| EP-SEM-FETCHSUM | `packet_fetch_observations` / `source_fetch_observations` arithmetic | partial | reference-backed-tests | approximation | high |
| EP-SEM-REPEAT | What `--verify-repeat` compares | supported | reference-backed-tests | strict | high |
| EP-SEM-MODE | What strict vs exploratory mode switches change | supported | reference-backed-tests | approximation | high |
| EP-PCM-SCOPE | Exact scope of the PCM S16→float32 result | supported | reference-backed-tests | strict | high |
| EP-CIRC-REPLAY | Deterministic replay as the gate mechanism | unsupported | untested | approximation | high |
| EP-CIRC-LOOPORACLE | Loop-scheduler "frozen oracle" regression | partial | untested | approximation | high |
| EP-CHAIN-EXTERNAL | Input-checkpoint provenance chain | unsupported | untested | unknown | high |
| EP-CHAIN-REPLAYPY | `replay.py` (the gate author) is not hash-pinned anywhere | unsupported | untested | unknown | high |
| EP-GATE-SHALLOW | `gate.json` omits all input provenance and assumptions | partial | untested | approximation | high |
| EP-GATE-ABORTED | Pre-execution manifests survive aborted runs claiming eligibility | unsupported | untested | unknown | high |
| EP-TEST-CLASSIFY | Regression-suite classification and current numbers | supported | reference-backed-tests | strict | medium |
| EP-TEST-NOARTIFACT | No test re-verifies any recorded `runs/` artifact | unsupported | untested | unknown | high |
| EP-DOC-STALE | Documented counts/hashes vs current tree | partial | reference-backed-tests | strict | high |

(Full per-row detail, including `exclusions_and_approximations`,
`next_acceptance_test` and the per-row evidence, is carried in the structured
object returned to the coordinator; this table is the readable index.)

## 1. What each artifact actually asserts

All artifact hashes I could check are internally self-consistent. Every
`coverage.json`, `repeat-coverage.json`, `final.cdjdsp`, `repeat-final.cdjdsp`,
`trace.jsonl`, `repeat.jsonl`, `dsp-tx.jsonl` and `repeat-dsp-tx.jsonl` hashes
to exactly the value its own `gate.json` records, in all four headline runs. No
missing file, no hash mismatch, no absent gate among the four.

### `runs/dsp-semantic-inventory-3.json`

`validation_eligible: false` and `scope: "GNU disassembly of confirmed source
addresses only; no reachability expansion"`. Five caveats, the first being
"Mnemonic/operand decoding is not emulator semantic validation."

`validation_eligible=False` is **hard-coded**, not computed —
`tools/cdj_dsp/semantic_inventory.py:96` returns
`dict(schema=1, validation_eligible=False, ...)`. The artifact can therefore
never be promoted by its own producer, which is correct design.

Provenance chain verified: its `sha256.checkpoint`
(`6097dd30…ba217bec`) equals `runs/dsp-circular-transcript-replay-1`'s
`gate.final_checkpoint.checkpoint_sha256`, and its `sha256.coverage`
(`bff725aa…9664a668`) equals that run's `gate.coverage_sha256`. Its
`sha256.analyzer` equals the current `tools/cdj_dsp/semantic_inventory.py`.
So the inventory is exactly the disassembly of that run's coverage.

Two integrity gaps: the disassembler binary it pins
(`sha256.disassembler = 7845ca22…`) is **not present in this checkout**, and the
test that would re-run it is skipped (`tests/test_dsp_semantic_inventory.py:95:
set C6X_DISASSEMBLER to the built tic6x frontend`). The 68 mnemonics / 262
groups cannot be regenerated or re-verified here. The mnemonics also come from
**GNU binutils**, not from TI — independent of our emulator, but not a TI oracle,
and decode-only.

### `runs/dsp-circular-transcript-replay-1`

`dsp_timing_mode: functional-runahead`, `dsp_audio_mode: coarse-packet-slots`,
`architectural_validation_eligible: false`, `inherited_exploratory_state: true`,
**17 approximations** (identical list in gate and manifest), checkpoint schema 9.
This is the *exploratory* run. `passed: true`, `verified_connected_stops: 71`,
`repeat_matches`/`final_state_and_memory_match`/`repeat_coverage_matches`/
`dsp_tx_capture_matches` all true. `trace.jsonl` and `repeat.jsonl` are
byte-identical (`cmp` confirms). Stop reason "phase budget exhausted" at the
100,000,000-step ceiling — so the transcript suffix beyond that was not replayed.
It is the source of the 4,469 / 5,396 / 4,585 / 33 / 4,843 figures.

The TX capture is explicitly labelled `sample_encoding: "unsigned 32-bit
serializer word; not PCM interpretation"` and `timing: "functional coarse packet
slots; not serializer-clock or audio timing"`, `synthesized_samples: false`.

### `runs/nxs-pcm-observe-4`

The only headline run claiming `architectural_validation_eligible: true` with
`approximations: []`. Mode: `dsp_timing_mode: strict`,
`dsp_audio_mode: stopped-clock`, `dsp_scheduler_mode: legacy`,
`inherited_exploratory_state: false`, `observe_pcm: true`,
`connected_stops: 16`, schema 11. `passed: true`,
`verified_connected_stops: 16`, all four repeat comparisons true.

**The empty `approximations` array is not a claim that nothing is approximated.**
It is a mode-conditional list (`tools/cdj_dsp/replay.py:~640`): entries are
appended only for `--functional-dsp-timing`, `--functional-dsp-audio`,
`deferred-v1` scheduling, or inherited exploratory state. The same manifest
simultaneously carries eleven `pll_assumptions` — including "catalog PLL
reset/lock bounds applied to custom DSP; not measured lock", "EMIFB register
readback and 32 MiB storage modeled; SDRAM command timing and arbitration
omitted", "divider GO completes after eight subsequent DSP cycles; not physical
clock timing" — and `boot_rom_executed: false`. None of those reach `gate.json`.
Also note `dsp_audio_mode: stopped-clock`: the McASP clock is stopped in this
run, and the gate has no `dsp_tx_capture` section at all. This run is not
evidence about audio output.

### `build/performance/09-combined/final-4`

`trace_mode: compact`, `dsp_timing_mode: functional-runahead`,
`dsp_audio_mode: coarse-packet-slots`,
`architectural_validation_eligible: false`, `inherited_exploratory_state: true`,
7 approximations, 300,000 steps, stop reason `step_limit`. `passed: true` with
all repeat comparisons true. It is the only one of the four with a complete local
provenance chain: `input_gate_sha256: 2dee8d46…` and
`input_manifest_sha256: 5511556c…` both verify against
`runs/dsp-post-interrupt-pipedown-5m-replay-2/{gate,manifest}.json`, and
`dump_sha256: de477c08…` verifies against that run's `final.cdjdsp`. Its
`sources["emulator/qemu/cdj_c674x.c"] = 34e6a1c1…` equals the **current** file,
so it alone describes today's interpreter. Its recorded `coverage.py` and
`inventory.py` hashes also match the current files.

### Gates cited by the committed docs

| Run | gate.json | Notes |
| --- | --- | --- |
| `runs/nxs-sic-mask-strict-120s-1` | **absent** | `nxs_vm` run: `run.json` + `result.json` only |
| `runs/nxs-sic-mask-strict-120s-2` | **absent** | same |
| `runs/nxs-dsp-entry-fixed-usb-1` | **absent** | same |
| `runs/dsp-sic-mask-120s-tail-1` | present, `passed: true` | strict/stopped-clock/legacy, `approximations: []`, `verified_connected_stops: 9`, 0 faults; doc's trace and final-checkpoint hashes both verify exactly |
| `runs/dsp-interrupt-entry-fixed-events-3` | **absent** | only `manifest.json` + `trace.jsonl` |
| `runs/dsp-interrupt-entry-fixed-events-4` | **absent** | same |

The three `nxs_vm` runs are connected-boot observation runs, not replay gates;
they legitimately have no gate. The `-3`/`-4` interrupt-entry runs are *aborted*
replays — manifest has no `complete`, no `stop`, no `coverage`, and there is no
`final.cdjdsp`. `DSP_INTERRUPT_ENTRY.md:46-51` is explicit and correct about
this ("not a passed connected equivalence gate"), and both documented trace
hashes verify exactly (`a8564c58…`). The integrity problem is not the doc; see
EP-GATE-ABORTED below.

Spot check of the doc's manual citation: `DSP_INTERRUPT_ENTRY.md:8` cites
SPRUFE8B Figure 5-4 at page 641. Verified — `@@ PDFPAGE 641 PRINTED 641` carries
"Figure 5-4. Nonreset Interrupt Detection and Processing: Pipeline Operation",
and page 640 carries the "In clock cycle 4 of Figure 5-4" text. Printed page
equals PDF page, as stated.

## 2. Source-hash verification against `DSP_BOOT_MILESTONE_AUDIT.md`

| File | Documented | Current | |
| --- | --- | --- | --- |
| `emulator/qemu/cdj_c674x.c` | `6131477b…ef800e` | `34e6a1c1…42232f` | **STALE** |
| `emulator/qemu/cdj_c674x_loop.c` | `a7df446a…07dec2` | `3e96810d…634c00` | **STALE** |
| `emulator/qemu/cdj_c6747_spi.c` | `9de8f13a…5d334b` | `9de8f13a…5d334b` | match |
| `tools/cdj_main/nxs_vm.py` | `e66c600b…b73533` | `bfcc704a…2b5996` | **STALE** |
| `tools/cdj_dsp/boot_handshake.py` | `f3cdf30b…a42739` | `f3cdf30b…a42739` | match |
| `patches/07-gdb-17.2-sic-mask-order.patch` | `678fcfd0…e2958b` | `678fcfd0…e2958b` | match |

Three of six no longer match. The audit is **not wrong about its own run**: its
documented `cdj_c674x.c` hash `6131477b…` is exactly the hash recorded in
`runs/dsp-sic-mask-120s-tail-1/manifest.json`'s `sources`. The audit accurately
describes the code it measured; that code is now three revisions behind the
interpreter and the loop buffer. Every E-7010 boot-milestone and tail-replay
conclusion in that document is about that older interpreter, not about HEAD.

Four distinct `cdj_c674x.c` revisions are in play across the evidence chain:
`6131477b` (boot-milestone audit + tail replay), `1d02d672`
(dsp-circular-transcript-replay-1), `cd3e5d06` (nxs-pcm-observe-4), `34e6a1c1`
(final-4 = current). The two headline DSP-coverage artifacts were both produced
by interpreters that no longer exist in the tree.

The two reference PDFs verify clean: `.venv/bin/python -m tools.cdj_dsp.refdocs
--check` exits 0 (sprufe8b 771 PDF pages / 739 footers; spruh91d 1473 / 1435).

## 3. What the coverage counters actually count

All line numbers are `tools/cdj_dsp/coverage.py` at its current hash
`57ff339f…`, which is the hash `final-4`'s manifest records.

**`confirmed_instructions` / `confirmed_instruction_addresses`** is
`len(rows)` — one row per *instruction address inside a source packet that was
fetched and whose replay step completed*:

```python
confirmed_sources = {
    pc for pc, event in pc_events.items()
    if event.get('direct_fetches', 0) + event.get('loop_fetches', 0) > 0
}
```
(lines 273-276), and each row is tagged
`classification='confirmed_executable'`,
`emulator_status='completed_without_unsupported_fault'` (lines 310-311). The
artifact's own caveat states the limit exactly:

> "A completed packet proves that this emulator accepted that observed encoding;
> it does not prove architectural correctness or that a predicate body was true."
> (line 410)

**`source_fetches`** is a *per-packet* count copied onto every instruction in
that packet:

```python
fetches = pc_event.get('direct_fetches', 0) + pc_event.get('loop_fetches', 0)
...
row = dict(..., source_fetches=fetches, ...)
```
(lines 294, 317). `loop_fetches` and `direct_fetches` are summed, so a single
number mixes loop-buffer source fetches with straight fetches. Caveat line 412
warns separately that software-loop `scheduler_cycles` "count issue cycles at a
parked fetch PC and are not instruction-issue frequencies".

**`source_predicate_outcomes`** is decoded from a 64-bit bitmap of predicate
*register* patterns sampled **before** each successful source fetch
(`observed_predicate_outcomes`, lines 209-222). Its own docstring is the answer
to the question: *"Source-fetch observations only; buffered issue and SPMASK are
separate."* Caveat line 411 repeats it: "…it does not establish buffered
instruction execution or account for SPMASK suppression."

**Does coverage distinguish a source fetch from an executed predicate body?
No.** There is no such distinction anywhere in the schema. The closest available
signal is `source_predicate_audit.false_only_addresses` — addresses whose
predicate was observed **only false** at fetch time:

| Run | confirmed addresses | true-observed | false-only |
| --- | ---: | ---: | ---: |
| `dsp-circular-transcript-replay-1` | 5396 | 5148 | **248** |
| `nxs-pcm-observe-4` | 2185 | 2027 | **158** |
| `dsp-sic-mask-120s-tail-1` | 1089 | 1002 | **87** |
| `09-combined/final-4` | 1217 | 1163 | **54** |

So at least 248 of the headline 5,396 "confirmed instruction addresses" (4.6%)
were never observed with a true predicate at fetch. They are fetched code, not
demonstrated-executed code. `unavailable_addresses` and
`no_observations_addresses` are 0 in all four, so no addresses are hidden behind
a null.

**`--verify-repeat` compares our output to our own output, four ways**
(`tools/cdj_dsp/replay.py`, gate construction):

```python
gate['repeat_matches'] = actual == repeated
gate['final_state_and_memory_match'] = final == repeat_final
gate['repeat_coverage_matches'] = coverage_data['coverage.json'][1] == repeated_coverage
gate['dsp_tx_capture_matches'] = tx_bytes == repeat_tx_bytes
```

`repeat_command = command.copy()` — the **same compiled binary**, same input,
run twice in the same process tree. `--expect-trace` adds
`gate['expected_matches'] = actual == expected`, comparing against an earlier
saved trace of ours. Nothing in the gate compares against TI documentation, TI
tooling, or hardware.

`gate['passed']` is the AND of those comparisons plus `verified_stops > 0`. Two
consequences worth recording: (a) `passed` does **not** depend on
`architectural_validation_eligible`, and (b) `passed` does **not** depend on
the absence of a fault — a fail-closed fault run still writes `passed: true` if
the repeat matches (the fault is recorded separately in `failure.json`). Also
`verified_connected_stops` is gated only as `> 0`; the gate records no transcript
stop total, so "all 71 stops" / "all nine remaining stops" are claims the gate
cannot substantiate on its own.

**What the mode switches change.** Nothing about the gate's comparisons. They
change (i) environment variables handed to the native binary —
`CDJ_NXS_DSP_FUNCTIONAL_TIMING`, `CDJ_NXS_DSP_FUNCTIONAL_AUDIO`,
`CDJ_DSP_OBSERVE_PCM`, `CDJ_DSP_CONNECTED_STOPS`, `CDJ_DSP_COMPACT_TRACE`,
`CDJ_NXS_DSP_TX_CAPTURE` — and (ii) two derived labels:

```python
validation_eligible = not (inherited_exploratory or
                          args.functional_dsp_timing or args.functional_dsp_audio or
                          dsp_scheduler_mode != 'legacy')
```

`architectural_validation_eligible` is therefore a **mode-purity flag**, not a
statement that anything was architecturally validated. And
`inherited_exploratory` comes from `exploratory_ancestry(capture_manifest)`,
which inspects only the **immediate parent** manifest's four flags — it does not
walk the ancestry. `nxs-pcm-observe-4` resumes a checkpoint already at
2,736,099,500 packets / 4,915,565,517 cycles; one generation of flag checking
cannot establish that three billion packets of accumulated state were produced in
strict mode.

## 4. Exact scope of the PCM evidence

`tools/cdj_dsp/pcm_evidence.py` is 80 lines. `tests/test_pcm_bank_evidence.py`
and `tools/cdj_dsp/pcm_bank_evidence.py` are unrelated uncommitted work; I read
enough of them only to confirm they are separate and left them untouched.

The check is **genuinely non-circular in its expected values**, which makes it
the strongest single piece of evidence in the chain:

```python
source_pcm = samples[offset:offset+2352]
channels = struct.unpack('<1176h', source_pcm)
input_matches = raw[offset:offset+2352] == source_pcm
for channel in range(2):
    expected = struct.pack('<588f', *(x/32768 for x in channels[channel::2]))
    output_matches.append(bytes.fromhex(span['hex']) == expected)
```

`samples` comes from an external WAV (`TESTTONE.WAV`, sha256
`1f276d81…ade37a3a`) that the emulator did not produce. So the S16→float32
conversion is checked against an independent artifact, not against recorded
emulator output.

**What the 16 stops / four blocks / 2352 stereo frames establishes:** on one
hash-pinned checkpoint, under strict timing and a stopped McASP clock, the
modeled C674x executed the stock firmware's stereo unpack kernel at
`c003c398` four times (blocks 0-3, bank 0 only, `b6==2`, `a6==1176`), and for
each call both the 2352 input bytes in RAM matched the loaded WAV and all 588
float32 samples in each of the two output planes (`11800200`, `11800fc8`,
separation `0xdc8`) equalled source-S16 / 32768 exactly. That is 4 × 588 = 2352
stereo frames, about 0.53% of the 441,000 frames of the 10-second track.

**What it does not establish**, beyond the list the doc already gives (bank 1,
full bank/block range, metadata modes, ownership/release protocol, input refill
lifetime, downstream processing, mono/48k/S24, continuous playback, McASP output,
true stereo separation, headroom): it also does not establish channel order or
separation, because the test tone has identical L/R channels —
`tests/test_pcm_evidence.py`'s distinct-channel check is a **synthetic
host-tooling test** that fabricates both the trace records and the `gate.json`,
so it validates the checker's logic and nothing about the emulator. The
RAM-base-to-WAV-offset alignment (`base != 0x118381e0` is rejected, and
`samples[offset:]` reuses the same offset) is asserted from the emulator's own
load path, not derived independently. And the checker's precondition is the
circular repeat gate (`gate['passed'] and repeat_matches and
final_state_and_memory_match`), so the *run's admissibility* is established
circularly even though the *expected values* are not. Observations are capped at
64 and "PC observations can include fall-through; they are not automatically
calls."

## 5. Circular validation in the evidence chain

1. **The gate mechanism itself (EP-CIRC-REPLAY).** This is the canonical case and
   it is the actual mechanism, not an incidental weakness. Every
   `gate.json` in `runs/` and `build/performance/` derives `passed` from
   byte-equality between two runs of the same binary (`repeat_matches`,
   `final_state_and_memory_match`, `repeat_coverage_matches`,
   `dsp_tx_capture_matches`) and, where used, from equality with an earlier
   saved trace of ours (`expected_matches`). I confirmed
   `runs/dsp-circular-transcript-replay-1/trace.jsonl` and `repeat.jsonl` are
   byte-identical, 352,008,549 bytes each. Determinism is real and useful; it is
   regression evidence, not correctness evidence.

2. **Checkpoint lineage (EP-CHAIN-EXTERNAL).** Each run's input is a prior run's
   output. `nxs-pcm-observe-4` starts at 2.7 billion packets of prior emulator
   execution; the "strict" label covers only its own 16 million packets.

3. **The loop-scheduler regression (EP-CIRC-LOOPORACLE).** `PERFORMANCE.md:66`
   claims "The loop regression test compares against the frozen original
   algorithm across 326,144 combinations". `tests/cstub/c674x-loop.c:10` confirms
   it: a `reference_issue()` function commented "Frozen linear scheduler oracle"
   — a re-implementation of *our own* prior algorithm inside the test, with one
   SPRUFE8B citation (7.13.1) for one rule. It detects optimization regressions,
   exactly as claimed, but cannot detect a misreading of the manual shared by
   both implementations.

4. **Not circular:** the PCM expected values (§4); the TI assembler/disassembler
   oracle in `tests/test_c674x_spkernel_fields.py::test_ti_compact_spkernel_oracle`,
   which runs TI's own `cl6x` and `dis6x` and asserts `9c67`/`dc66`/`1f66` decode
   as SPKERNEL stages 3/6/24 — note this test never executes emulator code, so
   the non-circular pair is oracle-test plus cstub-test together; and the GNU
   binutils `tic6x-insn-formats.h` format specs used by `coverage.py`
   (third-party, not TI, decode-only).

## 6. Additional integrity findings

**EP-SEM-FETCHSUM — `packet_fetch_observations` over-counts fetches.**
`coverage.py:383` accumulates `group['packet_fetch_observations'] +=
row['source_fetches']`, summing a *per-packet* count once per *instruction
address* in that packet. Measured:

| Run | real per-packet fetch sum | group sum | inflation |
| --- | ---: | ---: | ---: |
| `dsp-circular-transcript-replay-1` | 56,536,711 | 66,611,053 | **1.178×** |
| `nxs-pcm-observe-4` | 15,993,439 | 17,443,450 | 1.091× |
| `09-combined/final-4` | 288,127 | 314,298 | 1.091× |

The widest packet in `final-4` (`0x1180446c`) has 5 instruction rows each
carrying `source_fetches=54`, contributing 270 to group totals for 54 real
fetches. `semantic_inventory.py` renames the field
`source_fetch_observations`, and its group totals sum to exactly 66,611,053 —
e.g. `bnop .S1` reports `source_fetch_observations: 3,336,131`. Anyone reading
that as a fetch or issue count is over-counting by roughly 18% in aggregate and
up to 5× locally. The field name in `coverage.py` is arguably defensible;
the rename in `semantic_inventory.py` is not.

**EP-GATE-SHALLOW — `gate.json` carries no input provenance.** Comparing key
sets for `nxs-pcm-observe-4`: the gate has 19 keys, the manifest 33, and the
following exist **only** in the manifest — `dump_sha256`, `dump_path`,
`input_checkpoint`, `input_manifest_sha256`, `input_gate_sha256`,
`event_transcript_sha256`, `sources`, `analysis_sources`,
`capture_source_sha256`, `capture_firmware_sha256`, `pll_assumptions`,
`boot_rom_executed`, `dsp_timing_mode`, `dsp_audio_mode`, `dsp_scheduler_mode`,
`dsp_scheduler_provenance`, `inherited_exploratory_state`, `observe_pcm`,
`connected_stops`, `steps`, `complete`. A reader of `gate.json` alone sees
`passed: true`, `architectural_validation_eligible: true`,
`approximations: []` and learns nothing about eleven PLL assumptions, an
unexecuted boot ROM, a stopped audio clock, or where the input came from. Gates
must never be cited without their manifest.

**EP-GATE-ABORTED — pre-execution manifests claim eligibility for runs that
never finished.** `replay.py` writes `manifest.json` *before* launching the
native binary and patches it afterwards. Both
`runs/dsp-interrupt-entry-fixed-events-3` and `-4` therefore sit on disk with
`architectural_validation_eligible: true`, `approximations: []`,
`inherited_exploratory_state: false` and **no** `complete`, `stop`, `coverage`
or `output_checkpoint`, for runs that exited nonzero. Any future tooling that
selects runs by `architectural_validation_eligible == true` must also require
`complete == true`; today nothing enforces that.

**EP-CHAIN-EXTERNAL — two of the four headline runs cannot be re-verified
here.** `runs/dsp-circular-transcript-replay-1` and `runs/nxs-pcm-observe-4`
both record `dump_path` under
`/Users/gpotvin/Git/cdj2000nxs-research/references/cdj2000-emulator/runs/…`.
That repository exists; the two checkpoint files do not:
`nxs-return-bnop-connected-1/dsp-checkpoints/…0001.cdjdsp` and
`nxs-native-load-mvd-1/dsp-checkpoints/…2776.cdjdsp` are both absent. Same for
`runs/dsp-sic-mask-120s-tail-1` (`nxs-sic-mask-strict-120s-1/dsp-checkpoints/
…0482.cdjdsp`) and the interrupt-entry runs. All four also have
`input_gate_sha256: null` — the producing run's gate was never pinned, so even
the recorded chain stops one link short. `final-4` is the only headline artifact
with a complete, locally verifiable chain.

**EP-CHAIN-REPLAYPY — the gate's author is not hash-pinned.** `replay.py`
decides `architectural_validation_eligible`, assembles `approximations`, and
writes every gate. It appears in **no** manifest: `sources` pins `replay.c` and
the emulator C/H files, `analysis_sources` pins `coverage.py`, `inventory.py`,
`tx_capture.py` and the format header. `grep -l "replay.py"` across all
`runs/*/manifest.json` and `build/performance/*/*/manifest.json` returns
nothing. Current `replay.py` is `ccc39c59…`; no artifact records which version
judged it eligible. `replay.c` (`5f30f995…`) and `coverage.py` (`57ff339f…`) do
match `final-4`'s records, so the analysis pipeline is otherwise unchanged.

**EP-TEST-NOARTIFACT — nothing re-verifies the recorded artifacts.** No test in
the suite reads any committed `runs/` artifact. `grep -l "runs/" tests/*.py`
returns only `tests/test_panel_control.py`, and those are string-planning
assertions about path names (`runs/r099/s.txt`), not artifact reads. The gate,
manifest, coverage and trace hashes of every recorded run are unguarded: a
silent edit or bit-rot in any of them would not fail the suite. I verified them
by hand for this audit; that check exists nowhere in CI.

## 7. Regression-suite classification and current numbers

Command run, exactly as specified:

```
DEVELOPER_DIR=/Library/Developer/CommandLineTools \
C6X_TI_BIN=/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin \
.venv/bin/python -m pytest -q
```

**Real result: `1 failed, 511 passed, 30 skipped in 50.61s` (exit 1).** Not
506/31.

The failure is `tests/test_nxs_usbh.py::test_usbh_remote_wakeup_interrupt`. I
re-ran it alone (`1 failed in 0.30s`) — it is **not** a port conflict with the
coordinator, it reproduces in isolation. Both the test file and the
`cdj_usbh_wakeup()` implementation it exercises are uncommitted work in the tree
(`tests/test_nxs_usbh.py` untracked; `emulator/qemu/cdj2000_usbh.c` modified,
+18 lines). Excluding the three untracked test files gives
**`505 passed, 30 skipped in 52.40s`, green** — so the committed tree passes and
the single red test belongs to work in progress outside my track.

Classification rule, stated explicitly. Counts are pytest test *items*, not
assertions; several items compile and run a C stub containing hundreds of
internal asserts, so item counts understate architectural coverage by a lot
(`tests/cstub/c674x.c` alone carries 843 asserts behind 2 items).

- **A. Architecture tests** — the item compiles and executes emulator device or
  CPU model C source (`emulator/qemu/*.c`) against expected values fixed in the
  test or stub, or invokes the TI toolchain as an oracle. Split A1 (C674x/C6747
  DSP) / A2 (SH4 board, Blackfin, USB, PHY, input, MFi).
- **B. Transcript / replay / evidence-pipeline regressions** — the item
  exercises the replay, checkpoint, coverage, inventory or evidence-checker
  pipeline. Inputs are synthetic fixtures the test builds, or emulator-produced
  artifacts; the property under test is pipeline behaviour, determinism or
  refusal, not a manual-derived architectural value.
- **C. Host-tooling tests** — no emulator device model executed: panel control,
  GUI/deck layout, network peers, firmware packing/unpacking, media, link decode.

| Category | Items | Files |
| --- | ---: | --- |
| A1 DSP architecture | **28** | `test_c674x.py` 17, `test_c674x_spkernel_fields.py` 2, `test_dsp_fetch_state.py` 2, `test_c674x_circular.py` 1, `test_c674x_saturation.py` 1, `test_c6747_spi_timed.py` 1, `test_c6747_spi_clock.py` 1, `test_dsp_scheduler.py` 1, `test_dsp_transaction_mapping.py` 1, `test_nxs_hpi.py` 1 |
| A2 non-DSP device models | **53** | `test_input_channel.py` 25, `test_main_dmac.py` 12, `test_bfin_sic_mask.py` 4, `test_bfin_sport_capture.py` 2, `test_bfin_link_cache.py` 2, and 1 each: `sh7764_iic`, `sh7764_eth`, `rtl8201fl`, `mfi_identity`, `nxs_usbh`, `bfin_frame_change`, `bfin_cold_lzss`, `blackfin_parallel` |
| B evidence pipeline | **134** | `test_dsp_boot_handshake.py` 32, `test_dsp_coverage.py` 29, `test_dsp_semantic_inventory.py` 18, `test_dsp_checkpoint_replay.py` 13, `test_dsp_deferred_replay.py` 13, `test_dsp_inventory.py` 8, `test_dsp_replay.py` 7, `test_dsp_isa_audit.py` 4, `test_dsp_event_replay.py` 3, `test_replay_build_cache.py` 2, `test_pcm_evidence.py` 2, `test_pcm_bank_evidence.py` 2, `test_dsp_read_fast.py` 1 |
| C host tooling | **327** | `test_panel_control.py` 71, `test_nxs_boot_evidence.py` 47, `test_deck_redesign.py` 31, `test_panel_layout.py` 22, `test_dhcp_peer.py` 20, `test_ethernet_peer.py` 15, + 21 smaller files |
| **Total collected** | **542** | |

Of the 28 A1 items, how many are **reference-backed** in the strict sense —
expected values traceable in the test source to a TI document or TI tooling?

- **1** item has expected values from an independent oracle:
  `test_ti_compact_spkernel_oracle`, which runs TI `cl6x`/`dis6x`. It does not
  execute emulator code; its companion `test_c674x_spkernel_fields` checks the
  emulator against the same claim. Together: genuinely reference-backed, scope =
  three compact SPKERNEL encodings.
- **6** A1 items are backed by stubs carrying at least one printed-page or
  figure citation: `c674x.c` (3 citations / 843 asserts), `c674x-loop.c` (2),
  `c674x-circular.c` (1), `c674x-saturation.c` (1),
  `c674x-spkernel-fields.c` (1), `c6747-spi.c` (1 — SPRUH91D 27.3.5/14/19).
- **Only 7 of 31 stub files in `tests/cstub/` cite a TI document at all.** The
  remaining 24 — including `c6747-mcasp.c` (195 asserts), `c6747-edma.c` (123),
  `c6747-syscfg.c` (89), `c6747-pll.c` (86), `c6747-intc.c` (48) — carry no
  recorded reference. Their expected values may well be hand-derived from
  SPRUH91D; the test source does not say so, and nobody can confirm it without
  redoing the derivation.

Where citations do exist they are accurate. I spot-verified every page in
`c674x-saturation.c` and `c674x.c` against the text index: SADD p422, SSHL p493,
SSUB p499, SPKERNEL p481 all land on the correct instruction pages; Table 7-1
("SPLOOP Instruction Flow"), Figure H-7 ("Uspk Instruction Format") and
Figures C-18/C-19 ("Dx5"/"Dx5p Instruction Format") all exist as cited. Printed
page equals PDF page throughout.

Category B's 134 items are almost entirely **synthetic-fixture** tests of the
analysis tooling. `test_pcm_evidence.py` is the clearest example: it fabricates
`pcm_observation` records, writes a fake `gate.json`, and asserts the checker
accepts the clean case and rejects a duplicated channel. `test_dsp_coverage.py`
unit-tests `predicate_description` and `observed_predicate_outcomes` against
hand-written words. These tests are valuable — they are what stops the evidence
tooling from silently laundering a claim — but they are not emulator validation,
and `test_dsp_isa_audit.py`'s own docstring says so: "Neither tool is part of the
emulator".

## 8. Stale documentation findings

| File:line | Claim | Current truth |
| --- | --- | --- |
| `DSP_BOOT_MILESTONE_AUDIT.md:19` | `cdj_c674x.c` = `6131477b…` | now `34e6a1c1…`; all audit conclusions describe an older interpreter |
| `DSP_BOOT_MILESTONE_AUDIT.md:20` | `cdj_c674x_loop.c` = `a7df446a…` | now `3e96810d…` |
| `DSP_BOOT_MILESTONE_AUDIT.md:22` | `nxs_vm.py` = `e66c600b…` | now `bfcc704a…` |
| `DSP_BOOT_MILESTONE_AUDIT.md:12` | "worktree was clean before this audit document" | worktree now carries modified `cdj2000_usbh.c` and seven untracked files |
| `DSP_BOOT_MILESTONE_AUDIT.md:120-121` | tail gate "passes all nine remaining connected stops" | gate records `verified_connected_stops: 9` and gates only `> 0`; it records no transcript total, so "all nine remaining" is not substantiated by the gate |
| `DSP_BOOT_MILESTONE_AUDIT.md:171,177` | "403 passed, 27 skipped" / "407 tests, 27 optional skips" | 505 passed / 30 skipped (committed tree) |
| `DSP_INTERRUPT_ENTRY.md:30` | "427 passed, 29 optional skips" | 505 passed / 30 skipped |
| `CLEAN_BOOT_EVIDENCE.md:83` | "408 passed, 27 optional skips" | 505 passed / 30 skipped |
| `ITERATION_ANALYSIS.md:467` | "**506 passed, 31 skipped in 67.76 s**" | 505/30 committed, 511/30/**1 failed** with WIP |
| `HANDOFF.md:554` | "**Current-source** `runs/dsp-circular-transcript-replay-1`…" | that run's manifest pins `cdj_c674x.c = 1d02d672…`, two revisions behind HEAD; no longer current-source |
| `HANDOFF.md:554` | "matches all 71 previous connected stops" | 71 stops verified within a 100M-step budget that ended by "phase budget exhausted"; the transcript suffix was not replayed, and the transcript is in an absent external repo |
| `PCM_EXECUTION_EVIDENCE.md:71` | "492 passed / 29 optional skips" | 505 passed / 30 skipped |

Stale test counts are low-severity and expected as the suite grows; I list them
because four separate documents each assert a different "final" number, so none
of them can be used as the baseline. The `cdj_c674x.c`/`cdj_c674x_loop.c` hash
drift is the consequential one: it is the precise boundary between what the
boot-milestone audit proved and what today's interpreter does.

Worth recording on the other side of the ledger: `PCM_EXECUTION_EVIDENCE.md`,
`DSP_INTERRUPT_ENTRY.md` and the `coverage.py` caveat block are unusually
accurate about their own limits. Every scope restriction I tried to falsify in
those three was already stated. The integrity risk in this repository is not
over-claiming prose; it is that `gate.json` is structurally thinner than the
prose and will over-claim if read alone.

## 9. Open questions

1. Can the absent input checkpoints under
   `/Users/gpotvin/Git/cdj2000nxs-research/references/cdj2000-emulator/runs/`
   be restored? Without them, `dsp-circular-transcript-replay-1`,
   `nxs-pcm-observe-4` and `dsp-sic-mask-120s-tail-1` are unreproducible and
   their `dump_sha256` values unverifiable.
2. How many connected stops does
   `nxs-return-bnop-connected-1/dsp-events.jsonl` actually contain? Required to
   turn `verified_connected_stops: 71` into the "all 71" claim in
   `HANDOFF.md:554`. The transcript is in the absent external repo.
3. Is the `nxs-pcm-observe-4` ancestry strict all the way back? Its input sits
   at 2.7 billion packets and `exploratory_ancestry()` checks one generation.
4. Where are the expected values in the 24 uncited `tests/cstub/` files derived
   from? Without a citation, their "reference-backed" status is unknown, not
   absent. Track 4 (peripherals) is better placed to answer for the `c6747-*`
   stubs.
5. Is the tic6x disassembler binary (`7845ca22…`) recoverable? Without it the
   68-mnemonic / 262-group inventory cannot be regenerated.
6. Does `DSP_BOOT_MILESTONE_AUDIT.md` need re-running against current source, or
   should it be re-labelled as historical evidence about `6131477b`? That is a
   coordinator decision, not a finding.

## 10. Commands run

| Command | Result |
| --- | --- |
| `git log --oneline -5` / `git status --short` | HEAD `2eb2476`, not `3da5ff2`; `cdj2000_usbh.c` modified, 7 untracked files |
| `jq` over the four headline gate/manifest/coverage files | modes, approximations, counts, eligibility flags as tabulated above |
| `shasum -a 256` on 8 artifacts per run vs their gate values | all match in all four runs; no missing gate, manifest or hash |
| `cmp -s runs/dsp-circular-transcript-replay-1/{trace,repeat}.jsonl` | byte-identical (352,008,549 bytes each) |
| `shasum -a 256` on the six files documented in `DSP_BOOT_MILESTONE_AUDIT.md` | 3 of 6 stale (`cdj_c674x.c`, `cdj_c674x_loop.c`, `nxs_vm.py`) |
| `ls` on the four recorded `dump_path` inputs | all absent from the external research repo |
| `shasum -a 256` tail-replay trace / final checkpoint vs doc | both match exactly (`74bddf5f…`, `719510a0…`) |
| `shasum -a 256` interrupt-entry `-3`/`-4` traces vs doc | both match `a8564c58…` exactly |
| `shasum -a 256 tools/cdj_dsp/{coverage,replay,inventory,semantic_inventory}.py` | `coverage.py`, `inventory.py`, `semantic_inventory.py` match recorded values; `replay.py` pinned nowhere |
| `.venv/bin/python -m tools.cdj_dsp.refdocs --check` | exit 0; sprufe8b 771/739, spruh91d 1473/1435 |
| Python sum of `packet_fetch_observations` vs real per-packet fetches, 3 runs | 1.178× / 1.091× / 1.091× inflation |
| `awk`/`grep` over `build/references/sprufe8b.txt` for pp. 422/481/493/499/641, Table 7-1, Figs H-7, C-18/C-19, 5-4 | every cited page/figure verified; printed = PDF page |
| `pytest -q` (full, specified env) | **1 failed, 511 passed, 30 skipped**, exit 1 |
| `pytest -q tests/test_nxs_usbh.py::test_usbh_remote_wakeup_interrupt` | 1 failed in 0.30s — reproduces alone, not a port conflict |
| `pytest -q --ignore` the 3 untracked test files | **505 passed, 30 skipped**, green |
| `pytest -q --collect-only` | 542 items; per-file counts as tabulated |
| `grep -l "runs/" tests/*.py` | only `test_panel_control.py`, and only as path strings |
| `grep -l "emulator/qemu" tests/*.py`, `grep -l C6X_TI_BIN tests/*.py` | 19 files compile emulator C; 2 invoke the TI toolchain |
| `grep -c 'SPRUFE8\|SPRUH91' tests/cstub/*.c` | 7 of 31 stubs cite a TI document |
