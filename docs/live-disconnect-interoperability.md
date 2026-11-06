# Live disconnect and reconnect interoperability

This records the Windows investigation for #50. The reproducible harness is
`tools/run_disconnect_interop.py`; the [sanitized evidence](evidence/lifecycle-live-2026-11-05.json)
contains executable hashes, content manifests, per-case results, callback logs,
header-only control traces and observed timing. No game binaries or assets are
distributed with this evidence.

The engine behavior baseline is OpenT1 `a7a2e9b`, the persistent-client-world
change in #51. The subsequent handshake audit adds tests and clarifies the
existing transport behavior. Source expectations use TribesRebirth
`1105fd0890c19c13f816b91e51b9cf0658ffc63c`; runtime tests use the separately hashed
native executable, not a source build standing in for native 1.11.

## What passed

All four pairings were separate processes communicating over native UDP through
a loopback relay. Minimal scripts isolate transport/delegate lifecycle from
mission and GUI initialization. The final installed-content runs additionally
execute each installation's original bootstrap and mission scripts. Logging
wrappers call the original installed callback bodies; they do not replace their
gameplay work. Each run uses a separate writable copy of the assets and fresh
configuration, with synthetic player/profile fields and public heartbeats/IRC
autoconnection disabled.

| Client -> server | Orderly disconnect/reconnect | Server kick/drop | Client shutdown | Real timeout | Installed base mission + EndGame |
| --- | --- | --- | --- | --- | --- |
| Native -> native | 3 accepts / 3 departures | 3 accepts / 3 Dropped callbacks | Exit 0, one Disconnect / departure | Accepted -> TimedOut; one departure | Two complete joins and EndGame returns |
| Native -> OpenT1 | 3 / 3 | 3 / 3 | Same | Same | Two complete joins and EndGame returns |
| OpenT1 -> native | 3 / 3 | 3 / 3 | Same | Same | Two complete joins and EndGame returns |
| OpenT1 -> OpenT1 | 3 / 3 | 3 / 3 | Same | Same | Two complete joins and EndGame returns |

Every completed row preserves `LifecycleProbe` without a suffix, has one server
departure hook for each departed connection, and ends with an empty roster.
The base mission series starts with Raindance and lets the installed server's
normal last-client scripts choose subsequent mission reloads. Each pairing
records two completed ghost batches, two EndGame returns, and an absent final
server packet-stream object. The native and OpenT1 observer IDs are visible in
the departure callback log. These are lobby/observer joins; they do not certify
spawned-player combat behavior.

For the Release orderly samples, the first departure hook was observed roughly
18–43 ms after the relay received the outgoing Disconnect. Observation uses a
50 ms log poll, so these are observation delays, not precise server processing
latencies. They are far below the ordinary timeout. In the timeout cases the
relay first forwards the acceptance ACK, then blackholes both directions;
termination takes about 49–51 seconds of actual wall time. No orderly Disconnect
is substituted for a timeout.

The observations are deliberately separate:

- The relay records an outgoing Disconnect and its forwarding, without recording
  payloads. Forwarding is not itself proof of application-level server receipt.
- The server callback records a real departure, its count and the representative's
  name before the installed script's cleanup work.
- Installed probes subsequently observe the empty roster and that the peer's
  packet-stream object has retired. Reusing the endpoint and completing another
  join checks replacement connection progress.
- Transport retirement and owned-body/observer ordering also have focused
  protocol/native-UDP regressions in the standard suite. The native application's
  internal VC object is not directly instrumented by this CLI probe.

## Wasteland RPG and native limits

The installed Wasteland revision is identified by a complete base/mod tree hash
in the evidence. Both sides in each RPG attempt use a fresh copy of that same
installation and `rpgmap5`.

| RPG pairing | Observed result |
| --- | --- |
| OpenT1 -> OpenT1 | Two accepts, ghost batches, EndGame returns and departure hooks; final stream absent and roster empty |
| T1Vista native client -> OpenT1 | Same result, with exported native client/server counters as well as logs |
| Stock native dedicated server control | Did not reach readiness before a peer could be launched |
| T1Vista native dedicated server control | Still not ready after about 102 seconds; mission loading reports failed Moveable registrations |
| OpenT1 -> native RPG server | Unsupported in this environment because the native server control cannot finish initialization |

