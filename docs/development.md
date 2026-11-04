# Building and testing

## Windows presets

Install CMake 3.25 or newer, Ninja, Visual Studio's Desktop development with C++
workload, and a Windows SDK. The MSVC toolchain file locates the compiler and
SDK, so a developer command prompt is not required.

| Preset | Configuration |
| --- | --- |
| `x64-debug` | Debug, SDL3, OpenAL Soft, tests |
| `x64-release` | Release, SDL3, OpenAL Soft, tests |

Each preset supports the same three commands:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug --parallel 8
ctest --preset x64-debug
```

The first desktop configuration needs internet access to fetch dependencies.
SDL3 3.4.2 is fetched from its official GitHub release if no suitable installed
SDL3 package is found. OpenAL Soft 1.25.2 is built as a shared library. Both
downloads have SHA-256 checks, a 120-second timeout per attempt, and a 30-second
inactivity timeout. CMake may retry failed downloads. The desktop presets and
CI show download progress so a stalled connection is visible.
Archives are kept in `<binary directory>/downloads`. CI restores these archives
before configuration and saves them after successful configuration, even if a
later build or test fails. Restored archives are checked against their pinned
SHA-256 hashes before use.
The presets keep build output, fetched dependencies and embedded shader headers
under `build/`. Custom configurations use their chosen binary directory. The
downloaded SDL source receives a build-local event-watch dispatch correction;
an installed SDL package does not receive that correction.

To include all five demonstration executables in the default build, configure
with `cmake --preset x64-debug -DOPENT1_BUILD_EXAMPLES=ON`.

## Linux presets

Install CMake 3.25 or newer, Ninja, GCC 14 (`gcc-14` and `g++-14`), and SDL's
platform development dependencies (see the Linux installation step in CI).
The Linux presets use GCC 14, matching CI:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 3
ctest --preset linux-debug
```

Use `linux-release` for an optimized build. On distributions with different executable
names, override `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` at configuration time
with a compiler supporting the C++20 facilities listed below.

## macOS presets

Install CMake 3.25 or newer, Ninja, the Xcode Command Line Tools, and Homebrew
LLVM (`brew install cmake ninja llvm`). The macOS presets select Homebrew's
compiler, matching libc++ headers/runtime, and the macOS SDK automatically:

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug --parallel 3
ctest --preset macos-debug
```

Use `macos-release` for an optimized build. A custom LLVM installation can be selected
with `-DOPENT1_LLVM_ROOT=/path/to/llvm` in a fresh build directory. The toolchain
prefers the current `llvm` formula and falls back to `llvm@20` if installed.
After upgrading or switching compilers, run `cmake --fresh --preset <name>` to
discard stale compiler paths and feature checks before rebuilding.

## VS Code

With CMake Tools, run **CMake: Select Configure Preset**, choose a preset for
your platform, then run **CMake: Configure**. Configure presets are filtered by
host operating system. All platforms have configure, build, and test presets:

| Platform | Debug | Release |
| --- | --- | --- |
| Windows (MSVC) | `x64-debug` | `x64-release` |
| macOS (Homebrew LLVM) | `macos-debug` | `macos-release` |
| Linux (GCC 14) | `linux-debug` | `linux-release` |

Each preset uses its own build directory, avoiding cached compiler selections
from a different platform.

## Other toolchains

On Linux or macOS, use an out-of-tree build with a compiler and standard
library supporting C++20, including `std::format`, `std::jthread`, and
`std::stop_token` (GCC 14 or newer, or LLVM 20 or newer with matching libc++).
Install SDL's platform development dependencies first; CI's Linux
installation step lists the packages used on Ubuntu 24.04. Configuration fetches
SDL3 if an installed package is unavailable
and always fetches OpenAL Soft.

```sh
cmake -S . -B build/desktop -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DOPENT1_BUILD_EXAMPLES=ON
cmake --build build/desktop --parallel 3
ctest --test-dir build/desktop --output-on-failure --no-tests=error
```

On macOS, install [Homebrew LLVM](https://formulae.brew.sh/formula/llvm)
and select both its compiler and its C++ library before configuring a fresh
build directory. The Apple C++ library in
the macOS 15 CI image does not provide `std::jthread` or `std::stop_token`.
Using only Homebrew's compiler with Apple's runtime is insufficient.

```sh
brew install llvm
llvm_prefix="$(brew --prefix llvm)"
export CC="$llvm_prefix/bin/clang"
export CXX="$llvm_prefix/bin/clang++"
export CXXFLAGS="-nostdinc++ -isystem $llvm_prefix/include/c++/v1"
export LDFLAGS="-L$llvm_prefix/lib/c++ -Wl,-rpath,$llvm_prefix/lib/c++"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
```

Then run the CMake commands in this section. The runtime
search path keeps tests and executables using the same Homebrew libc++ that
they were built against.

Every build includes SDL3 and OpenAL support. Use the same executable for
interactive play and dedicated servers; `--headless --no-audio` skips video,
GPU, gamepad, and playback-device initialization. There are no separate
headless build presets or dependency-free runtime variants.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Build and register the unified test runner |
| `OPENT1_FETCH_SDL` | `ON` | Fetch SDL3 if an installed package is unavailable |
| `OPENT1_BUILD_EXAMPLES` | `OFF` | Build native demonstration applications |
| `OPENT1_BUILD_TERRAIN2` | `ON` | Include the additional Terrain2 authoring/render branch |
| `OPENT1_TEST_GPU` | `OFF` | Register the optional real-device GPU test |
| `OPENT1_TEST_GPU_DRIVER` | empty | Select its SDL GPU backend; empty selects automatically |

Warnings are errors in project C++ code. Public headers use the
`<opent1/...>` include prefix. The main targets are `opent1`,
`opent1_asset_tool`, and `opent1_tests`.
Project arithmetic keeps multiply and add operations separate so ARM FMA
contraction does not change legacy rasterization and collision boundaries.

## Shader maintenance

Normal builds embed the committed shader files through
`cmake/embed_gpu_shaders.cmake`; Python and a shader compiler are not build
prerequisites. To regenerate DXIL and SPIR-V after editing the HLSL programs,
install the DXC command-line compiler and run:

```powershell
python src/gfx/shaders/compile.py "C:\Tools\dxc\bin\x64\dxc.exe"
```

Update the corresponding `material.vert.msl` and `material.frag.msl` sources
alongside the HLSL; Metal compiles them at runtime. Review all shader outputs and
exercise the affected GPU backends before distributing a shader change.

## Test suite

The build registers standard C++ cases in one executable and 12 subsystem suites:
core, math, console, network, simulation, graphics, world, GUI, game, video,
audio, and platform.
CTest also runs the AI parity audit and application startup checks. The headless
smoke test and dedicated startup test select an invalid SDL video driver to
verify that these modes do not initialize video. Default
checks use the bundled fixtures and do not need a game installation, Python,
or an external reference executable.

```powershell
# Run everything, or choose a subsystem through CTest.
ctest --preset x64-debug
ctest --preset x64-debug -R "^(game|network)$"

