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