The failed server control is retained as a failure, not converted into a passing
mixed-peer result or attributed to OpenT1. Resolving that native installation's
RPG dedicated startup is outside a demonstrated OpenT1 disconnect regression.
The harness deliberately supports unique loose-script mod hook definitions and
rejects ambiguous definitions; it is not a certification tool for arbitrary mods.

## Pending joins and the manual UI check

The [handshake audit](handshake-cancellation.md) records cancellation at the
request/accept/ACK boundaries. OpenT1 peers promptly consume generation-validated
handshake Disconnect. A native server may instead retire an accepting VC on the
replacement Request; its successful replacement join must not be reported as
proof that the original cancellation was consumed.

The user operated the full installed T1Vista shell against both an OpenT1 and a
native server with acceptance withheld. Back reached the original installed
EndGame script and returned to a visible, usable join menu. Both cases logged
`disconnect: not connected` and sent **no Disconnect packet**. Requests continued
after returning to the menu. This installed native pending-cancellation path is
therefore explicitly unsupported; a receiver cannot promptly act on a control
packet the client did not send. The native control reproduces the same limit.
This runtime observation is distinct from the source-only
`cancelDuringConnection` implementation in the reference corpus.

OpenT1's own pending cancellation removes its retained local request, including
asynchronous lookup, and tests reject old Accept/Reject/Disconnect before ACK or
timeout state mutates. Delivery of Disconnect before an unknown peer's Request,
delayed Request generations and the one-bit generation limit of standalone
data/ACK packets remain the documented wire-contract boundaries.

## Reproduce the matrix

From the repository root on Windows, select existing runtimes and a new output
directory. These commands launch hidden test processes and use no UI automation:

```powershell
python tools/run_disconnect_interop.py `
  --opent1 build/x64-release/opent1.exe --vanilla C:/Games/Tribes `
  --output build/lifecycle-controls --scenarios orderly kick shutdown

python tools/run_disconnect_interop.py `
  --opent1 build/x64-release/opent1.exe --vanilla C:/Games/Tribes `
  --output build/lifecycle-timeouts --scenarios timeout --timeout 70

python tools/run_disconnect_interop.py `
  --opent1 build/x64-release/opent1.exe --vanilla C:/Games/Tribes `
  --installed-assets C:/Games/Tribes --output build/lifecycle-base `
  --scenarios orderly --timeout 100

python tools/run_disconnect_interop.py `
  --opent1 build/x64-release/opent1.exe --vanilla C:/Games/Tribes `
  --vanilla-executable C:/Games/Wasteland/T1Vista.exe `
  --installed-assets C:/Games/Wasteland --mod RPG --mission rpgmap5 `
  --output build/lifecycle-rpg --scenarios orderly --timeout 110
```

`--pairings` accepts client/server pairs `vv`, `vo`, `ov`, `oo` (`v` is the native
runtime). `--memory-dll` selects an existing compatible DLL explicitly; this run
used `mem.dll v0.04(87)` to avoid the installation DLL that loads membot. Both
DLL and executable hashes are always recorded. Do not silently substitute a
compatibility executable for the stock runtime in a reported native control.

Full mission probes retain original callback bodies, add counters exported to
an isolated state file, and also record console markers. Earlier native probe logs were incomplete, so exported counters provide an
independent observation of callback execution rather than relying on console
markers alone. Completion still requires the expected accepts,
departures, ghost batches, EndGame calls, names, final roster and retired stream.
Startup failures include their phase and process exit state and remain failures
in the summary. Keep copied executables/DLLs/assets and unsanitized process logs
out of source control.

## Scope still requiring acceptance

These runs establish the listed lifecycle behavior on one Windows machine with
loopback UDP. They do not establish internet/firewall behavior, another OS's
native process interoperability, GPU/audio acceptance, all mods, or keyboard/
cursor/dialog usability after every loading, kick and timeout scenario.
The user-operated native Back/menu check and the installed OpenT1 base/RPG
button/input/render regressions in #51 are additional, narrower UI evidence.
Hosted direct loopback ownership is covered separately by those tests; it is
not a remote transport pairing. Do not turn the passing matrix rows into a claim
of complete vanilla lifecycle parity.
