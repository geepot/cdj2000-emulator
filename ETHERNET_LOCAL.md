# Local Ethernet checkpoint — 2026-09-09

The NXS board now connects the genuine MAIN driver to SH7764 EtherC/E-DMAC,
an RTL8201FL PHY, and an optional isolated localhost peer. No physical NIC
is bridged, no firmware is flashed, and no push is authorized.

## Sources and scope

Parent `references/vendor/pioneer-cdj-2000nxs-service-manual-rrv4356.pdf`,
pages 51/98, identifies IC704 as RTL8201FL-VB-CG. Page 98 connects LED0 via
R764 to CPU_LNKSTA. SH7764 manual R01UH0360EJ0300 chapters 19/20 and INTC
tables back the MAC/DMA model; Realtek RTL8201FL datasheet revision 1.4 backs
PHY registers and Clause 22.

Implemented: register aliases, observed DMA descriptor mode, bounded TX/RX
chains, ownership/wrapping, filtering, status/W1C, EtherC interrupt masking,
MDIO, virtual negotiation, LED0 link monitor, vendor LED/EEE initialization.
Unsupported modes fail closed. Connected execution exposed and corrected an
MDIO bit-phase bug: the stock routine samples before the rising MDC edge.

Approximations: atomic coherent DMA; no bus arbitration, FCS or wire timing;
synthetic 1 ms reset/100 ms negotiation and 10 ms backend polling; explicit
virtual 100BASE-TX full-duplex partner; no external PHY reset GPIO or LED
activity timing. Register 13 is documented write-only: zero readback for the
firmware RMW is an unverified compatibility assumption. Only EtherC's bit of
the shared interrupt mask is modeled. MAIN networking is not saved in DSP
checkpoints; this device disallows migration.

Pure PHY and MAC harnesses pass ASan/UBSan with warnings treated as errors.
Full regression with `CDJ_ETH_QEMU_TEST=1` and the TI toolchain available:
**465 passed, 29 skipped** (53.15 seconds). Local socket access is required;
the restricted-sandbox attempt failed socket-dependent tests and was rerun
with authorized localhost access, not counted as a passing validation.
The late connected DSP slice also repeats exactly (one million packets, one
connected stop): `runs/nxs-dante-ethernet-replay-3/gate.json`, trace SHA256
`4095333662e171ce25ac12baee6312f24fa2a6211075762af41dcc2b0de81a80`.
This is DSP trace equivalence, not a replay of the Ethernet network or full boot.

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-dante-ethernet-2/dsp-checkpoints/00000000000000000711.cdjdsp \
  runs/NEW_ETHERNET_DSP_TAIL --steps 3000000 \
  --events runs/nxs-dante-ethernet-2/dsp-events.jsonl --verify-repeat
```

## Reproduce

After rebuilding dependencies per BUILD.md:

```sh
sh scripts/build-qemu-sh4.sh build/qemu
.venv/bin/python -m tools.cdj_main.ethernet_peer runs/NEW_PEER --seconds 300 --ip 192.168.42.1
```

Use the allocated port from `NEW_PEER/endpoint.json` in another terminal:

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_CONNECTED \
  --seconds 180 --frame-interval 10 --port 6080 --qemu-sync-profile \
  --main-firmware firmware/dante-stereo-20260909/main-firmware.bin \
  --ethernet-peer-port PORT
CDJ_ETH_QEMU_TEST=1 .venv/bin/python -m pytest -q \
  tests/test_nxs_ethernet_qemu.py tests/test_ethernet_peer.py \
  tests/test_rtl8201fl.py tests/test_sh7764_eth.py
```

The optional QEMU test executes no firmware: synthetic descriptors verify
socket TX/RX, byte order, MDIO, link timing, interrupt sources/masks and reset.
The peer implements ARP/ICMP echo only, with directional raw-frame capture
and host timestamps. It is not a DHCP server, PTP clock or Dante audio device.

## Connected evidence

`runs/nxs-dante-ethernet-2`: 180 seconds with rebuilt modified Dante stereo
MAIN, real Blackfin/C674x execution and strict DSP handling. Final screen is
the normal player, “Not Loaded,” with no DSP error banner. Input hashes are
unchanged. Final DSP count is 672,099,500 packets / 1,220,907,630 cycles,
checkpoint 712. Packet counts do not establish semantic parity.

