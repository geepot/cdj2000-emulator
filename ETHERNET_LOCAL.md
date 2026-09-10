# Local Ethernet checkpoint — 2026-09-09

## Directional traffic audit

`tools.cdj_main.network_inventory` inventories captured Ethernet/IPv4/UDP
tuples without treating port numbers as protocol validation. It checks IPv4
and present UDP checksums and lengths, keeps fragments separate, hashes the
capture, and exits nonzero for malformed records.

```sh
.venv/bin/python -m tools.cdj_main.network_inventory runs/nxs-dhcp-peer-1/frames.jsonl
.venv/bin/python -m pytest -q tests/test_network_inventory.py tests/test_dhcp_peer.py tests/test_ethernet_peer.py
```

This capture has 345 records (343 guest frames, two peer replies), with no
invalid records. After addressing, observed UDP destinations include
239.192.77.83:17683 (102), 224.0.0.233:8708 (102),
192.168.42.255:50000 (6), and 224.0.0.251:5353 (5).
There are no captured UDP source/destination ports 319/320. This absence
does not establish a PTP implementation defect: the peer supplies no clock.
The next fixture must use the modified firmware's PTPv1 contract and an
explicit clock domain before any lock claim. ARC subscription and genuine
audio capture remain separate gates. Focused tests: 37 passed with localhost
socket permission; the first sandboxed attempt could not start its listener.

## Optional DHCP fixture

`--dhcp-lease 192.168.42.2` enables a single-client SELECTING-state DHCP
fixture in the isolated peer. The peer address defaults to 192.168.42.1;
server and reserved client address must be distinct hosts in one /24.
It validates IP/UDP checksums and lengths, BOOTP identity, transaction ID,
requested address and selected server before sending Offer/ACK. Unsupported
relay, renewal, option-overload and other modes receive no reply.

This follows [RFC 2131](https://www.rfc-editor.org/rfc/rfc2131) and
[RFC 2132](https://www.rfc-editor.org/rfc/rfc2132), with client-identifier echo.
The infinite test lease deliberately avoids a false host-time/guest-time
expiration model. There is no router, DNS, forwarding or physical interface.
An ACK sent by the fixture is not proof that firmware accepted it: check
subsequent packets and the genuine interface address in a read-only snapshot.

```sh
.venv/bin/python -m tools.cdj_main.ethernet_peer runs/NEW_DHCP_PEER \
  --seconds 400 --dhcp-lease 192.168.42.2
# Use its allocated localhost port with nxs_vm --ethernet-peer-port PORT.
.venv/bin/python -m pytest -q tests/test_dhcp_peer.py tests/test_ethernet_peer.py
```

Historical ARP/ICMP-only results below remain unchanged; the DHCP fixture
does not retroactively validate RX in those runs.

### Genuine firmware lease acceptance

`runs/nxs-dhcp-connected-1` with `runs/nxs-dhcp-peer-1` captures the complete
Discover -> Offer -> Request -> ACK exchange. Read-only `network-before.json`
shows IP 0.0.0.0/state 1/tick 1370; `network-after.json` shows
**192.168.42.2/state 3/tick 1874**. Subsequent genuine firmware packets use
192.168.42.2. This establishes real firmware reception and address assignment
through the emulated controller, not only transmission or synthetic DMA RX.
No firmware memory writes, forced flags or relaxed DSP settings were used.

The completed 300-second run has 343 guest frames and two peer replies,
normal GUI exit, MAIN alive before teardown, unchanged input hashes, and a
normal unloaded player screen without a DSP error banner. Final DSP count:
1,097,099,500 packets / 1,981,680,083 cycles. This is not playback evidence.
`runs/nxs-dhcp-replay-1` verifies one late connected DSP stop with exact repeat;
trace SHA256 `53290d6c821afb8e05634353c49e5760395c2309bd485501a9b0498a83309aac`.
It replays DSP state/events, not MAIN or the network peer:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-dhcp-connected-1/dsp-checkpoints/00000000000000001136.cdjdsp \
  runs/NEW_DHCP_DSP_REPLAY --steps 3000000 \
  --events runs/nxs-dhcp-connected-1/dsp-events.jsonl --verify-repeat
```

DHCP/peer focused tests: 35 passed. Full regression with TI toolchain and
`CDJ_ETH_QEMU_TEST=1`: 489 passed / 29 skipped. The interface snapshot extension
also passes its five focused tests. This does not establish PTP or audio.

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

1. DHCP initial lease acceptance is now demonstrated above. Renewal, finite
   lease clocks, AutoIP fallback and fault/recovery cases remain untested.
   Do not infer a stall from host elapsed time alone.
2. Extend application-level RX validation with ARP/ICMP after addressing.
3. Add reference-backed PTP/Dante fixtures with an explicit clock domain;
   observe lock/subscriptions rather than assuming them from initialization.
4. Prove actual stereo DSP audio before expanding flows. Current channel
   descriptions and producer remain stereo; 16-channel packetization tests
   do not establish 16 independent sources or routing.

Physical Dante interoperability, precise PTP timing, complete boot, track
loading, working audio and 16-channel readiness remain unproven.
