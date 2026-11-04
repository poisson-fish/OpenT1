# Effect parity audit

Compared with `../TribesRE/legacy/tribes-rebirth` on November 3, 2026.
The installed-content checks use the external `../TribesRE/PCRPG` installation.
No installation scripts, models, textures or volumes are included in this change.

## Reported symptoms

In that RPG installation, `spells.cs::DoCastSpell` implements fireball (spell 14)
with `CreateAndDetBomb(..., "Bomb9", ...)`. `rpgstats.cs::RefreshScore` implements
forced-remort sparks by repeatedly creating `Bomb7` at the player's current
position. These sparks follow the character through repeated placement; they
are not a mounted item image. `Bomb9` selects `plasmaExp` (`plasmaex.dts`) and
`Bomb7` selects `energyExp` (`enex.dts`).

Both use the same path:

```text
CreateAndDetBomb -> newObject("", "Mine", datablock)
  -> scheduled Mine::Detonate -> server Destroyed state
  -> ghost state update -> ShapeBase::blowUp -> local Explosion
  -> WorldShapeQuery -> software / hardware rendering
```

## Repaired divergences

| Path | Legacy behavior | Missing OpenT1 behavior and repair |
| --- | --- | --- |
| `program/code/mine.cpp::processArguments` | Accepts one datablock argument and bypasses `Item` argument parsing. | `Mine` inherited `Item`'s three-to-five-argument requirement. Script bombs were deleted during construction. Restored the override. |
| `program/code/shapebase.cpp::blowUp` | Creates the selected explosion at the shape's box center, using its local Z axis. | OpenT1 only updated debris/hulk flags. Restored explosion creation with the originating datablock manager. This also repairs destruction effects for other `ShapeBase` subclasses. |
| `program/code/staticBase.cpp::initResources` | Reapplies the damage pose starting from state `None`. | Reapplying `Destroyed` could replay the newly restored explosion. Reset the previous state during resource initialization; live damage transitions still create the effect. |
| `program/code/FearCSDelegate.cpp` and `smokePuff.cpp` | Creates a client smoke manager and advances its puffs. | Client worlds never created or stepped a smoke manager. Added both. |
| `program/code/smokePuff.cpp::onSimRenderQueryImage` | Submits every active puff, facing the camera with its random spin. | The query returned only the first puff, with no shape/material resources; the manager also lacked render-set membership. Submit all active puffs with retained resources, authored root placement and camera-facing rotation. |

The repairs use existing datablocks, geometry, animations, materials and network
state. They do not introduce a new effect type or wire format.

### Hellstorm mine cleanup (November 4, 2026)

The restored mine construction exposed two further lifecycle omissions. Hellstorm
(spell 18) schedules 25 `Bomb9` mines and one `Bomb1`. Their datablocks use
`className = "Handgrenade"` and `description = "Handgrenade"`. The objects
survived destruction and still dispatched collision callbacks, falling back to
`Item::onCollision`. The RPG pickup script passed their unset `%this.delta` to
`Item::giveItem`, producing `You received  Handgrenade.` with an empty quantity.

Vanilla `Mine::onCollisionNotify` suppresses callbacks after destruction. Its
constructor also enables solid collision and `deleteOnDestroy`; vanilla
`StaticBase::setState` posts a deletion message 0.5 seconds after destruction,
allowing clients to receive the explosion state first. OpenT1 now restores these
mine defaults, the collision guard, and the delayed deletion event. StaticBase
also honors an explicit `deleteOnDestroy` object field.

The asset-free `mine_lifecycle` regression reproduced the destroyed collision
callback before the repair and checks live contacts, suppression after
destruction, and deletion at the half-second boundary. The installed effects
test runs the original Hellstorm `DoCastSpell` branch, observes all 26 mines and
their explosions, and verifies that every mine and client ghost is removed.

### Live spell mine callback dispatch (November 4, 2026)

The destruction guard did not cover a hostile caster's live spell effect touching
the player during its fuse. Legacy `program/code/gamebase.cpp` implements
`GameBase::getScriptDefaultClass` with `getClassName()`, so its `scriptName`
fallback for a `Mine` is `Mine`, regardless of its C++ parent `Item`. OpenT1's
`Mine` inherited `Item`'s hardcoded default namespace. With no datablock or
`Handgrenade::onCollision` handler, a live bomb therefore dispatched the RPG
pickup callback. The same mismatch bypassed the default `Mine::onDamage` handler.

