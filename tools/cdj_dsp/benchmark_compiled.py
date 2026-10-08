"""Compare Clang PGO/LTO DSP replay builds with byte-exact continuation gates.

This measures local checkpoint continuations, not connected playback or realtime
audio. Generated binaries, profiles and firmware-derived output stay in --output.
Use separate --train-checkpoint inputs to measure on held-out checkpoints.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time

from .replay import ROOT, SOURCES, checkpoint_info

try:
    import resource
except ImportError:  # Windows: wall time remains available.
    resource = None


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def positive(text):
    n = int(text)
    if n <= 0:
        raise argparse.ArgumentTypeError('must be positive')
    return n


def cpu_time():
    return resource.getrusage(resource.RUSAGE_CHILDREN).ru_utime if resource else None


def run(binary, checkpoint, steps, destination, env, boot_phase):
    stdout = destination.with_suffix('.jsonl')
    stderr = destination.with_suffix('.stderr')
    final = destination.with_suffix('.cdjdsp')
    before = cpu_time()
    started = time.perf_counter()
    with stdout.open('wb') as out, stderr.open('wb') as err:
        subprocess.run([str(binary), str(checkpoint), str(steps), '0', '0', '0',
                        str(boot_phase), str(final)], env=env, stdout=out,
                       stderr=err, check=True)
    elapsed = time.perf_counter() - started
    after = cpu_time()
    stop = None
    with stdout.open() as source:
        for line in source:
            event = json.loads(line)
            if event.get('event') == 'stop':
                stop = event
    if not stop or stop['reason'] != 'step_limit' or stop['fault']:
        raise RuntimeError(f'{destination}: continuation did not reach its step limit')
    return dict(wall_seconds=elapsed,
                user_seconds=after - before if resource else None,
                stdout_sha256=digest(stdout), checkpoint_sha256=digest(final),
                packets=stop['packets'], cycles=stop['cycles'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, action='append', required=True)
    parser.add_argument('--train-checkpoint', type=Path, action='append')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steps', type=positive, default=60000000)
    parser.add_argument('--train-steps', type=positive, default=60000000)
    parser.add_argument('--trials', type=positive, default=3)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--profdata', help='llvm-profdata executable (default: PATH or xcrun)')
    parser.add_argument('--pgo', action='store_true', help='train and apply Clang instrumentation PGO')
    parser.add_argument('--lto', choices=('none', 'thin', 'full'), default='none',
                        help='candidate link-time optimization')
    parser.add_argument('--aot', type=Path, help='same generated AOT include in both builds')
    parser.add_argument('--functional-audio', action='store_true')
    parser.add_argument('--functional-timing', action='store_true')
    parser.add_argument('--boot-phase', type=int, choices=range(8), default=7)
    args = parser.parse_args()
    if not args.pgo and args.lto == 'none':
        parser.error('select --pgo and/or --lto')
    cc = shutil.which(args.cc)
    if not cc:
        parser.error('C compiler not found')
    version = subprocess.check_output([cc, '--version'], text=True)
    if 'clang' not in version.lower():
        parser.error('this experiment requires Clang')
    profdata = None
    if args.pgo:
        profdata = shutil.which(args.profdata or 'llvm-profdata')
        if not profdata and not args.profdata and shutil.which('xcrun'):
            profdata = subprocess.check_output(['xcrun', '--find', 'llvm-profdata'],
                                             text=True).strip()
        if not profdata:
            parser.error('llvm-profdata not found; specify --profdata')
    # Validate before creating an output directory or starting expensive builds.
    checkpoints = list(dict.fromkeys(p.resolve() for p in args.checkpoint))
    training = list(dict.fromkeys(p.resolve() for p in
                                 (args.train_checkpoint or checkpoints)))
    inputs = {str(p): checkpoint_info(p.read_bytes()) for p in
              dict.fromkeys(checkpoints + training)}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    input_dir = output / 'input'
    input_dir.mkdir()
    frozen = {}
    for i, ck in enumerate(dict.fromkeys(checkpoints + training)):
        dest = input_dir / f'{i}.cdjdsp'
        shutil.copyfile(ck, dest)
        if digest(dest) != inputs[str(ck)]['checkpoint_sha256']:
            raise RuntimeError(f'{ck}: checkpoint changed during snapshot')
        frozen[ck] = dest
    snapshot = output / 'source'
    snapshot.mkdir()
    sources = []
    for p in [*SOURCES, *sorted((ROOT / 'emulator/qemu').glob('*.h'))]:
        dest = snapshot / p.name
        shutil.copyfile(p, dest)
        if p in SOURCES:
            sources.append(str(dest))
    aot_flags = []
    if args.aot:
        dest = snapshot / 'cdj_c674x_aot.inc'
        shutil.copyfile(args.aot, dest)
        aot_flags = ['-DCDJ_C674X_AOT_FILE="' + dest.as_posix() + '"']
    source_hashes = {p.name: digest(p) for p in sorted(snapshot.iterdir())}
    # Keep caller tracing, capture and compiler-profile settings out of runs.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('CDJ_') and k != 'LLVM_PROFILE_FILE'}
    env.update(CDJ_C674X_JIT='1', CDJ_C674X_AOT='1' if args.aot else '0',
               CDJ_DSP_COMPACT_TRACE='1', CDJ_DSP_REPLAY_HORIZON='1',
               CDJ_DSP_REPLAY_RAM_DIRECT='1', CDJ_DSP_REPLAY_STATS='1',
               CDJ_NXS_DSP_FUNCTIONAL_AUDIO=str(int(args.functional_audio)),
               CDJ_NXS_DSP_FUNCTIONAL_TIMING=str(int(args.functional_timing)))
    builds = {}

    def build(name, flags):
        binary = output / (name + ('.exe' if os.name == 'nt' else ''))
        command = [cc, '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                   *flags, *aot_flags, '-I', str(snapshot), *sources,
                   '-lm', '-o', str(binary)]
        with (output / (name + '.build.log')).open('wb') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        builds[name] = dict(command=command, sha256=digest(binary))
        return binary

    print('Building baseline', flush=True)
    baseline = build('baseline', [])
    candidate_flags = [] if args.lto == 'none' else ['-flto=' + args.lto]
    if args.pgo:
        print('Building and training instrumented replay', flush=True)
        instrumented = build('instrumented', [*candidate_flags, '-fprofile-instr-generate'])
        raw = []
        for i, ck in enumerate(training):
            profile = output / f'train-{i}.profraw'
            run(instrumented, frozen[ck], args.train_steps, output / f'train-{i}',
                dict(env, LLVM_PROFILE_FILE=str(profile)), args.boot_phase)
            raw.append(str(profile))
        profile = output / 'replay.profdata'
        subprocess.run([profdata, 'merge', '-output=' + str(profile), *raw], check=True)
        candidate_flags.append('-fprofile-instr-use=' + str(profile))
    print('Building candidate', flush=True)
    candidate = build('candidate', candidate_flags)
    rows, summaries = [], []
    for i, ck in enumerate(checkpoints):
        reference = None
        for trial in range(args.trials):
            order = [('baseline', baseline), ('candidate', candidate)]
            if trial % 2:
                order.reverse()
            for name, binary in order:
                row = dict(checkpoint=i, trial=trial, build=name,
                           **run(binary, frozen[ck], args.steps, output / f'ck-{i}-{trial}-{name}',
                                 env, args.boot_phase))
                signature = (row['stdout_sha256'], row['checkpoint_sha256'])
                if reference is None:
                    reference = signature
                row['matches'] = signature == reference
                rows.append(row)
                print(f'checkpoint {i}, trial {trial}, {name}: '
                      f'{row["user_seconds"] or row["wall_seconds"]:.3f}s; '
                      f'matches={row["matches"]}', flush=True)
        metric = 'user_seconds' if resource else 'wall_seconds'
        medians = {name: statistics.median(r[metric] for r in rows
                   if r['checkpoint'] == i and r['build'] == name)
                   for name in ('baseline', 'candidate')}
        summaries.append(dict(checkpoint=str(ck), metric=metric, medians=medians,
                              cpu_or_wall_reduction=1 - medians['candidate'] / medians['baseline']))
    passed = all(r['matches'] for r in rows)
    report = dict(passed=passed, evidence='standalone checkpoint continuations; no connected realtime claim',
                  compiler=version, sources=source_hashes, inputs=inputs,
                  training=list(map(str, training)), steps=args.steps,
                  train_steps=args.train_steps,
                  builds=builds, trials=rows, summaries=summaries)
    # Retain only experiment settings, never the user's inherited environment.
    report['environment'] = {k: v for k, v in env.items() if k.startswith('CDJ_')}
    (output / 'measurements.json').write_text(json.dumps(report, indent=2) + '\n')
    if not passed:
        raise RuntimeError('candidate/repeat mismatch; inspect retained output')


if __name__ == '__main__':
    main()
