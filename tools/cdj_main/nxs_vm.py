# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the experimental NXS MAIN and GUI profiles over a direct serial link.

No proxy or generated status packets. DSP uses UHPI transport and a partial C674x interpreter.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

from tools.cdj_dsp.tx_capture import tx_capture_metadata

ROOT = Path(__file__).resolve().parents[2]

CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')
CHECKPOINT_MAGIC = {1: b'CDJDSP1\0', 2: b'CDJDSP2\0', 3: b'CDJDSP3\0',
                    4: b'CDJDSP4\0', 5: b'CDJDSP5\0', 6: b'CDJDSP6\0',
                    7: b'CDJDSP7\0', 8: b'CDJDSP8\0', 9: b'CDJDSP9\0'}
SHARED_RAM_SIZE = 0x20000
MAX_FRAME_BYTES = 16 * 1024 * 1024


def fnv1a(data) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def input_metadata(path: Path) -> dict:
    """Hash the resolved artifact before launch, rejecting concurrent rewrites."""
    path = path.resolve()
    before = path.stat()
    digest = sha256(path)
    after = path.stat()
    if file_signature(before) != file_signature(after):
        raise RuntimeError(f'input changed while hashing: {path}')
    return dict(path=str(path), size=after.st_size, sha256=digest)


def file_signature(stat) -> tuple:
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)


def complete_ppm_dimensions(raw: bytes) -> tuple[int, int] | None:
    """Validate one complete P6/255 frame, the simulator's output format.

    Consume only the header delimiter after maxval: raster bytes may themselves
    be whitespace or '#'. Reject concatenated, truncated and oversized images.
    """
    if not raw.startswith(b'P6') or len(raw) > MAX_FRAME_BYTES:
        return None
    cursor = 0
    tokens = []
    for _ in range(4):
        while cursor < len(raw):
            if raw[cursor] in b' \t\r\n\v\f':
                cursor += 1
            elif raw[cursor] == ord('#'):
                end = raw.find(b'\n', cursor)
                if end < 0:
                    return None
                cursor = end + 1
            else:
                break
        start = cursor
        while cursor < len(raw) and raw[cursor] not in b' \t\r\n\v\f#':
            cursor += 1
        tokens.append(raw[start:cursor])
    if tokens[0] != b'P6' or any(not token.isdigit() for token in tokens[1:]):
        return None
    try:
        width, height, maximum = map(int, tokens[1:])
    except ValueError:
        return None
    if width <= 0 or height <= 0 or maximum != 255 or cursor == len(raw):
        return None
    if raw[cursor] not in b' \t\r\n\v\f':
        return None
    cursor += 2 if raw[cursor:cursor + 2] == b'\r\n' else 1
    return (width, height) if len(raw) - cursor == width * height * 3 else None


def read_frame(path: Path) -> tuple[bytes | None, dict]:
    """Read an immutable published frame, or reject an incomplete/in-place write."""
    try:
        with path.open('rb') as stream:
            before = os.fstat(stream.fileno())
            raw = stream.read(MAX_FRAME_BYTES + 1)
            after = os.fstat(stream.fileno())
    except FileNotFoundError:
        return None, dict(status='missing')
    except OSError as error:
        return None, dict(status='unreadable', error=str(error))
    if file_signature(before) != file_signature(after):
        return None, dict(status='changed_during_read')
    dimensions = complete_ppm_dimensions(raw)
    if dimensions is None:
        return None, dict(status='invalid_or_incomplete_ppm')
    return raw, dict(status='captured', width=dimensions[0], height=dimensions[1],
                     size=len(raw), source_mtime_ns=after.st_mtime_ns)


