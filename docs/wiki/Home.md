# OpenT1

OpenT1 modernizes Starsiege: Tribes in C++20, targeting **Tribes 1.11** gameplay,
asset formats, scripting and network compatibility. It provides SDL3 windowing
and presentation, software/hardware rendering, and OpenAL Soft audio. It remains
a work in progress; a passing automated build does not establish compatibility
with every installation, mod or hardware driver.

Start with [Getting started](Getting-started.md). Download a fixed version from
[Releases](https://github.com/poisson-fish/OpenT1/releases), or use the
[continuous main build](https://github.com/poisson-fish/OpenT1/releases/tag/continuous)
to try current development. Game data must come from your own installation.

| I want to… | Read |
| --- | --- |
| Install and play | [Getting started](Getting-started.md) |
| Create or adapt a game mod | [Game modding](Game-modding.md) |
| Write Tribes scripts | [Scripting reference](Scripting-reference.md), [examples](Scripting-examples.md), [native command index](Command-index.md) |
| Edit a mission or inspect assets | [Maps and assets](Maps-and-assets.md) |
| Host a dedicated server | [Server hosting](Server-hosting.md) |
| Change the C++ engine | [Source modding](Source-modding.md) |
| Diagnose a problem | [Compatibility and troubleshooting](Compatibility-and-troubleshooting.md) |
| Publish a build or maintain these pages | [Releasing](Releasing.md) |

The engine version and the compatibility target are separate: OpenT1 `0.1.0`
targets Tribes `1.11`. This wiki follows `main`; each binary archive includes the
documentation and original script examples from its own source revision.

Documentation sources live in
[`docs/wiki`](https://github.com/poisson-fish/OpenT1/tree/main/docs/wiki).
Submit corrections there so code and documentation can be reviewed together.
