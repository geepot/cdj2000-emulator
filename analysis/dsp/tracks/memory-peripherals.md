# Track 4 — memory, cache, addressing and C6747 peripherals

Repository `/Users/gpotvin/Git/cdj2000-emulator`, branch `codex/macos-nxs`, HEAD `3da5ff2`.
Assessment only. No emulator source, test or tool file was modified. The only files
this track created are this report and three throwaway probes under the session
scratchpad (`probe_manual_examples.c`, `probe_double_read.c`, `probe_block.c`).

References used, verified with `.venv/bin/python -m tools.cdj_dsp.refdocs --check`:
SPRUFE8B (771 PDF pages, 739 footers) and SPRUH91D (1473 PDF pages, 1435 footers).
Printed page == PDF page for both; no page was found where they differ. Every
citation below is "printed page N (PDF page N)".

## 1. Scope

### Covered
- All SPRUFE8B `.D`-unit address generator options (Table 3-11), offset scaling,
  the B14/B15 15-bit displacement form, alignment rules, the nonaligned
  LDNW/LDNDW/STNW/STNDW family, and ADDAB/ADDAH/ADDAW/ADDAD + SUBAB/SUBAH/SUBAW.
- AMR: field layout, block-size encoding, per-register mode selection, reserved
  mode, the nonaligned 32-byte rule, the issue-time circular-width capture in
  `CdjC674xStore.size` / `CdjC674xLoad.size`, and the SPRUFE8B 3.8.9 AMR-write
  interlock.
- Memory ordering: when a queued store becomes visible to a later load, what the
  E1/E3/E5 model guarantees, and what the header's read contract excludes.
- Memory map as actually implemented by the replay/board bus, and what is unmapped.
- `cdj_c6747_cache.c` in full.
- Register depth and validation for every implemented C6747 peripheral in this
  track's list, plus an explicit deferred/unassessed list derived from the
  SPRUH91D chapter list and Table 3-1.
- McASP in depth, including `tests/cstub/c6747-mcasp.c` and the replay driver.

### NOT covered
- CPU-side interrupt acceptance, IFR/IER/ISTP/NMI behaviour, ISR entry and return
  (Track 3). This track covers only the device-side INTC registers and the event
  path into the CPU request mask.
- The instruction-set denominator, decode acceptance, ALU/FP semantics, SPLOOP and
  the software-loop buffer (Tracks 1/2).
- SPI timing/WM8740 behaviour beyond register depth; the SPI serial clock model and
  the WM8740 DAC belong to the audio-path track. I recorded SPI register depth and
  one read-side-effect defect, nothing more.
- Checkpoint schema/ABI correctness, scheduler, replay determinism.
- `cdj2000_nxs_hpi.c` was assessed only for its HPI transport surface and the bus
  map it installs; its 40 KB of MAIN-facing board logic was not audited.
- I did not attempt to prove which of the deferred peripherals the real CDJ-2000
  firmware actually touches. "Deferred" here means "no model exists", not "not needed".
- No percentage of peripheral coverage is given. A peripheral is not a countable
  unit of equal size, and register counts per chapter were not enumerated
  exhaustively for the unmodelled chapters.

## 2. Coverage table

Three independent axes per row: **Impl** (supported/partial/unsupported/unknown),
**Val** (reference-backed-tests / firmware-observation-only / untested / not-assessed),
**Fid** (strict / approximation / unknown).

