#!/usr/bin/env python3
"""Produce a shareable measurements-only summary; never copy asset payloads."""
import argparse
import collections
import json
from pathlib import Path
import statistics


def records(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def catalog(path):
    rows = records(path)
    assets = [r for r in rows if r["kind"] == "asset"]
    archives = [r for r in rows if r["kind"] == "archive"]
    groups = collections.defaultdict(lambda: collections.Counter())
    roots = collections.defaultdict(lambda: collections.Counter())
    numeric = ["source_bytes", "decoded_heap_bytes", "triangles", "height_samples",
               "terrain_cells", "bitmap_pixels_bytes", "rgba_bytes", "light_rgba_bytes", "pcm_bytes"]
    for asset in assets:
        for group in (groups[asset["extension"]], roots[asset["source"].split("/")[0]]):
            group["entries"] += 1
            group["decoded"] += asset.get("decoded") is True
            for field in numeric:
                group[field] += asset.get(field, 0)
    return {
        "asset_entries": len(assets), "archives": len(archives),
        "archive_file_bytes": sum(r["disk_bytes"] for r in archives),
        "decoded_heap_sum_bytes": sum(r.get("decoded_heap_bytes", 0) for r in assets),
        "decode_failures": [{k: r[k] for k in ("source", "name")} for r in assets if r.get("decoded") is False],
        "entry_errors": [r for r in rows if r["kind"] == "error"],
        "allocation_release_mismatches": sum(r.get("decoded_heap_bytes", 0) != r.get("released_heap_bytes", 0) for r in assets),
        "extensions": dict(sorted(groups.items())), "directories": dict(sorted(roots.items())),
    }


def mission(case, root):
    snapshots = case.get("snapshots", [])
    warm = next((r for r in snapshots if r["stage"] == "simulation_warm"), None)
    result = {"label": case["label"], "returncode": case["returncode"]}
    if warm:
        loaded = next(r for r in snapshots if r["stage"] == "mission_loaded")
        result.update({"cpp_warm_bytes": warm["cpp_live_bytes"], "cpp_peak_bytes": warm["cpp_peak_bytes"],
                       "private_warm_bytes": warm["process_private_bytes"],
                       "working_set_warm_bytes": warm["working_set_bytes"],
                       "source_mesh_terrain_bytes": sum(loaded["tags"][k]["live_bytes"] for k in ("shape", "geometry", "grid", "block")),
                       "interior_geometry_bytes": loaded["tags"]["geometry"]["live_bytes"],
                       "interior_source_triangles": loaded["tags"]["geometry"]["triangles"],
                       "shape_source_triangles": loaded["tags"]["shape"]["triangles"],
                       "source_bitmap_bytes": loaded["tags"]["bitmap"]["live_bytes"],
                       "all_loaded_bitmap_rgba_bytes_estimate": loaded["tags"]["bitmap"]["rgba_bytes"],
                       "source_lighting_bytes": loaded["tags"]["lighting"]["live_bytes"],
                       "all_loaded_lightmap_rgba_bytes_estimate": loaded["tags"]["lighting"]["light_rgba_bytes"],
                       "terrain_height_samples": loaded["tags"]["block"]["height_samples"],
                       "terrain_source_cells": loaded["tags"]["block"]["terrain_cells"],
                       "terrain_block_placements": loaded["tags"]["grid"]["block_placements"]})
        rows = records(root / (case["label"] + ".jsonl"))
        mounts = next(r for r in rows if r["kind"] == "mounts")
        result["archive_file_bytes"] = mounts["archive_file_bytes"]
    if case.get("gpu"):
        gpu = case["gpu"]
        before = next((r for r in snapshots if r["stage"] == "before_shutdown"), snapshots[-1])
        result["gpu"] = {
            "device": case["devices"],
            "completed": case["returncode"] == 0,
            "final_snapshot_stage": before["stage"],
            "world_rendered": any(r.get("scene_polygons", 0) > 0 for r in gpu),
            "maximum_rgba_bytes": max(r["rgba_bytes"] for r in gpu),
            "maximum_textures": max(r["textures"] for r in gpu),
            "maximum_dxgi_local_bytes": max(r.get("dxgi_local_bytes", 0) for r in gpu),
            "maximum_dxgi_nonlocal_bytes": max(r.get("dxgi_nonlocal_bytes", 0) for r in gpu),
            "cpp_before_shutdown_bytes": before["cpp_live_bytes"],
            "cpp_peak_bytes": before["cpp_peak_bytes"],
            "private_before_shutdown_bytes": before["process_private_bytes"],
            "last": gpu[-1],
        }
    if case["returncode"] != 0:
        result["error"] = (root / (case["label"] + ".stderr.log")).read_text(encoding="utf-8").strip()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--date", required=True)
    args = parser.parse_args()
    cases = json.loads((args.input / "runs.json").read_text())
    missions = [mission(c, args.input) for c in cases if not c["label"].endswith("-catalog")]
    base = [r for r in missions if r["label"].startswith("base-") and not r["label"].endswith("-gpu")]
    success = [r for r in base if r["returncode"] == 0]
    values = [r["cpp_warm_bytes"] for r in success]
    summary = {
        "schema_version": 1, "date": args.date, "units": "bytes",
        "measurement_host": "Windows x64, MSVC Release; GPU cases use D3D12 / RTX 5090",
        "scope": "Installed catalogs include custom maps, shadowed sources and alternate LOD/light states. Catalog sums are not simultaneous residency. Mission runs include local server/client; audio playback and video playback are not initialized.",
        "catalogs": {label: catalog(args.input / (label + "-catalog.jsonl")) for label in ("base", "rpg")},
        "base_missions": {"attempted": len(base), "loaded": len(success),
                          "minimum_cpp_warm_bytes": min(values), "median_cpp_warm_bytes": statistics.median(values),
                          "maximum_cpp_warm_bytes": max(values)},
        "missions": missions,
        "provenance": json.loads((args.input / "provenance.json").read_text()),
    }
    # Keep private installation paths in the local capture, not repository docs.
    for key in ("base", "rpg"):
        summary["provenance"].pop(key, None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"base_missions": summary["base_missions"], "output": str(args.output)}))


if __name__ == "__main__":
    main()
