# DSP E-7010 milestone evidence audit

Audit date: 2026-09-09. This is a DSP-specific startup gate, not a claim of
clean full boot, full DSP parity, cycle accuracy, storage operation or audio.
The visible **E-7206 AUTH CHIP ERROR remains unresolved** and is not E-7010.
Both 120-second cold runs and the repeat's late interaction are now evidenced.
This audit supports the narrow DSP startup milestone, subject to final
documentation/commit completion, with the sampling limitations below.

## Source and execution provenance

Emulator source revision: `bd042d92dd0ca7072f3e18b7a17446dde07fa5a9`
on `codex/macos-nxs`; worktree was clean before this audit document.
Parent research revision: `2d46cc7cd8522322bd3c733fd5193538f4bbcd1f`.
Source hashes measured during the audit:

| File | SHA-256 |
| --- | --- |
| `emulator/qemu/cdj_c674x.c` | `6131477ba205f6a96580b103ea017f29d511f0600fd5a967c380e9a957ef800e` |
| `emulator/qemu/cdj_c674x_loop.c` | `a7df446a7128e5d1d48591ace546ec47e0fbbda2fb8594f3147cf0c3d207dec2` |
| `emulator/qemu/cdj_c6747_spi.c` | `9de8f13afa031be19220afbf99cb46e285bc8935dd9abbaffed06256685d334b` |
| `tools/cdj_main/nxs_vm.py` | `e66c600bf6b062eee915c50d7029db602df311b7c4d181519aeeb74302b73533` |
| `tools/cdj_dsp/boot_handshake.py` | `f3cdf30bfd29d7e2d7e702d6762f7d193fcf530560e1e18d6d9e12371da42739` |
| `patches/07-gdb-17.2-sic-mask-order.patch` | `678fcfd086443a49ed4a099bc2e08cb48e76b859e5beab13284827d944e2958b` |

`runs/nxs-sic-mask-strict-120s-1/run.json` captures actual QEMU and Blackfin
commands, five input hashes before/after execution and an empty
`inputs_differ_at_exit`. QEMU hash is
`f69c34b53bdbb92bee51d3ec6db6a0ade27cd9f935700c65237dcf3e7798bd71`;
Blackfin simulator hash is
`2663e8402b14d16a077665a835017f8f2dc298c0a5515699cb945db670591841`.
MAIN firmware hash is
`02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85`.
GUI boot ELF hash is
`f873acf392c51475ec812e4a2cf809e674d8723f04dbe25eb2ba354a962945cb`;
GUI flash hash is
`75b4bb0d000429604e5c1dc63f24ab52b43a1a578bdf90232eb95489d38c9e30`.

The launcher source removes inherited `CDJ_*`/`BFIN_*` variables before
constructing child environments. This run records legacy DSP scheduling,
no functional timing/audio switches, no sync profiler, status freshness 0 and
link-row rewriting off. Its actual commands start MAIN with its firmware BIOS
and BF531 with GUI boot/flash inputs, not a resumed DSP checkpoint.

## Boot-critical semantic evidence

The existing NXS MAIN trace in HANDOFF identifies loader `0x041fc698`, final
handler `0x041fc646`, ready polling at `0x1183fff4`, acknowledgement at
`0x1183fff0`, and device-1 status `0x04cf2468`. Failure maps to caution 2,
whose table entry is E-7010; DSP stage 1 publishes ready at `0x1180304c`.
This map is specific to the MAIN hash above.

Current `cdj_c674x.c` reconstructs compact SPKERNEL fields according to
SPRUFE8B Figure H-7 before Table 3-29 stage reversal. Thus `0xdc66` means
stage 6 at II=1, not stage 3. No loop drain delay or resource-check relaxation
is part of that correction. `tests/cstub/c674x-spkernel-fields.c` checks all
64 fields at nine II values, invalid cycles, full/compact schedule equivalence
and the exact firmware encoding. The independent TI assembler/disassembler
fixture checks `9c67`/`dc66`/`1f66` as stages 3/6/24 respectively. Reproduce:

```sh
C6X_TI_BIN=/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin \
  .venv/bin/python -m pytest -q tests/test_c674x.py tests/test_c674x_spkernel_fields.py
```

Timed SPI support and the narrow Blackfin SIC store-mask-before-forward fix
are also required by the connected evidence; their documented limitations
remain applicable. A strict-default label is not proof of complete hardware
accuracy; cache timing, inactive audio clocks and other documented model
limitations remain.

## First 120-second cold connected run

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_STRICT_120 \
  --seconds 120 --frame-interval 5 --port 6080 \
  --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.boot_handshake \
  runs/nxs-sic-mask-strict-120s-1/dsp-events.jsonl
