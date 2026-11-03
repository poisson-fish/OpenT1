"""Summarize native render memory captures without using instrumented timings."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path


def records(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]


def catalog(rows):
    assets = [r for r in rows if r['kind'] == 'asset']
    archives = [r for r in rows if r['kind'] == 'archive']
    return {
        'archives': len(archives), 'asset_entries': len(assets),
        'decode_failures': sum(r.get('decoded') is False for r in assets),
        'entry_errors': [r for r in rows if r['kind'] == 'error'],
        'allocation_release_mismatches': sum(r.get('decoded_heap_bytes', 0) !=
                                           r.get('released_heap_bytes', 0) for r in assets),
    }


def mission(rows):
    snapshots = [r for r in rows if r['kind'] == 'snapshot']
    loaded = next(r for r in snapshots if r['stage'] == 'mission_loaded')
    warm = next(r for r in snapshots if r['stage'] == 'simulation_warm')
    end = next(r for r in snapshots if r['stage'] == 'before_shutdown')
    gpu = [r for r in rows if r['kind'] == 'gpu']
    render = [r for r in snapshots if r['stage'] == 'render_warm']
    result = {
        'source_mesh_terrain_bytes': sum(loaded['tags'][k]['live_bytes']
                                         for k in ('shape', 'geometry', 'grid', 'block')),
        'cpp_simulation_warm_bytes': warm['cpp_live_bytes'],
        'cpp_before_shutdown_bytes': end['cpp_live_bytes'],
        'cpp_peak_bytes': end['cpp_peak_bytes'],
        'private_before_shutdown_bytes': end['process_private_bytes'],
        'working_set_before_shutdown_bytes': end['working_set_bytes'],
        'render_tag_live_bytes': end['tags']['frame']['live_bytes'],
        'mounts': next(r for r in rows if r['kind'] == 'mounts'),
        'gpu_last': gpu[-1],
        'gpu_maximum': {key: max(r.get(key, 0) for r in gpu) for key in (
            'physical_textures', 'rgba_bytes', 'allocated_texture_payload_bytes',
            'dxgi_local_bytes', 'dxgi_nonlocal_bytes', 'retained_mesh_bytes',
            'vertex_capacity_bytes', 'transfer_capacity_bytes', 'instance_capacity_bytes')},
    }
    # Frame-tag allocations include render preparation/submission, not simulation.
    # Use the final interval, after the first rotating working-set warmup.
    if len(render) >= 2:
        interval = gpu[-1]['frame'] - gpu[-2]['frame']
        result['last_interval_render_allocations_per_frame'] = (
            render[-1]['tags']['frame']['allocations'] -
            render[-2]['tags']['frame']['allocations']) / interval
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    cases = json.loads((args.input / 'runs.json').read_text())
    summary = {
        'schema_version': 1, 'summarized_utc': datetime.now(timezone.utc).isoformat(),
        'units': 'bytes unless named otherwise',
        'host': 'Windows x64 MSVC Release, D3D12, RTX 5090, 800x600 local server/client',
        'notes': [
            'Allocation-tracking audit captures; do not use their execution times for performance.',
            'Tracked C++ requested bytes, process private commit, file-mapped virtual bytes and GPU usage are separate overlapping domains. Do not add them.',
            'Mapped archive bytes are virtual file backing, not measured resident pages. Unsupported mappings retain owned buffers.',
            'GPU payload counts exclude driver alignment/metadata. DXGI usage includes targets, buffers and backend allocations. Defaults are not universal mobile budgets.',
            'Render-tag live bytes are an attributable subset, not all derived CPU data. Render allocation rates exclude simulation and include occasional measurement output.',
            'Rotating spawn-view working set; not exhaustive traversal. No audio or presentation.',
        ],
        'provenance': json.loads((args.input / 'provenance.json').read_text()),
        'catalogs': {}, 'missions': {}, 'capture_sha256': {},
    }
    for key in ('base', 'rpg'):
        summary['provenance'].pop(key, None)
    for case in cases:
        label = case['label']
        if not (label.endswith('-catalog') or label.endswith('-gpu')):
            continue
        if case['returncode'] != 0:
            raise ValueError(f'Incomplete capture: {label}')
        path = args.input / (label + '.jsonl')
        rows = records(path)
        summary['capture_sha256'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        if label.endswith('-catalog'):
            summary['catalogs'][label] = catalog(rows)
        else:
            summary['missions'][label] = mission(rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