* QEMU SHA256: `ba74545c1222b148931e6412c1e42e0317277b0a7b23183862acce73c80e1f1c`.
* MAIN SHA256: `d88369e4b1986a9d3dcd58b68b784968ff1a756b7e59637c71dc13e4f9d891fe`.
* Rebuilt parent UPD SHA256: `a1f41285d852b8e9e5f825592c06c3481c9f74183c421dd41146a7383e30ccf5`.

`runs/nxs-ethernet-peer-2` captured **182 actual firmware TX frames**, zero
peer replies. Traffic includes Dante multicast announcements, diagnostics
and IGMP, with source IP **0.0.0.0**. The peer's `unsupported_or_invalid`
counter includes valid unhandled protocols; these are not 182 malformed
frames. Firmware RX was not demonstrated. Synthetic QEMU RX tests do not
establish firmware network-stack reception.

### Correction: DHCP starts; guest time is slow

A subsequent full-frame audit found one genuine DHCP Discover in **each**
of captures `nxs-ethernet-peer-2` and `-3`. Completed run 4 (240 seconds)
captures 264 frames including two Discovers (transaction IDs 1 and 2),
confirming the first retry. MAIN stays alive until normal teardown, GUI exits
zero, and input hashes remain unchanged.
The earlier inference of a DHCP startup blocker was wrong. The first
Discovers are 342-byte Ethernet frames, UDP 68 -> 67, IPv4 0.0.0.0 ->
255.255.255.255, BOOTP xid 1, broadcast flag set, DHCP option 53 = 1.
Our ARP/ICMP peer deliberately provides no DHCP Offer.

Read-only snapshots in `runs/nxs-dante-ethernet-4` establish the path:

| Snapshot | Kernel tick | DHCP deadline | Saved SP | Evidence |
| --- | ---: | ---: | --- | --- |
| first | 758 | 1571 | 04631a2c | Initial delay return address 0411588a |
| second | 1307 | 1571 | 04631a2c | Same initial wait |
| third | 1538 | 1571 | 04631a2c | Deadline not yet reached |
| fourth | 1778 | 2571 | 04631998 | UDP receive path, return addresses 041163ec / 0411595c |
| fifth | 2468 | 2571 | 04631998 | Waiting for an Offer; DHCP timer callback last time 2007 |

Stock disassembly: `04115886` calls `dly_tsk(1000)`; `04115944` constructs
and sends Discover; `04115958` waits for Offer. The kernel timer insertion
at `04369a46` adds the delay to tick word `04d13690` and writes the task
deadline at offset 12. Task 39's live table entry resolves to `04d16128`.
This shows progress, not a frozen kernel timer. Host seconds and kernel
ticks are very different under the current synchronous DSP scheduling.
No clock ratio, scheduler behavior, task flags or firmware RAM was changed.

Reproduce the observer while the exact-image run's monitor is live:

```sh
.venv/bin/python -m tools.cdj_main.network_snapshot runs/NEW_CONNECTED first
.venv/bin/python -m pytest -q tests/test_network_snapshot.py
```

The observer gates on the MAIN hash, briefly stops/resumes MAIN, saves raw
RAM with hashes and records its pause duration. Kernel/task/stack addresses
are specific to this candidate; decoded task fields require a matching live
task-table pointer. It is a diagnostic, not a faithful machine checkpoint.
Five focused tests pass, including rejecting wrong images before connection
and resuming MAIN after capture failure.

## Next blockers

1. Add an explicit isolated DHCP test-server mode and observe Offer/Request/Ack
   through the real driver. DHCP startup is confirmed above; AutoIP fallback
   needs more guest ticks than these bounded runs supplied. Do not force IP
   addresses, task flags, or infer a stall from host elapsed time alone.
2. Prove genuine firmware RX with ARP/ICMP after addressing works.
3. Add reference-backed PTP/Dante fixtures with an explicit clock domain;
   observe lock/subscriptions rather than assuming them from initialization.
4. Prove actual stereo DSP audio before expanding flows. Current channel
   descriptions and producer remain stereo; 16-channel packetization tests
   do not establish 16 independent sources or routing.

Physical Dante interoperability, precise PTP timing, complete boot, track
loading, working audio and 16-channel readiness remain unproven.