```

Recorded run: `runs/nxs-sic-mask-strict-120s-1`. Result reports GUI exit 0,
not timed out and framebuffer present. DSP log contains 451 phase-budget
boundaries and 13 HINT yields, no fault stop; terminal state is 451,099,500
packets / 825,308,974 cycles, PC `0xc003b508`, fault word 0, checkpoint 491.
These counts are execution evidence, not correctness percentages.

The independently rerun handshake checker observes ready/clear/ack events
104211/104213/104215 in 119,903 events. Transcript SHA-256 is
`78f1514dd8b448b9f279f8fab2fa96d5d9ac37a060e6fdcd2c33698907caa8bf`.
This is ordered transcript evidence, not independent authentication of a
hardware model or a substitute for GUI validation.

Frame manifest observations show normal `Not Loaded.` player at 40.0299s
(`frames/000009.ppm`, hash
`9ea6b13c57b8b37eb842d2811e0519f0499a7de86e9d8025bd48dbbbd9f0978b`),
auth-error transition at 55.0157s and UTILITY at 60.1012s, retained through
115.0259s. The last observation is 74.996 seconds after the normal player.
The player, auth transition and UTILITY images were visually inspected; MENU
changed the genuine GUI to UTILITY. Final UTILITY hash:
`e8531d063ad97852a4fa1f3d3698b1e7f69e1dfebc8ad95c99b94e2321bb7713`.
No E-7010 is visible in these inspected states. Five-second sampling leaves
unobserved intervals; it is not an absolute continuous visual proof of absence.

The unchanged final image alone is not liveness evidence. GUI stats continue
from link_rx 26,600 at wall100s to 31,856 at wall120s, with 3,538,223,104
instructions and 7,199 frames by exit. Together with ongoing strict DSP
execution and the MENU response, this supports continued operation. The late
interaction check on the repeat is recorded below.

## Deterministic tail replay

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-sic-mask-strict-120s-1/dsp-checkpoints/00000000000000000482.cdjdsp \
  runs/NEW_STRICT_TAIL --steps 10000000 \
  --events runs/nxs-sic-mask-strict-120s-1/dsp-events.jsonl --verify-repeat
```

Recorded result `runs/dsp-sic-mask-120s-tail-1/gate.json` passes all nine
remaining connected stops with exact repeat trace, coverage, final state and
memory, no faults and legacy/strict provenance. Trace SHA-256:
`74bddf5f3dd1d7529b4bb14f966e007d784d9a10c9de2086472aebc7c10aa0a9`.
Final schema-11 checkpoint SHA-256:
`719510a0b9fc30b0bc38b0dd9c8ef62f3ea6d8ddf09c53cd9c2d9c24c82de7b1`.
This covers the final nine million packets, not a full-startup replay.

## Second cold run and late interaction

`runs/nxs-sic-mask-strict-120s-2` has terminal GUI exit 0, framebuffer present
and no timeout. Root independently verified all five input hashes match run 1
and none change at exit. It reports 446 budget boundaries plus 13 HINT yields,
no DSP fault, ending at 446,099,500 packets / 816,358,705 cycles,
PC `0x11802724`, fault word 0. Different wall-bounded packet totals are not a
deterministic replay failure or correctness comparison.

The independently rerun handshake checker confirms the same
104211/104213/104215 sequence in 119,718 events. Transcript SHA-256:
`e0d11549f5d1cbb8f0b95c9d09fe11e95ad31f6898fba6e765d3ec7540acfc54`.
The normal player appears at 40.0723s, UTILITY at 65.0108s and final sampled
frame at 115.0450s: 74.9727 seconds beyond the first normal player observation.

Root recorded MENU down/up Unix times 1788959796.1735628 /
1788959806.326834. A late encoder +1 at 1788959836.8640718 visibly moved
selection from PLAY MODE to EJECT/LOAD LOCK. The changed selection first
appears at 105.0842s, more than 65 seconds after the normal player, and is
retained at 115.0450s. Its frame hash is
`a961a208308a3df2286281f74bab2c414c82123f6153aef404e9907e13091783`.
Root visually inspected the player, auth transition, UTILITY and changed
selection. This late response strengthens continued-operation evidence beyond
a static final frame. E-7206 remains visible; E-7010 is not observed.

Current-source focused gates rerun by root: SPKERNEL field/TI oracle 2 passed,
timed SPI/clock 2 passed, handshake/SIC/boot evidence 70 passed. These focused
tests support their named components, not all firmware or full ISA correctness.

## Completion interpretation and remaining deliverables

The repeated strict cold-start, genuine handshake, sampled normal player,
more-than-60-second continued DSP/GUI operation and real late interaction
support the explicit E-7010 milestone. This does **not** prove a clean full boot:
the authentication peripheral still raises E-7206. Frame sampling leaves
unobserved intervals and broad hardware fidelity remains incomplete.

HANDOFF/BUILD and the evidence were committed locally in `c96c8af`, with this
final gate update following the harness correction. No push is authorized.
Retain authentication, faithful
peripherals/storage, full DSP parity and working audio as separate backlog
items rather than treating this DSP-specific result as readiness.

Final full-suite rerun with TI tools enabled: 403 passed, 27 skipped, one
failure in `test_every_control_of_the_window_moves_the_payload`: the ping
segment received no reply. Harness-only correction `1ecb971` reassembles TCP
lines and waits for each command's reply before advancing its segment marker,
without adding guest frames/time. Missing replies still fail after one second;
fragmented and deferred replies have focused regressions. No emulator binary
changed. Subsequent complete suite passed **407 tests, 27 optional skips**
in 53.28 seconds with `C6X_TI_BIN` set as above. This closes the regression
gate; the original failure is retained here for an honest evidence history.

All explicit DSP-startup milestone gates are satisfied by the evidence above.
This conclusion is limited to the specified E-7010 goal, not clean full boot,
full hardware fidelity or working audio. No firmware bypass or exploratory
DSP override was used in the qualifying cold runs.
