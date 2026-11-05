"""Publish tested CI artifacts to a rolling prerelease or an existing version tag."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import zipfile

from package_release import PLATFORMS, sha256


def gh(*args: str) -> str:
    return subprocess.check_output(["gh", *args], text=True).strip()


def tag_version(tag: str, version: str) -> bool:
    match = re.fullmatch(r"v(\d+\.\d+\.\d+)(?:-((?:alpha|beta|rc)\.[1-9]\d*))?", tag)
    if not match or match[1] != version:
        raise ValueError(f"Tag {tag!r} must match VERSION ({version}), optionally with -alpha.N/-beta.N/-rc.N")
    return match[2] is not None


def validate_artifacts(directory: Path) -> list[Path]:
    expected = {"OpenT1-source.tar.gz"}
    expected.update(f"OpenT1-{p}.{'zip' if p.startswith('windows') else 'tar.gz'}" for p in PLATFORMS)
    actual = {p.name for p in directory.iterdir() if p.is_file()}
    if actual != expected:
        raise ValueError(f"Expected complete artifact set {sorted(expected)}, got {sorted(actual)}")
    return sorted(directory.iterdir())


def validate_identity(archives: list[Path], commit: str, version: str) -> None:
    for archive in archives:
        if archive.name == "OpenT1-source.tar.gz":
            continue
        platform = archive.name.removeprefix("OpenT1-").removesuffix(".zip").removesuffix(".tar.gz")
        member = f"OpenT1-{platform}/release.json"
        if archive.suffix == ".zip":
            with zipfile.ZipFile(archive) as package:
                metadata = json.loads(package.read(member))
        else:
            with tarfile.open(archive) as package:
                stream = package.extractfile(member)
                if stream is None:
                    raise ValueError(f"No identity in {archive.name}")
                metadata = json.load(stream)
        if any(metadata.get(k) != value for k, value in {
            "commit": commit, "version": version, "platform": platform, "configuration": "Release"
        }.items()):
            raise ValueError(f"Artifact identity does not match tested commit: {archive.name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--ref", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        raise ValueError("Expected a full commit SHA")
    repo = gh("repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner")
    version = Path("VERSION").read_text().strip()
    rolling = args.ref == "refs/heads/main"
    if not rolling and not args.ref.startswith("refs/tags/"):
        raise ValueError("Publishing is restricted to main or a version tag")
    tag = "continuous" if rolling else args.ref.removeprefix("refs/tags/")
    prerelease = True if rolling else tag_version(tag, version)
    main_sha = gh("api", f"repos/{repo}/git/ref/heads/main", "--jq", ".object.sha")
    if rolling and main_sha != args.commit:
        print("A newer main commit exists; leave continuous downloads unchanged.")
        return
    subprocess.run(["git", "fetch", "origin", "main"], check=True)
    subprocess.run(["git", "merge-base", "--is-ancestor", args.commit, "origin/main"], check=True)
    if not rolling:
        tag_sha = subprocess.check_output(["git", "rev-parse", f"{tag}^{{commit}}"], text=True).strip()
        if tag_sha != args.commit:
            raise ValueError("Tag does not refer to the tested commit")
    archives = validate_artifacts(args.directory)
    validate_identity(archives, args.commit, version)
    checksums = args.directory / "SHA256SUMS.txt"
    checksums.write_text("".join(f"{sha256(p)}  {p.name}\n" for p in archives))
    notes = args.directory / "release-notes.md"
    body = (f"{'Continuous development build' if rolling else 'OpenT1 ' + tag}\n\n"
            f"Built from [`{args.commit[:12]}`](https://github.com/{repo}/commit/{args.commit}). "
            "Targets Starsiege: Tribes 1.11 compatibility. Game data is supplied separately.\n\n"
            f"[Installation](https://github.com/{repo}/wiki/Getting-started) · "
            f"[Known limitations](https://github.com/{repo}/wiki/Compatibility-and-troubleshooting)\n\n"
            "Windows x64, Linux x64 (Ubuntu 24.04 or compatible), and macOS ARM64 "
            "(macOS 15 or newer). All Debug/Release CI jobs passed. Extracted packages passed "
            "headless smoke tests; real GPU, audio hardware, installed mods and live vanilla "
            "interoperability require separate acceptance testing.\n\n"
            "`OpenT1-source.tar.gz` includes the engine source, exact SDL/OpenAL source archives, "
            "and dependency alteration/build recipes. See THIRD_PARTY.md. Verify downloads "
            "against SHA256SUMS.txt.\n")
    if rolling:
        body += "\nThis prerelease changes as main advances. Use a numbered release for a fixed version.\n"
        # Only this dedicated development tag is allowed to move.
        subprocess.run(["git", "tag", "--force", "continuous", args.commit], check=True)
        subprocess.run(["git", "push", "--force", "origin", "refs/tags/continuous"], check=True)
    else:
        generated = json.loads(gh("api", f"repos/{repo}/releases/generate-notes", "-X", "POST",
                                  "-f", f"tag_name={tag}", "-f", f"target_commitish={args.commit}"))
        body += "\n" + generated["body"]
    notes.write_text(body)
    exists = subprocess.run(["gh", "release", "view", tag], capture_output=True).returncode == 0
    if exists and not rolling:
        release = json.loads(gh("release", "view", tag, "--json", "isDraft"))
        if not release["isDraft"]:
            raise ValueError("Numbered releases are immutable; use a new version tag")
    if not exists:
        gh("release", "create", tag, "--verify-tag", "--draft", "--title",
           "Continuous main build" if rolling else f"OpenT1 {tag}", "--notes-file", str(notes))
    gh("release", "upload", tag, *map(str, archives), str(checksums), "--clobber")
    gh("release", "edit", tag, "--draft=false", f"--prerelease={'true' if prerelease else 'false'}",
       f"--latest={'false' if prerelease else 'true'}", "--notes-file", str(notes))
    print(f"https://github.com/{repo}/releases/tag/{tag}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
