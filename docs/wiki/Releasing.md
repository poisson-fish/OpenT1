# Releasing and maintaining docs

## Release channels

`VERSION` holds OpenT1's own three-part version, initially `0.1.0`. Tribes `1.11`
is the compatibility target, not the engine release version. `opent1 --version`
prints the engine version and source commit; every binary archive contains
`release.json` with the same identity.

The Build and test workflow runs Debug and Release on Windows x64, Linux x64 and
macOS ARM64. Release jobs stage explicit install inputs, inspect them, archive
them, extract each download and test the extracted executable away from the build
tree. Publication requires all six build/test jobs and the tools check to pass. PRs produce reviewable
artifacts but cannot publish releases.

After a successful push to `main`, the workflow publishes a fixed prerelease
snapshot and updates [Continuous builds](https://github.com/poisson-fish/OpenT1/wiki/Continuous-builds)
with current download links. Snapshot tags contain the source commit and a digest
of the download checksums: `main-COMMIT-DIGEST`. Rebuilding the same source with
different archive bytes creates a distinct snapshot. No published tag or asset is
overwritten. Main snapshots are never marked the latest stable release.

This works with the repository's immutable-release enforcement enabled.
[GitHub locks published immutable tags and assets](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases).
The continuous wiki page changes its links while the files behind each link stay
fixed. A stale main run cannot roll the index back after a newer main commit exists.
The original `continuous` tag is an archived initial snapshot, not a moving tag.

## Make a numbered release

1. Update `VERSION` in a reviewed PR when starting a new version. Keep its numeric
   part aligned with the proposed tag. Complete the acceptance checklist below.
2. Merge the release preparation into `main` and wait for CI. Record any remaining
   limitations in the PR/release notes.
3. Tag that tested main commit and push the tag:

   ```sh
   git fetch origin main
   git tag -a v0.1.0 origin/main -m "OpenT1 0.1.0"
   git push origin v0.1.0
   ```

4. The tag promotes the exact artifacts from that successful main build into a
   numbered release with generated change notes. It checks the full commit/version
   identity of every binary archive. Use `v0.1.0-alpha.1`, `v0.1.0-beta.1`, or
   `v0.1.0-rc.1` for a numbered prerelease. The numeric part must match `VERSION`;
   tags outside main history are rejected.
5. Confirm all downloads/checksums and append the manual validation/known-issues
   notes. Stable tags become latest; alpha/beta/rc tags remain prereleases.

An existing published release is verified against the tested asset digests on a
retry. Identical bytes are reused; different bytes require a new tag. Use a new
numbered version for corrections. A failed upload can leave a draft that a
rerun completes. If a tag was pushed before its main build completed, rerun the
numbered-release workflow after main succeeds. Build artifacts are retained for
14 days; rerun the exact main build to regenerate expired inputs before promoting
an older main commit. The build workflow can also be dispatched on `main` to refresh rolling
downloads. It uses the repository's standard `GITHUB_TOKEN` with contents-write
permission only in the publishing job; no personal token is needed for releases.

## Before a public numbered release

- Check the six matrix jobs and tools check, and inspect the archive lists/hashes.
- On a clean target machine, extract and launch with actual game data. Verify
  menus, input, window/fullscreen behavior, hardware/software rendering and audio.
- Play a base mission, host/join using separate processes, reconnect and reload
  missions. Run a dedicated server and representative mods.
- Test live vanilla 1.11 interoperability before claiming it for that release.
- If editor changes ship, save/reload a disposable edited mission and terrain.
- Record the tested OS/hardware/content revisions and unresolved defects. Mark
  unperformed checks explicitly; headless CI cannot substitute for them.
- Verify notices/source inputs are present and no installations, saves,
  credentials or logs slipped into downloads.

Manual acceptance is a maintainer checklist, not an automated guarantee. Rolling
downloads are development prereleases and state their limited CI coverage.

## Build packages locally

```sh
cmake --preset x64-release -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON
cmake --build --preset x64-release --parallel 8
ctest --preset x64-release
python tools/package_release.py --build build/x64-release --output build/dist --platform windows-x64
```

Use `linux-release`/`linux-x64` or `macos-release`/`macos-arm64` on the corresponding
host. Add `--source` to create the combined source bundle. Commit the inputs first:
source packaging uses `git archive HEAD` and never copies arbitrary working-tree
files. It verifies the exact pinned SDL/OpenAL source archive hashes before
including them under `dependencies/`; CMake alteration recipes are in `cmake/`.

To build the source bundle without redownloading those dependencies, extract the
source archives in `dependencies/`, then configure with
`-DFETCHCONTENT_SOURCE_DIR_SDL3=/absolute/path/to/SDL3-3.4.2` and
`-DFETCHCONTENT_SOURCE_DIR_OPENAL_SOFT=/absolute/path/to/openal-soft-1.25.2`.
The dependency alteration scripts still run. Follow normal toolchain/platform
prerequisites in the bundled development docs or repository README.

## Wiki maintenance

The canonical pages are in `docs/wiki/`. Edit those through normal PRs. Run:

```sh
python tools/sync_wiki.py --write-index
python tools/sync_wiki.py --check
python tools/sync_wiki.py --destination build/wiki-preview
```

`--write-index` regenerates literal native command registrations and source links.
Validation checks the index and local page links. Export converts Markdown file
links into wiki page links. Binary packages preserve `.md` links for offline use.

The Publish wiki workflow syncs repo-owned pages after main documentation changes;
unrelated manually created wiki pages are preserved. Initialize the wiki's first
Home page through GitHub once before using Git publication. The workflow first
uses `GITHUB_TOKEN`; if the repository's wiki policy rejects that token, supply
a repository secret `WIKI_TOKEN` with write access to that wiki. No token is
embedded in source, command URLs, generated docs or logs.

`Continuous-builds.md` is generated directly by the successful main publisher and
is deliberately outside `docs/wiki/`. Ordinary documentation sync preserves it;
do not add a canonical page with that filename. It is the stable entry point for
current main downloads. Main/numbered release assets remain immutable.

Maintainers can publish with existing GitHub CLI credentials:

```sh
python tools/sync_wiki.py --publish poisson-fish/OpenT1
```
