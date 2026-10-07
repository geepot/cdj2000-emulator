"""The NXS GUI board's update gate: install a GUI body, then roll it back.

    python -m tools.cdj_main.nxs_gui_update_check --body gui-body.bin --out runs/nxsupd/x

The CDJ-2000NXS counterpart of gui_update_check.  Stock NXS MAIN 1.44 on
nxs_vm, booted with RELOOP/EXIT + USB held (its USB update chord, mode 1) and
a USB stick holding an updater built from the stock C2KNXS.UPD with the
mods repo's cdjfw.repack.Updater, runs the player's own update path twice:

  install   the stock GUI takes the candidate body (title retitled Ver1.205,
            as tools/device/device_updater.py does) over the deck link --
            command 6, receiver 0x00d1019a, CRC-16/XMODEM, installer
            0x00d09db0 -- and programs its flash;
  rollback  the GUI booted from the flash the install left, through the LDR
            stream at 0x10000 (no ELF), takes the stock body retitled
            Ver9.999 (the frozen recovery's GUI title) and programs it back,
            under stock MAIN or, with --rollback-main, the MAIN a deck would
            be running then (the mods' native-transport GUIs cannot talk to
            stock MAIN at all: E-8709, the link dead after a dozen records).

The GUI reports its own version from its flash footer (1200 for stock and
these candidates), and MAIN selects a GUI component only when its title is
newer, hence the retitles.

--file all (default) puts the four-component C2KNXS.UPD on the stick.  MAIN
runs GUI first; DRIVE and PANEL follow and are selected too (the emulator has
no drive answering with a version, and no panel version), and the DRIVE
writer never finishes here, so the run is stopped once MAIN has moved past
the GUI component.  --file gui uses C2KNXSG.UPD, the GUI-only file of the
same dispatch table, where MAIN also reaches its own end ("*** Update END !
***", the "Firmware update is complete" screen).

After each pass the GUI's flash is dumped (BFIN_CFI_DUMP) and checked:
  - MAIN's updater selected the GUI component (update_progress_gui 1: the
    version gate passed) and left it without an error phase
    (update_phase_saved 0, gui_update_status not 0xff) and moved on, which it
    only does after the GUI reported success (status 2); with --file gui
    also update_phase 0xff and gui_update_status 3;
  - the body is at 0x10000 byte for byte;
  - the 64 KiB in front of it (a placeholder pattern: the real first-stage
    loader is in no update file and is not emulated) is untouched;
  - the brightness journal sector at 0x1FC000 was never erased and its
    seeded entries are intact (the GUI may append, which is normal).
If the body carries the code-pack unpacker (an LDR block to 0x01e40000), the
rollback pass must show that code ran (cdj-gui-run's line coverage).

What this cannot tell: the real sector-0 loader is not modelled -- the
emulator jumps straight into the LDR at 0x10000 -- so "the loader starts the
new body" and "a power cut while programming" stay untested.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

from tools.cdj_main.qmp import Qmp
from tools.paths import GUI_RUN

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable
MODS = ROOT.parent / "cdj-2000nxs-mods"
# The neutral NXS frame (REV high) plus RELOOP/EXIT (16.2) and USB (19.2).
UPDATE_FRAME = "00000000000000000000000000000002040000040000"
BODY = 0x1EC000
BODY_AT = 0x10000
TOP = 0x1FC000
SIZE = 0x200000
PLACEHOLDER = b"NXS GUI SECTOR 0: placeholder, no real loader dump. "
JOURNAL_SEED = b"\x00\x03" * 2          # two entries of what the stock GUI appends
UNPACKER = (0x01E40000, 0x01E50000)     # the code pack's BOOT_UNPACK region
# MAIN 1.44 updater state (main-normal.bin, update_mode1_task 0x043524a0).
# update_progress_gui is 1 once phase 6 has selected the GUI component (the
# version gate passed).  The updater code and its literal pools
# (0x04351000-0x04354000) are identical in the mods' merged MAIN, so the same
# addresses hold for --rollback-main.
STATE = dict(update_phase=0x055773B4, update_phase_saved=0x055773B8,
             update_write_index=0x055773C0, update_progress_gui=0x055773C8,
             gui_update_status=0x049874BC)
FILES = dict(all="C2KNXS.UPD", gui="C2KNXSG.UPD")

BUILD = r"""
import sys
from pathlib import Path
from cdjfw.repack import Updater
stock, body, version, kind, out = sys.argv[1:]
u = Updater.load(Path(stock))
if body != '-':
    data = Path(body).read_bytes()
    if len(data) != len(u.bodies['gui']):
        raise SystemExit(f'{body}: 0x{len(data):x} bytes, the GUI body is 0x{len(u.bodies["gui"]):x}')
    u.bodies['gui'] = data
