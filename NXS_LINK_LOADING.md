# Track-loading link comparison

`nxs_vm --fresh-link` opts into the existing Blackfin simulator's
`BFIN_LINK_FRESH_ONLY=1`: real MAIN records are consumed once, rather than
synthetic repeated DMA deliveries of cached status/payloads. This is an
explicit transport diagnostic, not a hardware clock model or a default change.
`--trace-link-tx` captures actual outgoing SPORT packets in `gui-link-tx.bin`.
Each record is `SPTX`, little-endian u32 SPORT base, u32 length, payload.
Both options and their environment values are recorded in `run.json`.
Inherited BFIN variables remain sanitized. Launcher regression: 38 passed.

## Comparison underway

The cached-delivery SD run `runs/nxs-track-load-sd-1` progressed to real
filesystem reads and writes and SD readiness mode 3. It still showed Wait.
Its receive queue remained at 63 after delivery (64 before delivery); the
existing overflow policy drops the oldest frame. Input hashes were unchanged
when the diagnostic was interrupted cleanly through the launcher. The GUI
had seen hundreds of repeated deliveries of individual MAIN payloads.

`runs/nxs-track-load-sd-fresh-1` uses the same firmware, QEMU, GUI simulator,
strict DSP and protected SD fixture, with fresh-only delivery and TX capture.
At 123 virtual seconds, actual SD 19/08 input causes delivered request words
`0000 0001 0001 0007 0002 0a00`; queue depth is zero, and the GUI displays
NO CARD (before SD initialization completes). This is a real browser response,
not track-load success. Continue through mounting and TESTTONE.WAV selection.

The old `link_exchanges` tool classifies request/repeat by bit 15 using the
legacy firmware protocol. NXS emits nonzero request types without that bit;
its report of "requests 0" must not be interpreted as no NXS requests. Use
raw request words and the SPTX capture for this comparison.

Fresh-only delivery previously exposed the SIC mask-order defect; that defect
has since been fixed (see NXS_GUI_STALL.md). Replaying cached frames is not
needed to work around that old interrupt failure. The present comparison is
still needed to establish the effect on actual media browsing and loading.

## Mounted-card follow-up

The fresh-only run reaches SD mode 3 and visibly lists TESTTONE.WAV at
approximately 580 seconds. This establishes detection, mounting and listing,
not loading or audio output. MAIN's 0x11/0x1b list replies may be only
64 bytes long; a decoder that assumes every 64-byte record is status will
misreport these replies as missing.

GUI-only diagnostic reconnects retain the running MAIN and its SD snapshot
overlay, and are recorded as warm reconnects rather than cold boots.
Watches verify encoder press 17/01 reaches GUI status 6003e8 bit 0, emits
key 0x10, drains the first event queue (60753c), and enters the general
window-event queue at 7e911c as event 5. The second queue also drains.
Neither queue is permanently full in this case. The remaining investigation
is the browser-window handler and its outgoing load request.
