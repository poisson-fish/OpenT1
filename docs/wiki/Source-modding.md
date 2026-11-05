# Source modding

OpenT1 is C++20. Public interfaces live in `include/opent1/`, implementations in
`src/`, tests in `tests/`, and native demos/tools in `examples/` and `tools/`.
See the [architecture](https://github.com/poisson-fish/OpenT1/blob/main/docs/architecture.md)
for dependency direction, ownership and application flow.

## Build and test

Clone [the repository](https://github.com/poisson-fish/OpenT1), install CMake 3.25+
and Ninja, and select a supported compiler/toolchain:

| Host | Toolchain | Presets |
| --- | --- | --- |
| Windows x64 | Visual Studio Desktop development with C++ workload and Windows SDK | `x64-debug`, `x64-release` |
| Linux x64 | GCC/G++ 14 and SDL platform development packages | `linux-debug`, `linux-release` |
| macOS ARM64 | Homebrew LLVM 20+ and matching libc++ | `macos-debug`, `macos-release` |

```sh
cmake --preset x64-debug
cmake --build --preset x64-debug --parallel 8
ctest --preset x64-debug
```

Substitute your host's preset. Windows presets locate MSVC/SDK automatically.
Configuration fetches pinned, hash-verified SDL/OpenAL sources when needed.
Linux package lists, custom toolchain options, shader maintenance, tests and
optional installed-game acceptance cases are documented in
[development](https://github.com/poisson-fish/OpenT1/blob/main/docs/development.md).
Use the same binary with `--headless --no-audio` for a server; a different headless
build is not required.

## Choose the smallest appropriate layer

Start from an observable behavior and trace its callers. Keep core formats,
console evaluation, transport and simulation independent of SDL/OpenAL adapters.
Maintain ownership rules for simulation objects, resource handles, callbacks and
GPU/audio lifetimes. An object ID belongs to its manager/world; retaining a raw
pointer across mission deletion is unsafe.

For a new script command, register a native callback through the owning plugin's
`Console::addCommand`. `argv[0]` is the command name. Validate argument count,
numeric bounds, IDs, world availability and permissions before changing state.
Return the command's documented console value, and unregister borrowed callbacks
when their owner dies (or use scoped registration). The
[C++ console guide](https://github.com/poisson-fish/OpenT1/blob/main/docs/script-runtime.md)
has a complete binding example and lifetime contracts.

Update [scripting documentation](Scripting-reference.md) and run
`python tools/sync_wiki.py --write-index` when changing native registrations. The
generated [command index](Command-index.md) links implementation sites; registrations
assembled dynamically are intentionally not inferred by that source scanner.

## Preserve compatibility and demonstrate changes

The core targets vanilla Tribes 1.11 behavior and a common network protocol.
Portability, stability, bug fixes and modest presentation improvements fit the
project. Gameplay redesigns, incompatible protocols and major rendering changes
belong in forks. If you intentionally make an incompatible fork, identify it
clearly to users and avoid claiming vanilla interoperability without testing it.

Add regression coverage for the failing behavior, preferably using small original
or synthetic inputs. Keep expected results independent of the implementation.
Run the relevant subsystem, then the full applicable suite. Test Release too;
CI checks Debug and Release on Windows x64, Linux x64 and macOS ARM64. Hardware
GPU/audio checks, live networking and installed-game acceptance remain separate.

Read [CONTRIBUTING.md](https://github.com/poisson-fish/OpenT1/blob/main/CONTRIBUTING.md)
before opening a PR. Describe the trigger, resulting behavior, validation and
remaining limits. Update docs alongside behavior changes. Never commit game
installations, generated build trees, credentials or runtime state.
