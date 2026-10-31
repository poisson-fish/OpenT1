# Runtime reference

Gameplay uses an external Tribes installation. The standalone executable is
`opent1.exe` on Windows and `opent1` on other platforms. The examples below use
the Windows Debug preset; substitute the appropriate build path on other
platforms.

## Launching

```powershell
.\build\x64-debug\opent1.exe --assets "C:\Games\Tribes"
```

The root should contain `base/` and the game VOL archives. Normal startup mounts
available core volumes and executes `console.cs`, which supplies the shell and
game scripts. A missing or failing required startup script stops initialization.
The small built-in demo can run independently of an installation:

```powershell
.\build\x64-debug\opent1.exe --smoke --headless --no-audio --no-assets --spawn-demo --frames 4
```

The built-in demo exercises simulation and rendering; it is not a full game
content replacement or an installation compatibility test.

## Data paths and startup

`--assets PATH` explicitly selects the installation and requests mounting.
`mountAssetRoot` also accepts its `base/` directory when it contains
`entities.vol`. Search directories include the root, `config/`, `base/`, and
the existing mission, font, skin, voice, and script subdirectories. Named core
and biome volumes are mounted in the order listed in `FearPlugin.cpp`.

Without an explicit path, discovery searches a starting directory and up to
six parents, checking each directory itself and its `TRIBES/` and `PCRPG/`
children. Detection looks for `base/entities.vol`, `base/Entities.vol`, or
`base/darkstar.vol`. The standalone SDL host starts this lookup beside the
executable; the fallback host starts in the process working directory.
Detection alone does not establish that an installation has all required files.

`--no-assets` suppresses automatic host mounting, even when mounting flags are
present. `--spawn-demo` selects the built-in session and suppresses automatic
`console.cs` execution; `--exec-console-cs` explicitly requests that script.
Mounted installations normally execute `console.cs` without needing the latter
flag. `--mission NAME_OR_FILE` requests a mission after startup.

Game command-line processing also exposes arguments through `$cargc` and
`$cargvN`. `+exec FILE` loads a startup script; when `console.cs` is active, that
script handles its startup command conventions. Other `+command` arguments
dispatch console commands, and a quoted `'$VariableName' VALUE` pair sets a
console variable. These operations have the same capabilities as the console.

Selecting an installation does not change the process working directory.
The standalone host uses an empty `FearGameApp::Config::writeRoot`: relative
configuration exports, recordings, and other output paths therefore resolve
from the working directory. Embedded hosts can set `writeRoot` for the game
plugins that use it; this is not a global filesystem redirect. In particular,
console logging and built-in exports use their own filesystem paths.

## Command-line options

| Option | Behavior |
| --- | --- |
| `--assets PATH` | Select and mount an installation root |
| `--mount-assets` | Request discovery and mounting without selecting a path |
| `--no-assets` | Suppress automatic installation mounting |
| `--exec-console-cs` | Request mounting and execution of `console.cs` |
| `--mission NAME_OR_FILE` | Request mounting and load the selected mission |
| `--spawn-demo` | Create the built-in test session |
| `--headless` | Suppress native window creation; execution-loop behavior depends on the host, as described below |
| `--frames N` | Limit execution to a positive number of frames; zero has host-dependent behavior |
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

Presentation options require a build with `OPENT1_BUILD_PLATFORM=ON`. An audio
runtime also requires `OPENT1_BUILD_AUDIO=ON`. The fallback loop does not create
a native window, GPU surface, or OpenAL playback runtime.

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
| Standalone executable built with SDL | Runs until quit, unless smoke mode or a positive frame limit bounds it | Keeps the SDL callback loop and omits the window |
| Standalone executable built without SDL | Runs the fallback loop for two frames, or a positive requested limit; smoke defaults to four | Uses the same bounded fallback loop |
| Embedded `runFearMain` with SDL | Defaults to two frames; `--sdl-loop` removes that default limit | Selects the bounded fallback loop |
| Embedded `runFearMain` without SDL | Uses the bounded fallback loop | Uses the same bounded fallback loop |

`--frames 0` removes the positive frame limit in the SDL loop, but the fallback
still runs two frames. All fallback frames use a 32 ms simulation step. SDL
smoke and positive-frame-limit runs also use 32 ms frame times; interactive SDL
runs use elapsed wall time. Consequently, the build without SDL is currently a
bounded execution/test host, not a continuous dedicated-server loop.

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
