# Compatibility and troubleshooting

OpenT1 targets Starsiege: Tribes 1.11 gameplay, asset formats, scripting and wire
behavior. That target is not a blanket claim that every mod, original executable,
platform or graphics driver has been verified. Record compatibility against a
specific engine commit, installation/mod revision and test scenario.

## Current validation boundaries

| Area | Automated coverage | Additional acceptance needed |
| --- | --- | --- |
| Platforms | Debug/Release on Windows x64, Linux x64 and macOS ARM64 | Other OS versions/architectures and clean end-user machines |
| Packages | Extract, inspect dependencies, run version/headless/dedicated smoke | Native window, real GPU, audio device and input |
| Scripts/simulation | Regression fixtures and original script examples | Your actual installation/mod and full mission lifecycle |
| Networking | Protocol regressions and recorded Windows native/OpenT1 lifecycle matrix | Separate machines, internet hosting, unverified mods/platforms and the documented native pending-cancellation limit |
| Editor | Native/editor regression coverage; optional installed acceptance documented in the repo | Your installation's editor scripts, resources and save/reload workflow |

Historical installed-content audits in `docs/` describe particular validation
runs; they do not certify every later release. Numbered release notes should state
which manual acceptance checks were actually performed and list unresolved issues.

The client shell manager, input service, scheduler, IRC client, canvases and startup
mounts persist across connection teardown. Mission objects and connection clients
are removed before rejoining. A direct loopback `disconnect()` leaves the server
alive; normal EndGame scripts decide when to call `deleteServer()`. The repository
[client lifecycle audit](https://github.com/poisson-fish/OpenT1/blob/main/docs/client-lifecycle.md) distinguishes tested behavior from
source comparisons and live vanilla acceptance.

OpenT1 intentionally handles validated Disconnect during the handshake as well
as after connection. Vanilla can require acknowledgment of Accept first. The
[handshake audit](https://github.com/poisson-fish/OpenT1/blob/main/docs/handshake-cancellation.md) records native observations and generation/order limits;
a successful reconnect alone does not prove that the original cancellation was
consumed.

The [live lifecycle matrix](https://github.com/poisson-fish/OpenT1/blob/main/docs/live-disconnect-interoperability.md) records both native/OpenT1 directions, base mission
rejoins, kicks, shutdown and timeout. It also records the unsupported native RPG
server startup and the native pending Back action that returns to a menu without
sending Disconnect. Those limits remain explicit.

## Common startup problems

- **Missing bootstrap or volumes:** point `--assets` at the supplied installation
  root containing `console.cs`, `base/` and the VOL files. A package alone is not
  the complete game.
- **Mod does not load:** inspect that installation's `console.cs`, `-mod` handling,
  resource roots and script initialization order. Merely creating a folder does
  not execute its `.cs` files.
- **Works on Windows, fails elsewhere:** check filename case, path assumptions,
  startup preferences and availability of the selected resources.
- **No window or GPU failure:** check `--headless`/`-dedicated` options, then try
  `--windowed --renderer software`. A headless smoke pass does not test your GPU.
- **Missing runtime library:** extract the whole binary archive, preserving its
  directory layout. Linux packages require a compatible OS runtime. macOS packages
  bundle Homebrew C++ libraries; Homebrew itself should not be required to play.
- **Hosting bind failure:** inspect `$Server::Port`, conflicting processes and
  interface configuration. Test firewall/router reachability separately.
- **Unknown script function:** verify its defining file loaded successfully and
  the required plugin/world exists before the call.
- **A rebuilt Windows client still shows an old connection bug:** run `--version`
  on the exact executable being launched. A cached
  `CMAKE_RUNTIME_OUTPUT_DIRECTORY` override can redirect builds while leaving an
  old `build/x64-release/opent1.exe` behind. Reconfigure with the Windows preset,
  close the running client normally, and rebuild the documented executable.
  The presets clear that cached override. Server transfers are covered by the
  [client lifecycle audit](https://github.com/poisson-fish/OpenT1/blob/main/docs/client-lifecycle.md).

## Collect useful evidence

Run `opent1 --version` (`.\opent1.exe --version` on Windows), and save the package's
`release.json`. Record OS/architecture, GPU/driver, renderer, mod/version, mission,
launch arguments, reproduction steps, expected behavior and observed behavior.
Try reproducing with base content when practical, and distinguish a regression
from behavior never verified on that installation.

Startup failures report to stderr. Headless/dedicated script logs stream to stdout.
`$Console::logMode = 1;` enables file logging to `console.log` under the console's
file root (normally the working directory). Last-result variables include
`$OpenT1::StartupError`, `$OpenT1::LastMissionError`, `$OpenT1::LastMissionEvalError`,
`$OpenT1::LastNetworkError`, `$OpenT1::LastGuiError` and `$OpenT1::LastRenderError`.
`--render-stats` provides frame/render diagnostics.

Use the [bug report form](https://github.com/poisson-fish/OpenT1/issues/new/choose).
Remove passwords, private server addresses and personal paths as appropriate
before sharing logs. Reference the exact continuous commit rather than just
"latest", and include a minimal original script when a script triggers the issue.
