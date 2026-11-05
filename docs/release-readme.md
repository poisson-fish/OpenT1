# OpenT1 binary distribution

OpenT1 targets Starsiege: Tribes 1.11 compatibility. Game data is supplied
separately. Extract the whole archive and keep its runtime libraries together.
`release.json` identifies this download's version, commit and platform.

From the extracted directory:

```powershell
.\opent1.exe --assets "C:\Games\Tribes" --windowed
.\opent1.exe --version
```

```sh
./opent1 --assets "$HOME/Games/Tribes" --windowed
./opent1 --version
```

The installation root must include `base/`, the game VOL archives and `console.cs`.
Linux binaries target Ubuntu 24.04 or compatible systems; macOS binaries target
Apple Silicon on macOS 15 or newer. macOS packages are ad-hoc signed, without
Developer ID signing/notarization. Windows binaries target x64.

See [Getting started](docs/Getting-started.md), [game modding](docs/Game-modding.md),
[scripting](docs/Scripting-reference.md), [examples](docs/Scripting-examples.md),
[server hosting](docs/Server-hosting.md), and [troubleshooting](docs/Compatibility-and-troubleshooting.md).
`opent1_asset_tool` provides native asset inspection/conversion utilities.

Continuous downloads are development prereleases. Numbered releases are fixed.
Automated package tests cover headless startup, not real GPU/audio hardware or
every game/mod. Verify archives with `SHA256SUMS.txt` from the same release.

Source and exact SDL/OpenAL source archives/build alterations are provided in
`OpenT1-source.tar.gz` alongside the downloads. Read `LICENSE`, `THIRD_PARTY.md` and
`LICENSES/` for notices. Installation game assets are not distributed here.

[Releases](https://github.com/poisson-fish/OpenT1/releases) ·
[Wiki](https://github.com/poisson-fish/OpenT1/wiki) ·
[Issues](https://github.com/poisson-fish/OpenT1/issues/new/choose)
