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