The underlying port error was broader than mines: `GameBase` returned a literal
namespace and nine derived-class overrides returned other literals. Derived
classes without overrides inherited their parent's script identity. For example,
`Turret`, `Vehicle`, `Flier` and `Sensor` resolved to `StaticBase`, and several
projectiles resolved to `ShapeBase`. Adding a `Mine` override addressed one
instance but retained the structural mismatch.

`GameBase::getScriptDefaultClass` now looks up the object's exact dynamic type in
the host's existing persistent class registry, matching vanilla's `getClassName`.
`SimGame::setWorld` binds that instance-owned registry to both world managers;
script-created, native-created, loaded and ghost objects share the same lookup.
The redundant hardcoded overrides are removed. The three concrete base classes
exposed by existing factories are also registered. No spell IDs, effect datablock
names, message filters, or mod-script edits participate in the repair.

Named-object, datablock, datablock-class, concrete engine class and global
callbacks retain that order. C++ parent namespaces are not another fallback.
Ordinary items still dispatch their pickup callbacks.

Before the repair, the installed-effects regression reproduced the `Handgrenade`
message by casting fireball through `DoCastSpell` from a goblin AI, then moving
the player into the live `Bomb9`. It checks for no such message through the fuse,
client explosion and server cleanup. The asset-free `mine_script_callbacks`
regression covers live contacts without a mine collision handler, default mine
damage and collision handlers, named-object/datablock/class precedence, ordinary
item callbacks and the destroyed collision guard. The installed check discovers
the complete mine datablock group and verifies its collision dispatch, invoking
live contacts for effects without a collision handler. `runtime_script_callbacks`
checks 24 registered classes, including a new Item subclass with no namespace
override, plus parent-callback rejection, global fallback, client ghosts and
independent hosts with different names for the same C++ type. Earlier installed
checks used self casts or remote effects; the throw-source blackout and distance
hid this live-contact path.

### Review of the three preceding effect fixes

| Commit | Finding against vanilla | Why the pickup bug survived |
| --- | --- | --- |
| `4a3dd79` | Mine argument parsing, explosion construction and client smoke restore shared native paths. | Making mines constructible exposed incorrect callback dispatch; these checks established effect visibility, not hostile live contacts. |
| `c36ee70` | The unarmed beam origin is a generic fallback for an unsupported native case: vanilla assumes a mounted weapon and its console muzzle fallback reads an uninitialized transform. No spell IDs are used. | Beam rendering is separate from mine collision callbacks. This fallback is not evidence of exact vanilla rendering parity. |
| `18ff58e` | Destroyed-mine collision suppression, the half-second deletion delay, damage-state callbacks and grenade ray endpoints follow the corresponding native code. | Destruction guards cannot prevent a live mine from using the wrong script namespace during its fuse. |

The subsequent `3b1996d` loading/quantization change does not modify this callback
resolution path. The missing runtime identity, rather than an individual spell's
datablock or pickup text, caused this remaining symptom.

Validation for the runtime-identity repair (November 4):

- With the committed namespace behavior, the installed hostile-cast check emits
  the reported message and `mine_script_callbacks` observes an Item pickup.
- With registry-based dispatch, all seven effects cases pass in headless Debug
  and desktop Release, including all 19 installed mine datablocks (18 live
  contacts) and the 24-class runtime test.
- Release `opent1.exe` is rebuilt. Full CTest passes 11/12 headless suites and
  13/14 Release suites; the only failing suite is the separate AI behavior audit.
  Its latest Release result reports a movement precision difference
  (`attack_mode0_follows_route`: 0.999890 versus 1.0). The spell/gameplay cases pass.
- Local reproduction, build and test logs are under
  `build/effect-parity/runtime-callback-*`.

### Grenade behavior audit (November 4, 2026)

Hand grenades use `Mine` with the `Handgrenade` datablock and a scripted
two-second `Mine::Detonate` fuse. Launcher grenades use the separate `Grenade`
projectile class. The comparison found and repaired two additional divergences:

- **Mine blast damage:** OpenT1 polled for state changes inside
  `Mine::serverProcess`, missing destruction caused by `Mine::Detonate` between
  ticks. Restored vanilla's virtual `StaticBase::onDamageStateChange` dispatch,
  including the parent script callback before mine radius damage. A destroyed
  object without a visibility/hulk sequence also stops supplying collision
  images, matching vanilla and preventing the spent mine from blocking its own
  blast coverage rays.
- **Launcher collision rays:** `Grenade::advanceGrenade` constructed a `Box3F`
  from its ray endpoints. That constructor sorts each coordinate, changing the
  ray for descending and backward movement. Assign the start and end separately,
  as vanilla `Grenade::losStep` does.

