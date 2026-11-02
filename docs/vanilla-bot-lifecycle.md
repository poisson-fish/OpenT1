# Vanilla bot representatives and player connections

Compared against `../TribesRE/legacy/tribes-rebirth/program/code` on 2026-11-02.
This audit covers bot identity, ownership, replication, roster presentation, and
player connection handling. It does not claim parity for every AI behavior.
Vanilla source defines the behavior. Installed mods are acceptance inputs, not
grounds for engine exceptions, and their scripts/assets are not changed here.

Vanilla shares one representative ID pool between AI and players. It does not
reserve ID 2049 for a local player or allocate a separate range for NPCs.
`aiObj.cpp: AIObj::onAdd` obtains `PlayerManager::getFreeId`, registers a
representative with the AI argument, then assigns the player's owner ID.
`fearCSDelegate.cpp: FearCSDelegate::onConnectionRequested` obtains an ID from
the same pool before creating the real player's packet stream.

| Path | Vanilla behavior | OpenT1 correction |
| --- | --- | --- |
| Representative registration | `PlayerManager::clientAdded` uses its AI argument to classify the representative before linking it. Only real clients get connection callbacks, datablocks, a PSC, and an observer camera. | AI registration uses a common record initializer with the AI type already set; it does not call the real-client registration route. |
| Lists and lookup | `clientLink`/`findClient` contain real clients. `baseRepLink`/`findBaseRep` include players and AI. The two lists have independent links. | Separate list links and strict `findClient`; ownership and AI consumers use `findBaseRep`. |
| ID allocation | AI and connections consume the same free list. | Local and remote joins use the representative allocator. Removed the host-ID reservation and the fixed local ID. |
| Roster replication | `PlayerAddEvent` carries `isBaseRepType`; AI receives no `onClientJoin` callback or connected announcement. Team events do call `onClientChangeTeam` for either kind of representative. | Preserve the type on the wire and during local replication; late AI additions, team changes, and removals reach real clients. Preserve the vanilla team callback. |
| Removal | `clientDropped` removes either representative, but the player disconnect path and announcements apply only to real clients. | AI removal releases its record and broadcasts removal without player callbacks. Reused IDs retain the new representative type. |
| Score lists, browser details, ping lists | Enumerate the real-client list. | These lists exclude AI structurally, including browser player counts and details. |
| Ownership and command targets | Use base representatives, allowing AI bodies and targets. `GameBase::setOwnerClient` maintains both sides of ownership and detaches a representative's previous body. | Corrected script player resolution, owner links, commander targets, and AI target/follow lookup. The script setter delegates ownership bookkeeping to `GameBase`. |
| Identity reuse | `loginId` identifies a particular representative incarnation independently of its type. Projectiles credit only real clients; items capture only the control client. | Separate type flags and incarnation IDs. Match the distinct vanilla projectile and item attribution paths, including no native credit for an owner-only AI body. |
| Skins and teams | `setClientSkin` finds only real clients. Team changes set the skin of either kind of representative. Received skin events find base representatives. | Keep these paths separate, replicate team skins, and refresh owned shapes on received skin/team events. |

The following references were checked independently in the original source,
under `../TribesRE/legacy/tribes-rebirth/program/code` (line numbers refer to that
tree, not OpenT1):

- `PlayerManager.cpp:1010`: `setGenericDefaults` classifies AI before linking;
  `clientAdded` at 1217 initializes the shared record and only gives real clients
  connection services. `removeClient` at 1509 and `clientDropped` at 1542 unlink
  both kinds and release the slot. `findClient` at 2424 checks the type, while
  `findBaseRep` at 2437 does not.
- `aiObj.cpp:1673`: `onAdd` uses `getFreeId`, registers the AI, and sets its
  body's owner. `onRemove` at 1696 drops the representative. `aiHelp.cpp:20`
  resolves target/follow IDs through `findBaseRep(id)->ownedObject`, with no AI
  manager search or fallback to a PSC's control object.
- `FearPlugin.cpp:710`: client enumeration uses the client list; the ownership
  setter at 750 delegates to `GameBase::setOwnerClient` (`gameBase.cpp:740`).
  `fearCSDelegate.cpp:300` selects the connection ID from `getFreeId`; its browser
  count at 634 uses `getNumClients`. `FearGuiScoreList.cpp:124` and
  `PlayerManager.cpp:1315` enumerate only clients for scores and pings.
- `PlayerManager.cpp:2256` and 2279 restrict join/drop callbacks and announcements
  to clients. In contrast, the team event at 2005 resolves a **base**
  representative and calls `onClientChangeTeam` unconditionally when found.
  Valid team changes also produce the native team message. The earlier version
  of this document incorrectly claimed AI team callbacks were silent.
