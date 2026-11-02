"""Run base/RPG optimized timing cases serially, with isolated writable game state."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import os
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--rpg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--driver', default='direct3d12')
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--projected-interiors', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    repository = Path(__file__).resolve().parent.parent
    provenance = {
        'captured_utc': datetime.now(timezone.utc).isoformat(),
        'executable_sha256': hashlib.sha256(args.executable.read_bytes()).hexdigest(),
        'git_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repository,
                                                text=True).strip(),
        'git_status': subprocess.check_output(['git', 'status', '--porcelain'], cwd=repository,
                                              text=True).splitlines(),
        'frames': args.frames, 'warmup_frames': 120, 'driver': args.driver,
        'projected_interiors': args.projected_interiors,
        'instrumented_allocations': False,
    }
    (args.output / 'capture-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    env = os.environ.copy()
    if args.projected_interiors:
        env['OPENT1_PROJECTED_INTERIORS'] = '1'
    else:
        env.pop('OPENT1_PROJECTED_INTERIORS', None)
    for mode in ('stationary', 'rotate', 'travel'):
        for label, root, mission, mod in (('base', args.base, 'Raindance', []),
                                          ('base', args.base, 'Broadside', []),
                                          ('rpg', args.rpg, 'rpgmap1', ['RPG']),
                                          ('rpg', args.rpg, 'rpgmap5', ['RPG'])):
            name = f'{label}-{mission}-{mode}'
            command = [str(args.executable.resolve()), str(root.resolve()), mission,
                       str((args.output / (name + '.json')).resolve()), mode, str(args.frames), args.driver, *mod]
            with (args.output / (name + '.log')).open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, env=env)
            print(name, flush=True)


if __name__ == '__main__':
    main()
