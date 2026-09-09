# NXS GUI communication stall: stopped-RAM diagnostic

The deck contact/release fixes are committed separately. A successful host
`down`/`up` reply does **not** establish that MAIN processed the key or that the
GUI received a new status record. A static `Not Loaded` screen is not boot proof.

## Reproduce the state inspection

Stop MAIN through QMP, then use the human-monitor command:

```
pmemsave 0x04000000 0x08000000 /absolute/path/ram.bin
```

Run this read-only diagnostic from the repository:

```
python -m tools.cdj_main.nxs_gui_state /absolute/path/ram.bin
```

It checks the known pool descriptors before interpreting task stacks and mailbox
chains. The fixed addresses are for the research NXS MAIN image whose source
firmware SHA-256 is
`02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85`.
Descriptor checks are a layout sanity check, not image authentication. Do not
apply the diagnosis to another firmware version without re-deriving the layout.
The tool rejects invalid pointers/cyclic chains and marks records outside a
short dump unavailable. Absence of its stall signature is **not** a success gate.

## Observed circular wait

Stopped snapshot `/tmp/cdj-panel-capture-trace-2/ram.bin`:

| Task or queue | Observed state |
| --- | --- |
| Task 102, `GuiCom_RcvTASK` | Fixed-pool wait; saved PC `0x0425998c`, pool 60 exhausted (35 blocks) |
| Mailbox 44, DbCli requests | 34 queued command `0x709` messages; one further request held by DbCli |
| Task 85, `DbCli_TASK` | Fixed-pool wait; saved PC `0x0414d5fa`, pool 52 exhausted (8 blocks) |
| Mailbox 50, GuiCom replies | All 8 response buffers queued, command `0x13be` |
| Task 103, `GuiCom_SndTASK` | Polls mailbox 50 only after its protocol gate permits; gate is closed |

Task pointers: table `0x04d12b48 + 4 * task_id`; saved SP at TCB +0,
task-state byte +5, saved PC at SP +56. Pool table: `0x04d133dc`.
Mailbox table: `0x04d1326c`. Ready word `0x04984a68` and special-send
word `0x04985404` are both zero in this snapshot.

Receiver handler `0x0425799e` writes its ready word from the parser's return.
The parser calls `0x042581a4` with request word 0: zero requests retransmission
of the last payload and can return false; one acknowledges/reset the pending
payload and returns true. Therefore a zero ready word is not by itself a bug:
the protocol is waiting for the GUI acknowledgment. Queue exhaustion prevents
the receiver from reaching later acknowledgments. This explains the terminal
state, not yet the first incorrect transport/scheduling event.

## Controls and rejected timing workaround

All these isolated probes used the same final timed-SPI QEMU binary, original
firmware bytes, and disabled legacy request/status rewriting. Direct MENU hold
was sent at 40–50 seconds, followed by release. No RAM writes forced progress.

| Probe | Result |
| --- | --- |
| `cdj-panel-capture-stall-1` | DSP capture on, link trace off: both pools exhausted |
| `cdj-panel-capture-trace-2` | Capture and trace on: both pools exhausted |
| `cdj-panel-current-nocapture-1` | Capture off: same pool exhaustion; tracing overhead is not necessary |
| `cdj-panel-ready-watch-1` | 7 ms RX gap plus ready-word watch: MENU opened UTILITY; all buffers free at 60 s |
| `cdj-panel-gap7-no-watch-1` | 7 ms RX gap without watch: traffic stopped by 40 s and remained stopped through 90 s; MENU did not open UTILITY |

The 7 ms gap is **not a fix**. Instrumentation can perturb execution timing;
the uninstrumented repeat rejected that apparent improvement. No launcher
default was changed based on it. The successful instrumented screen still
reported E-7206 AUTH CHIP ERROR, a separate unresolved device-model issue.

Two further mechanisms are established but not yet a complete causal proof:

- SPORT's current cache repeats status and announced payload records without a
  new MAIN transmission. The trace-2 dump delivered 4,007 48-byte payloads while
  MAIN actually sent two. See the independent wire/dump audit in `HANDOFF.md`.
- The synchronous DSP host callback runs a million interpreter steps without
  letting MAIN execute. With QEMU's default non-icount clock, host elapsed time
  still advances MAIN virtual time. The independent DSP task measured roughly
  258–262 ms executing a quantum, versus about 2 ms for checkpoint reporting.
  Fair continuation scheduling is being addressed in that separate change.

Revalidate with repeated uninstrumented connected boots and visible post-boot
controls, not just free pools or increasing cached-DMA counts. USB/SD test-track
loading remains pending that gate; this diagnostic does not implement media
loading or claim successful playback.

## Subsequent controls

- `cdj-panel-deferred-diagnostic-1`, 90 seconds: the experimental deferred-v1
  DSP policy shortened individual callbacks but still ended in the same two
  exhausted pools, with 34 requests and 8 replies queued. MENU did not open.
