"""Resume a machine a gdb client left halted.

    python -m tools.cdj_main.vm_resume PORT        # PORT is the run's --port / CDJ_LINK_PORT; the gdb stub is PORT+3

A client that connects to the QEMU gdb stub and goes away without `c` or `D` (a probe that crashed, a Ctrl+C
in the middle of a poll) leaves the machine stopped: 0 % CPU, guest time frozen at one value, and with the
Pro DJ Link hub every other deck waits for its promise.  Connecting again and sending `D` starts it.
"""
import argparse

from tools.cdj_main.gdbprobe import Rsp


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("port", type=int, help="the run's --port / CDJ_LINK_PORT (the gdb stub is PORT+3)")
    port = parser.parse_args().port
    with Rsp(port + 3, timeout=5.0) as stub:
        stub.buf = b""
        print("stub answered:", stub.cmd("?", 3.0)[:20] or "(nothing)")
    print("detached, the machine runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