| ID | Requirement | Manual | Printed page | Impl | Where | Val | Fid | Notes / exclusions |
|---|---|---|---|---|---|---|---|---|
| MEM-ADDR-MODES | 12 valid `.D` address generator options, 4 reserved | SPRUFE8B 3.9.3 Table 3-11 | 90 | supported | `cdj_c674x.c:1625-1675` | reference-backed-tests | strict | mode bit roles (bit0 add/sub, bit2 reg/cst, bit3 modify, bit1 pre/post) and the reserved set {2,3,6,7} match Table 3-11 exactly |
| MEM-ADDR-SCALE | Offset scaling by data size; LDNDW/STNDW `sc` bit; nonscaled forms | SPRUFE8B 3.9.1.1, LDNDW/STNDW pages | 87, 292, 504 | supported | `cdj_c674x.c:1611-1616,1645` | reference-backed-tests | strict | `sc` read from bit 23, scale 8 vs 1, matches LDNDW opcode figure |
| MEM-ADDR-B14B15 | `*+B14/B15[ucst15]`, .D2 only, linear only, no base update | SPRUFE8B 3.9.3, LDW/STW 15-bit pages | 90, 302, 408 | supported | `cdj_c674x.c:1627-1631` | reference-backed-tests | strict | forces base B14/B15 from `y`, mode=1, no writeback; ucst15 scaled by size |
| MEM-ADDR-ALIGN | Aligned transfers must be naturally aligned | SPRUFE8B LDW/LDDW/STW pages | 302, 287, 408 | supported | `cdj_c674x.c:1652-1656` | reference-backed-tests | strict | misaligned aligned-form access faults "unaligned or unmapped scalar memory access" |
| MEM-ADDR-NONALIGN | LDNW/LDNDW/STNW/STNDW may use any byte boundary | SPRUFE8B 3.8.5, LDNDW p292 | 80, 292 | supported | `cdj_c674x.c:1609-1614`, `read_scalar`/`read_transfer` 790-835 | reference-backed-tests | strict | byte-lane assembly across word reads; extended op map 2/3/4/5/6/7 verified against each instruction's opcode figure |
| MEM-AMR-FIELDS | AMR layout, 2-bit per-register mode, BK0/BK1, block = 2^(N+1) bytes, reserved mode 3, bits 31:26 read 0 | SPRUFE8B 2.8.3 Tables 2-7/2-8 | 36-37 | supported | `cdj_c674x.c:178-179, 235-252, 2396-2398` | reference-backed-tests | strict | only A4-A7/B4-B7; mode 3 faults "reserved circular addressing mode"; width 32 treated as linear (block = 2^32) |
| MEM-AMR-CIRC-LDST | Circular arithmetic on LD/ST: only bits N..0 update | SPRUFE8B 3.9.2.1 | 88 | supported | `cdj_c674x.c:247-252, 1643-1646` | reference-backed-tests | strict | **I reproduced SPRUFE8B Examples 3-4 and 3-6 literally against the manual's own printed result values** (see §6) |
| MEM-AMR-CIRC-ADDA | Circular arithmetic on ADDAB/H/W/D and SUBAB/H/W; no SUBAD | SPRUFE8B 3.9.2.2, ADDAB/ADDAD pages | 88, 114, 117 | supported | `cdj_c674x.c:1686-1703` | reference-backed-tests | strict | opfields 110000/110010 … 111100/111101 verified against ADDAB and ADDAD opcode figures; **Example 3-5 reproduced literally** |
| MEM-AMR-NONALIGN | Nonaligned circular wrap behaves as a byte sequence; buffer must be >= 32 bytes | SPRUFE8B 3.9.2.3 | 89 | supported | `cdj_c674x.c:810-836, 1639-1641` | reference-backed-tests | strict | per-byte `circular_address`; buffers < 32 bytes fault closed rather than being "undefined" |
| MEM-AMR-WIDTH-CAPTURE | Circular width sampled at E1 and reused at E3/E5 | SPRUFE8B 3.9.2/3.9.2.3 + LD pipeline tables | 88-89, 303 | supported | `cdj_c674x.h:12-20`, `cdj_c674x.c:809-836` | reference-backed-tests | approximation | the manual never states that the nonaligned wrap uses the E1-sampled AMR; E1 address formation makes it the only coherent reading, but it is an inference, not a quoted rule |
| MEM-AMR-BLOCK-VS-SIZE | Access wider than the AMR block size | SPRUFE8B 2.8.3 ("buffer must be aligned on a byte boundary equal to the block size"), 3.9.2.3 | 36, 89 | partial | `cdj_c674x.c:1639-1641` | untested | approximation | the <32-byte check applies **only** to nonaligned transfers. An aligned LDW inside a 2-byte circular block is accepted and behaves linearly (probe A, §6). Manual-undefined territory, but asymmetric: one undefined case fails closed, the other silently degrades |
| MEM-AMR-INTERLOCK | MVC→AMR followed by a LD/ST/ADDA/SUBA using A4-A7/B4-B7 stalls 1 cycle | SPRUFE8B 3.8.9 | 82 | unsupported | `cdj_c674x.c:240, 1629, 1635-1637, 1691, 1700, 2398` | reference-backed-tests | strict (fail-closed) | the stall is **not** modelled; the affected packet faults "AMR use interlock not implemented". The refusal window (`control_ready[0] = cycles+2`) covers exactly the next execute packet, matching the manual's scope. `tests/cstub/c674x-circular.c:177-186` asserts fail-without-NOP / succeed-with-NOP |
| MEM-PARALLEL-NONALIGN | No other memory access in parallel with a nonaligned access | SPRUFE8B 3.8.5 | 80 | supported | `cdj_c674x.c:1633, 2440-2441` | reference-backed-tests | strict | `nonaligned_memory && memory_count > 1` → fault |
| MEM-PARALLEL-SIDE | Two LD/ST using data src/dst from the same register file cannot be in one execute packet; address register must be on the .D unit's side | SPRUFE8B 3.8.5 | 80 | unsupported | — | untested | unknown | **not enforced.** Only same-register write conflicts are checked (`cdj_c674x.c:1667-1672`). The TI assembler rejects these packets, so firmware images should not contain them, but a corrupt or hand-built packet is silently accepted |
| MEM-ORDER-E1E3E5 | Store writes RAM in E3; load samples RAM in E3 and writes dst in E5 | SPRUFE8B LD/ST pipeline tables | 303, 409 | supported | `cdj_c674x.c:1657-1680, 2475-2495` | reference-backed-tests | strict | store due = cycles+3, load sample at due-2 = cycles+3, writeback cycles+5. `tests/cstub/c674x.c:1707-1722` mutates RAM between E1/E3/E5 and asserts the E3-sampled value wins; `c674x-circular.c:214-226` does the same with a genuine intervening MVC to AMR |
| MEM-ORDER-VISIBILITY | When a queued store becomes visible to a later load | SPRUFE8B pipeline tables (no explicit arbitration rule) | 303, 409 | supported | `cdj_c674x.c:2475-2495` | reference-backed-tests | strict (narrow) | per cycle the store-commit loop runs before the load-sample loop, so a store issued in any **earlier** packet is visible to a later load (probe B, §6). The only same-cycle case is an overlapping store+load in one packet, which faults "simultaneous overlapping RAM accesses not implemented" (`cdj_c674x.c:2445-2458`, asserted in `c674x.c:1755-1762` and `c674x-circular.c:262-289`). There is **no** bus-arbitration, priority, SCR or bridge model |
| MEM-READ-CONTRACT | "Reads must be side-effect-free and remain mapped between E1 validation and E3 sampling" | header contract, `cdj_c674x.h:73-74` | — | partial | `cdj_c674x.c:1653-1656` (E1 probe) and `2477-2483` (E3 sample) | reference-backed-tests (for the CPU half) | approximation | the contract is a *requirement*, and at least three implemented device registers already break it: Timer64P TIM12/TIM34 (read-reset and the TIM34 shadow, `cdj_c6747_timer.c:49-64`), SPI SPIBUF 0x40 (`consume_receive`, `cdj_c6747_spi.c:236-244`) and SPIINTVEC0 0x64 (`interrupt_vector`, 199-214). Probe shows the E1 read applies the side effect and the E3 read then returns 0 (§6, finding F3). The header's "read-clear registers require a future bus transaction API" is the correct fix and is not yet built |
| MEM-MAP | C6747 address map visible to the DSP | SPRUH91D 2.3 and 4.1 **defer to the device data manual**; sizes/defaults in 4.2 | 74, 80 | partial | `tools/cdj_dsp/replay.c:333-385`, `emulator/qemu/cdj2000_nxs_hpi.c:30-36` | firmware-observation-only | approximation | mapped: L2 RAM 256 KB at 0x11800000 with local alias 0x00800000, shared RAM 128 KB at 0x80000000, EMIFB SDRAM 32 MB at 0xC0000000 gated by SDCFG.SDREN, plus the MMIO windows. Unmapped → read/write fails → CPU faults (fail-closed). **Not mapped:** L1P 0x00E00000/0x11E00000, L1D 0x00F00000/0x11F00000, L2 ROM 0x00700000, EMIFA, and every deferred peripheral window. **The map itself is not verifiable from either indexed reference** — SPRUH91D printed page 74 says "Refer to your device-specific data manual"; the addresses come from SPRS377F, which is not in `build/references/` |
| MEM-L2-SIZES | L1P 32 KB, L1D 32 KB, L2 256 KB RAM + 1024 KB ROM; defaults 32 KB cache / 32 KB cache / all-RAM | SPRUH91D 4.2 | 80 | supported | `cdj_c6747_cache.c:17-25` | reference-backed-tests | strict | reset `l1pcfg=7`, `l1dcfg=7`, `l2cfg=0` exactly matches printed page 80. L2 ROM is not mapped; replay enters after the boot ROM |
| MEM-CACHE-REGS | L2CFG, L1PCFG, L1PCC, L1DCFG, L1DCC, block-base + word-count pairs, global coherence ops, MAR0-255 | SPRUFK5A (ch 2-4) register map; SPRUH91D 4.2 for sizes only | 80 (sizes only) | partial | `cdj_c6747_cache.c` (whole file, 4141 bytes) | reference-backed-tests | approximation | register **storage** plus acceptance rules. Block-base registers are write-only; word-count and global-operation registers read 0 ("operation complete"); MAR is restricted to indices 64-103, 128, 192-223. The field widths (`l1pcfg & 7`, `l2cfg & 0xf`) and the whole register map come from **SPRUFK5A, which is not in `build/references/`**; the MAR index range comes from SPRS377F, also absent. Test `tests/cstub/c6747-cache.c` (45 lines) checks resets, masks, write-only-ness and the MAR range, nothing else |
| MEM-CACHE-BEHAVIOUR | Cache allocation, line fill, writeback, invalidate, L1/L2/external coherence, cache-miss timing | SPRUFK5A; SPRUH91D 5.2.5 references DSP L1/L2 cache controller accesses | 87 | unsupported | — | untested | — | **There is no cache model.** No lines, no tags, no allocation, no writeback, no eviction, no coherence, no miss penalty, no stall. Every coherence command is a no-op justified by the comment "Unified backing memory is already coherent" (`cdj_c6747_cache.c:124`) and the header's "this is a functional abstraction, not a cache/timing model". MAR cacheability bits have no effect on anything. Backing memory is a flat array, so coherence is trivially true and unobservable. `DSP_BOOT_MILESTONE_AUDIT.md:69` is correct to say cache timing is not modelled — but the absence is total, not just timing |
| PER-EDMA3CC | EDMA3 channel controller: PaRAM, ER/ESR/CER/EER/SER/IER/IPR/EMR, QER/QEER/QSER/QEMR, DRAE/QRAE, DMAQNUM/QDMAQNUM, QCHMAP, CCERR, shadow regions, A/AB sync, linking, chaining, STATIC | SPRUH91D ch 16 | 436-569; PID value 4001 5300h verified | supported | `cdj_c6747_edma.c` (19805 bytes) | reference-backed-tests | approximation | real functional transfers with linking, intermediate/final chaining and interrupts; 32 channels, 8 QDMA, 128 PaRAM sets, 4 shadow regions. **Excluded:** constant addressing (`OPT_SAM`/`OPT_DAM` rejected outright), event-queue arbitration and priority, transfer-controller timing (transfers complete atomically at event time), QWMTHRA watermark behaviour (stored only), CCSTAT/queue-status registers read 0, and the read aliases at 0x300/0x308/0x310/0x314/0x318/0x31c are a deliberate firmware-compatibility readback the code itself flags as "not a claim that physical silicon exposes them" |
| PER-EDMA3TC | EDMA3 transfer controllers TC0/TC1 and their registers | SPRUH91D ch 16 + Table 3-1 | 436, 77 | unsupported | — | not-assessed | — | no TC model, no TCCFG, no TC status/error registers, no burst size, no FIFO, no read/write controller, no destination FIFO. Transfers are instantaneous |
| PER-INTC-DEV | DSP INTC device registers: EVTFLAG0-3, EVTSET0-3, EVTCLR0-3, EVTMASK0-3, MEVTFLAG0-3, EXPMASK0-3, MEXPFLAG0-3, INTMUX1-3, event combiner | SPRUFK5A ch 7; SPRUH91D 4.2 names the INTC as DSP-internal | 80 | partial | `cdj_c6747_intc.c` (5771 bytes) | reference-backed-tests | approximation | reset INTMUX 0x07060504/0x0b0a0908/0x0f0e0d0c (CPUINT4..15 select same-numbered events); events 0-3 permanently masked combiner outputs; exceptions reset fully masked. **Missing:** INTXSTAT/INTXCLR, INTDMASK, AEGMUX0/1, and any exception/NMI delivery path — MEXPFLAG is readable but nothing consumes it. The **event-number-to-peripheral map** is hardcoded in `tools/cdj_dsp/replay.c` (EDMA channels 3/5 for McASP1/2 TX, INTC event 8 for EDMA region 1) and comes from SPRS377F, not from either indexed reference. Track 3 owns the CPU side |
| PER-TIMER64P | Timer64P0/1 registers: REVID, EMUMGT, GPINT/GPEN, GPDAT/GPDIR, TIM12/34, PRD12/34, TCR, TGCR, WDTCR, REL12/34, CAP12/34, INTCTLSTAT, CMP0-7 | SPRUH91D ch 28 | 1226-1259; REVID 4472 020Ch p1247; TCR p1252 | partial | `cdj_c6747_timer.c` (4829 bytes) | reference-backed-tests | approximation | **full register set, zero time base.** TIM12/TIM34 never increment; there is no tick function, no period compare, no reload, no interrupt or EDMA event generation, no watchdog timeout, no CAP capture, no CMP output, no clock source selection effect. What *is* modelled and manual-checked: the TCR write mask 0x04C03FFE (verified field-by-field against Figure 28-21, printed page 1252), TGCR mask 0xFF1F, read-reset mode, the TIM34 read shadow for coherent 64-bit reads, INTCTLSTAT W1C status vs RW enables. `tests/cstub/c6747-timer.c` (73 lines) asserts those with manual-derived masks |
| PER-PLLC | PLL0 controller: PLLCTL, OCSEL, PLLM, PREDIV, PLLDIV1-3, OSCDIV, POSTDIV, PLLDIV4-7, PLLCMD, PLLSTAT | SPRUH91D ch 7 | 115-138 | partial | `cdj_c6747_pll.c` (6596 bytes) | reference-backed-tests | approximation | models reset-hold timing, lock wait (ceil(2000·N/sqrt(M)) OSCIN periods from SPRS377F Table 6-4 — again a non-indexed reference), GO divider latency (8 ticks) and a fractional OSCIN phase accumulator driven from `cycle_tick`. **Missing:** REVID, RSTYPE, ALNCTL, DCHANGE, CKEN, CKSTAT, SYSTAT, EMUCNT0/1, and **PLL1 entirely** (SPRUH91D printed page 8602-context names PLL1; the EMIFB/MMC clock domain has no model). PLLSTAT STABLE is asserted unconditionally ("assume it completed before ROM handoff"). The input frequency 16.9344 MHz is a board constant, not a device property |
| PER-PSC | PSC0/PSC1: MDSTAT, MDCTL, PTCMD, PTSTAT; populated-module and always-enabled masks | SPRUH91D ch 8, Tables 8-1/8-2/8-14/8-22 | 139-163 | partial | `cdj_c6747_psc.c` (4153 bytes) | reference-backed-tests | approximation | module state transitions with a fixed 8-tick domain latency. **Excluded:** auto-sleep/wake, FORCE, emulation interrupts, DSP self-reset, EPCPR/EPCCR, INTEVAL, PDSTAT/PDCTL as real registers, and the LRSTDONE/MRST/MCKOUT bits are "logical signal levels only". PTCMD reads 0 — the code flags that as "an explicit, unmeasured bus assumption" |
| PER-SYSCFG | KICK0R/KICK1R, MSTPRI0-2, PINMUX0-19, CFGCHIP0-4 | SPRUH91D ch 10 Table 10-1 | 172 | partial | `cdj_c6747_syscfg.c` (5221 bytes) | reference-backed-tests | approximation | the kicker unlock sequence (83E7 0B13h / 95A4 F1E0h, printed page 173) is modelled, including the "locked writes complete on the bus but change nothing" rule. **Missing from Table 10-1:** REVID, DIEIDR0-3, DEVIDR0, BOOTCFG, CHIPREVID, **HOST1CFG (0x44)**, IRAWSTAT/IENSTAT/IENSET/IENCLR/EOI/FLTADDRR/FLTSTAT (0xE0-0xF8), SUSPSRC (0x170), **CHIPSIG / CHIPSIG_CLR (0x174/0x178)**. `cdj_c674x_reset` cites HOST1CFG as the source of the ISTP reset value 0x00700000 but the register itself is unwritable, so firmware cannot relocate the IST base through it. AMUTE clear pulses are bookkeeping only |
| PER-EMIFB | REVID, SDCFG, SDRFC, SDTIM1, SDTIM2, SDCFG2, BPRIO | SPRUH91D ch 19 Table 19-23 | 809; REVID 4033 131Fh p809 | partial | `cdj_c6747_emifb.c` (3803 bytes) | reference-backed-tests | approximation | 7 of 15 registers. Good field-level validation: BOOT_UNLOCK/TIMUNLOCK protection, CL 2-3, IBANK <= 2, PAGESIZE <= 3, SDCFG2 PASR/ROWSIZE legality, SDRFC minimum-rate substitution. **Missing:** PC1, PC2, PCC, PCMRS, PCT (performance counters) and IRR/IMR/IMSR/IMCR (interrupt/error). **Behaviour:** SDRAM is a flat 32 MB array gated only by SDCFG.SDREN. No refresh, no CAS latency effect, no bank/page/row timing, no initialization sequence requirement, no arbitration. `runs/dsp-circular-transcript-replay-1/gate.json` states this itself: "SDRAM command timing, arbitration and retention are not modeled" |
| PER-EMIFA | EMIFA: async ASRAM/NOR/NAND (up to 4 CS), 4-bit NAND ECC, 16-bit SDRAM, 128 MB space | SPRUH91D ch 18; sizes 4.2 | 694-780, 80 | unsupported | — | not-assessed | — | **no model and no address mapping.** Any DSP access to an EMIFA window is an unmapped-access fault. NAND ECC is part of this chapter and is likewise absent |
| PER-SPI | SPI0/SPI1: GCR0/1, INT/LVL, FLG, PINFNC/DIR/PINS, DAT0/1, BUF, EMU, DELAY, DEFAULTCS, FMT0-3, INTVEC0 | SPRUH91D ch 27 | 1171-1225 | supported | `cdj_c6747_spi.c` (22817 bytes) | reference-backed-tests | approximation | deepest peripheral after EDMA; has a half-tick serial-clock model (`spi-timed`/`spi-clock` tests) for the SPI1/WM8740 path. Gates record "SPI1 WM8740 control transfers complete at commit; serial timing is not modeled" for functional mode. SPIBUF and SPIINTVEC0 reads have side effects — see MEM-READ-CONTRACT. Deeper SPI/DAC assessment belongs to the audio-path track |
| PER-MCASP-PINS | PFUNC, PDIR, PDOUT, PDSET, PDCLR per instance; 16/12/4 serializers for McASP0/1/2 | SPRUH91D ch 24 Table 24-7; counts from SPRS377F Table 6-43 | 1036 | supported | `cdj_c6747_mcasp.c:5-57` | reference-backed-tests | approximation | pin masks 0xFE00FFFF / 0xFE000FFF / 0xFE00000F. REV (0x0) is **not** implemented; PDIN (0x1C read) is explicitly refused — "external inputs are not modeled". Writes update the output latch regardless of PDIR; the code flags that interpretation as "not board-measured" |
| PER-MCASP-TXREGS | GBLCTL/RGBLCTL/XGBLCTL, AMUTE, DLBCTL, DITCTL, XMASK, XFMT, AFSXCTL, ACLKXCTL, AHCLKXCTL, XTDM, XINTCTL, XSTAT, XSLOT, XCLKCHK, XEVTCTL, DITCSRA/B + DITUDRA/B, SRCTL0-15, XBUF0-15, DMA port 0x2000 | SPRUH91D ch 24 Tables 24-7/24-8 | 1036-1039 | supported | `cdj_c6747_mcasp.c:59-470` | reference-backed-tests | strict (register level) | I verified three reset/field claims against the manual directly: ACLKXCTL reset 0x60 (ASYNC=1, CLKXM=1, printed page 1077), AHCLKXCTL reset 0x8000 (HCLKXM=1, printed page 1078 — including the reserved 13:12 hole in the 0xCFFF write mask), and XFMT.XSSZ legality = odd and >= 3 (printed page 1075 table, values 0-2h/4h/6h/8h/Ah/Ch/Eh reserved). XBUSEL bit 3 port selection matches the printed page 1075 description, including "writes through the unselected port are ignored, not rejected". `tests/cstub/c6747-mcasp.c` (488 lines) is a genuine register-semantics suite with manual-derived expected values |
| PER-MCASP-RXREGS | RMASK, RFMT, AFSRCTL, ACLKRCTL, AHCLKRCTL, RTDM, RINTCTL, RSTAT, RSLOT, RCLKCHK, REVTCTL (0x64-0x8C), RBUF0-15 (0x280-0x2BC), RBUF DMA port read | SPRUH91D ch 24 Table 24-7/24-8 | 1036, 1038-1039 | unsupported | — | untested | — | **the entire receive side is absent.** Every offset 0x64-0x8C and 0x280-0x2BC is unmapped, and a DSP read of the 0x2000 DMA port is unmapped (only writes are handled). RGBLCTL is accepted because it aliases GBLCTL, but no receive state exists behind it. Any firmware that configures McASP receive faults |
| PER-MCASP-AFIFO | AFIFOREV, WFIFOCTL, WFIFOSTS, RFIFOCTL, RFIFOSTS at offset 0x1000 | SPRUH91D ch 24 Table 24-9 | 1039 | unsupported | — | untested | — | **no AFIFO model.** Offset 0x1000 is in `locate()`'s range but has no case, so it faults. The AFIFO is the normal way EDMA feeds McASP on this device |
| PER-MCASP-FORMAT | Transmit format unit: XMASK, XFMT.XROT rotation, XPAD/XPBIT padding, XRVRS bit reversal, XSSZ slot size | SPRUH91D ch 24, 24.1.26-24.1.27 | 1074-1075 | unsupported | `cdj_c6747_mcasp.c:299-304` | untested | — | XMASK/XFMT are stored and validated but **have no effect on data**. `tx_slot` copies XBUF to XRSR verbatim: `s->xrsr[b][n] = s->xbuf[b][n]`. A 16-bit slot with rotation, padding or masking would produce the wrong bits if anything ever serialized them |
| PER-MCASP-CLOCK | ACLKX/AHCLKX generation: CLKXDIV (divide 1-32), HCLKXDIV (divide 1-4096), frame sync from AFSXCTL, XCLKCHK | SPRUH91D 24.1.28-24.1.30, 24.1.35 | 1075-1079 | unsupported | `cdj_c6747_mcasp.c:439-441` (stored only); slot driver `tools/cdj_dsp/replay.c:597-653` | firmware-observation-only | approximation | **CLKXDIV and HCLKXDIV are stored and never used.** There is no bit clock, no high-frequency clock, no frame sync timing and no sample rate anywhere in the model. A transmit slot boundary is advanced by `advance_functional_mcasp_slots()` **once every 1024 executed DSP packets** (`CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL`, `cdj_dsp_checkpoint.h:30`). The capture record labels its own clock `"functional-coarse-packet-slot"`. `runs/dsp-circular-transcript-replay-1/gate.json` lists exactly this: "functional McASP scheduling advances one slot every 1024 executed DSP packets; not serializer-clock, sample-rate, or audio-output evidence" |
| PER-MCASP-OUTPUT | Serialized audio reaching AXR pins / an external DAC | SPRUH91D 24.0.21 | 981-1035 | unsupported | — | untested | — | **nothing leaves the McASP.** XRSR is the end of the chain; there is no pin, no external device, no bit stream. Confirms that the PCM evidence in `PCM_EXECUTION_EVIDENCE.md` cannot and does not establish McASP output — and that document says so itself (printed line 68: "Unproven: … McASP output"). `runs/nxs-pcm-observe-4/gate.json` has an **empty** approximations list, which means the McASP-slot approximation was not active in that run at all: the strict PCM evidence was collected with the McASP clock stopped |
| PER-HPI | C6747 HPI peripheral registers: REVID (0x00), HPIC (0x30), HPIAW/HPIAR, power/emulation | SPRUH91D ch 21 | 857-889; REVID 4421 210Ah p872 | partial | `cdj_c6747_hpi.c` (2423 bytes) | reference-backed-tests | approximation | only REVID and HPIC from the DSP side, plus a host-side HPIC view. HPIA/HPID as peripheral registers are absent. The MAIN-facing transport (HPIC/HPIA/HPID-autoincrement/HPID-fixed windows at 0x0C000000 in the SH-4 map) lives in `cdj2000_nxs_hpi.c` and is exercised by `tests/test_nxs_hpi.py` through real QEMU qtest. The gate text records "physical HPI pins, FIFO/HRDY timing and DSP interrupt delivery are not modeled" |
| PER-I2C | I2C0/I2C1 | SPRUH91D ch 22 | 890-930 | partial | `cdj_c6747_i2c.c` (2143 bytes) | reference-backed-tests | approximation | **only the GPIO-mode pin registers**: ICMDR (0x24) reset/nGPIO gating, ICPFUNC (0x48), ICPDIR (0x4C), ICPDOUT (0x54), ICPDSET (0x58), ICPDCLR (0x5C). No I2C protocol at all: ICOAR, ICIMR, ICSTR, ICCLKL/H, ICCNT, ICDRR/ICDXR, ICSAR, ICIVR, ICEMDR, ICPSC are unmapped. No bus, no slave, no ACK, no arbitration, no interrupts. Firmware can only bit-bang SCL/SDA through the pin registers |
| PER-GPIO | GPIO banks 0-7 (4 register pairs): DIR, OUT_DATA, SET_DATA, CLR_DATA, IN_DATA, SET/CLR_RIS_TRIG, SET/CLR_FAL_TRIG, BINTEN | SPRUH91D ch 20 | 824-856 | partial | `cdj_c6747_gpio.c` (2695 bytes) | reference-backed-tests | approximation | DIR resets to all-input (1 = input, opposite of McASP — the comment flags this). **Missing:** REVID (0x00), **INTSTAT** (explicitly refused: "INTSTAT needs edge/event state, not a guessed zero value"), and bank 8 (correctly absent for C6747). No interrupt generation, no edge detection, no PINMUX gating of physical drive |
| PER-MPU | Memory Protection Unit: privilege levels, fixed + programmable ranges, MPPA permissions, FLTADDRR/FLTSTAT, protection faults | SPRUH91D ch 5 | 82-102 | unsupported | — | not-assessed | — | no MPU. No privilege enforcement anywhere: SYSCFG "privileged mode" registers are reachable from any code, and `cdj_c674x.c:2427-2432` notes that TSR privilege fields "need a later execution-mode model" |
| DEFER-EMAC | EMAC/MDIO | SPRUH91D ch 17 | 570-693 | unsupported | — | not-assessed | — | no model. The DSP-side Ethernet is absent (MAIN-side networking is a different subsystem and another track's concern) |
| DEFER-MMCSD | MMC/SD controller | SPRUH91D ch 25 | 1094-1146 | unsupported | — | not-assessed | — | no model |
| DEFER-UART | UART0, UART1, UART2 | SPRUH91D ch 29 | 1260-1292 | unsupported | — | not-assessed | — | no model for any of the three |
| DEFER-USB | USB2.0 controller and USB1.1 OHCI host | SPRUH91D ch 30, ch 31 | 1293-1318, 1319-1434 | unsupported | — | not-assessed | — | no model. Both are system bus masters per Table 3-1 (printed page 77) |
| DEFER-LCDC | LCD controller (C6747 only, not C6745) | SPRUH91D ch 23 | 931-980 | unsupported | — | not-assessed | — | no model. Also an EMIFB master per Table 3-1 |
| DEFER-RTC | Real-Time Clock | SPRUH91D ch 26 | 1147-1170 | unsupported | — | not-assessed | — | no model |
| DEFER-PRUSS | PRU subsystem: PRU0, PRU1, PRU RAM0/RAM1, PRU Config | SPRUH91D ch 12; Table 3-1 | 241-242, 77 | unsupported | — | not-assessed | — | no model. PRU0/PRU1 are full system bus masters with access to EMIFA, EMIFB, shared RAM and the peripheral group |
| DEFER-MOTORCTL | eCAP0-2, eHRPWM0-2, eQEP | SPRUH91D ch 13, 14, 15 | 243-281, 282-395, 396-435 | unsupported | — | not-assessed | — | no models. Grouped because they share a likely-unused role on this product, not because they are one peripheral |
| DEFER-PWRMGT | Power Management (ch 9) and Device Clocking (ch 6) as models | SPRUH91D ch 6, ch 9 | 103-114, 164-169 | unsupported | — | not-assessed | — | no power-domain or clock-tree model beyond PSC module states and PLL0. CPU power-down (CSR.PWRD) is explicitly ignored (`cdj_c674x.c:2400-2403`) |
| DEFER-MEGAMOD | Megamodule internal peripherals other than INTC: IDMA, BWM (bandwidth manager), PDC (power-down controller) | SPRUH91D 4.2 names all four | 80 | unsupported | — | not-assessed | — | no IDMA (channel 0/1 L1↔L2 transfers), no bandwidth manager, no power-down controller. IDMA is a plausible firmware tool for exactly the L1/L2 traffic this model cannot express |
| DEFER-BOOT | Boot Considerations (boot ROM, boot modes, AIS) | SPRUH91D ch 11 | 239-240 | unsupported | `cdj_c6747_hpi.c:14-21` (handoff only) | firmware-observation-only | approximation | the boot ROM is **not executed**. `cdj_c6747_hpi_rom_boot_ready` models the post-reset HPI-boot handoff per SPRABB1C 4.1 (a third reference not in `build/references/`). Every gate lists "DSP boot ROM is not executed; its documented HPI-ready handoff is modeled" |
| NOTEXIST-C6747 | Peripherals that do **not** exist on C6745/C6747 and are therefore correctly absent | SPRUH91D chapter list (31 chapters) + Table 3-1 | 2-17, 77 | n/a | — | n/a | n/a | SPRUH91D has no chapter for uPP, VPIF, DDR2/mDDR EMIF, McBSP, SATA, PWM (it is eHRPWM), or a standalone ECC module. EMIFB **is** the SDRAM interface on this device; EMIFA carries the 4-bit NAND ECC. LCDC and McASP2 exist on C6747 but not C6745. Do not record these as coverage gaps |

## 3. Findings

**F1 — there is no cache model of any kind, only register storage.** (`emulator/qemu/cdj_c6747_cache.c`,
4141 bytes.) L2CFG/L1PCFG/L1PCC/L1DCFG/L1DCC, seven block-base registers, the
word-count and global-operation registers, and MAR0-255 are stored or stubbed.
Every coherence operation returns "complete" immediately; MAR cacheability bits
affect nothing; there are no lines, tags, allocation, writeback, eviction, miss
penalty or stall. The reset configuration (L1P 32 KB cache, L1D 32 KB cache, L2
all RAM) is correct per SPRUH91D printed page 80, so firmware that only reads
back its configuration is satisfied. Firmware that depends on an actual
invalidate/writeback for correctness would also pass here — but only because the
backing store is a single flat array, which makes coherence vacuously true rather
than modelled. Firmware/audio impact: no cache-miss timing, so all timing
evidence from this emulator is timing of a zero-latency memory system. Any claim
about DSP throughput, buffer deadlines or audio headroom inherits that.

**F2 — the McASP has no clock, no format unit, no receive side and no output;
its "audio rate" is one TDM slot per 1024 executed DSP packets.** CLKXDIV
(divide-by-1..32, SPRUH91D printed page 1077) and HCLKXDIV (divide-by-1..4096,
printed page 1078) are stored and never read. `cdj_c6747_mcasp_tx_slot` is driven
by `advance_functional_mcasp_slots()` in `tools/cdj_dsp/replay.c:597-653` at
`cpu.packets % 1024 == 0`. The transmit format unit is absent: `tx_slot` copies
XBUF to XRSR verbatim, so XSSZ, XROT, XPAD, XPBIT, XRVRS and XMASK have no effect
on the data. Offsets 0x64-0x8C (all receive control), 0x280-0x2BC (RBUF), 0x1000
(the whole AFIFO block, SPRUH91D Table 24-9, printed page 1039) and DMA-port reads
are unmapped. Audio impact: the emulator can show that firmware *wrote* the right
words into XBUF in the right order, and nothing more. It cannot show sample rate,
slot size, channel assignment, bit alignment, underrun timing, or that any audio
would be produced. This is consistent with, and is the mechanism behind,
`PCM_EXECUTION_EVIDENCE.md`'s own "Unproven: … McASP output".

**F3 — suspected bug: device registers with read side effects are double-applied
by the E1 validation read, and the load returns the post-side-effect value.**
`cdj_c674x.c:1653-1656` performs a full `read_transfer` at E1 purely to validate
mapping, and `cdj_c674x.c:2477-2483` performs a second `read_transfer` at E3 to
sample. `cdj_c674x.h:73-74` documents the resulting requirement ("Reads must be
side-effect-free and remain mapped between E1 validation and E3 sampling.
Read-clear registers require a future bus transaction API") — but three
*already-implemented* registers violate it and are reachable through
`tools/cdj_dsp/replay.c:344-366`:
- Timer64P TIM12 with TCR.READRSTMODE12=1, and the TIM34 read shadow
  (`cdj_c6747_timer.c:49-64`);
- SPI SPIBUF at offset 0x40, which calls `consume_receive` and clears the
  per-character receive status (`cdj_c6747_spi.c:236-244`);
- SPI SPIINTVEC0 at offset 0x64, which clears flag bits and may call
  `consume_receive` (`cdj_c6747_spi.c:199-214`).

Demonstrated (§6, probe 2): one `LDW .D2 *+B10[0],B2` with B10 = TIMER0 + 0x10
invokes the read callback twice; `TIM12` is zeroed by the E1 read and the
destination register receives 0 instead of 0x11223344. Reachability caveat: I did
not establish that the CDJ firmware uses read-reset mode or reads SPIBUF with a
CPU load — the SPI1/WM8740 path is transmit-only in practice. The defect is in the
contract, and it is latent rather than proven live.

**F4 — circular-buffer validation is asymmetric.** The manual's two
undefined-behaviour cases around circular block size are handled differently. A
nonaligned transfer in a buffer smaller than 32 bytes fails closed
(`cdj_c674x.c:1639-1641`, matching SPRUFE8B printed page 89 "will cause undefined
results"). An **aligned** transfer wider than the block size does not: probe 3
shows an LDW inside a 2-byte circular block is accepted and reads four linear
bytes, escaping the circle. SPRUFE8B printed page 36 requires the buffer to be
"aligned on a byte boundary equal to the block size", which implies the programmer
never creates this case; but given that the sibling case fails closed, silently
degrading here is inconsistent. Low firmware impact (the TI compiler would not
emit it), recorded for completeness.

**F5 — SPRUFE8B 3.8.5's side/register-file constraints on parallel loads and
stores are not enforced.** Printed page 80: "Two load and store instructions using
a destination/source from the same register file cannot be issued in the same
execute packet. The address register must be on the same side as the .D unit
used." Only the nonaligned-parallel rule and same-register write conflicts are
checked. The address-register side *is* structurally enforced (the base register
bank comes from the `y` bit), but the data-register-file restriction is not. A
packet the TI assembler would reject is accepted here. Low impact for real
firmware; matters if hand-built or corrupted packets are ever executed.

**F6 — Timer64P is register-complete and behaviour-empty.** Every register in
SPRUH91D chapter 28 is present with manual-verified masks (I checked TCR's
0x04C03FFE field by field against Figure 28-21, printed page 1252, and the REVID
value 4472 020Ch against printed page 1247). Nothing counts. There is no tick
function at all — `grep tick cdj_c6747_timer.c` is empty. Impact: firmware cannot
measure elapsed time, cannot get a periodic interrupt, and a watchdog can never
fire. Any firmware timing loop that polls TIM12 spins forever. This is a plausible
cause of future "firmware hangs in a wait loop" investigations.

**F7 — the memory map, every peripheral base address, the MAR cacheable-region
list, the cache register map and the EDMA event map are not verifiable from either
indexed reference.** SPRUH91D printed page 74 (2.3) and printed page 80 (4.1) both
say "refer to your device-specific data manual". The implementation's addresses
come from SPRS377F (device data manual) and SPRUFK5A (C674x Megamodule Reference
Guide), and the boot handoff from SPRABB1C — none of which are in
`build/references/`. Four REVID/PID constants that *are* in SPRUH91D all check out
(Timer 4472 020Ch p1247, EMIFB 4033 131Fh p809, HPI 4421 210Ah p872, EDMA3CC
4001 5300h p456-context), which raises confidence that the rest was transcribed
from a real document rather than invented — but this track cannot certify the
address map. Recommendation: add SPRS377F and SPRUFK5A to the provenance set
before any claim of device-map fidelity.

**F8 — EMIFA is absent and unmapped, so a large part of the device's external
memory space faults.** SPRUH91D chapter 18 (printed pages 694-780) plus the
summary on printed page 80 describe async ASRAM/NOR/NAND with 4-bit ECC and a
16-bit SDRAM interface with a 128 MB space. There is no model and no address
window, so any DSP access faults as unmapped. Fail-closed is the right default, but
the consequence should be stated plainly: only EMIFB SDRAM and on-chip memory are
reachable.

**F9 — the EDMA3 model is the channel controller only; transfers are
instantaneous.** `cdj_c6747_edma.c` is a genuine functional EDMA3CC (PaRAM,
A/AB sync, linking, intermediate/final chaining and completion interrupts, shadow
regions with DRAE/QRAE masking, STATIC suppression) with careful validation and a
staged-write preview so that a failed transfer leaves no partial state. But there
is no transfer controller, no event-queue arbitration or priority, no burst size
and no latency: an event completes the whole frame before the issuing bus write
returns. Constant addressing (OPT.SAM/DAM) is rejected outright rather than
modelled. Audio impact: EDMA-driven buffer timing — the thing that determines
whether an audio deadline is met — is not modelled at all.

**F10 — no MPU and no privilege model.** SPRUH91D chapter 5 and SYSCFG's
"Privileged mode" access column (Table 10-1, printed page 172) both assume a
Supervisor/User split. There is none: SYSCFG privileged registers are reachable
from any code, no protection range is checked, no protection fault can be raised,
and `cdj_c674x.c:2427-2432` explicitly defers TSR privilege fields. Any firmware
that relies on a protection fault, or that is expected to *fail* an illegal access,
will not.

## 4. Stale documentation

**S1 — `emulator/qemu/cdj_c6747_mcasp.h:7-9`** claims "No physical pin routing,
external input, serializer clock progression, FIFO or interrupt delivery yet."
"Serializer clock progression" is now partly wrong: `cdj_c6747_mcasp_tx_slot`
(added later, documented further down the same header) advances XSLOT, consumes
XBUF into XRSR, sets underrun and re-arms AXEVT. Current truth: there is no
*clock*, but there *is* caller-timed slot progression. The header's own later
comment on `tx_slot` is accurate; the file-top summary was not updated with it. Low
severity, but the top comment is the first thing a reader sees and it understates
what the model does.

**S2 — `PCM_EXECUTION_EVIDENCE.md:70`** reports "Validation: 492 passed / 29
optional skips". The task brief records the latest full regression as 506 passed /
31 skipped. The document's count is a snapshot, not a standing claim, but it reads
as current. (Cross-track: the regression-count claim is not mine to fix.)

Not stale, and worth saying so explicitly because it is unusual: **`DSP_BOOT_MILESTONE_AUDIT.md:69`**
("cache timing, inactive audio clocks and other documented model limitations
remain") and **`PCM_EXECUTION_EVIDENCE.md:68`** ("Unproven: … McASP output") are
both accurate and, if anything, understate the gap — cache is absent rather than
merely untimed. The three gate files I inspected
(`runs/dsp-circular-transcript-replay-1/gate.json`,
`runs/nxs-pcm-observe-4/gate.json`,
`build/performance/09-combined/final-4/gate.json`) carry explicit
`architectural_validation_eligible` flags and approximation lists that name the
1024-packet McASP slot rate and the unmodelled SDRAM timing. `nxs-pcm-observe-4`
is `architectural_validation_eligible: true` with an **empty** approximations
list, which is itself the evidence that the strict PCM run used a stopped McASP
clock.

## 5. Open questions

1. The memory map, peripheral base addresses, MAR cacheable-region list, cache
   register map and the EDMA3/INTC event-number map cannot be verified from
   SPRUFE8B or SPRUH91D. SPRS377F and SPRUFK5A need to be added to
   `build/references/` before any of those can be called reference-backed.
2. Does the real firmware ever read Timer64P TIM12 with READRSTMODE set, or read
   SPIBUF/SPIINTVEC0 with a CPU load? That decides whether F3 is latent or live.
   Answering it needs a firmware-trace search, which is another track's territory.
3. Is the E1-captured circular width for nonaligned transfers actually what the
   hardware does? The manual does not say. It is the only self-consistent reading
   given E1 address formation, but it is an inference.
4. Does any firmware path configure McASP receive, the AFIFO, or the DMA-port
   read? Each would fault immediately today, so a clean boot is weak evidence that
   they are unused — it may just mean that path has not been reached.
5. PSC's 8-tick domain latency, PLL's 8-tick GO latency and the 1024-packet McASP
   slot are all unmeasured constants. Are any of them chosen to match an observed
   firmware timing, or are they arbitrary? The comments say "deterministic
   approximations" without saying what they approximate.
6. EDMA3 `transfer_bytes` ignores CIDX for the non-final A-sync path in one branch
   and uses `p[6]` (CIDX) in others; I read the logic as matching Table 16-3 but
   did not build a test that pins every sync/count combination against the manual's
   worked examples. An acceptance test over {A,AB} x {ACNT,BCNT,CCNT} x
   {STATIC, link, null-link} with hand-computed PaRAM updates would close this.

## 6. Commands run, with results

```sh
.venv/bin/python -m tools.cdj_dsp.refdocs --check
# sprufe8b: 771 pdf pages, 739 footers; spruh91d: 1473 pdf pages, 1435 footers
# provenance -> build/references/provenance.json
```

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools \
C6X_TI_BIN=/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin \
  .venv/bin/python -m pytest -q tests/test_c674x_circular.py tests/test_c674x.py \
      tests/test_c6747_spi_clock.py tests/test_c6747_spi_timed.py tests/test_nxs_hpi.py
# 21 passed in 3.59s  (no skips: the custom QEMU build is present, so
# test_nxs_hpi really ran)
```

The 17 tests inside `tests/test_c674x.py` are: `test_c6747_spi_registers`,
`test_c6747_cache_registers`, `test_c6747_edma_registers_and_transfers`,
`test_c6747_timer64p_registers`, `test_c6747_interrupt_controller`,
`test_c6747_emifb_configuration`, `test_c6747_hpi_control`,
`test_dsp_checkpoint_round_trip`, `test_c6747_pll_cycle_clock`,
`test_c6747_pll_configuration`, `test_c6747_i2c_gpio_mode`,
`test_c6747_gpio_registers`, `test_c6747_mcasp_pin_registers`,
`test_c6747_psc_transitions`, `test_c674x_packets_and_branch_delays`,
`test_c674x_loop_schedule`, `test_c6747_syscfg_unlock_and_pipeline`.

### Probe 1 — SPRUFE8B circular-addressing examples, expected values taken verbatim from the manual

The repo's own `tests/cstub/c674x-circular.c` re-implements the wrap rule in a
local `wrap()` helper. That is a faithful transcription of SPRUFE8B printed page
88 ("only bits N through 0 of the result are updated"), so its expected values are
*independent of the emulator's output* — but they are a parallel implementation of
the same rule, so a shared misreading would pass. The manual's three worked
examples, with their own printed result values, are not pinned by any test. I
reproduced all three.

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cc -std=c11 -Wall -Wextra \
  -I emulator/qemu <scratch>/probe_manual_examples.c \
  emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c -o <scratch>/probe
<scratch>/probe
# ex3-4 after E1: A4=00000104 (manual 00000104)      LDW .D1 *++A4[9],A1
# ex3-4 after E5: A1=12345678 (manual 12345678)
# ex3-5 A4=00000106 (manual 00000106)                ADDAH .D1 A4,A1,A4
# ex3-6 after E1: A4=00000022 (manual 00000022)      LDNW .D1 *++A4[2],A1
# ex3-6 after E5: A1=56789abc (manual 56789ABC)
# all three SPRUFE8B circular examples reproduced
```

Examples 3-4 and 3-5 are on printed page 88 (PDF page 88); Example 3-6 is on
printed page 89 (PDF page 89). AMR = 0004 0001h in all three. This is genuine
reference-backed evidence: the expected values are the manual's own figures, not
the emulator's output and not a re-derivation. It would be worth adding to
`tests/cstub/c674x-circular.c` — I did not, because this track is read-only.

### Probe 2 — E1/E3 double read against the real Timer64P model (finding F3)

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cc -std=c11 -Wall -Wextra \
  -I emulator/qemu <scratch>/probe_double_read.c emulator/qemu/cdj_c674x.c \
  emulator/qemu/cdj_c674x_loop.c emulator/qemu/cdj_c6747_timer.c -o <scratch>/probe2
<scratch>/probe2
# after E1: read callbacks=1  TIM12=0x00000000
# after E5: read callbacks=2  TIM12=0x00000000  B2=0x00000000
# LOST: E1 validation read already applied the read-reset side effect
```
TIM12 was preset to 0x11223344 with TCR.READRSTMODE12=1 and TGCR=0x17. One LDW
produces two calls into `cdj_c6747_timers_read`; the first zeroes the counter and
the destination register receives 0.

### Probe 3 — aligned access wider than the circular block, and store→later-load visibility

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cc -std=c11 -Wall -Wextra \
  -I emulator/qemu <scratch>/probe_block.c emulator/qemu/cdj_c674x.c \
  emulator/qemu/cdj_c674x_loop.c -o <scratch>/probe3
<scratch>/probe3
# A: accepted=1  A2=0xa3a2a1a0  (AMR A4 circular BK0, BK0=0 -> 2-byte block,
#    LDW reads four linear bytes and escapes the circle; see finding F4)
# B: next-packet load B4=0xdeadbeef (STW at packet N, LDW same address at N+1)
```

### Evidence artifacts inspected (not re-run)

```sh
.venv/bin/python -c "import json; ..."  # gate.json scope/limits/approximations
# runs/dsp-circular-transcript-replay-1/gate.json
#   scope: "trace equivalence only; not architectural correctness or boot"
#   architectural_validation_eligible: False
#   approximations include: "functional McASP scheduling advances one slot every
#     1024 executed DSP packets; not serializer-clock, sample-rate, or audio-output
#     evidence"; "SDRAM command timing, arbitration and retention are not modeled";
#     "DSP boot ROM is not executed"
# runs/nxs-pcm-observe-4/gate.json
#   architectural_validation_eligible: True, approximations: []  (strict; McASP
#   clock stopped, so this run carries no McASP evidence at all)
# build/performance/09-combined/final-4/gate.json
#   trace_mode: compact, architectural_validation_eligible: False,
#   approximations include "coarse packet-driven McASP slots; not audio-rate or
#   cycle-accurate"
```
All three gate files exist alongside `manifest.json`, `coverage.json` and
`trace.jsonl`/`repeat.jsonl`, and each gate carries `trace_sha256` and
`coverage_sha256`. I did not re-verify those hashes against the files.

## 7. Cross-track notes

- `cdj_c674x.c:2425-2432`: TSR/ITSR privilege and hardware-owned fields are
  preserved rather than modelled; combined with the absent MPU this means no
  Supervisor/User distinction exists anywhere. Interrupt-side consequences belong
  to Track 3.
- `tools/cdj_dsp/replay.c:344-366` is the real C6747 memory map, not
  `emulator/qemu/`. Anyone auditing "what the device looks like" has to read a
  tool, not the device model. Worth a note in whichever track owns repository
  structure.
