# NXS panel profile

The legacy CDJ-2000 key map is not valid for the NXS panel. NXS MAIN's
decoder at `042f5810` maps raw byte 19 bits 0..4 to status +5e/10,
+5f/80, +5f/40, +5f/20, +5f/10. Service masks at unpacked offsets
`b0eb4`, `b0fac`, `b0fdc` and names at `3c32f0` identify these as
REKORDBOX, LINK, USB, SD, DISC. TIME/ACUE moves to 19.5.

The same decoder also moves REC/PREVIOUS/NEXT/SEARCH, JOG MODE,
TEMPO RANGE, MASTER TEMPO, TEMPO RESET, CALL, DELETE, and MEMORY.
`tools/cdj_main/nxs_panel.py` records the separate NXS map. Its changed
contacts are checked against the real NXS service-name tables in
`tests/test_nxs_panel_profile.py`. This verifies names from decoded status
bits; it does not claim every control has received an end-to-end live test.

With `--nxs-panel` (already supplied by the NXS launcher), both viewer skins
use NXS contacts. The faceplate's existing visual positions resolve by
function, and pressed-state feedback maps back to those positions. Raw
inspector inputs remain literal bytes/bits; NXS-only contacts are available
there. Legacy names and coverage are unchanged, and legacy run annotations
are not presented as NXS evidence. Analogue-field attribution is unchanged.

## Live evidence and limits

The old `runs/nxs-usb-track-long-1` USB-labeled 19/02 probe displayed LINK,
consistent with the corrected map. In the strict-DSP run
`runs/nxs-track-load-sd-1`, source selection uses actual 19/08 contacts,
and BROWSE 20/01 toggles between the browser and player view. The browser
still displays Wait at the time of this change: this is not track-load proof.

This longer SD run has advanced beyond identification to CMD17, CMD18,
CMD25, automatic CMD12, and firmware filesystem writes into the temporary
overlay. A stopped snapshot `after-writes-ram.bin` records SD mode
`04cf2180=3`. Earlier short runs ending at mode 2 did not establish a
permanent SD initialization failure. The strict DSP has passed 2.6 billion
packets without the previous interrupt-entry conflict. Continued browser and
track-load investigation is required; TESTTONE.WAV is not yet verified loaded.

Focused profile/layout/control/socket regression run: 119 passed, 2 optional
skips. Name/profile/layout set: 28 passed, 8 optional skips.

Full suite at the first profile commit: 430 passed, 29 optional skips.
Native Tk follow-up: 35 passed, including actual NXS USB mouse down/up,
the corresponding faceplate highlight, no LINK highlight for the raw
REKORDBOX contact, and unchanged legacy encoder/inspector coverage.

Further decoder/table comparison identifies 17.1 as LOOP MODE (status
+53 bit 3, service code 35) and 17.3 as SLIP (+53 bit 4, code 34).
The legacy 4-BEAT LOOP visual slot is not assigned an invented NXS contact;
the verified functions remain available in the raw inspector. Native Tk
profile/layout tests after this correction: 35 passed.