class FrameSnapshots:
    """Periodic observations, not a renderer-liveness or boot-success oracle."""
    def __init__(self, run: Path, interval: float, started: float):
        self.run, self.interval, self.started = run, interval, started
        self.next_elapsed = 0.0
        self.observations = []
        self.previous_sha256 = None

    def manifest(self) -> dict:
        return dict(interval_seconds=self.interval, clock='monotonic_since_gui_launch',
                    meaning='frame observations only; unchanged images do not prove renderer liveness or boot',
                    observations=self.observations)

    def poll(self, now: float) -> None:
        elapsed = now - self.started
        if elapsed < self.next_elapsed:
            return
        # Skip missed intervals: do not fabricate historical observations.
        self.next_elapsed = (math.floor(elapsed / self.interval) + 1) * self.interval
        raw, metadata = read_frame(self.run / 'screen.ppm')
        record = dict(elapsed_seconds=elapsed, **metadata)
        directory = self.run / 'frames'
        directory.mkdir(exist_ok=True)
        if raw is not None:
            digest = hashlib.sha256(raw).hexdigest()
            filename = f'{len(self.observations) + 1:06d}.ppm'
            temporary_frame = directory / f'{filename}.tmp'
            temporary_frame.write_bytes(raw)
            temporary_frame.replace(directory / filename)
            record.update(file=f'frames/{filename}', sha256=digest,
                          same_as_previous_capture=(digest == self.previous_sha256))
            self.previous_sha256 = digest
        self.observations.append(record)
        temporary = directory / 'manifest.json.tmp'
        temporary.write_text(json.dumps(self.manifest(), indent=2) + '\n')
        temporary.replace(directory / 'manifest.json')


def checkpoint_metadata(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) < CHECKPOINT_HEADER.size:
        raise RuntimeError(f'incomplete DSP checkpoint: {path}')
    fields = CHECKPOINT_HEADER.unpack_from(raw)
    magic, schema, endian, header_size, state_size = fields[:5]
    component_sizes = list(fields[5:14])
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size, payload_checksum = fields[19:21]
    if (CHECKPOINT_MAGIC.get(schema) != magic or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or
            len(raw) != header_size + payload_size or
            fnv1a(memoryview(raw)[header_size:]) != payload_checksum):
        raise RuntimeError(f'incompatible or incomplete DSP checkpoint: {path}')
    shared_size = SHARED_RAM_SIZE if schema >= 2 else 0
    shared_start = header_size + state_size + l2_size
    shared = raw[shared_start:shared_start + shared_size]
    return dict(file=path.name, sha256=hashlib.sha256(raw).hexdigest(),
                size=len(raw), schema=schema, endian='little', state_size=state_size,
                component_sizes=component_sizes, l2_size=l2_size,
                shared_ram_size=shared_size,
                shared_ram_sha256=(hashlib.sha256(shared).hexdigest()
                                   if shared_size else None),
                sdram_size=sdram_size, page_size=page_size,
                page_count=page_count, present_pages=present_pages,
                payload_checksum=f'{payload_checksum:016x}')


