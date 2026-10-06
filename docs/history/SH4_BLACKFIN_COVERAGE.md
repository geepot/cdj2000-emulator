# SH4 and Blackfin coverage status

This is a board-side audit of the current tree after the SH4 USB host
remote-wakeup follow-up. It covers the
custom SH7764/QEMU integration, the patched Blackfin simulator integration,
and the two-board evidence that exercises them. It does not claim complete
SH-4 or Blackfin ISA coverage; those CPU cores come from QEMU and GNU sim and
are outside the custom board-model test denominator.

## Current verdict

The emulator is in good shape for the demonstrated firmware workflow:
MAIN and GUI startup, DSP handshaking, framebuffer publication, panel protocol,
selected SH4 DMA/HPI/IIC/Ethernet paths, and the patched Blackfin behaviors all
have executable tests or connected evidence. It is not yet a general-purpose
hardware-accurate SH7764/BF531 model. Sustained audio, complete peripheral
behavior, exact cross-board timing, and arbitrary firmware changes remain outside
the validated envelope.

## Evidence and tests

The latest rebuilt connected smoke run is
`runs/optimization-09-smoke-20260910`. It ran for 35 seconds with GUI exit 0,
no launcher timeout, a framebuffer, and unchanged input artifacts. The final
Blackfin statistics reported 1,041,891,328 instructions, 2,098 scanned frames,
10 published frames and zero dropped milliseconds. This is a bounded integration
smoke, not a full boot, audio, or timing proof.

The following focused checks were run with
`DEVELOPER_DIR=/Library/Developer/CommandLineTools`:

| Area | Result | What it establishes |
| --- | --- | --- |
| Blackfin helpers, assembled parallel packets and DMA/SPORT guest integration | 21 passed, 0 skipped | Helper semantics, instruction write ordering, DMA data/status and SIC request acknowledgement |
| Blackfin connected path | Included in the 35-second smoke | GUI execution, MAIN link, frame scanning/publication and wall-clock operation |
| SH4 boot evidence, GUI state/profile helpers | 59 passed, 6 skipped | Launcher contracts, manifests, firmware selection and evidence handling |
| SH4 panel protocol, Ethernet model and PHY | 72 passed, 1 skipped | Panel transport plus custom Ethernet/PHY behavior |
| Opt-in QEMU HPI and Ethernet integration | 2 passed | Actual QEMU process, QMP/HPI addressing and Ethernet integration |
| SH4 IIC, MAIN DMAC and Ethernet checks | 48 passed, 1 skipped | Standalone IIC, DMA address modes and Ethernet regression coverage |
| QEMU build | Passed | The tested board source is present in the installed SH4 binary |

The SH4 USB host model now handles QEMU downstream remote-wakeup callbacks.
When the firmware enables `DVSTCTR.RWUPE`, an accepted device wake raises the
documented `INTSTS1.BCHG` status and schedules controller work; callbacks are
ignored while the port is detached or wake detection is disabled. The model
leaves the firmware-controlled `RESUME`/`UACT` sequencing to the guest, as the
hardware manual specifies. The rebuilt QEMU binary and the 15 focused SH4
DMA/IIC/Ethernet/HPI regressions passed after this change. This fills the
previously empty callback, but does not claim cycle-accurate USB suspend or
resume timing.

The assembled Blackfin parallel-writeback regression now runs with local
binutils tools: both the corrected behavior and a specific old-write-order
negative control pass. Firmware-image and opt-in integration tests retain
their separate prerequisites; skipped coverage is not counted as passing.

## SH4 / SH7764

Covered with direct tests or evidence:

- SH7764 IIC register sequencing, empty-bus behavior, SCL hold and identity
  exchange (`tests/test_sh7764_iic.py`, clean-boot evidence).
- MAIN DMAC address modes, including fixed/decrementing source behavior
  (`tests/test_main_dmac.py`).
- HPI host addressing and fixed-port DMA through an actual QEMU process
  (`tests/test_nxs_hpi.py`).
- EtherC/E-DMAC register behavior, MDIO/PHY behavior, interrupt masking and
  the opt-in QEMU integration (`tests/test_sh7764_eth.py`,
  `tests/test_nxs_ethernet_qemu.py`).
- Panel input transport, command framing and evidence manifests.
- ATA task-file attachment and ATAPI identify status with a supplied CD image
  (`tests/test_sh7764_ata.py`).
- Real firmware startup and a genuine DHCP Discover/Offer/Request/ACK exchange;
  see `CLEAN_BOOT_EVIDENCE.md` and `ETHERNET_LOCAL.md`.

Important unvalidated or approximate areas:

- The SH-4 CPU and its complete instruction, MMU, exception and cache behavior
  are delegated to QEMU; this repository has no complete SH-4 architecture
  matrix.
- `emulator/qemu/cdj2000_ata.c`, `cdj2000_usb.c`, `cdj2000_usbh.c` and related
  paths still do not have complete standalone model tests. The USB host now
  has explicit remote-wakeup behavior. ATA now accepts a standard QEMU IDE
  CD backend and has a direct qtest for `IDENTIFY PACKET DEVICE`, but full
  packet data, DMA and error sequencing remain outside the fixture.
- Ethernet uses atomic coherent DMA, omits bus arbitration, FCS and wire
  serialization, and uses synthetic reset/negotiation/backend timing. Register
  13 readback is an explicitly unverified write-only-register assumption.
