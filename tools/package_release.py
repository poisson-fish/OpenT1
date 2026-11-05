"""Stage, archive, extract and smoke-test a release using only stdlib + CMake."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = ("windows-x64", "linux-x64", "macos-arm64")


def run(*args: str, **kwargs) -> str:
    return subprocess.check_output(args, text=True, **kwargs).strip()


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_stage(stage: Path, platform: str) -> None:
    """Fail closed if install rules start pulling development or game data in."""
    executable = "opent1.exe" if platform.startswith("windows") else "opent1"
    for required in (executable, "README.md", "LICENSE", "THIRD_PARTY.md", "release.json"):
        if not (stage / required).is_file():
            raise ValueError(f"Missing package input: {required}")
    if not any(p.name.lower().startswith(("openal", "libopenal")) for p in stage.iterdir()):
        raise ValueError("Missing OpenAL runtime")
    for path in stage.rglob("*"):
        relative = path.relative_to(stage)
        if path.is_symlink() and not path.resolve().is_relative_to(stage.resolve()):
            raise ValueError(f"Package symlink escapes staging: {relative}")
        if path.is_dir():
            continue
        top = relative.parts[0]
        allowed = False
        if len(relative.parts) == 1:
            allowed = top in {"opent1", "opent1.exe", "opent1_asset_tool", "opent1_asset_tool.exe",
                              "README.md", "LICENSE", "THIRD_PARTY.md", "VERSION", "release.json"}
            allowed |= bool(re.fullmatch(r"[\w.+-]+\.(?:dll|dylib|so(?:\.[\d.]+)?)", top, re.I))
        elif top == "LICENSES":
            allowed = path.suffix == ".txt"
        elif top == "docs":
            allowed = path.suffix == ".md"
        elif relative.parts[:2] == ("examples", "scripts"):
            allowed = path.suffix in {".cs", ".md"}
        if not allowed:
            raise ValueError(f"Unexpected release input: {relative}")


def verify_portability(stage: Path, platform: str) -> None:
    if platform == "linux-x64":
        for binary in [stage / "opent1", *stage.glob("*.so*")]:
            output = run("ldd", str(binary))
            if "not found" in output:
                raise ValueError(f"Unresolved runtime dependency: {output}")
            for line in output.splitlines():
                if "libopenal" in line.lower() and str(stage) not in line:
                    raise ValueError(f"OpenAL resolved outside package: {line}")
    elif platform == "macos-arm64":
        for binary in [stage / "opent1", stage / "opent1_asset_tool", *stage.glob("*.dylib")]:
            for line in run("otool", "-L", str(binary)).splitlines()[1:]:
                dependency = line.strip().split(" (", 1)[0]
                if not dependency.startswith(("/usr/lib/", "/System/Library/", "@executable_path/", "@loader_path/")):
                    raise ValueError(f"Nonportable macOS dependency in {binary.name}: {dependency}")


def smoke(stage: Path, platform: str) -> None:
    executable = stage / ("opent1.exe" if platform.startswith("windows") else "opent1")
    env = os.environ.copy()
    # Prevent success through a development PATH or loader override.
    for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
        env.pop(name, None)
    if platform.startswith("windows"):
        env["PATH"] = str(Path(env["SystemRoot"]) / "System32")
    env.update(SDL_VIDEODRIVER="opent1-invalid-driver", ALSOFT_DRIVERS="null")
    with tempfile.TemporaryDirectory(prefix="opent1-smoke-") as working:
        version = run(str(executable), "--version", cwd=working, env=env, timeout=30)
        metadata = json.loads((stage / "release.json").read_text())
        if metadata["commit"] not in version or metadata["version"] not in version:
            raise ValueError(f"Executable does not match package metadata: {version}")
        print(version)
        subprocess.run([str(executable), "--smoke", "--headless", "--no-audio", "--no-assets",
                        "--spawn-demo", "--frames", "4"], cwd=working, env=env, timeout=30, check=True)
        subprocess.run([str(executable), "--headless", "--no-audio", "--no-assets", "-dedicated",
                        "--frames", "4"], cwd=working, env=env, timeout=30, check=True)


def binary_package(build: Path, output: Path, platform: str) -> Path:
    commit = run("git", "rev-parse", "HEAD", cwd=ROOT)
    version = (ROOT / "VERSION").read_text().strip()
    name = f"OpenT1-{platform}"
    with tempfile.TemporaryDirectory(prefix="opent1-package-") as temporary:
        stage = Path(temporary) / name
        subprocess.run(["cmake", "--install", str(build), "--config", "Release", "--component",
                        "Runtime", "--prefix", str(stage)], check=True)
        metadata = {"version": version, "commit": commit, "platform": platform,
                    "compatibility_target": "Tribes 1.11", "configuration": "Release"}
        (stage / "release.json").write_text(json.dumps(metadata, indent=2) + "\n")
        validate_stage(stage, platform)
        verify_portability(stage, platform)
        extension = "zip" if platform.startswith("windows") else "gztar"
        archive = Path(shutil.make_archive(str(output / name), extension, stage.parent, name))
        # Test bytes from the actual download, after moving away from the build.
        extracted = Path(temporary) / "extracted"
        shutil.unpack_archive(str(archive), str(extracted))
        unpacked = extracted / name
        validate_stage(unpacked, platform)
        verify_portability(unpacked, platform)
        smoke(unpacked, platform)
    return archive


def source_package(build: Path, output: Path) -> Path:
    """Ship tracked engine sources plus the exact original dependency archives.

    Dependency alterations/build recipes are tracked in cmake/. No installation,
    generated state, or entire dependency build tree is an input.
    """
    expected = {
        "SDL3-3.4.2.tar.gz": "ef39a2e3f9a8a78296c40da701967dd1b0d0d6e267e483863ce70f8a03b4050c",
        "openal-soft-1.25.2.tar.bz2": "1dbaac44e7579d5bc8847ca8db4b2e8b9fd3961041f35ee20def4958301e1089",
    }
    with tempfile.TemporaryDirectory(prefix="opent1-source-") as temporary:
        source = Path(temporary) / "source.tar"
        subprocess.run(["git", "archive", "--format=tar", "--prefix=OpenT1-source/", "HEAD",
                        "-o", str(source)], cwd=ROOT, check=True)
        with tarfile.open(source, "a") as archive:
            for name, digest in expected.items():
                dependency = build / "downloads" / name
                if sha256(dependency) != digest:
                    raise ValueError(f"Dependency archive hash mismatch: {name}")
                archive.add(dependency, f"OpenT1-source/dependencies/{name}")
        destination = output / "OpenT1-source.tar.gz"
        import gzip
        with source.open("rb") as original, gzip.open(destination, "wb") as compressed:
            shutil.copyfileobj(original, compressed)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=PLATFORMS, required=True)
    parser.add_argument("--source", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary_package(args.build.resolve(), args.output.resolve(), args.platform)
    if args.source:
        source_package(args.build.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