def finalize_dsp_artifacts(run: Path, firmware: Path, functional_dsp_timing: bool,
                           functional_dsp_audio: bool,
                           capture_dsp_tx: bool) -> None:
    checkpoint_dir = run / 'dsp-checkpoints'
    checkpoints = [checkpoint_metadata(path) for path in sorted(checkpoint_dir.glob('*.cdjdsp'))]
    events = run / 'dsp-events.jsonl'
    event_counts = Counter()
    last_sequence = 0
    boot_phases = []
    if events.is_file():
        with events.open() as stream:
            for line in stream:
                event = json.loads(line)
                if event['sequence'] != last_sequence + 1:
                    raise RuntimeError('DSP event transcript sequence is incomplete')
                last_sequence = event['sequence']
                event_counts[event['event']] += 1
                if event['event'] == 'boot_phase':
                    boot_phases.append(event['value'])
    tx_path = run / 'dsp-tx.jsonl'
    if capture_dsp_tx and not tx_path.is_file():
        raise RuntimeError('requested DSP transmit capture is missing')
    tx_capture = tx_capture_metadata(tx_path) if capture_dsp_tx else None
    sources = sorted((ROOT / 'emulator/qemu').glob('cdj_c674*.c')) + \
              sorted((ROOT / 'emulator/qemu').glob('cdj_c674*.h')) + \
              [ROOT / 'emulator/qemu/cdj_dsp_checkpoint.c',
               ROOT / 'emulator/qemu/cdj_dsp_checkpoint.h',
               ROOT / 'emulator/qemu/cdj2000_nxs_hpi.c']
    manifest = dict(schema=9, format=('ABI-bound native state including C6747 INTC/Timer64P/SPI/cache/McASP TX, EDMA, SYSCFG priority and WM8740 control, '
                                     'L2 and shared RAM plus sparse zero-default SDRAM pages'),
        dsp_timing_mode=('functional-runahead' if functional_dsp_timing else 'strict'),
        dsp_audio_mode=('coarse-packet-slots' if functional_dsp_audio else 'stopped-clock'),
        architectural_validation_eligible=not (functional_dsp_timing or functional_dsp_audio),
        byte_order=sys.byteorder,
        complete=bool(checkpoints and events.is_file() and
                      (not capture_dsp_tx or tx_capture is not None)),
        checkpoints=checkpoints, latest=checkpoints[-1]['file'] if checkpoints else None,
        event_transcript=dict(file=events.name, sha256=sha256(events) if events.is_file() else None,
                              events=last_sequence, counts=dict(sorted(event_counts.items())),
                              boot_phases=boot_phases),
        dsp_tx_capture=tx_capture,
        firmware_sha256={path.name: sha256(path) for path in
            (firmware / 'main-firmware.bin', firmware / 'gui-boot-memory.elf',
             firmware / 'gui-flash-image.bin')},
        source_sha256={str(path.relative_to(ROOT)): sha256(path) for path in sources},
        approximations=[
            'DSP boot ROM is not executed; its documented HPI-ready handoff is modeled',
            'checkpoint schema 9 is ABI-bound and rejects structure-size or endianness changes',
            '128 KiB C6747 shared RAM is captured losslessly',
            'sparse SDRAM pages are lossless because omitted pages restore as zero',
            'SDRAM command timing, arbitration and retention are not modeled',
            'PSC transition ticks and PLL divider GO latency remain deterministic approximations',
            'physical HPI pins, FIFO/HRDY timing and DSP interrupt delivery are not modeled',
            *(['functional run-ahead adds two SPLOOPD epilog cycles; not cycle-validation evidence']
              if functional_dsp_timing else []),
            *(['functional run-ahead collapses each evidence-backed SPI1/WM8740 transfer to its committing write; not SPI timing evidence']
              if functional_dsp_timing else []),
            *(['interrupt-return SPMASK pipe-up is reconstructed from the stable program image; retained-buffer timing is not modeled']
              if functional_dsp_timing else []),
            *(['an ISR SPLOOP may replace retained loop validation state; a later SPLX return is reconstructed from the current program image and self-modifying loop bodies are unsupported']
              if functional_dsp_timing else []),
            *(['interrupt entry retires already-issued results with minimum empty cycles; exact interrupt pipeline latency is not modeled']
              if functional_dsp_timing else []),
            *(['functional McASP scheduling advances one slot every 1024 executed DSP packets; '
               'not serializer-clock, sample-rate, or audio-output evidence']
              if functional_dsp_audio else [])])
    checkpoint_dir.mkdir(parents=True, exist_ok=True)
    (checkpoint_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path, help='new run directory, relative to repository')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--frame-interval', type=float, default=0,
                        help='save complete framebuffer observations every N seconds (0 disables)')
    parser.add_argument('--ui', action='store_true',
                        help='open the interactive deck; closing it stops this run')
    parser.add_argument('--port', type=int, default=5980)
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/qemu/build/qemu-system-sh4')
    parser.add_argument('--functional-dsp-timing', action='store_true',
                        help='run past the unresolved SPLOOPD epilog with a labeled two-cycle approximation')
    parser.add_argument('--functional-dsp-audio', action='store_true',
                        help='schedule coarse McASP TX slots to exercise genuine firmware DMA/ISR flow')
    parser.add_argument('--capture-dsp-tx', action='store_true',
                        help='capture genuine XBUF words consumed by coarse McASP slot progression')
    args = parser.parse_args()
    if args.capture_dsp_tx and not args.functional_dsp_audio:
        parser.error('--capture-dsp-tx requires --functional-dsp-audio')
    if args.seconds <= 0 or not 1024 <= args.port <= 65531:
        parser.error('positive duration and port 1024..65531 required')
    if not math.isfinite(args.frame_interval) or args.frame_interval < 0:
        parser.error('--frame-interval must be finite and nonnegative')
    run = (ROOT / args.run).resolve()
    firmware = ROOT / 'firmware/nxs'
    simulator = ROOT / 'bin/cdj-run'
    for path in (args.qemu, simulator, firmware / 'main-firmware.bin', firmware / 'gui-boot-memory.elf', firmware / 'gui-flash-image.bin'):
        if not path.is_file(): parser.error(f'missing input: {path}')
    inputs = dict(qemu=args.qemu, simulator=simulator,
                  main_firmware=firmware / 'main-firmware.bin',
                  gui_boot=firmware / 'gui-boot-memory.elf',
                  gui_flash=firmware / 'gui-flash-image.bin')
    input_artifacts = {name: input_metadata(path) for name, path in inputs.items()}
    run.mkdir(parents=True, exist_ok=False)
    main_command = [str(args.qemu.resolve()), '-M', 'cdj2000nxs-main', '-bios', str(firmware / 'main-firmware.bin'),
        '-display', 'none', '-no-reboot', '-d', 'unimp,guest_errors', '-D', str(run / 'main.log'),
        '-serial', f'tcp:127.0.0.1:{args.port},server,nowait',
        '-serial', f'tcp:127.0.0.1:{args.port + 2},server,nowait', '-serial', 'null']
    gui_command = [str(simulator), '--model', 'bf531', '--environment', 'operating', '--memory-region', '0,64M',
        '--hw-board-file', 'emulator/cdj2000-gui-nxs.hw', str(firmware / 'gui-boot-memory.elf')]
    overrides = dict(BFIN_PARALLEL_WRITEBACK='1', BFIN_GUI_COLOR='rgb555le',
        BFIN_GUI_OUTPUT=str(run / 'screen.ppm'), BFIN_MAIN_LINK=f'127.0.0.1:{args.port}',
        BFIN_MAIN_LINK_DUMP=str(run / 'main-link.bin'), BFIN_GPIO5_READY_TOGGLE='1',
        BFIN_STATS='5', BFIN_EXCEPTION_TRACE='1', BFIN_EXIT_AFTER_WALL=str(args.seconds))
    # Do not inherit replay/proxy data or a firmware shortcut from the shell.
    gui_env = {k:v for k,v in os.environ.items() if not k.startswith('BFIN_')}
    gui_env.update(overrides)
    main_env = {k:v for k,v in os.environ.items() if not k.startswith('CDJ_')}
    main_env['CDJ_INPUT_PORT'] = str(args.port + 4)
    main_env['CDJ_NXS_HPI_DUMP'] = str(run / 'dsp-l2.bin')
    main_env['CDJ_NXS_DSP_EVENTS'] = str(run / 'dsp-events.jsonl')
    main_env['CDJ_NXS_DSP_CHECKPOINT_DIR'] = str(run / 'dsp-checkpoints')
    if args.functional_dsp_timing:
        main_env['CDJ_NXS_DSP_FUNCTIONAL_TIMING'] = '1'
    if args.functional_dsp_audio:
        main_env['CDJ_NXS_DSP_FUNCTIONAL_AUDIO'] = '1'
    if args.capture_dsp_tx:
        main_env['CDJ_NXS_DSP_TX_CAPTURE'] = str(run / 'dsp-tx.jsonl')
    main_env['CDJ_REQ_STATUS_FRESH'] = '0'
    run_manifest = dict(main=main_command, gui=gui_command,
        gui_environment=overrides, main_environment={k:v for k,v in main_env.items() if k.startswith('CDJ_')},
        dsp='NXS UHPI plus partial C674x interpreter; incomplete ISA, ROM handoff abstraction', profile='experimental NXS',
        input_artifacts=input_artifacts, frame_interval_seconds=args.frame_interval)
    if args.frame_interval:
        run_manifest['frame_snapshots_manifest'] = 'frames/manifest.json'
    (run / 'run.json').write_text(json.dumps(run_manifest, indent=2) + '\n')
    processes = []
    result = {}
    viewer = None
    snapshots = None
    with (run / 'main-stderr.log').open('w') as mainlog, (run / 'gui.log').open('w') as guilog:
        try:
            main_process = subprocess.Popen(main_command, cwd=ROOT, env=main_env, stdin=subprocess.DEVNULL, stdout=mainlog, stderr=mainlog)
            processes.append(main_process)
            time.sleep(1)
            if main_process.poll() is not None: raise RuntimeError('MAIN exited; see main-stderr.log')
            gui = subprocess.Popen(gui_command, cwd=ROOT, env=gui_env, stdin=subprocess.DEVNULL, stdout=guilog, stderr=guilog)
            processes.append(gui)
            if args.frame_interval:
                snapshots = FrameSnapshots(run, args.frame_interval, time.monotonic())
            if args.ui:
                viewer = subprocess.Popen([sys.executable, '-m', 'tools.cdj_gui.view_ui',
                    '--attach', '--device-name', 'CDJ-2000NXS',
                    '--output', str(run / 'screen.ppm'),
                    '--control-port', str(args.port + 4)], cwd=ROOT)
                processes.append(viewer)
            print(f'MAIN {main_process.pid}, GUI {gui.pid}; logs: {run}', flush=True)
            deadline = time.monotonic() + args.seconds + 5
            while gui.poll() is None and time.monotonic() < deadline:
                if snapshots is not None:
                    snapshots.poll(time.monotonic())
                if main_process.poll() is not None: raise RuntimeError('MAIN exited during run')
                if viewer is not None and viewer.poll() is not None:
                    if viewer.returncode != 0: raise RuntimeError('Deck viewer exited with an error')
                    break
                time.sleep(.1)
            closed = viewer is not None and viewer.poll() == 0
            result = dict(gui_exit=gui.poll(), viewer_closed=closed,
                          timed_out=gui.poll() is None and not closed,
                          frame_exists=(run / 'screen.ppm').exists())
            print(json.dumps(result), flush=True)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            if snapshots is not None:
                run_manifest['frame_snapshots'] = snapshots.manifest()
            final_inputs = {}
            for name, path in inputs.items():
                try:
                    final_inputs[name] = input_metadata(path)
                except (OSError, RuntimeError) as error:
                    final_inputs[name] = dict(error=str(error))
            run_manifest['input_artifacts_after_run'] = final_inputs
            run_manifest['inputs_differ_at_exit'] = [name for name in inputs
                if input_artifacts[name] != final_inputs[name]]
            run_manifest['input_provenance_notes'] = ('Hashes observed before launch and after exit; '
                                                       'not continuous file-mutation monitoring')
            (run / 'run.json').write_text(json.dumps(run_manifest, indent=2) + '\n')
            finalize_dsp_artifacts(run, firmware, args.functional_dsp_timing,
                                   args.functional_dsp_audio, args.capture_dsp_tx)
            (run / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result.get('viewer_closed') or (result.get('gui_exit') == 0 and result.get('frame_exists')) else 1


if __name__ == '__main__':
    raise SystemExit(main())
