# Runtime reference

Gameplay uses an external Tribes installation. The standalone executable is
`opent1.exe` on Windows and `opent1` on other platforms. The examples below use
the Windows Debug preset; substitute the appropriate build path on other
platforms.

## Launching

```powershell
.\build\x64-debug\opent1.exe --assets "C:\Games\Tribes"
```

The root should contain `base/`, the game VOL archives, and `console.cs`.
Normal startup reads that physical bootstrap file; its scripts select the mods,
search directories, archive mounts, shell, and game initialization. A missing or failing required startup script stops initialization.
The small built-in demo can run independently of an installation:

```powershell
.\build\x64-debug\opent1.exe --smoke --headless --no-audio --no-assets --spawn-demo --frames 4
```

The built-in demo exercises simulation and rendering; it is not a full game
content replacement or an installation compatibility test.

### Dedicated servers

Use the normal SDL build in dedicated mode to run a continuous server without
a game window or audio playback:

```powershell
.\build\x64-release\opent1.exe --assets "C:\Games\Tribes" -mod RPG -dedicated rpgmap5 --headless --no-audio
```

The installation's `console.cs` handles `-mod` and `-dedicated`. Put the mission
name immediately after `-dedicated`; a following option can otherwise be
interpreted as the mission name. Dedicated startup creates a server world
without a local player. After startup scripts load the server preferences,
OpenT1 binds the final `$Server::Port` and fails initialization if it cannot
bind that port. Server preferences also control the hostname, player limit,
and public-hosting setting. Headless and dedicated launches without a window
initialize SDL's event subsystem only, so they require neither a GPU nor a
display server. SDL and OpenAL runtime libraries are still part of the normal
binary distribution; `--no-audio` disables playback-device initialization.

## Data paths and startup

`--assets PATH` explicitly selects the installation and requests mounting.
`mountAssetRoot` also accepts its `base/` directory when it contains
`entities.vol`. `$ConsoleWorld::DefaultSearchPath` controls resource discovery;
assigning it rescans the selected directories relative to the installation.
`console.cs` normally selects config, active mods, base, recordings and temp.
Each selected root is recursive, with additional roots supplying the legacy
bare-name aliases. Missing directories are skipped, and path components match
Windows spelling on case-sensitive hosts. The installation root and inactive
mods are not implicit resource roots. `SimVolume` objects mount archives in
script order and release them when removed. Mission scripts execute through
this same namespace; their declarations initialize in declaration order.

Without an explicit path, discovery searches a starting directory and up to
six parents, checking each directory itself and its `TRIBES/` and `PCRPG/`
children. Detection looks for `base/entities.vol`, `base/Entities.vol`, or
`base/darkstar.vol`. The standalone host starts this lookup beside the
executable; embedded callers start in the process working directory.
Detection alone does not establish that an installation has all required files.

`--no-assets` suppresses automatic host mounting, even when mounting flags are
present. `--spawn-demo` selects the built-in session and suppresses automatic
`console.cs` execution; `--exec-console-cs` explicitly requests that script.
Mounted installations normally execute `console.cs` without needing the latter
flag. `--mission NAME_OR_FILE` requests a mission after startup.
In a visible standalone SDL window, it presents the selected mod's loading
screen before starting the blocking mission load.

Game command-line processing also exposes arguments through `$cargc` and
`$cargvN`. `+exec FILE` loads a startup script; when `console.cs` is active, that
script handles its startup command conventions. Other `+command` arguments
dispatch console commands, and a quoted `'$VariableName' VALUE` pair sets a
console variable. These operations have the same capabilities as the console.

Selecting an installation anchors physical file commands, exports, recordings,
and console logging to that installation without changing the process working
directory. Embedded hosts may set `FearGameApp::Config::writeRoot` to redirect
those outputs. Corresponding selected directories under that write root overlay
the installation directories, allowing saved configuration and temporary scripts
to be read after the next search-path refresh. Without an installation, relative
paths retain their normal working-directory behavior.

`isFile` opens one or more physical files at the filesystem anchor, independently
of world focus. Archive entries count only as resources. Resource enumeration
preserves source spelling, backslashes and duplicate virtual names; it follows
resource creation order rather than sorting names. Repeated mounts of the same
archive share one reference-counted entry set.

## Command-line options

| Option | Behavior |
| --- | --- |
| `--assets PATH` | Select and mount an installation root |
| `--mount-assets` | Request discovery and mounting without selecting a path |
| `--no-assets` | Suppress automatic installation mounting |
| `--exec-console-cs` | Request mounting and execution of `console.cs` |
| `--mission NAME_OR_FILE` | Request mounting and load the selected mission |
| `--spawn-demo` | Create the built-in test session |
| `--headless` | Skip native window, video-driver, gamepad, and GPU-device initialization while keeping the normal execution loop |
| `--frames N` | Limit execution to a positive number of frames; zero runs until quit |
| `--smoke` | Select hidden presentation and loopback audio, default to four frames, and check that simulation renders nonzero output |
| `--hidden` | Create the SDL window hidden |
| `--renderer software` or `--renderer hardware` | Select the rendering path in the SDL host; explicitly requested hardware fails initialization if no GPU surface is available |
| `--driver NAME` | Request an SDL_GPU backend, such as `vulkan`, `direct3d12`, or `metal`, subject to platform availability |
| `--fullscreen` or `--windowed` | Override the fullscreen preference in the SDL host |
| `--no-audio` | Disable the SDL host's OpenAL runtime |
| `--audio-loopback` | Use OpenAL loopback rendering instead of a playback device |
| `--render-stats` | Print per-frame rendering timings and work counters in the SDL host |
| `--verify-input` | Inject a short SDL input sequence and verify player movement/camera response; requires a controllable player and enough frames |
| `--sdl-loop` | Keep an embedded SDL invocation running without its default frame limit; also permits window creation in dedicated mode |
| `-dedicated` | Set dedicated-server mode; the SDL host normally omits the window |

