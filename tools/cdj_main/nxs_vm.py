# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the experimental NXS MAIN and GUI profiles over a direct serial link.

No proxy or generated status packets. DSP uses UHPI transport and a partial C674x interpreter.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path, help='new run directory, relative to repository')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--port', type=int, default=5980)
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/qemu/build/qemu-system-sh4')
    args = parser.parse_args()
    if args.seconds <= 0 or not 1024 <= args.port <= 65531:
        parser.error('positive duration and port 1024..65531 required')
    run = (ROOT / args.run).resolve()
    firmware = ROOT / 'firmware/nxs'
    simulator = ROOT / 'bin/cdj-run'
    for path in (args.qemu, simulator, firmware / 'main-firmware.bin', firmware / 'gui-boot-memory.elf', firmware / 'gui-flash-image.bin'):
        if not path.is_file(): parser.error(f'missing input: {path}')
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
    main_env['CDJ_REQ_STATUS_FRESH'] = '0'
    (run / 'run.json').write_text(json.dumps(dict(main=main_command, gui=gui_command,
        gui_environment=overrides, main_environment={k:v for k,v in main_env.items() if k.startswith('CDJ_')},
        dsp='NXS UHPI plus partial C674x interpreter; incomplete ISA, ROM handoff abstraction', profile='experimental NXS'), indent=2) + '\n')
    processes = []
    result = {}
    with (run / 'main-stderr.log').open('w') as mainlog, (run / 'gui.log').open('w') as guilog:
        try:
            main_process = subprocess.Popen(main_command, cwd=ROOT, env=main_env, stdin=subprocess.DEVNULL, stdout=mainlog, stderr=mainlog)
            processes.append(main_process)
            time.sleep(1)
            if main_process.poll() is not None: raise RuntimeError('MAIN exited; see main-stderr.log')
            gui = subprocess.Popen(gui_command, cwd=ROOT, env=gui_env, stdin=subprocess.DEVNULL, stdout=guilog, stderr=guilog)
            processes.append(gui)
            print(f'MAIN {main_process.pid}, GUI {gui.pid}; logs: {run}', flush=True)
            deadline = time.monotonic() + args.seconds + 5
            while gui.poll() is None and time.monotonic() < deadline:
                if main_process.poll() is not None: raise RuntimeError('MAIN exited during run')
                time.sleep(.1)
            result = dict(gui_exit=gui.poll(), timed_out=gui.poll() is None, frame_exists=(run / 'screen.ppm').exists())
            print(json.dumps(result), flush=True)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            (run / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result.get('gui_exit') == 0 and result.get('frame_exists') else 1


if __name__ == '__main__':
    raise SystemExit(main())
