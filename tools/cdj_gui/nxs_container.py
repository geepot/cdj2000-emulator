# SPDX-License-Identifier: GPL-2.0-or-later
"""Split the NXS combined updater into byte-identical component updaters.

This prepares inputs; it does not assert that the original CDJ-2000 MAIN
board model supports NXS MAIN firmware.
"""
from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path

COMPONENTS = (("gui", "C2KGUI.UPD", 4, "big"),
              ("drive", "C2KDRIV.UPD", 2, "little"),
              ("main", "C2KMAIN.UPD", 2, "little"),
              ("panel", "C2KPANL.UPD", 2, "little"))


def split_container(data: bytes) -> dict[str, bytes]:
    cursor = 0
    sizes = []
    for _ in COMPONENTS:
        end = data.find(b"\r\n", cursor)
        if end < 0 or not data[cursor:end].isdigit():
            raise ValueError("expected four CRLF-terminated decimal lengths")
        sizes.append(int(data[cursor:end]))
        cursor = end + 2
    if cursor + sum(sizes) != len(data):
        raise ValueError("container size does not match manifest")
    result = {}
    for (name, filename, width, order), size in zip(COMPONENTS, sizes):
        if size < 32 + width:
            raise ValueError(f"{name}: truncated component")
        raw = data[cursor:cursor + size]
        cursor += size
        expected_title = f"CDJ-2000NXS {dict(drive='DRIV', panel='PANL').get(name, name.upper())}".encode()
        if not raw[:32].startswith(expected_title):
            raise ValueError(f"{name}: unsupported component title")
        if int.from_bytes(raw[-width:], order) != binascii.crc_hqx(raw[:-width], 0):
            raise ValueError(f"{name}: CRC mismatch")
        result[filename] = raw
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("update", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = args.update.read_bytes()
    try:
        components = split_container(data)
    except ValueError as error:
        parser.error(str(error))
    args.output.mkdir(parents=True, exist_ok=True)
    for filename, raw in components.items():
        (args.output / filename).write_bytes(raw)
    manifest = {"source_sha256": hashlib.sha256(data).hexdigest(),
                "model": "CDJ-2000NXS", "main_board_support": "unverified",
                "components": {name: {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
                               for name, raw in components.items()}}
    (args.output / "container.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
