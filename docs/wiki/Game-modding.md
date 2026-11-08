# Game modding

Game mods change scripts, datablocks, missions, GUI definitions and assets loaded
by the engine. [Source modding](Source-modding.md) changes the compiled C++ engine.
Start with game scripts when the existing commands and asset formats support the
behavior you need.

## Keep a mod separate

Create a directory alongside `base` in your supplied game installation. Give it
a distinctive name, keep it under your own version control, and avoid replacing
the original installation's files. A typical project may contain:

```text
Tribes/
  console.cs             installation bootstrap, supplied separately
  base/                  original game content
  ExampleMod/
    scripts/             your .cs files
    missions/            your missions and dependencies
```

This is an organization example, not an automatically discovered mod manifest.
The installation's `console.cs` and subsequent scripts determine the exact mod
startup convention, recursive search roots, script filenames and load order.
There is no OpenT1 `mod.json` or automatic entrypoint implied by these directories.

Launch an installation that supports the legacy `-mod` argument:

```powershell
.\opent1.exe --assets "C:\Games\Tribes" -mod ExampleMod
```

OpenT1 passes the legacy arguments to startup scripts. If `-mod` has no effect,
inspect that installation's bootstrap before changing engine code. A mod folder
alone does not execute every script inside it.

## Load and override deliberately

Add your script root to the installation's normal startup/search configuration,
then call `exec("score.cs");` from a suitable mod initialization script or console.
[`examples/scripts`](https://github.com/poisson-fish/OpenT1/tree/main/examples/scripts)
contains original scripts you can copy into that root. Confirm the root is active
and check console errors before depending on a function defined by another file.

Function/native command names share a case-insensitive registry. A later
definition replaces an earlier one. Use your own namespace, such as
`ExampleMod::award`, to reduce accidental collisions. Preserve an installation's
required callbacks and their argument/return contracts when overriding behavior.
An `exec` parse failure runs no part of that file; a runtime error can leave
mutations made earlier in the execution in place.

## Respect server and client roles

Authoritative gameplay belongs to the server. HUD/input belongs to the client.
Their script globals and object managers are distinct; assigning a `$` variable
does not replicate it. Use existing replication and validated remote commands
for data crossing the connection. A dedicated server has no local player's GUI.
Do not trust a client-supplied object ID, score, team or inventory change merely
because a remote callback received it.

`spawnPlayer(armor, position, rotation)` creates an unowned player body in
`MissionCleanup`; it does not connect a player or choose a client from server
preferences. Assign the returned body to an existing client with
`Client::setOwnedObject(clientId, body)` and `Client::setControlObject(clientId, body)`.
This also applies to hosted games: spawning another body must not take ownership
or control away from the local host.

V-menu animations use the armor's `animData[38..50]`, selected by
`remotePlayAnim(clientId, 0..12)`. They follow the vanilla animation priorities:
one-shot signals finish before equal-priority movement replaces them, while
`HoldPose` keeps the final pose until movement can interrupt it. The menu's
`playVoice(clientId, sound)` call sends a separate nearby voice event from the
speaker's owned body and honors the receiver's `$pref::playVoices` setting.

## Test and distribute

Start with a small reproducible script change. Test a local mission, a separate
client/server connection, and a dedicated server when your mod supports hosting.
Record the engine commit/version, installation, mission and mod revision. Check
interaction with existing callbacks and behavior after mission reload/reconnect.

Distribute your original/authorized mod files with a README explaining supported
engine versions, required content, installation, launch arguments and uninstall.
Keep user preferences, credentials, logs and saves out of the archive. OpenT1's
repository and binary packages do not redistribute installation game content.
See [scripting](Scripting-reference.md), [examples](Scripting-examples.md), and
[maps/assets](Maps-and-assets.md) for the next steps.
