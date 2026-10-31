# Building and testing

## Windows presets

Install CMake 3.25 or newer, Ninja, Visual Studio's Desktop development with C++
workload, and a Windows SDK. The MSVC toolchain file locates the compiler and
SDK, so a developer command prompt is not required.

| Preset | Configuration |
| --- | --- |
| `x64-debug` | Debug, SDL3, OpenAL Soft, tests |
| `x64-release` | Release, SDL3, OpenAL Soft, tests |
| `headless` | Debug, tests, no SDL3 or OpenAL dependency |

Each preset supports the same three commands:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug --parallel 8
ctest --preset x64-debug
```

The first desktop configuration needs internet access to fetch dependencies.
SDL3 3.4.2 is fetched if no suitable installed SDL3 package is found. OpenAL
Soft 1.25.2 is built as a shared library. Both downloads have SHA-256 checks.
The presets keep build output, fetched dependencies and embedded shader headers
under `build/`. Custom configurations use their chosen binary directory. The
downloaded SDL source receives a build-local event-watch dispatch correction;
an installed SDL package does not receive that correction.

To include all five demonstration executables in the default build, configure
with `cmake --preset x64-debug -DOPENT1_BUILD_EXAMPLES=ON`.

## Other toolchains

On Linux or macOS, use an out-of-tree build with a compiler and standard
library supporting C++20, including `std::format` (GCC 14 or newer, or a recent
Apple Clang). For a full desktop build, install SDL's platform development
dependencies first; CI's Linux installation step lists the packages used on
Ubuntu 24.04. Configuration fetches SDL3 if an installed package is unavailable
and always fetches OpenAL Soft when audio is enabled.

```sh
cmake -S . -B build/desktop -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DOPENT1_BUILD_EXAMPLES=ON
cmake --build build/desktop --parallel 3
ctest --test-dir build/desktop --output-on-failure --no-tests=error
```

For the dependency-free subset:

```sh
cmake -S . -B build/headless -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DOPENT1_BUILD_PLATFORM=OFF -DOPENT1_BUILD_AUDIO=OFF
cmake --build build/headless --parallel 8
ctest --test-dir build/headless --output-on-failure --no-tests=error
```

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Build and register the unified test runner |
| `OPENT1_BUILD_PLATFORM` | `ON` | Build SDL3 window/input/GPU adapters |
| `OPENT1_FETCH_SDL` | `ON` | Fetch SDL3 if an installed package is unavailable |
| `OPENT1_BUILD_AUDIO` | `ON` | Build OpenAL audio and simulation adapters |
| `OPENT1_BUILD_EXAMPLES` | `OFF` | Build native demonstration applications |
| `OPENT1_BUILD_TERRAIN2` | `ON` | Include the additional Terrain2 authoring/render branch |

Warnings are errors in project C++ code. Public headers use the
`<opent1/...>` include prefix. The main targets are `opent1`,
`opent1_asset_tool`, and `opent1_tests`.

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

The full build registers 34 C++ cases in one executable and 12 subsystem suites:
core, math, console, network, simulation, graphics, world, GUI, game, video,
and optional audio/platform.
CTest also runs an application smoke check, for 13 CTest checks in total.
Headless builds register 32 C++ cases in 10 subsystem suites plus the smoke
check, for 11 CTest checks. They omit the audio and platform cases. Default
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

Add regression cases to the relevant subsystem directory. Register the case
with `OPENT1_REGISTER_TEST` from `test_registry.hpp`, add its source to
`tests/CMakeLists.txt`, and return zero on success. Prefer the shared `CHECK`
and `opent1::test::run` helpers for new cases. Tests that need temporary files
should use their working directory or a uniquely named temporary directory.
CTest gives each subsystem a separate working directory so suites can run
concurrently.

## Continuous integration and validation limits

The [CI workflow](../.github/workflows/build.yml) is configured to build and test
desktop and headless Debug configurations on Linux x64 (`ubuntu-24.04`, GCC 14), macOS
ARM64 (`macos-15`, Apple Clang), and Windows x64 (`windows-2025`, MSVC).
Desktop jobs enable all optional libraries, Terrain2, and all five demos, and
build the default all-targets graph. Every job runs all applicable CTest suites
and the application smoke test without an external installation, using SDL's dummy
video and OpenAL's null audio drivers. CI fetches the pinned SDL source even
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
