# Native PLAY: speculative buffer metadata reads and SDRAM aliases

## Evidence

The unmodified native transport path faults at DSP PC `0xc0032e58`, a compact
`LDW .D1T1 *+A5(0),A5`, after PLAY with functional McASP scheduling. The captured
state is `runs/agent-play-fault-capture/dsp-checkpoints/00000000000000000001.cdjdsp`.
A4 is `0xffff`, A5 is `0xd2ccff9c`, A10 is `-26`.

The fault is not a skipped branch delay slot:

1. Caller `0xc0007310` uses `[B14+196] - 26` as a requested sector offset;
   the initial counter is zero. `0xc0004784` explicitly initializes it to zero.
2. `0xc0032db0` converts `-26` into a `-13` buffer offset. The BNOP at
   `0xc0032e28` calls `0x11804ac4`; `LDBU [B14+885]` in its delay slot supplies
   stream 0. ADDKPC establishes return PC `0xc0032e40`.
3. `0x11804ac4` loads stream 0's head index (0 at `0x1182e2fa`) and calls
   `0x11804940`, which walks predecessor entries at `0xc1f38b90`. Entry 0's
   predecessor is sentinel `0xffff`.
4. After returning the sentinel, the code computes `4804 * 0xffff`, adds
   metadata base `0xc0090000` and field offset 4704, then loads the result.
   Further speculative fields use `0xd2ccffa8` and `0xd2ccfffc`.
5. Only at `0xc0032ed6` does it test the buffer index against 6692 and return
   `0xffff`. The caller recognizes that result and uses a zero-fill pre-roll
   path. The speculative metadata results are not audio or transport evidence.

## Device model and limits

The shared EMIFB address translator maps its `0xc0000000..0xdfffffff` aperture
onto the board's populated 32 MiB SDRAM. Reads access actual backing bytes;
writes, HPI transactions, and EDMA spans use the same physical offsets. No
constant data or transport acknowledgement is synthesized.

Primary local references:

- `build/references/spruh91d.txt`, Table 5-1, printed page 84: MPU2 manages the
  EMIFB region `0xc0000000..0xdfffffff`. Table 5-2 resets it to assume-allowed.
- Section 5.2.2, printed page 85: unpopulated SDRAM must be protected to avoid
  aliased accesses to populated memory.
- Section 19.2.6.10 and Tables 19-14/19-15, printed pages 797–798: upper logical
  address bits do not drive SDRAM row/column/bank pins. Captured SDCFG `0x14621`
  selects 16-bit width, four banks, and 512 columns. With 13 row address pins,
  the populated geometry has 25 byte-address bits, or 32 MiB.
- `build/references/sprs377f.txt`, Table 3-4, printed page 23: the device data
  sheet labels `0xd0000000..0xdfffffff` reserved. Thus extending physical alias
  behavior through that half is an inference from the MPU2 aperture and pin
  mapping, not a hardware measurement or guarantee about reserved addresses.

The model uses fixed populated board capacity. SDCFG geometry changes and MPU2
protection are not modeled. It preserves the existing SDRAM-enable gate and
master alignment requirements. Contiguous spans that cross the physical wrap
or decoder boundary fail rather than overrun backing memory. Splitting such
transfers is outside this change.

## Validation

Before the patch, an explicitly exploratory scratch replay supplying zero for
reserved reads ran 2 million further DSP steps. A second exploratory replay
reading the actual aliased physical SDRAM ran 10 million further steps without
a fault. These runs diagnose the bus mapping; they do not establish native
position progression or audio correctness.

Focused tests exercise real replay dispatchers and the board read dispatcher:
nonzero data at the original fault address, bidirectional alias writes, check
versus commit, byte/halfword/64-bit transfers, HPI backing pointers, EDMA staged
write forwarding across aliases, aperture ends, overflow/physical-wrap rejection,
and the disabled-memory gate. Native integration must be established separately.

The patched production replay subsequently resumed the genuine fault checkpoint
for 10,000,000 steps with functional McASP slots and genuine TX capture in
`runs/agent-play-alias-checkpoint-replay`. It reached the step limit without a
fault and its repeated-run gate passed; architectural-validation eligibility is
false. Transaction, checkpoint, connected-event, and deferred replay regression
suites passed 35 tests in addition to the three focused model/dispatcher tests.
