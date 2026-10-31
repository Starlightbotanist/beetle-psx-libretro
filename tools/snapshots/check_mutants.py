"""Build isolated software-restore mutants and require the real-core test to fail."""
import argparse
import hashlib
import json
import os
import shutil
import stat
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--make', default='make')
parser.add_argument('--make-arg', action='append', default=[])
parser.add_argument('--jobs', type=int, default=2)
parser.add_argument('--output', type=Path, required=True,
                    help='new writable output directory; no source files are changed')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
extension = '.dll' if os.name == 'nt' else '.so'
host = output / ('snapshot_smoke.exe' if os.name == 'nt' else 'snapshot_smoke')
source = (root / 'mednafen/psx/gpu.c').read_text(encoding='utf-8')
settings = [*args.make_arg, 'HAVE_HW=0', 'HAVE_LIGHTREC=0']
records = []

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def invoke(command, log):
    with (output / log).open('wb') as stream:
        result = subprocess.run(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT)
    return result.returncode

# Compile the same shipping host used by make check. The normal core object
# directory and normal core library are never used or replaced.
command = [args.make, '-C', str(root / 'tools/snapshots'), *args.make_arg,
           'build/snapshot_smoke.exe' if os.name == 'nt' else 'build/snapshot_smoke']
assert invoke(command, 'host-build.log') == 0, 'See host-build.log'
original_host = root / 'tools/snapshots/build' / host.name
shutil.copy2(original_host, host)
assert digest(original_host) == digest(host), 'Copied host contents changed'
assert stat.S_IMODE(original_host.stat().st_mode) == stat.S_IMODE(host.stat().st_mode), 'Copied host permissions changed'
assert os.access(host, os.X_OK), 'Copied host is not executable'

for mutation in ['control', 'skip_software_vram', 'skip_enhanced_pages']:
    changed = source
    if mutation != 'control':
        anchor = 'static GPUMemorySnapshot *memory_snapshot_load;'
        assert changed.count(anchor) == 1
        extra = '\n#include <stdio.h>\nstatic unsigned mutation_hits;\n'
        if mutation == 'skip_software_vram':
            extra += '''static uint16_t mutation_vram[1024 * 512];
static uint16_t *mutation_target(uint16_t *normal, int load)
{
   if (load && memory_snapshot_load && memory_snapshot_load->type == RHI_SOFTWARE)
   {
      if (!mutation_hits++) fputs("MUTANT skip_software_vram hit\\n", stderr);
      return mutation_vram;
   }
   return normal;
}
'''
            field = 'SFARRAY16N(vram_new, 1024 * 512, "&GPURAM[0][0]")'
            assert changed.count(field) == 1
            changed = changed.replace(field, field.replace('vram_new,', 'mutation_target(vram_new, load),'))
        changed = changed.replace(anchor, anchor + extra)
        call = 'history_pages_load(memory_snapshot_load->pages, GPU.vram);'
        assert changed.count(call) == 1
        changed = changed.replace(call, 'if (!mutation_hits++) fputs("MUTANT ' + mutation + ' hit\\n", stderr);')
    generated = output / (mutation + '.c')
    generated.write_text(changed, encoding='utf-8')
    core = output / ('core-' + mutation + extension)
    command = [args.make, '-f', 'Makefile', '-f', 'tools/snapshots/mutants.mk',
               '-j' + str(args.jobs), *settings, 'snapshot_mutant',
               'SNAPSHOT_BUILD=' + output.as_posix(), 'SNAPSHOT_MUTANT=' + mutation,
               'SNAPSHOT_TARGET=' + core.as_posix(), 'SNAPSHOT_GPU_SOURCE=' + generated.as_posix()]
    assert invoke(command, mutation + '-build.log') == 0, 'See ' + mutation + '-build.log'
    record = dict(mutation=mutation, source_sha256=digest(generated), core_sha256=digest(core),
                  host_sha256=digest(host), command=command, runs=[])
    records.append(record)
    for scale in [1, 4]:
        fixture = output / (mutation + '-' + str(scale))
        command = [str(host), str(core), str(fixture), str(scale), '--fixed-scale']
        log = mutation + '-' + str(scale) + '.log'
        result = invoke(command, log)
        text = (output / log).read_text(encoding='utf-8', errors='replace')
        should_pass = mutation == 'control' or (mutation == 'skip_enhanced_pages' and scale == 1)
        record['runs'].append(dict(scale=scale, exit_code=result, expected_pass=should_pass,
                                   mutation_observed='MUTANT ' + mutation + ' hit' in text))
        (output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
        if should_pass:
            assert result == 0, 'Unexpected failure: ' + log
            if mutation != 'control':assert 'MUTANT ' + mutation + ' hit' not in text, 'Negative case reached enhanced restore'
        else:
            assert result != 0 and 'MUTANT ' + mutation + ' hit' in text, 'Mutant not detected: ' + log
            assert 'retained VRAM at guest frame' in text, 'Wrong failure reason: ' + log
        print(mutation, str(scale) + 'x', 'PASS' if should_pass else 'detected', flush=True)