- `cdj-panel-no-payload-repeat-1`, 60 seconds: disabling only cached announced
  payload repeats produced `repeat_payload=0`, but the same pool deadlock
  remained. That setting alone is not a fix.
- `cdj-panel-fresh-only-1`, 90 seconds: patch 05's opt-in fresh-only live
  delivery left both pools free, **because the GUI failed earlier with E-8709**.
  MAIN sent 8 status records and 18 240-byte command-9 payloads; the GUI consumed
  no 240-byte payload. The later DbCli request phase was never reached. Thus
  free pools here do not establish either a fix or causation by cached repeats.

Frontend follow-up `20524e1` makes keyboard and Inspector/lab button holds
physical down/up contacts as well. Native Tk tests verify actual bindings,
repeat suppression, outside release and focus loss. The 122-test focused
suite passed; these host-input tests do not establish a firmware response.

## Confirmed premature receive cancellation: SIC mask ordering

Stock NXS GUI code at `0x00d106d6` checks the status announcement type and
halfword count; a valid announcement arms `0x01f00040` via `0x00d0c59a`.
`0x00d0f65c` drains the receive channel using a separate 200-byte buffer.
The shared interrupt handler checks TX DMA_DONE at `0x00d0cae8`, then RX
DMA_DONE at `0x00d0cb0e`. If neither is set, it requests communication recovery
by setting `0x00600301` at `0x00d0cb70`. This is distinct from the normal
5,000-tick timeout checked at `0x00cfc82c`.

Patch 06's timestamped trace made the failure concrete. In
`/tmp/cdj-panel-fresh-mmr-1/gui.log`, with fresh-only delivery:

| GUI simulator time | Event |
| --- | --- |
| 10.603334 | RX completion acknowledged at PC `0x00d0c650` |
| 10.603371–10.603373 | Shared ISR runs again; both TX and RX IRQ_STATUS are zero |
| 10.604098 | Payload receive armed: address `0x01f00040`, 120 halfwords |
| 10.604298 | Receive disabled, all 120 halfwords still outstanding |
| 10.604302 | Replaced by a 32-halfword status receive |

The cancellation occurs after **200 microseconds**, not the five-second
watchdog. MAIN subsequently sends the payload but no matching receive remains.
The earlier length census (`cdj-panel-fresh-lengths-1`) independently showed
three unsuccessful 240-byte read attempts, followed by repeated 64-byte reads;
the payload was not simply absent from MAIN or never armed by GUI.

GNU sim's SIC mask-write handlers forwarded pending sources **before** storing
the new mask. NXS uses the BF531 model's **bf537** register-layout handler.
Firmware masks RX in its ISR before acknowledging DMA_DONE; forwarding the old
mask re-latches that source in the CEC, producing the empty second ISR above.
Patch 07 stores the new mask first in all four register layouts. The actual
IMASK case-body tests cover masking, unmasking a pending source, retaining a
different shared source, and unmasking with no pending source. They do not
claim to test the complete SIC/CEC hardware protocol.

With patch 07, `/tmp/cdj-panel-mask-order-fresh-1` arms the payload at
11.390831 and receives it at 11.648998: the receive now survives the roughly
258 ms MAIN delay. The 60-second run consumes genuine payloads, MENU opens
UTILITY, both fixed pools remain completely free and both mailboxes are empty.
It still displays **E-7206 AUTH CHIP ERROR**. This establishes a specific
interrupt-order fix, not complete boot, media loading, or playback. Untraced
and normal cached-delivery controls must still pass before changing defaults.

Those controls subsequently passed:

- `cdj-panel-mask-order-fresh-quiet-1`, 60 seconds: detailed MAIN/GUI traces
  and statistics disabled; fresh-only delivery; MENU opened UTILITY.
- `cdj-panel-mask-order-default-quiet-1`, 80 seconds: normal cached delivery,
  detailed traces/statistics disabled. The actual native Tk deck sent MENU down
  at 40 seconds and released outside the button at 50 seconds. UTILITY opened;
  panel frames were 184 held and 223 released, with an empty pulse queue.
  Both fixed pools remained fully free and mailboxes 44/50 empty at 80 seconds.
  A second direct MENU hold/release also left no held bits.

These controls used simulator SHA-256
`2663e8402b14d16a077665a835017f8f2dc298c0a5515699cb945db670591841`
and QEMU SHA-256
`f69c34b53bdbb92bee51d3ec6db6a0ade27cd9f935700c65237dcf3e7798bd71`.
The original firmware and synchronous DSP scheduler were retained; no binary
was rebuilt between the controls. **Fresh-only remains opt-in**, since the
mask-order fix also passes with normal transport. Native Tk/input/RAM/cache/
SIC focused regression: **128 passed**. The separate E-7206 device-model
problem remains; no media-loading or playback gate has passed.
