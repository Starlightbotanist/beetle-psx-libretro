#!/usr/bin/env python3
"""Compile the shipping helper; optionally reproduce historical comparisons.

No network access, renderer, game, Python package or prebuilt core is required.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=(r'C:\msys64\mingw64\bin\gcc.exe'
        if os.name == 'nt' else os.environ.get('CC', 'cc')))
    parser.add_argument('--git', default=(r'C:\Program Files\Git\cmd\git.exe'
        if os.name == 'nt' else 'git'))
    parser.add_argument('--matrix-base', help='Existing local ref for the 12,736-case comparison')
    parser.add_argument('--depth-base', help='Existing local ref for the 160-case depth comparison')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error('Native C compiler missing; pass its path with --cc')
    env = os.environ | {'PATH': str(Path(compiler).parent) + os.pathsep + os.environ.get('PATH', '')}

    def git(*arguments):
        return subprocess.check_output([args.git, '-C', str(ROOT), *arguments])

    with tempfile.TemporaryDirectory(prefix='gpu-uv-') as temporary:
        work = Path(temporary)

        def build(name, ref=None):
            helper = ROOT/'mednafen/psx/gpu_polygon_sub.c'
            if ref:
                # Do not silently compile a historical helper against changed headers.
                changed = git('diff', '--name-only', ref, 'HEAD', '--',
                    ':(glob)mednafen/**/*.h', ':(glob)libretro-common/include/**/*.h',
                    'beetle_psx_globals.h', 'rhi/rhi_intf.h')
                if changed.strip():
                    raise RuntimeError('Historical headers changed; use an isolated checkout of ' + ref)
                helper = work/(name+'.c')
                helper.write_bytes(git('show', ref+':mednafen/psx/gpu_polygon_sub.c'))
            target = work/(name+('.exe' if os.name == 'nt' else ''))
            subprocess.run([compiler, '-O2', '-std=gnu89', '-fwrapv',
                '-I'+str(ROOT), '-I'+str(ROOT/'mednafen/psx'),
                '-I'+str(ROOT/'libretro-common/include'), str(HERE/'geometry.c'),
                str(helper), '-o', str(target)], env=env, check=True)
            return target

        def capture(executable, mode):
            text = subprocess.check_output([str(executable), mode], env=env, text=True)
            return [line.split(' : ') for line in text.splitlines()]

        current = build('current')
        subprocess.run([str(current)], env=env, check=True)
        if args.matrix_base:
            baseline = build('matrix-base', args.matrix_base)
            before, after = capture(baseline, '--matrix'), capture(current, '--matrix')
            assert len(before) == len(after) == 12736
            negatives = changed = 0
            clean = {}
            for (key, a), (other, b) in zip(before, after):
                assert key == other
                number, kind, renderer, scale, flip, order, window, stale = map(int, key.split())
                if number < 12480:
                    if kind in [0, 1, 4, 5, 6, 7, 8] or renderer == 0:
                        assert a == b, (key, a, b)
                        negatives += 1
                    changed += a != b
                    if renderer != 0 and kind in [2, 3, 9, 10, 11, 12]:
                        assert b.split()[:3] == ['0', '0', '0'], (key, b)
                else:
                    group = kind, renderer, flip
                    if not stale:
                        clean[group] = b
                    assert b == clean[group], (key, b)
            print(f'Historical matrix: {len(after)} cases, {negatives} unchanged negatives, '
                  f'{changed} changed cases, 256 reconstruction cases')
        if args.depth_base:
            baseline = build('depth-base', args.depth_base)
            before, after = capture(baseline, '--depth-matrix'), capture(current, '--depth-matrix')
            assert len(before) == len(after) == 160
            failures = 0
            for (key, a), (other, b) in zip(before, after):
                assert key == other
                if int(key.split()[-1]):
                    failures += a != '0 0 0'
                    assert b == '0 0 0', (key, b)
                else:
                    assert a == b, (key, a, b)
            print(f'Historical depth matrix: 160 cases, baseline failures {failures}, current failures 0')


if __name__ == '__main__':
    main()
