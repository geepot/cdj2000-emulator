"""Keep the manual-derived ISA audit tooling honest.

Neither tool is part of the emulator; both exist so that an instruction-coverage
claim has a denominator that comes from SPRUFE8B rather than from our own
decoder.  The parsing in them is real enough to break silently, so:

* the committed inventory must still match what the parser produces from the
  manual text index, and must still resolve every row (no row without a
  functional unit that TI gives one, no row without a description page);
* the probe's assembler-listing parser and the probe binary must still agree on
  encodings whose verdict we can state independently.

Everything here skips when its prerequisite is absent - the manuals live in
git-ignored build/references and the TI assembler is not part of the repository.
"""
from pathlib import Path
import json
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
INDEX = ROOT / "build" / "references" / "sprufe8b.txt"
INVENTORY = ROOT / "analysis" / "dsp" / "isa_manual_inventory.json"
PROBE = ROOT / "tests" / "cstub" / "c674x-isa-probe.c"


def test_committed_inventory_is_internally_consistent():
    data = json.loads(INVENTORY.read_text())
    counts, rows = data["counts"], data["instructions"]
    assert counts["c674x_instruction_rows"] == len(rows)
    assert counts["rows_without_description_page"] == 0
    # A check-marked line the row regex cannot parse is how the two MPY32
    # result-width rows were silently dropped once; it must never be nonzero.
    assert counts["unparsed_checkmark_lines"] == 0
    assert data["parse_diagnostics"]["unparsed_checkmark_lines"] == []
    # Only the Appendix H no-unit instructions may lack a functional unit.
    unitless = {name for name, row in rows.items() if not row["units"]}
    assert unitless == set(data["parse_diagnostics"]["rows_without_unit"])
    assert unitless == {
        "DINT", "IDLE", "NOP", "RINT", "SPKERNEL", "SPKERNELR", "SPLOOP",
        "SPLOOPD", "SPLOOPW", "SPMASK", "SPMASKR", "SWE", "SWENR",
    }
    # Every retained row is a C674x instruction with a real manual citation.
    for name, row in rows.items():
        assert row["families"]["c674x"], name
        first, last = row["description_pages"]
        assert 98 <= first <= last <= 708, (name, row["description_pages"])


def test_inventory_matches_the_manual_text_index():
    if not INDEX.exists():
        pytest.skip("run python -m tools.cdj_dsp.refdocs to fetch the manuals")
    from tools.cdj_dsp.isa_inventory import build

    fresh = build(INDEX)
    committed = json.loads(INVENTORY.read_text())
    assert fresh["counts"] == committed["counts"]
    assert fresh["instructions"] == committed["instructions"]


def test_probe_classifies_encodings_we_can_reason_about_independently(tmp_path):
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("requires C compiler")
    binary = tmp_path / "isa-probe"
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "emulator/qemu"), str(PROBE),
         str(ROOT / "emulator/qemu/cdj_c674x.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_uncond.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_mpy.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_sp.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_control.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_loop.c"), "-o", str(binary)],
        check=True)
    words = ["02988078", "0280022a", "ffffffff"]
    result = subprocess.run([str(binary), "pointer"], input="\n".join(words) + "\n",
                            capture_output=True, text=True, check=True, timeout=30)
    verdicts = dict(line.split(None, 1) for line in result.stdout.splitlines())
    # ADD .L1 A4,A6,A5 and MVKL .S2 4,B5, both straight from TI's assembler.
    assert verdicts["02988078"] == "accept"
    assert verdicts["0280022a"] == "accept"
    # creg=11111 is a reserved predicate in SPRUFE8B Table 3-1, so a core that
    # accepted it would be wrong; the probe must report the rejection, not hide it.
    assert verdicts["ffffffff"].startswith("execute-reject")


def test_probe_driver_reads_the_assembler_listing(tmp_path):
    ti_bin = Path("/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin")
    if not (ti_bin / "asm6x").exists():
        pytest.skip("requires the TI C6000 assembler")
    from tools.cdj_dsp.isa_probe import assemble

    rows = [
        {"source": "ADD .L1 A4, A6, A5"},
        {"source": "MVKL .S2 4, B5"},
        {"source": "NOTANINSTRUCTION .L1 A4, A5"},
    ]
    assemble(rows, ti_bin, tmp_path)
    assert rows[0]["word"] == "02988078"
    assert rows[1]["word"] == "0280022A"
    # A rejected line must surface the assembler's reason, never a silent None
    # that the report would then read as an unsupported instruction.
    assert rows[2]["word"] is None
    assert "E0002" in rows[2]["assembler_error"]


# --------------------------------------------------------------------------
# The three audit sweeps.  These assertions exist so that the numbers quoted in
# DSP_ARCHITECTURE_COVERAGE.md stay reproducible: if the core changes, a sweep
# count changes and one of these fails, which is the point.
# --------------------------------------------------------------------------

def _sweep(mode, tmp_path):
    from tools.cdj_dsp.audit_sweeps import build, run
    return run(build(tmp_path), mode)


