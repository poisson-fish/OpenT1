# OpenT1

OpenT1 is a C++20 modernization project for Starsiege: Tribes, targeting version
1.11 gameplay, asset formats, scripting behavior, and network compatibility.
It includes SDL3 windowing and GPU presentation, an indexed software renderer,
and OpenAL Soft audio.

Work focuses on portability, stability, bug fixes, and basic presentation
improvements while preserving vanilla gameplay and a common network protocol.
Major rendering overhauls, new advanced visual effects, gameplay redesigns, and
incompatible protocol changes are outside the core project's scope and belong
in forks.

The current implementation includes client/server simulation, player movement
and prediction, weapons and vehicles, terrain and interiors, mission loading,
the Tribes scripting runtime, HUD and shell GUI, and Smacker video playback.
It remains a work in progress; compatibility and platform support should be
verified for the particular game content and environment being used.

## Build

On Windows, install Visual Studio with the **Desktop development with C++**
workload and Windows SDK, **CMake 3.25+**, and **Ninja**. The presets locate MSVC
and the SDK automatically. Configuration uses an installed SDL3 package when
available, otherwise downloads a pinned, hash-verified source archive. OpenAL
Soft is always built from a pinned, hash-verified source archive.

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug --parallel 8
ctest --preset x64-debug
```

Use `x64-release` for an optimized build. The same executable supports windowed
play and headless dedicated servers. See [building and testing](docs/development.md)
for portable configuration, build options, and the test suite.

On macOS, install CMake, Ninja, and Homebrew LLVM (`brew install cmake ninja llvm`),
then use `macos-debug` or `macos-release`. In VS Code, run
**CMake: Select Configure Preset** and choose the corresponding macOS preset.
These presets select the compiler and matching C++ library automatically.

On Linux, install GCC 14, CMake, Ninja, and SDL's platform development dependencies,
then use `linux-debug` or `linux-release`.

## Run

Gameplay requires an externally supplied Tribes installation.

```powershell
.\build\x64-debug\opent1.exe --assets "C:\Games\Tribes"
```

The asset directory should contain `base/` and the game volumes.
A short demonstration can also run without any game data:

```powershell
.\build\x64-debug\opent1.exe --smoke --headless --no-audio --no-assets --spawn-demo --frames 4
```

See the [runtime reference](docs/runtime.md) for asset lookup, rendering options,
startup scripts, output files, and dedicated-server execution with
`-dedicated MISSION --headless --no-audio`. Headless startup requires neither
a GPU nor a display server.

## Code and documentation

- [Architecture](docs/architecture.md): module boundaries, ownership, and runtime flow.
- [Development](docs/development.md): configuration, tests, and adding regression coverage.
- [Runtime](docs/runtime.md): launch options, data paths, and diagnostics.
- [Render measurements and plan](docs/render-improvements.md): base/RPG memory baselines and platform-aware rendering work.
- [Script runtime](docs/script-runtime.md): console integration and scripting contracts.
- [Simulation networking](docs/simulation-networking.md): replication and wire behavior.

Public APIs live in `include/opent1/`, implementations in `src/`, and the
single test runner in `tests/`. `examples/` contains optional native demos;
`opent1_asset_tool` inspects and converts supported assets.

## License and dependencies

OpenT1 uses the [GNU General Public License, version 3](LICENSE).
Dependencies retain their respective licenses; see [third-party notices](THIRD_PARTY.md).

Runtime game installations are not part of the project distribution. Do not add
game archives, missions, scripts, GUI definitions, textures, models, sounds, or
videos from an installation to the repository or release packages. Regression
test data is inventoried separately in [test fixtures](tests/data/README.md).