- The TMU3 characterization is deterministic at 13.5 MHz, while firmware
  documentation assumes approximately 13.4825 MHz. No PTP clock lock or
  cycle-accurate network timing is established.
- DMAC error completion, burst/arbitration behavior, and all interrupt-ordering
  combinations are not covered by the current focused tests.

## Blackfin / BF531

Covered with direct tests or evidence:

- The patched parallel DSP32ALU implementation is present in the built
  simulator, and the connected path runs with `BFIN_PARALLEL_WRITEBACK=1`.
- SIC mask-before-forward ordering (`tests/test_bfin_sic_mask.py`).
- Frame-change detection and publication suppression
  (`tests/test_bfin_frame_change.py`).
- Cold LZSS helper extraction and decline behavior
  (`tests/test_bfin_cold_lzss.py`).
- Persistent SPORT capture visibility and normal/abrupt finalization
  (`tests/test_bfin_sport_capture.py`).
- Persistent SPORT transmit capture visibility and normal/abrupt finalization
  (`tests/test_bfin_sport_tx_capture.py`).
- DMA error latching for alignment, descriptor-fetch and partial-transfer
  failures (`tests/test_bfin_dma_error.py`).
- Guest DMA/SPORT TX, two-descriptor chains, RX payload/adjacent-memory checks,
  completion interrupt enable/disable, alignment/unreadable-descriptor errors,
  and W1C deassertion through SIC (`tests/test_bfin_dma_sport_integration.py`).
- MAIN-link fresh-record cache behavior (`tests/test_bfin_link_cache.py`).
- Connected GUI execution, frame scans, MAIN link traffic and wall-clock pacing
  in the bounded smoke run.

Important unvalidated or approximate areas:

- The assembled parallel-writeback regression now covers relocation and packed
  subtract/store packets, including an old-write-order negative control. This
  is targeted instruction coverage, not a complete parallel-issue matrix.
- The five firmware-image tests are unavailable without local C2KGUI/C2KMAIN
  update images. The current smoke uses the existing extracted NXS GUI inputs.
- There is no broad BF531 ISA, MMU, exception, cache or privilege coverage
  matrix for the patched GNU simulator.
- PPI line timing, descriptor/error/interrupt combinations and SPORT framing
  are only partially tested. The capture tests validate the host observer, not
  complete hardware serializer behavior.
- Guest DMA tests now cover unaligned buffers and an unreadable initial
  descriptor. Full DMAC global-error routing, mid-chain descriptor failures
  and malformed descriptor contents remain unvalidated.
- Wall-clock synchronization, parked-loop wake latency and host clock
  conversion remain approximations. Lower CPU usage or matching frame counts
  does not prove firmware timing equivalence.

## Highest-value next checks

1. Extend the now-executable Blackfin parallel-writeback checks to additional
   packet classes and exception cases as firmware evidence warrants.
2. Extend the new DMA/SPORT guest fixtures beyond their two-descriptor TX, RX,
   completion/error and SIC request checks: PPI frame rate, short records,
   mid-chain errors and actual CEC ISR entry still need coverage.
3. Add SH4 HPI/DMAC error, completion-interrupt and burst/fixed-port cases,
   including ordinary-RAM fixed-source fills and unmapped/unaligned accesses.
4. Extend the ATA/USB model tests from identify/reset into packet data transfer,
   DMA/error paths and interrupt delivery before relying on connected-screen
   evidence.
5. Build a cross-board timing fixture that records SH4 interrupt latency,
   Blackfin frame publication, MAIN-link delivery and panel-command response on
   one virtual-time timeline. Keep PTP, audio output and wire timing as separate
   acceptance gates.

The current evidence supports “reliable for the tested firmware paths,” not
“complete SH4/Blackfin emulation.”

## Wake-timing harness follow-up

The Blackfin set now reports **37 passed, 1 expected failure, no skips**.
`tests/test_bfin_wake_timing.py` runs nine deterministic scenarios against
extracted production `bfin_wall_wait`, `bfin_wall_deliver`, `bfin_wall_sync`,
the parked-loop predicate, and GNU sim's `sim_events_time/tick/tickn` helpers.
Six negative controls require specific failures when PC-change exits, prepaid
wait credit, link readiness, hardware-loop exclusion, same-PC wake returns or
the exact tick boundary are broken. All nine scenarios also pass ASan/UBSan.

Clock/select and device callbacks are controlled test doubles. The callbacks
model masked/unmasked and PLL-style outcomes; this is not full SIC/CEC ISR,
SPORT delivery, Windows-waiter, physical PLL or cross-board timing coverage.
The actual event-queue tick helpers are extracted to preserve their boundary
contract: a deadline reached exactly by `tickn` is processed on the next tick,
with its callback attributed to that deadline before time advances.

`tests/test_bfin_wake_guest.py` adds assembled probes through the installed
simulator, without firmware inputs. With normal interrupts disabled, a PLL
wake already pending before IDLE works. **A PLL lock arriving during IDLE
exposes an existing early-resume bug:** the next guest instruction can read
PLL_STAT before the event has been delivered. That exact failure is recorded
as a strict expected failure; tool errors, timeouts and other simulator failures
remain failures. A fix must graduate the test rather than leave an XPASS.

No runtime scheduler change is made. The known early-IDLE-resume gap must be
addressed before treating this harness as evidence that idle/clock optimizations
preserve guest wake timing. Full timer ISR and live MAIN-link integration remain
additional gates. Reproduction is in BUILD.md; evidence is under
`analysis/iterations/12-blackfin-wake/`.
