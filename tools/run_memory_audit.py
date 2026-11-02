#!/usr/bin/env python3
"""Measure installed base/RPG catalogs and missions in fresh, isolated processes.

No assets are copied. Runtime writes go under --output. Run cases serially so
GPU memory and process high-water marks can be compared independently.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def read_records(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--rpg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--all-missions", action="store_true")
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--frames", type=int, default=240)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    executable = args.executable.resolve()
    cases = []

    def run(label, arguments):
        output = args.output / (label + ".jsonl")
        command = [str(executable), *map(str, arguments)]
        started = time.monotonic()
        with (args.output / (label + ".stdout.log")).open("w") as stdout, \
             (args.output / (label + ".stderr.log")).open("w") as stderr:
            try:
                result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=120)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        case = {"label": label, "returncode": code,
                "seconds": round(time.monotonic() - started, 3)}
        if output.exists():
            records = read_records(output)
            case["snapshots"] = [r for r in records if r["kind"] == "snapshot"]
            case["gpu"] = [r for r in records if r["kind"] == "gpu"]
            case["devices"] = [r for r in records if r["kind"] == "device"]
        cases.append(case)
        (args.output / "runs.json").write_text(json.dumps(cases, indent=2) + "\n")
        print(json.dumps({k: v for k, v in case.items() if k not in ("snapshots", "gpu", "devices")}), flush=True)

    for label, root, directories in [("base", args.base, ["base"]),
                                      ("rpg", args.rpg, ["base", "RPG"])]:
        run(label + "-catalog", ["catalog", root, args.output / (label + "-catalog.jsonl"), *directories])
    if args.all_missions:
        for label, root, directory, mod in [("base", args.base, "base", []),
                                             ("rpg", args.rpg, "RPG", ["--rpg"])]:
            missions = sorted({p.stem for p in (root / directory).rglob("*")
                               if p.is_file() and p.suffix.lower() == ".mis"}, key=str.lower)
            for mission in missions:
                name = f"{label}-{mission}"
                run(name, ["mission", root, mission, args.output / (name + ".jsonl"), *mod, "--frames", "0"])
    if args.gpu:
        # Base maps span several interior/terrain styles. RPG cases use their
        # complete mission asset sets and a 360-degree observer sweep when present.
        for label, root, mission, mod in [
            ("base", args.base, "Raindance", []),
            ("base", args.base, "Broadside", []),
            ("base", args.base, "Stonehenge", []),
            ("base", args.base, "Scarabrae", []),
            ("rpg", args.rpg, "rpgmap1", ["--rpg"]),
            ("rpg", args.rpg, "rpgmap5", ["--rpg"]),
        ]:
            name = f"{label}-{mission}-gpu"
            run(name, ["mission", root, mission, args.output / (name + ".jsonl"),
                       *mod, "--gpu", "--frames", str(args.frames)])
    provenance = {"executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                  "base": str(args.base.resolve()), "rpg": str(args.rpg.resolve()),
                  "frames": args.frames, "all_missions": args.all_missions,
                  "gpu": args.gpu}
    (args.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


if __name__ == "__main__":
    main()