u.retitle_version('gui', b'Ver1.200', version.encode())
Path(out).write_bytes(u.build() if kind == 'all' else u.component_payload('gui'))
"""


def ldr_targets(body: bytes) -> list[int]:
    """Destinations of the body's LDR blocks, up to the final one."""
    off, out = 0, []
    while off + 10 <= len(body):
        dest, count, flags = struct.unpack_from("<IIH", body, off)
        off += 10 + (0 if flags & 1 else count)
        out.append(dest)
        if flags & 0x8000:
            break
    return out


def has_unpacker(body: bytes) -> bool:
    return any(UNPACKER[0] <= d < UNPACKER[1] for d in ldr_targets(body))


def gui_component(upd: bytes) -> bytes:
    """The GUI component of a C2KNXS.UPD or C2KNXSG.UPD: title, body, trailer."""
    at = upd.find(b"CDJ-2000NXS GUI ")
    if at < 0:
        raise SystemExit("nxs_gui_update_check: no GUI component")
    return upd[at:at + 32 + BODY + 4]


def body_of(upd: bytes) -> bytes:
    component = gui_component(upd)
    if component[31:32] != b"1":
        raise SystemExit("nxs_gui_update_check: GUI title byte 31 is not '1' -- the "
                         "installer would write from 0x0, over the loader; refused")
    return component[32:32 + BODY]


