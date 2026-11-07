# Persistent client connection lifecycle

The ownership reference is TribesRebirth `1105fd0890c19c13f816b91e51b9cf0658ffc63c`,
`darkstar/Sim/code/netCSDelegate.cpp` (`CSDelegate::simDisconnect`) and
`program/code/fearCSDelegate.cpp` (`simDisconnect`, `onConnectionDropped`,
`cancelDuringConnection`). These are source expectations, not observations of a
live vanilla peer.

Vanilla removes the server packet stream and its clients, clears the client
player/network state, and keeps the client manager alive. Its EndGame scripts
choose the next menu and shut down a locally hosted server separately.

OpenT1 now follows that ownership boundary. `disconnect()` cancels pending DNS
or a pending transport request, or disconnects the active stream. It preserves
the client world, input manager, console scheduler, IRC client, canvas objects,
and startup resource mounts. It does not call `deleteServer()`.

Connection teardown removes stream clients, the player controller, replicated
objects, MissionGroup/MissionCleanup, loose gameplay objects/effects, and
mission resource mounts. It clears player lists, datablocks/completion callbacks,
paths, sensor names, decals, smoke, queued movement, and connection flags. A
fresh local controller and ghost service make the same manager ready to join
again. Controller/ghost identities are connection state and are not preserved.

Locally queued events retain the target's object ID and generation and use the
owning manager independent of its temporary network ID. Deleting a mission
object removes its queued events; shell scheduler events survive. Global
script schedules intentionally remain script-owned, as in vanilla. Scripts
should bind mission work to a mission object when it must end with that object.

| Transition | Engine ownership and script boundary | Regression evidence |
| --- | --- | --- |
| Shell -> connecting | Keep shell; retain cancellable lookup/request | Pending delegate/DNS and native UDP cancellation tests |
| Connecting -> loading | Attach stream/controller/ghost clients; set network ID | Remote acceptance and installed joins |
| Loading -> play/lobby | Receive datablocks, mission ghosts and control | Base Raindance / Wasteland RPG rpgmap5 installed checks |
| Direct disconnect / cancel -> shell | Retire connection and mission state; preserve shell | Three immediate UDP reconnects, one player/name and one callback per departure |
| Remote script -> another server | Retire the old stream and its queued events before joining; do not report intentional stream removal as a packet failure | Repeated cross-port and same-port UDP transfers from scheduled and immediate remote callbacks; installed SinConnect/JoinGame transfers between local RPG servers |
| UI EndGame -> shell | Disconnect first; scripts select menus and delete hosted server | Two real JoinGame/Disconnect-button cycles for each installed base/RPG configuration |
| Rejection / remote kick | Reset connection before script notification; preserve shell | Password rejection and kick regressions, shell identity assertions |
| Remote timeout | Same reset path as remote drop | Transport timeout and departure tests; full UI timeout acceptance remains separate |
| Loopback disconnect / local kick | Remove local representative/body/observer once; preserve both worlds | Repeated direct loopback joins, body cleanup and local script kick regressions |
| Explicit deleteClient / quitting | Destroy client world and its shell services | Existing world deletion, shutdown and input lifetime regressions |

The installed UI checks exercise rendering, input-adapter release/capture,
delegate routing, resource availability and a usable join menu on the same
manager. Synthetic checks assert world revision and input/scheduler/IRC/canvas/
volume identity, surviving shell callbacks and cancelled mission callbacks.
Keeping the IRC object is tested; a live IRC connection is not part of these
tests. GPU devices, internet hosts, arbitrary mods and native vanilla peers are
separate acceptance dimensions. Do not infer full vanilla parity from these
OpenT1 peer tests.

## Portal regression and executable provenance

The November 6, 2026 investigation reproduced this sequence with the executable
at the normal Windows Release path, reporting commit
`010687f79f2be91861263dbeb5952d8df2b83f05`:

```text
first connection: Accepted
scheduled disconnect(): False
connect(second endpoint): False / Could not connect
connect(original endpoint): False / Could not connect
```

That revision refuses `disconnect()` while a simulation manager is dispatching.
The installed SinConnect portal schedules its transfer, so it hits that guard,
leaves the first session alive, and then fails the delegate's single-connection
check. The error is generated locally, before a new server request is sent.

The local CMake cache redirected newer Release executables into `score-fix-bin`,
leaving the launched executable unchanged. The newer executable reporting
`1a2273b8bf6c2d42050619d35be019a992ef97f2` passed the identical two-server scheduled
round trip. Restoring the standard output path and rebuilding is necessary to
actually deliver the persistent-client lifecycle fix. Windows presets now clear
the cached output-directory override.

The expanded regression also found a distinct current-code failure for an
immediate transfer inside a received remote event. Removing the old stream makes
`readPacket` stop; the delegate incorrectly converted that intentional removal
into `Disconnected`, whose game callback reset the replacement connection.
The delegate now stops processing the retired stream without reporting a packet
failure. Local stream/pending-request disposal also cancels queued events for
that connection after both successful and failed disconnect sends. The transport
wire protocol is unchanged.

Final Windows validation: all 17 CTest suites passed in both Debug and Release;
the Release installed `disconnect_assets` suite passed both portal transfers and
base/RPG UI disconnect/rejoin cases against Wasteworld assets. Portal coverage
includes four server-triggered transfers (including a same-port rejoin), a
destination rejection followed by recovery in both synthetic callback modes,
and a final manual reconnect. Installed checks use two local `rpgmap5` servers
and the actual `SinConnect.cs` callback, with temporary write directories.

The initial attempt to host local `Mapping` stopped at initialization of
`cchurchchurch1` (`cchurchchurch.dis`). That is a separate unresolved content/load
failure, not evidence of a transport rejection. These tests do not establish
public Iceworld gameplay or teleporter acceptance.
