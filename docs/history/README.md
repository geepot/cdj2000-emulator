# Investigation history (fork only)

These are the dated checkpoint journals from the NXS bring-up on the
`geepot/cdj2000-emulator` `develop` branch. They are records, not
descriptions of the current system: each says what was measured on the tree of
its day, and later work has superseded parts of most of them. The current
behaviour is described in the top-level README, BUILD, RUNNING, PERFORMANCE
and DEVELOPING. Committed evidence under `analysis/` cites these files by their
old top-level names and line numbers; the content is unchanged, only moved.

They are deliberately kept out of upstream pull requests (upstream excludes
investigation journals, captured runs and iteration dumps), and several name
private checkout paths and `runs/` directories that exist only on the
author's machine.

| File | Date | What it records |
|---|---|---|
| [HANDOFF.md](HANDOFF.md) | 2026-09-08..25 | Running macOS/NXS handoff log, newest first |
| [ITERATION_ANALYSIS.md](ITERATION_ANALYSIS.md) | 2026-09-10 | Replay/build/DSP throughput iteration audit (`analysis/iteration-audit-2026-09-10/`) |
| [DSP_ARCHITECTURE_COVERAGE.md](DSP_ARCHITECTURE_COVERAGE.md) | 2026-09-10..24 | C674x/C6747 coverage audit against SPRUFE8B/SPRUH91D; feeds `analysis/dsp/coverage_inventory.json` |
| [DSP_BOOT_MILESTONE_AUDIT.md](DSP_BOOT_MILESTONE_AUDIT.md) | 2026-09-09 | E-7010 DSP startup milestone evidence |
| [DSP_INTERRUPT_ENTRY.md](DSP_INTERRUPT_ENTRY.md) | 2026-09-09 | Strict C674x interrupt-entry interval |
| [CLEAN_BOOT_EVIDENCE.md](CLEAN_BOOT_EVIDENCE.md) | 2026-09-24 | Clean NXS startup runs and the E-7206 auth-chip investigation |
| [SH4_BLACKFIN_COVERAGE.md](SH4_BLACKFIN_COVERAGE.md) | 2026-09-10 | Board-side SH7764/BF531 model coverage audit |
| [ETHERNET_LOCAL.md](ETHERNET_LOCAL.md) | 2026-09-09 | Isolated EtherC/RTL8201FL, DHCP and TMU3 fixtures |
| [NXS_SD_READINESS.md](NXS_SD_READINESS.md) | 2026-09-09..11 | SD mount-gate analysis (`tools/cdj_main/sd_readiness.py`) |
| [NXS_MEDIA_DIAGNOSTICS.md](NXS_MEDIA_DIAGNOSTICS.md) | 2026-09-09 | Delayed SD insertion and media tracing |
| [NXS_BROWSE_BLOCKER.md](NXS_BROWSE_BLOCKER.md) | 2026-09-10 | NO CARD then genuine listing; why browse aids stay off |
| [NXS_LINK_LOADING.md](NXS_LINK_LOADING.md) | 2026-09-10..24 | Fresh-only link delivery and native track loading |
| [NXS_GUI_STALL.md](NXS_GUI_STALL.md) | 2026-09-09 | GuiCom pool deadlock diagnosed from stopped RAM |
| [PCM_EXECUTION_EVIDENCE.md](PCM_EXECUTION_EVIDENCE.md) | 2026-09-09 | Replayed stock PCM unpack execution |

The NXS panel contact map, which is still current, is in
[../NXS_PANEL_MAP.md](../NXS_PANEL_MAP.md).
