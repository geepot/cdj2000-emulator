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

## Next blockers

1. Trace genuine DHCP startup/fallback. Stock getter `0412d0b4` returns mode
   4 (DHCP first); mode 3 starts AutoIP directly. AutoIP initializes candidate
   `169.254.0.1` for the observed MAC but waits for an event. Read runtime RAM
   `045a6360..045a6380` and `046313b4..04631424`. DHCP task record `0463140c`
   contains live byte/task ID/semaphore; starter `04115cbe` returns -102 when
   not live. Do not force IP addresses or task flags.
2. Prove genuine firmware RX with ARP/ICMP after addressing works.
3. Add reference-backed PTP/Dante fixtures with an explicit clock domain;
   observe lock/subscriptions rather than assuming them from initialization.
4. Prove actual stereo DSP audio before expanding flows. Current channel
   descriptions and producer remain stereo; 16-channel packetization tests
   do not establish 16 independent sources or routing.

Physical Dante interoperability, precise PTP timing, complete boot, track
loading, working audio and 16-channel readiness remain unproven.