All builds include SDL3 presentation and OpenAL audio support. The runtime
switches select which services to initialize. `--headless --no-audio` starts
without a native window, video driver, GPU device, or audio playback device.

## Rendering and execution loops

The SDL host first attempts GPU presentation and otherwise keeps a software
surface. GPU availability and the video-driver preferences determine the actual
path unless `--renderer` explicitly selects it. Software frames can still be
presented through SDL_GPU. Running without a window does not exercise an
interactive GPU backend.

```powershell
.\build\x64-debug\opent1.exe --assets "C:\Games\Tribes" --renderer software --windowed
.\build\x64-debug\opent1.exe --assets "C:\Games\Tribes" --renderer hardware --driver direct3d12
```

| Host | Default loop | `--headless` behavior |
| --- | --- | --- |
| Standalone executable | Runs until quit, unless smoke mode or a positive frame limit bounds it | Keeps the SDL callback loop and omits video initialization and the window |
| Embedded `runFearMain` | Defaults to two frames; `--sdl-loop` removes that default limit | Uses the same execution loop without video initialization or a window |

`--frames 0` removes the frame limit. Smoke and positive-frame-limit runs use
32 ms frame times; continuous runs use elapsed wall time. There is one runtime
implementation for windowed, headless, and dedicated execution.

Player and bot shadows are enabled by default in both renderers, including the
local player's shadow in first person. Set `$pref::playerShadows = "FALSE";` to
disable them. This preference is independent of the legacy shadow detail mask
and scale. Animated silhouettes project onto terrain and interior surfaces using
the mission sun, with a downward fallback when no suitable sun is available.
Up to 32 nearby shadows are rendered per frame.

Hardware fog is applied to each material before transparency blending. Terrain
fog therefore stays behind foreground tree leaves, while cutout holes preserve
the background's color and depth. Translucent shapes receive their own haze;
fully fogged additive, subtractive and multiply effects fade to their blend's
neutral color. The indexed software renderer retains its native palette rules.

## Network and console services

Game sessions use UDP, with optional native IPX routing when the Windows OS
provides it. `$Server::Port` supplies the game server port when installation
startup is available; otherwise server setup requests an ephemeral port.
Discovery, game traffic, IRC, and the TCP console are distinct services.
See [simulation networking](simulation-networking.md) for connection behavior,
packet limits, and replication.

`SimTelnetPlugin` exposes a line-oriented TCP console driven by frame updates.
It starts without a listener and uses `127.0.0.1` as its default bind address.
Setting `$TelnetPort` to a positive port starts a listener; zero or `-1`
stops it. `$TelnetBindAddress` selects another interface explicitly.
Authentication requires a nonempty `$TelnetPassword`. Authenticated commands
are evaluated by the host console. The service does not encrypt traffic;
password authentication does not add transport encryption.

## Diagnostics

Startup failures and explicit hardware-renderer failures report to stderr.
The console exposes `$OpenT1::StartupError`, `$OpenT1::LastMissionError`,
`$OpenT1::LastMissionEvalError`, `$OpenT1::LastNetworkError`,
`$OpenT1::LastGuiError`, and `$OpenT1::LastRenderError` for their corresponding
paths. Their values describe the last recorded result and are not a replacement
for checking a host API's return value.

`--render-stats` prints `FRAME` records with renderer selection, framebuffer/UI
sizes, timing, terrain/lighting work, and hardware submission counters where
available. Console file logging is controlled separately by
`$Console::logMode`; see [script output](script-runtime.md#output-and-filesystem-access).

Hardware records also expose cumulative `gpuUploads`, `gpuReuses`, and
`gpuEvictions`, plus current `gpuTextures` and `gpuTextureBytes`. Uploads include
updates into recycled dynamic-overlay allocations; subtract `gpuReuses` from
`gpuUploads` to count successful uploads that required new texture allocations.
Compare counter differences across a warmed-up capture rather than cumulative
startup totals. The `total` timing covers rendering, GPU submission, and
presentation; simulation and audio updates occur outside that interval.

The hardware texture cache retains up to 8192 distinct content entries by
default, growing when necessary to fit the current frame. Set the startup
preference `$pref::GPUTextureCacheEntries` to a positive count to tune retention
for a device; embedders use `GpuSurfaceConfig::textureCacheEntries`. The separate
64 MiB limit counts RGBA payload, including mipmaps, rather than actual GPU
allocation cost or the shared CPU snapshots. Driver alignment, descriptors and
metadata consume additional memory. In particular, the bundled SDL D3D12 backend
creates committed textures with 64 KiB alignment, even for tiny lightmaps.
Use warmed-up upload/eviction deltas and device memory measurements together;
raising the entry count does not reserve that many textures up front.
