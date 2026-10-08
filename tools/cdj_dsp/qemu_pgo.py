"""Build isolated Clang PGO QEMU variants, replacing only DSP objects.

First run scripts/build-qemu-sh4.sh with CDJ_C674X_AOT_SOURCE. This tool
keeps Ninja's normal objects/binary intact. Train with the NXS launcher's
--dsp-playback-metrics and --dsp-playback-profile, then merge the raw file
with llvm-profdata and pass --profile plus --training-build here.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dsp_source(path: Path) -> bool:
    return (path.name.startswith(('cdj_c674', 'cdj_dsp_')) or
            path.name == 'cdj2000_nxs_hpi.c') and path.suffix == '.c'


def replace_output(command: list[str], output: Path) -> list[str]:
    command = command.copy()
    command[command.index('-o') + 1] = str(output)
    # Do not overwrite Ninja's depfiles for the original objects.
    for flag in ('-MF', '-MQ', '-MT'):
        if flag in command:
            index = command.index(flag)
            del command[index:index + 2]
    return command


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build/qemu/build'))
    parser.add_argument('--output', type=Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--train', action='store_true')
    group.add_argument('--profile', type=Path)
    parser.add_argument('--training-build', type=Path,
                        help='training variant build.json; required with --profile')
    parser.add_argument('--reuse', type=Path, help='reuse verified unchanged DSP objects from an earlier variant build.json')
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    if args.profile and not args.training_build:
        parser.error('--profile requires --training-build for source/object identity checks')
    if output.exists():
        parser.error('--output must be a new directory')
    commands = json.loads((build / 'compile_commands.json').read_text())
    selected = [entry for entry in commands if dsp_source(Path(entry['file']))
                and 'sh4-softmmu' in entry['command']]
    if not selected:
        parser.error('no SH4 DSP objects found')
    link_lines = subprocess.check_output(
        ['ninja', '-t', 'commands', 'qemu-system-sh4'], cwd=build, text=True).splitlines()
    links = [shlex.split(line) for line in link_lines if
             ' -o qemu-system-sh4' in line and ' -c ' not in line]
    if len(links) != 1:
        parser.error('expected one native QEMU link command (response files unsupported)')
    link = links[0]
    if any(arg.startswith('@') for arg in link):
        parser.error('response-file link commands are unsupported')
    version = subprocess.check_output([link[0], '--version'], text=True)
    if 'clang' not in version.lower():
        parser.error('Clang is required')
    source_dir = (build / Path(selected[0]['file']).parent).resolve()
    sources = {p.name: digest(p) for p in sorted(source_dir.glob('*'))
               if p.suffix in ('.h', '.c', '.inc') and p.name.startswith('cdj')}
    if 'cdj_c674x_aot.inc' not in sources:
        parser.error('build the stock AOT QEMU first; no cdj_c674x_aot.inc found')
    replacements = {}
    compile_plan = []
    for entry in selected:
        command = shlex.split(entry['command'])
        original = command[command.index('-o') + 1]
        target = output / (Path(entry['file']).stem + '.o')
        if original in replacements:
            parser.error(f'duplicate object: {original}')
        replacements[original] = str(target)
        command = replace_output(command, target)
        if args.train:
            command += ['-fprofile-instr-generate', '-DCDJ_DSP_PGO_TRAIN=1']
        else:
            command += [f'-fprofile-instr-use={args.profile.resolve()}',
                        '-Werror=profile-instr-out-of-date',
                        '-Werror=profile-instr-unprofiled']
        compile_plan.append(command)
    if not all(obj in link for obj in replacements):
        parser.error('selected objects are not direct inputs of the QEMU link')
    # Verify all other link inputs, not only the DSP, between train and use.
    inputs = {arg: digest(build / arg) for arg in link
              if (arg.endswith(('.o', '.a')) and (build / arg).is_file())
              and arg not in replacements}
    identity = dict(sources=sources, other_objects=inputs, compiler=version,
                    dsp_compile_commands=[entry['command'] for entry in selected],
                    base_link_command=link.copy())
    if args.profile:
        trained = json.loads(args.training_build.read_text())
        if trained.get('mode') != 'train':
            parser.error('--training-build must be an instrumented training manifest')
        if any(trained.get(key) != value for key, value in identity.items()):
            parser.error('QEMU sources, other objects, or compiler changed since training')
        if not args.profile.is_file():
            parser.error('profile does not exist')
    profile_hash = digest(args.profile) if args.profile else None
    output.mkdir(parents=True)
    binary = output / 'qemu-system-sh4'
    link = [replacements.get(arg, arg) for arg in link]
    unsigned = output / 'qemu-system-sh4-unsigned'
    signed = any('entitlement.sh' in line for line in link_lines)
    link = replace_output(link, unsigned if signed else binary)
    if args.train:
        link += ['-fprofile-instr-generate']
    def run(command: list[str], label: str) -> None:
        with (output / (label + '.log')).open('w') as log:
            subprocess.run(command, cwd=build, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    reused = []
    previous = json.loads(args.reuse.read_text()) if args.reuse else {}
    for command, entry in zip(compile_plan, selected):
        target = Path(command[command.index('-o') + 1])
        name = Path(entry['file']).name
        shared = [key for key in sources if key.endswith(('.h', '.inc'))] + [name]
        old_command = next((c for c in previous.get('compile_commands', [])
                            if Path(c[c.index('-o') + 1]).name == target.name), None)
        old_object = args.reuse.parent / target.name if args.reuse else None
        old_record = previous.get('objects', {}).get(target.name, {})
        if (old_command and previous.get('compiler') == version and
                previous.get('profile_sha256') == profile_hash and
                previous.get('other_objects') == inputs and
                all(previous.get('sources', {}).get(key) == sources[key] for key in shared) and
                replace_output(old_command, target) == command and old_object.is_file() and
                digest(old_object) == old_record.get('sha256')):
            shutil.copyfile(old_object, target)
            reused.append(target.name)
        else:
            run(command, target.stem)
    run(link, 'link')
    if signed:
        sign_lines = [shlex.split(line) for line in link_lines if 'entitlement.sh' in line]
        if len(sign_lines) != 1:
            raise RuntimeError('expected one entitlement command')
        sign = sign_lines[0]
        sign[1:3] = [str(binary), str(unsigned)]
        run(sign, 'entitlement')
    run([str(binary), '-M', 'help'], 'machines')
    if any(digest(source_dir / name) != value for name, value in sources.items()):
        raise RuntimeError('source changed during build; discard this variant')
    if any(digest(build / name) != value for name, value in inputs.items()):
        raise RuntimeError('non-DSP link input changed during build; discard this variant')
    manifest = dict(**identity, reused_objects=reused, mode='train' if args.train else 'use',
                    compile_commands=compile_plan, link_command=link,
                    binary=str(binary), binary_sha256=digest(binary),
                    objects={Path(path).name: dict(bytes=Path(path).stat().st_size,
                              sha256=digest(Path(path))) for path in replacements.values()},
                    profile_sha256=profile_hash)
    (output / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(binary)


if __name__ == '__main__':
    main()