# List or directly invoke the registered C++ cases.
.\build\x64-debug\tests\opent1_tests.exe --list
.\build\x64-debug\tests\opent1_tests.exe --suite core
```

The cases cover streams/compression/resources/persistence, numeric
geometry, script parsing and evaluation, transport and replication, object
lifecycle and collision, bitmap and batch rendering, terrain/shape/mission
loading, GUI routing/editing, player/weapons behavior, game integration, and
media decoding/playback. The [fixture inventory](../tests/data/README.md)
describes the checked-in test records, embedded test streams, and metadata,
with sizes and SHA-256 hashes.

Default CTest runs use SDL's dummy video driver and skip GPU rendering. To
exercise actual texture uploads, lightmap pages, arrays, terrain and shapes on
a Mac, enable the optional GPU check and select Metal:

```sh
cmake --preset macos-release -DOPENT1_TEST_GPU=ON -DOPENT1_TEST_GPU_DRIVER=metal
cmake --build --preset macos-release
ctest --preset macos-release -R '^gpu$'
```

The `gpu` check requires a native video driver and fails if
`SDL_VIDEODRIVER=dummy` is inherited. It runs serially to avoid native window
tests interfering with each other. The same check supports `direct3d12` and
`vulkan` on hosts providing those backends. Without reconfiguring CMake, run
`opent1_tests --suite gpu` directly with `SDL_GPU_DRIVER=metal` in the environment.
Patterned readbacks verify row widths around 256-byte alignment boundaries,
framebuffer/bitmap uploads, dynamic textures, array layers and authored mipmaps
against known pixel colors.

Add regression cases to the relevant subsystem directory. Register the case
with `OPENT1_REGISTER_TEST` from `test_registry.hpp`, add its source to
`tests/CMakeLists.txt`, and return zero on success. Prefer the shared `CHECK`
and `opent1::test::run` helpers for new cases. Tests that need temporary files
should use their working directory or a uniquely named temporary directory.
CTest gives each subsystem a separate working directory so suites can run
concurrently.

The diagnostic Python tools have asset-free regression tests using only the
Python standard library. CI runs these separately from CTest; Python is not
required for a normal C++ build:

```powershell
python -m unittest discover -s tests/tools -v
```

Gameplay regressions include target filtering, impact delivery, player animation
audio, looping weapon audio, target names and health fractions, mission clock
resets, and projectile velocity inheritance. An optional acceptance case uses an
external game installation to check chaingun damage and renderable impacts on
all five antenna models and a Scout, then cycles a hosted mission through the
summary screen and back to player control:

```powershell
$env:OPENT1_TEST_ASSETS = 'C:\Games\Tribes'
.\build\x64-debug\tests\opent1_tests.exe --suite fixpack
```

The optional cases do not bundle game assets or run unless this variable is set.
The `game` suite's installed-content integration checks use the same variable;
they do not auto-discover nearby installations during the standard test run.

When the selected installation includes `RPG/scripts.vol`, the same `fixpack`
suite also hosts `rpgmap5`, checks the mission terrain, advances eight simulation
frames and renders their world geometry, prepares a hardware frame, switches
resource roots and script volumes back to base, and hosts Raindance. It also checks
new-character group/class menus through Tab and numeric input, the Back option,
player spawning, save/reload, returning characters, and delayed disconnection of
wrong/empty-password profiles. These checks require the installed mod's quoted
kick reason to be escaped in `connectivity.cs`. It uses a separate temporary
write directory. The ordinary `game` suite includes synthetic coverage for mod
priority, duplicate enumeration, changed enumeration patterns, mission archive
ownership, native string-table spellings, exports, EMF grouping and lighting.
RPG acceptance also verifies the spawned race and skin, and renders installed
melee weapon models in both views using the active palette. Synthetic coverage
checks stale model spheres, skin ownership and palette selection without assets.

The optional `trpg` suite hosts `rpgmap5`, spawns an enemy through the installed
RPG scripts, and checks that the player's mounted pickaxe and the enemy's weapon
both deal damage. It also fires a bot's sling through the RPG scripts, collects
a loot chest through player collision, checks repeated enemy-zone scans, and
submits chat twice through the installed GUI. A second case checks town NPC
positions and rendered skins, server/client ferry movement, inventory reentry
through the installed key bindings and Escape, thrown loot collection, visible
enemy death packs, and an AI caster completing a damaging spell. A third case
hosts `rpgmap1` with its startup NPCs, verifies the local player's identity and
camera, renders software and hardware frames, and completes character creation.
These cases use the same asset variable and temporary write directories:

```powershell
.\build\x64-debug\tests\opent1_tests.exe --suite trpg
```

The ordinary `game` suite covers silent bot connection joins/drops, vanilla bot
team callbacks, shared representative-pool exhaustion and reuse, native/script
ownership transfers, ownership-based AI targeting/following, projectile/mine
attribution across ID reuse, and script weapon
callbacks and fallback lookup, pitched melee traces, crouch collision bounds,
uphill acceleration and turn timing, the jump arc, and grounded prediction during smoothing.
It also covers message recipients, sound-only chat, reopened chat and history
editing, throw aiming and collision callbacks, and the thrower pickup blackout.
Additional regressions cover item collisions with translated/rotated terrain and
interiors, bot skin replication and score filtering, client-ID damage callbacks,
zoom notifications triggered by input, and ferry waypoint arrival and reset.

## Continuous integration and validation limits

The [CI workflow](../.github/workflows/build.yml) is configured to build and test
the Debug configuration on Linux x64 (`ubuntu-24.04`, GCC 14), macOS
ARM64 (`macos-15`, Homebrew LLVM 20 and libc++ 20), and Windows x64 (`windows-2025`, MSVC).
Jobs enable Terrain2 and all five demos, and
build the default all-targets graph. Every job runs all applicable CTest suites
and the application startup tests without an external installation. Windowed
tests use SDL's dummy video driver and OpenAL's null audio driver; headless
startup tests run with an invalid video driver. CI fetches the pinned SDL source even
if a system SDL package is installed, so the nested-dispatch fix is exercised.
The workflow runs on pushes and pull requests and can be started manually.
The matrix describes configured coverage; successful workflow runs and the
hardware actually exercised must be assessed separately. Interactive GPU-driver
checks, live server interoperability, and installation-based acceptance runs
are outside the default suite. The C++ case count also differs from the CTest
check count because CTest groups cases into subsystem suites.

## Distribution inputs

The repository has no install rules or automated release-package workflow.
When preparing a binary distribution, include `LICENSE`, `THIRD_PARTY.md`, and
the applicable files in `LICENSES/`, alongside the required runtime libraries.
Provide corresponding source and dependency modifications as required by their
licenses; the dependency URLs and SDL alteration are recorded in
[third-party notices](../THIRD_PARTY.md). An installed SDL package may have
different build inputs from the pinned source configuration.

Keep game installations and generated runtime state outside release inputs.
`tests/data/` supports development and is not needed to run the game. Generated
build directories are not release source inputs. Check a proposed archive's
contents explicitly rather than copying an entire working directory.
