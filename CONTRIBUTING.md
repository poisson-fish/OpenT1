# Contributing

OpenT1 targets Tribes 1.11 gameplay, asset formats, scripting behavior and a common
network protocol. Portability, stability, fixes and modest presentation changes
fit the core project. Gameplay redesigns, incompatible protocols and major
rendering changes belong in forks.

Read [development](docs/development.md) for toolchains/tests and
[architecture](docs/architecture.md) for ownership and dependency boundaries.
Create a focused branch, reproduce the problem, and add meaningful regression
coverage for behavior changes. Prefer original/synthetic fixtures; never commit
game installations or regenerate expected results merely to match a regression.

Run applicable subsystem tests and the full suite in Debug and Release. The CI
matrix builds Windows x64, Linux x64 and macOS ARM64. State which hardware,
installed-game and live-network checks you actually performed. A headless test
does not establish graphics/audio-device or vanilla server interoperability.

PRs should explain the triggering problem, resulting behavior, tests and material
limitations. Keep protocol changes explicit and document script command contracts.
Update the wiki source pages in `docs/wiki/` alongside changes and regenerate the
command index with `python tools/sync_wiki.py --write-index`. Validate it with
`python tools/sync_wiki.py --check`. Do not edit generated wiki pages directly.

Use the issue template to report bugs. Include version/commit, platform,
installation/mod revision, mission, reproduction and sanitized logs. Keep
credentials, saves and build directories out of commits and attachments.

Contributions are distributed under the repository's [GPL-3.0 license](LICENSE).
Preserve third-party notices and identify dependency alterations in
[THIRD_PARTY.md](THIRD_PARTY.md).
