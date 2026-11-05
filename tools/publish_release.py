"""Publish immutable tested snapshots/versions and advance the main download index."""
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
import sync_wiki


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
            with tarfile.open(archive) as source:
                if source.pax_headers.get("comment") != commit:
                    raise ValueError("Source archive does not match the tested commit")
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


def snapshot_tag(commit: str, checksums: Path) -> str:
    # A rebuilt commit can produce different archive bytes (toolchain/timestamps).
    # Include the artifact-set digest so every build can stay immutable.
    return f"main-{commit[:12]}-{sha256(checksums)[:12]}"


def validate_published_assets(release: dict, files: list[Path]) -> None:
    expected = {p.name: f"sha256:{sha256(p)}" for p in files}
    actual = {asset["name"]: asset.get("digest") for asset in release["assets"]}
    if actual != expected:
        raise ValueError("Published assets differ from tested downloads; use a new release tag")


def download_page(repository: str, commit: str, version: str, tag: str) -> str:
    base = f"https://github.com/{repository}"
    download = f"{base}/releases/download/{tag}"
    labels = {"windows-x64": "Windows x64", "linux-x64": "Linux x64", "macos-arm64": "macOS ARM64"}
    body = (f"# Continuous builds\n\n"
            f"Latest tested main build: **OpenT1 {version}**, "
            f"[`{commit[:12]}`]({base}/commit/{commit}).\n\n"
            "These links update after successful main builds. Each linked snapshot "
            "keeps its original files. Game data is supplied separately.\n\n"
            "| Platform | Download |\n| --- | --- |\n")
    for platform in PLATFORMS:
        filename = f"OpenT1-{platform}.{'zip' if platform.startswith('windows') else 'tar.gz'}"
        body += f"| {labels[platform]} | [{filename}]({download}/{filename}) |\n"
    body += (f"\n[SHA-256 checksums]({download}/SHA256SUMS.txt) · "
             f"[Source and dependencies]({download}/OpenT1-source.tar.gz) · "
             f"[Build details]({base}/releases/tag/{tag})\n\n"
             f"Read [Getting started]({base}/wiki/Getting-started) for installation and "
             f"platform requirements. Use [numbered releases]({base}/releases) for fixed "
             "version labels. These development builds pass automated tests; consult "
             "their release notes for hardware and installed-content validation limits.\n")
    return body


def publish(repository: str, tag: str, title: str, prerelease: bool,
            files: list[Path], notes: Path) -> str:
    exists = subprocess.run(["gh", "release", "view", tag], capture_output=True).returncode == 0
    if exists:
        release = json.loads(gh("api", f"repos/{repository}/releases/tags/{tag}"))
        if not release["draft"]:
            # Both main snapshots and numbered releases are fixed after publish.
            # A retry with identical bytes succeeds without touching their assets.
            validate_published_assets(release, files)
            return release["html_url"]
    else:
        gh("release", "create", tag, "--verify-tag", "--draft", "--title", title,
           "--notes-file", str(notes))
    # Drafts remain editable even when the repository enforces immutability.
    gh("release", "upload", tag, *map(str, files), "--clobber")
    gh("release", "edit", tag, "--draft=false", f"--prerelease={'true' if prerelease else 'false'}",
       f"--latest={'false' if prerelease else 'true'}", "--notes-file", str(notes))
    return f"https://github.com/{repository}/releases/tag/{tag}"


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
    tag = "" if rolling else args.ref.removeprefix("refs/tags/")
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
    if rolling:
        tag = snapshot_tag(args.commit, checksums)
        existing = subprocess.run(["git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"],
                                  capture_output=True, text=True)
        if existing.returncode == 0:
            if existing.stdout.strip() != args.commit:
                raise ValueError("Main snapshot tag points to a different commit")
        else:
            subprocess.run(["git", "tag", tag, args.commit], check=True)
        subprocess.run(["git", "push", "origin", f"refs/tags/{tag}"], check=True)
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
        body += (f"\nThis main snapshot keeps its original assets and tag. "
                 f"[Continuous downloads](https://github.com/{repo}/wiki/Continuous-builds) "
                 "always link to the newest available tested main snapshot.\n")
    else:
        generated = json.loads(gh("api", f"repos/{repo}/releases/generate-notes", "-X", "POST",
                                  "-f", f"tag_name={tag}", "-f", f"target_commitish={args.commit}"))
        body += "\n" + generated["body"]
    notes.write_text(body)
    url = publish(repo, tag, f"Main build {args.commit[:12]}" if rolling else f"OpenT1 {tag}",
                  prerelease, [*archives, checksums], notes)
    if rolling:
        current_main = gh("api", f"repos/{repo}/git/ref/heads/main", "--jq", ".object.sha")
        if current_main == args.commit:
            sync_wiki.publish(repo, {"Continuous-builds.md": download_page(repo, args.commit, version, tag)})
        else:
            print("A newer main commit exists; retain this snapshot without changing the download index.")
    print(url)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