def test_packet_rejection_rolls_back_every_byte_of_state(tmp_path):
    if not shutil.which("cc"):
        pytest.skip("requires C compiler")
    from tools.cdj_dsp.audit_sweeps import atomicity

    result = atomicity(_sweep("atomicity", tmp_path))
    assert result["failures"] == 0, result
    # Each case must actually have been rejected, or it proves nothing.
    assert result["cases"] and not any(c["executed"] for c in result["cases"])
    for case in result["cases"]:
        assert case["state_unchanged"] and case["memory_unchanged"], case


def test_compact_decode_gap_is_exactly_the_measured_set(tmp_path):
    if not shutil.which("cc"):
        pytest.skip("requires C compiler")
    from tools.cdj_dsp.audit_sweeps import compact

    text = _sweep("compact", tmp_path)
    result = compact(text)
    assert result["words_swept"] == 0x10000
    # Decoder-only figure: independent of the probe's registers and memory window.
    # Was 11,440 when the audit was written.  Two formats account for the whole
    # drop and nothing else:
    #   Figure F-29 Sx2op, (w & 0x047e) == 0x002e - 9 free bits, 512 words;
    #   Figure E-5 M3, the compact .M multiply format - 4,096 words, of which 112
    #     are already claimed by Figure G-4 LSDx1 (unit field 11b), so it adds
    #     3,984.
    # 11,440 - 512 - 3,984 = 6,944.  Figure F-32 Sx1b with s = 0 is deliberately
    # NOT opened - whether it is architecturally legal is an open question, see
    # the Sx1b arm in cdj_c674x.c - so its 128 words remain in this count.
    assert result["defensible_coverage_figure"]["value"] == 6944
    # Nothing may be rejected for reasons that are properties of the probe.
    assert set(result["rejection_reasons"]) == {
        "compact instruction not implemented",
        "reserved compact LSDx1 instruction",
        "unaligned stack access or invalid register pair",
    }, result["rejection_reasons"]
    # Pin the 512-word delta to the format it is supposed to be, rather than to
    # a total that happens to agree, and assert that the Sx1b s = 0 words are
    # still refused.  The masks are read straight off the figures.
    verdicts = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            verdicts[int(parts[0], 16)] = parts[1]
    assert len(verdicts) == 0x10000
    sx2op = {w for w in range(0x10000) if w & 0x047e == 0x002e}
    sx1b_s0 = {w for w in range(0x10000) if w & 0x187f == 0x006e}
    assert len(sx2op) == 512 and len(sx1b_s0) == 128
    assert not sx2op & sx1b_s0
    unaccepted = sorted(w for w in sx2op if verdicts[w] != "accept")
    assert not unaccepted, [f"{w:04x}" for w in unaccepted]
    still_closed = sorted(w for w in sx1b_s0 if verdicts[w] == "accept")
    assert not still_closed, [f"{w:04x}" for w in still_closed]


def test_control_register_reachability_and_reserved_bits(tmp_path):
    if not shutil.which("cc"):
        pytest.skip("requires C compiler")
    from tools.cdj_dsp.audit_sweeps import control

    result = control(_sweep("control", tmp_path))
    assert result["ids_swept"] == 32
    # AMR CSR IFR IER ISTP IRP NRP ILC RILC FADCR FAUCR FMCR SSR TSR ITSR.
    assert result["readable_ids"] == [0, 1, 2, 4, 5, 6, 7, 13, 14, 18, 19, 20, 21, 26, 27]
    # ISR (3) is write-only, which is what SPRUFE8B specifies.
    assert result["writable_ids"] == [0, 1, 2, 3, 4, 5, 6, 7, 13, 14, 18, 19, 20, 21, 26, 27]

    masks = result["rows"]
    # IRP, NRP, ILC and RILC are full 32-bit registers with no reserved field
    # (SPRUFE8B Figures 2-21, 2-24 and the IRP/NRP figures), so storing every bit
    # is correct for these four.
    for ident in (6, 7, 13, 14):
        assert masks[str(ident)]["read_mask"] == "ffffffff"
    # FADCR, FAUCR and FMCR each reserve bits 31-27 and 15-11, which SPRUFE8B
    # Tables 2-25 (printed page 59), 2-26 (printed page 61) and 2-27 (printed
    # page 63) say are "always read as 0" and unaffected by writes; every other
    # bit is R/W by MVC in Figures 2-29/2-30/2-31.  ~0x07ff07ff is exactly the
    # two reserved ranges, so this is the manual's mask, not a measurement.
    for ident in (18, 19, 20):
        assert masks[str(ident)]["read_mask"] == "07ff07ff"


def test_single_precision_rounding_against_an_independent_oracle(tmp_path):
    """Put the audit's SP-rounding characterization test into the suite.

    The oracle is the host FPU under ``fesetround``, not the emulator: for SP
    normal operands the exact product and (for nearby exponents) the exact sum fit
    in a double, so one ``double -> float`` conversion is a single correctly
    rounded SP rounding.  Without this wrapper the measurement was an audit
    artefact that no regression run would ever repeat.
    """
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("requires C compiler")
    source = ROOT / "tests" / "cstub" / "c674x-audit-sp-rounding.c"
    binary = tmp_path / "sp-rounding"
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "emulator/qemu"), str(source),
         str(ROOT / "emulator/qemu/cdj_c674x.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_uncond.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_mpy.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_sp.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_control.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_loop.c"), "-o", str(binary)],
        check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            timeout=120, check=True)
    assert "failures=0" in result.stdout, result.stdout