- `item.cpp:706` checks the thrower's base representative and login ID.
  `throwObject` at 283 captures only `getControlClient`, without an owner fallback.
  `projectile.cpp:1009` checks the login ID but looks up only clients, even though
  its launch path at 434 records a base representative.
- `PlayerManager.cpp:1090` updates team skins for both kinds of representative,
  rejects invalid teams, ignores unchanged teams, refreshes client GUI state,
  and retains current commands. The client team event at 2005 updates roster
  state and skins independently of the server setter. `setClientSkin` at 1381
  only finds real clients; the received skin event at 2061 finds base reps.

History audit of main (no history edits):

| Commit | Bot-related hunk and disposition |
| --- | --- |
| `e9bf951` | The initial port merged the two lists, treated `loginStatus` as client/AI state, used an AI-only fallback ID counter, filtered client consumers individually, and resolved AI targets through several competing paths. Removed the remaining redundant client filters, AI/PSC target and follow fallbacks, fallback counter, redundant roster lookups/defaults, and script ownership bookkeeping. Also remove the non-vanilla item last-owner attribution fallback and team-change command resets. Restore vanilla team/skin control flow. Keep the actual AI manager lookups used to execute AI commands. |
| `e250f60` | Keep hosted mission-drop/PSC control handshakes and sensor initialization. These bridge the modern loopback transport and remain necessary with separate lists. Replace the fixed host ID through the existing separation correction. |
| `aa80529` | Keep the server-connect then client-profile callback order, observer/PSC header synchronization, deferred local kick, and native gender spelling. These are independent connection/script fixes. The retired direct ghost-control assignment already yielded to PSC headers here. |
| `38412ce` | The decrement after receiving an AI roster event compensated for registering it as a client; classification before linking makes that adjustment obsolete. Roster type propagation remains required, using the actual type flag. Keep scripted weapon callbacks, eye-based melee, AI targeting, and combat behavior. |
| `184f40c` | Keep message routing/sound-only chat handling and thrown-item collision, throw-source blackout, and attribution lifetime behavior. They do not depend on merged representative lists. |
| `4ff723f` | Remove the scoreboard's `loginStatus` filter; the client list now supplies precisely its rows. Remove the expansion of `setClientSkin` to AI. NPC skins follow vanilla team changes and received base-representative events. Keep client-ID damage callbacks and stationary item/NPC handling; these match separate vanilla ownership/rendering/script paths. |
| `6ad5ea7` | Keep single-frame rendering and GPU cache retention. No bot roster or allocation workaround appears in these rendering hunks. |

The audit used path-specific `git log main`, `git show` hunks, and `git blame
main`. The representative/list/allocation correction was already in the working
tree and is preserved. The cleanup does not change resource loading, renderer
policy, player movement, or unrelated work.

No mod compatibility exception is retained in these contracts. The vanilla
distinctions above apply even where projectile and item paths differ. Installed
mods exercise the engine; failures must be traced to the relevant engine layer
rather than changing these contracts to satisfy a mod.

Regression coverage includes startup bots before local and remote joins, roster
type flags, real-client-only counts and lists, silent AI connection join/removal
events, vanilla AI team callbacks, ID reuse, NPC and player name collisions, a
local kick after a bot takes the first ID, rehosting with a changed ID, remote
AI/player/AI slot reuse, shared pool exhaustion, deferred bot-body ownership,
native/script ownership transfers, refusal to target/follow an unowned AI body,
commander commands and PSC target data, native client-only projectile attribution,
control-only thrown-item attribution, incarnation checks across ID reuse, ignored
AI skin setters, replicated AI team skins, received skin events, invalid teams,
and command preservation across team changes. Installed `rpgmap1` coverage checks
software geometry, hardware batch generation, observer control and character
creation. The installed RPG suites also exercise AI combat,
loot, skins, and movement through the affected ownership and lookup paths.
These checks do not prove visual pixel parity or interoperability with a running
original executable; no such parity claim is made.

Validation on 2026-11-02 after removing the mod compatibility exceptions: the x64
Debug build and all 13 standard CTest checks passed (50 C++ cases plus the runtime
smoke check). Both installed-content
suites passed against `../TribesRE/PCRPG`: three `trpg` cases, including
`installed_rpgmap1_rendering`, and three `fixpack` cases. Logs are under
`build/bot-vanilla-*`. Synthetic fixtures explicitly create their teams instead
of relying on the port's former acceptance of invalid team IDs. An off-screen
D3D12 run also rendered 16 installed `rpgmap1` frames on the RTX 5090 with both
client/server control present and 2,150 scene polygons on the final frame
(`build/bot-vanilla-rpgmap1-gpu.jsonl`). Release,
headless and original-binary interoperability were not rerun for this cleanup.
