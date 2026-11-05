# Getting started

## Choose a download

[Numbered releases](https://github.com/poisson-fish/OpenT1/releases) identify fixed
versions. The [continuous download page](https://github.com/poisson-fish/OpenT1/wiki/Continuous-builds)
links the newest successful main snapshot. The page advances as main builds pass;
each linked snapshot retains its original files and tag. Keep the version or
commit identity when reproducing a development issue.

| Package | Target |
| --- | --- |
| `OpenT1-windows-x64.zip` | Windows x64; built/tested on Windows Server 2025, intended for modern desktop Windows |
| `OpenT1-linux-x64.tar.gz` | Linux x64, Ubuntu 24.04 or a compatible glibc/C++ runtime |
| `OpenT1-macos-arm64.tar.gz` | Apple Silicon, macOS 15 or newer; Intel Macs are not currently packaged |
| `OpenT1-source.tar.gz` | Source plus exact SDL/OpenAL source archives and alteration/build recipes |

Hardware rendering needs a graphics device/driver supported by SDL_GPU. CI uses
dummy video and null audio for automated tests; GPU/audio hardware coverage must
be assessed separately. The same executable supports a [dedicated server](Server-hosting.md)
without a window or playback device.

## Extract and verify

Extract the entire binary archive to a writable directory. Keep the executable,
runtime libraries, license files and docs together. Do not copy just `opent1.exe`.
Game archives, missions and installation scripts are intentionally not bundled.

Compare your archive's SHA-256 digest to `SHA256SUMS.txt` from the same release:

```powershell
Get-FileHash .\OpenT1-windows-x64.zip -Algorithm SHA256
```

```sh
sha256sum OpenT1-linux-x64.tar.gz
# macOS:
shasum -a 256 OpenT1-macos-arm64.tar.gz
```

## Supply game data and launch

Use a separate Tribes installation containing `base/`, VOL archives, and its
physical `console.cs` bootstrap file. Back up saves/configuration before testing
a new engine or mod. Run from the extracted package directory:

```powershell
.\opent1.exe --assets "C:\Games\Tribes" --windowed
```

```sh
./opent1 --assets "$HOME/Games/Tribes" --windowed
```

The installation's startup scripts initialize the shell, resource search paths
and game rules. Check filename spelling/case when moving installations between
Windows and a case-sensitive filesystem. `--assets` must point to the game root,
not the download directory or an individual VOL file.

## Configuration and diagnostics

The installation/mod scripts determine preferences and their persistence. Avoid
editing original game archives just to change a preference. `--renderer software`
selects indexed software rendering; `--renderer hardware` requires a working GPU
backend. Use `--windowed` when diagnosing fullscreen problems.

```sh
./opent1 --version
./opent1 --assets "$HOME/Games/Tribes" --renderer software --windowed
```

`release.json` records the package's version, full commit, platform and build
configuration. Include it when [reporting a problem](Compatibility-and-troubleshooting.md).
`--version` can run without game data, a display or an audio device.

An unsigned macOS archive is not Developer ID signed/notarized. Follow your
organization's normal software approval process if macOS blocks launch. Do not
disable platform security settings as part of installation.

The complete option/path reference is in
[`docs/runtime.md`](https://github.com/poisson-fish/OpenT1/blob/main/docs/runtime.md).