The `mine_lifecycle` regression also reproduced the missing blast damage before
the repair. It checks immediate, single blast damage and callback ordering.
The installed test verifies a thrown `Handgrenade` stays live until its original
two-second fuse, displays its explosion, and removes the server object and ghost.

The asset-free `grenade_lifecycle` test covers forward and backward descending
throws, the reflected normal and tangential velocities scaled by both grenade
and surface elasticity, material selection for bounce sounds, gravity, bouncing
before arming, no airburst merely on reaching `liveTime`, contact detonation
after arming, `totalTime` expiry, one blast, client prediction and authoritative
explosion replication without duplicates, smoke removal, and half-second server
cleanup. Existing vanilla wire fixtures continue to cover grenade datablocks,
initial/bounce packets, aiming and truncated-packet rollback. These checks do not
establish frame-by-frame trajectory parity for every surface and network delay.

## Validation

`tests/game/effects_test.cpp` covers the mine constructor contract, one explosion
per destruction transition, correct placement/axis, no replay on resource reload
or initially destroyed ghosts, smoke render-set membership, multiple drawable
puffs, scene capture, billboarding, motion, expiry and snapshot resource lifetime.

The installed-content case runs the original `CreateAndDetBomb` function for
`Bomb9` and `Bomb7`, waits for server-to-client replication and verifies their
rendered effects. It also mounts `OrbOfLuminance0` in slot 2 and renders two
animated `smoke.dts` puffs. Checks include software pixels, projected hardware
batches and retained hardware batches.

```powershell
$env:OPENT1_TEST_ASSETS = (Resolve-Path ../TribesRE/PCRPG).Path
./build/x64-debug/tests/opent1_tests.exe --suite effects
```

The asset-free case also runs in the ordinary `game` suite. Debug and Release
CTest suites pass. A local offscreen GPU probe additionally verified visible
pixels on Vulkan and Direct3D 12 for both hardware paths:

| Effect | GPU visible pixels, each backend/path | Software visible pixels |
| --- | ---: | ---: |
| Fireball / plasma explosion | 807 | 815 |
| Remort / energy explosion | 566 | 490 |
| Mounted orb | 615 | 616 |
| First smoke puff | 3387 | 3283 |
| Second smoke puff | 3420 | 3303 |

Local probe source, readbacks and logs are under `build/effect-parity/`.
Pixel counts establish visibility; they are not comparisons against vanilla
framebuffer captures. The installed test exercises the shared effect function,
not the spell's skill/mana checks or the entire remort progression.

## Unarmed spell casting

The follow-up regression uses `BeginCastSpell` for Beam, Fireball and Icespike
on `rpgmap5`, with slot 0 unmounted. Fireball and Icespike create explosions at
the line-of-sight target; the installed scripts do not launch moving projectiles
for these spells. Both require a target within 80 units.

Beam uses `sniperLaser`. Its client rendering required
`Player::getAimedMuzzleTransform` to succeed, so an unarmed caster produced no
beam geometry. The client also left the initial beam start at the world origin
when it could not find a muzzle. The repair uses the animated `dummy hand` node
for the visual origin and the eye ray for the local caster's contact query.
Shapes without a hand node fall back to their box center. A remote caster keeps
the replicated endpoint. This avoids an eye-origin ribbon collapsing to zero
projected area in first person.

The native `projLaser.cpp::readjustEnd` also assumes a mounted weapon, and
`FearPlugin.cpp::c_getMuzzleTransform` leaves `total` uninitialized in its
unarmed branch. The hand origin is an explicit fallback for that unsupported
native edge case. Weapon-backed beams continue to use the aimed muzzle path.

The asset-free `unarmed_beams` regression checks initial placement, remote
contacts, local eye targeting, movement and expiry. The installed test exercises
all three original cast functions, verifies first-person Beam pixels and hardware
geometry, and compares scene pixels with and without each target-point explosion.

## Other confirmed parity gaps

This audit does not establish complete parity for every visual effect:

- **Destruction debris:** vanilla `ShapeBase::blowUp` also constructs a
  `PartDebrisCloud`. OpenT1 still omits that construction. Separately,
  `Debris::processQuery` omits render resources and standalone debris lacks
  render-set registration. Debris requires a separate repair and tests for
  shape-part selection and placement.
- **Mounted-image lights:** vanilla `Player::onSimLightQuery` calls
  `getImageLight` for constant, pulsing and weapon-fire lights. OpenT1 does not
  implement that player query. The mounted-orb check above verifies its model;
  its illumination and weapon muzzle lighting still need parity work.

Existing projectile trails, beam geometry, shields and decals use separate
render paths. Passing the standard regressions is not a frame-by-frame vanilla
comparison of those effects.