def seed_flash(body: bytes) -> bytes:
    flash = bytearray(b"\xff" * SIZE)
    flash[:BODY_AT] = (PLACEHOLDER * (BODY_AT // len(PLACEHOLDER) + 1))[:BODY_AT]
    flash[BODY_AT:BODY_AT + len(body)] = body
    flash[TOP:TOP + len(JOURNAL_SEED)] = JOURNAL_SEED
    return bytes(flash)


def build_updater(mods: Path, stock: Path, body: Path | None, version: str, kind: str,
                  out: Path) -> Path:
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([PY, "-c", BUILD, str(stock), str(body) if body else "-", version, kind,
                    str(out)], cwd=mods / "tools", check=True)
    return out


def stick(directory: Path, upd: Path) -> Path:
    image = directory.with_suffix(".img")
    subprocess.run([PY, "-m", "tools.cdj_main.make_sd_image", str(directory), str(image),
                    "--size", "64M"], cwd=ROOT, check=True, capture_output=True)
    return image


def read_state(qmp: Qmp) -> dict:
    state = {}
    for name, address in STATE.items():
        text = qmp.command("human-monitor-command", {"command-line": f"xp /1wx {address:#x}"})
        state[name] = int(text.rsplit(":", 1)[1].split()[0], 16)
    return state


def main_verdict(state: dict | None, kind: str) -> tuple[bool | None, str]:
    """True/False once MAIN has decided the GUI component, None while it runs."""
    if not state:
        return None, "no MAIN state read"
    if state["update_phase_saved"] or state["gui_update_status"] == 0xFF:
        return False, f"updater error phase {state['update_phase_saved']:#x}"
    if kind == "gui":
        if state["update_phase"] == 0xFF and state["update_progress_gui"] != 1:
            return False, "GUI component not selected (version gate)"
        if state["update_phase"] == 0xFF and state["gui_update_status"] == 3:
            return True, "Update END (phase 0xff, status 3)"
        return None, "running"
    if state["update_write_index"] >= 2:
        if state["update_progress_gui"] != 1:
            return False, "GUI component not selected (version gate)"
        if state["gui_update_status"] != 2:
            return False, (f"moved on with GUI status {state['gui_update_status']}, "
                           "not 2 (the GUI's success report)")
        return True, f"past the GUI component (write index {state['update_write_index']})"
    return None, "running"


def run_update(name: str, out: Path, port: int, usb: Path, flash: Path, kind: str,
               qemu: Path, timeout: float, main_firmware: Path | None = None) -> tuple[Path, dict | None, str, str]:
    """One update run; returns the dump, MAIN's last state, its verdict text and gui.log."""
    run = out / name
    dump = out / f"{name}-flash-after.bin"
    command = [PY, "-m", "tools.cdj_main.nxs_vm", os.path.relpath(run, ROOT),
               "--port", str(port), "--seconds", str(int(timeout) + 60), "--debug",
               "--frame-interval", "10", "--qemu", str(qemu), "--usb", str(usb),
               "--source-key", "none", "--panel-frame", UPDATE_FRAME,
               "--gui-flash-boot", str(flash),
               "--gui-env", f"BFIN_CFI_DUMP={dump}",
               # A link burst lands once, whole, as the SPORT DMA delivers it:
               # the mods GUIs' own receivers need it, the stock GUI is happy.
               "--gui-env", "BFIN_LINK_NATIVE_PARTIAL_DMA=1",
               "--gui-env", f"BFIN_CODE_RANGE={UNPACKER[0]:#x}:{UNPACKER[1]:#x}"]
    if main_firmware:
        command += ["--main-firmware", str(main_firmware)]
    log = open(out / f"{name}-nxs_vm.out", "w")
    proc = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    state, verdict, settled = None, None, None
    start = time.time()
    try:
        while time.time() - start < timeout and proc.poll() is None:
            time.sleep(2)
            try:
                with Qmp(run / "qmp.sock") as qmp:
                    state = read_state(qmp)
            except (OSError, ValueError, IndexError):
                continue
            ok, _ = main_verdict(state, kind)
            if ok is not None and settled is None:
                settled = time.time()
            if settled and time.time() - settled > 6:      # let the GUI finish its screen
                break
    finally:
        if run.is_dir():
            (run / "stop-request.json").write_text("{}\n")
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=30)
        log.close()
        shutil.rmtree(run / "dsp-checkpoints", ignore_errors=True)
    leftover = subprocess.run(["pgrep", "-f", f"qemu-system-sh4.*127.0.0.1:{port},"],
                              capture_output=True, text=True).stdout.split()
    if leftover:
        raise SystemExit(f"nxs_gui_update_check: qemu {leftover} outlived the run; stop it")
    _, verdict = main_verdict(state, kind)
    (out / f"{name}-main-state.json").write_text(json.dumps(state, indent=1) + "\n")
    gui_log = (run / "gui.log").read_text(errors="replace") if (run / "gui.log").exists() else ""
    return dump, state, verdict, gui_log


def erases(gui_log: str) -> list[tuple[int, int]]:
    return [(int(a, 16) - 0x20000000, int(n, 16))
            for a, n in re.findall(r"flash erase 0x([0-9a-f]+) \+0x([0-9a-f]+)", gui_log)]


def verify(name: str, dump: Path, before: bytes, body: bytes, state: dict | None,
           kind: str, gui_log: str, unpacker: bool) -> list[tuple[str, bool]]:
    ok, why = main_verdict(state, kind)
    checks = [(f"{name}: MAIN ended the GUI component normally ({why})", ok is True),
              (f"{name}: the GUI booted from the flash's LDR at 0x10000, not an ELF",
               "booting the flash's LDR at 0x10000" in gui_log)]
    wiped = erases(gui_log)
    checks += [(f"{name}: no erase below 0x10000 or of the 0x1FC000 journal",
                bool(wiped) and all(BODY_AT <= at and at + n <= TOP for at, n in wiped))]
    if unpacker:
        run = re.search(r"RANGE 0x[0-9a-f]+:0x[0-9a-f]+ lines_run=(\d+)", gui_log)
        checks.append((f"{name}: the code pack's unpacker ran "
                       f"({run.group(1) if run else 'no'} code lines)",
                       bool(run and int(run.group(1)))))
    if not dump.exists():
        return checks + [(f"{name}: flash dump written", False)]
    after = dump.read_bytes()
    journal = [i for i in range(TOP, SIZE) if before[i] != 0xFF]
    checks += [
        (f"{name}: body at 0x10000 byte for byte", after[BODY_AT:BODY_AT + BODY] == body),
        (f"{name}: sector 0 (the first-stage loader's place) untouched",
         after[:BODY_AT] == before[:BODY_AT]),
        (f"{name}: journal entries at 0x1FC000 kept",
         all(after[i] == before[i] for i in journal)),
    ]
    return checks


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--body", type=Path, help="the GUI body to check (default: stock)")
    parser.add_argument("--stock", type=Path, default=MODS / "inputs/C2KNXS.UPD",
                        help="the stock C2KNXS.UPD 1.44 the updaters are built from")
    parser.add_argument("--mods", type=Path, default=MODS,
                        help="the cdj-2000nxs-mods checkout (for cdjfw.repack)")
    parser.add_argument("--file", choices=sorted(FILES), default="all",
                        help="all: C2KNXS.UPD (default); gui: C2KNXSG.UPD")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=47300, help="takes PORT..PORT+11 (a fresh block for each pass)")
    parser.add_argument("--qemu", type=Path,
                        default=Path(os.environ.get("CDJ_QEMU",
                                                    ROOT / "build/qemu/build/qemu-system-sh4")))
    parser.add_argument("--rollback-main", type=Path,
                        help="the MAIN firmware the rollback pass runs (default stock 1.44): "
                             "a GUI built for the mods' native link transport only talks to "
                             "its paired MAIN, e.g. build/merged-main/main-firmware.bin")
    parser.add_argument("--timeout", type=float, default=600, help="wall seconds per run")
    args = parser.parse_args(argv)

    for port in range(args.port, args.port + 12):
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", port))     # as nxs_vm probes: TIME_WAIT counts
            except OSError:
                raise SystemExit(f"nxs_gui_update_check: port {port} is in use; pick --port")
    if not GUI_RUN.is_file() or not args.qemu.is_file():
        raise SystemExit(f"nxs_gui_update_check: need {GUI_RUN} and {args.qemu}")
    out = args.out.resolve()
    if out.exists():
        raise SystemExit(f"nxs_gui_update_check: {out} exists")
    out.mkdir(parents=True)
    name = FILES[args.file]
    install = build_updater(args.mods, args.stock.resolve(),
                            args.body.resolve() if args.body else None, "Ver1.205", args.file,
                            out / "stick-install" / name)
    rollback = build_updater(args.mods, args.stock.resolve(), None, "Ver9.999", args.file,
                             out / "stick-rollback" / name)
    new_body = body_of(install.read_bytes())
    old_body = body_of(rollback.read_bytes())
    before = seed_flash(old_body)
    (out / "flash-before.bin").write_bytes(before)

    print(f"install: the stock GUI takes {args.body or 'the stock body'}", flush=True)
    dump_a, state, verdict, log_a = run_update("install", out, args.port,
                                               stick(install.parent, install),
                                               out / "flash-before.bin", args.file, args.qemu,
                                               args.timeout)
    checks = verify("install", dump_a, before, new_body, state, args.file, log_a, False)

    if all(ok for _, ok in checks):
        print("rollback: the installed GUI, booted from its flash, takes the stock body",
              flush=True)
        flash_a = out / "installed-flash.bin"
        shutil.copyfile(dump_a, flash_a)
        dump_b, state, verdict, log_b = run_update("rollback", out, args.port + 6,
                                                   stick(rollback.parent, rollback), flash_a,
                                                   args.file, args.qemu, args.timeout,
                                                   args.rollback_main and args.rollback_main.resolve())
        checks += verify("rollback", dump_b, flash_a.read_bytes(), old_body, state, args.file,
                         log_b, has_unpacker(new_body))

    lines = [f"{'ok  ' if ok else 'FAIL'} {what}" for what, ok in checks]
    verdict = "PASS" if checks and all(ok for _, ok in checks) else "NOT SAFE"
    rollback_main = args.rollback_main or "stock MAIN 1.44"
    report = "\n".join(lines) + (f"\n\n{verdict}: {args.body or 'stock GUI body'} ({name}; "
                                  f"rollback under {rollback_main})\n")
    (out / "REPORT.txt").write_text(report)
    print(report)
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
