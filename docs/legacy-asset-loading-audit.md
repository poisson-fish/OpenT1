# Tribes asset loading and PCRPG audit

The supplied PCRPG installation selects its RPG scripts correctly when launched
with `-mod RPG`. Its client HUD installation is incomplete. Tribes also retains
mounted archives when a server changes the search path, which can leave vanilla
scripts active alongside loose RPG files. These are separate failure mechanisms.

This audit covers the files present on November 1, 2026 in `../TribesRE/PCRPG`,
the comparison installation `../TribesRE/TRIBES`, and
`../TribesRE/legacy/tribes-rebirth`. The original executables were not run;
source behavior and current file contents cannot establish exactly what happened
in an earlier installation. The comparison installation contains community mods
and is not a pristine vanilla control.

## Original loading rules

`FearMain::executeConsoleCS` opens `console.cs` directly from the process working
directory. The working directory therefore matters even if the executable lives
elsewhere. Resource lookup begins after this physical bootstrap script assigns
`$ConsoleWorld::DefaultSearchPath`.

Both supplied PCRPG shortcuts specify `-mod RPG` and the PCRPG working directory.
Their targets currently exist, although one targets an executable in the Windows
Installer cache and the other targets `T1Vista.exe`.

For a single `-mod RPG`, `EvalSearchPath` builds this ordered path:

```text
config;
RPG;RPG\missions;RPG\fonts;RPG\skins;RPG\voices;RPG\scripts;
base;base\missions;base\fonts;base\skins;base\voices;base\scripts;
recordings;temp
```

Each root is scanned recursively. Names are relative to that root: adding `RPG`
exposes `RPG\connectivity.cs` as `connectivity.cs`; adding `RPG\scripts` also
exposes files in that directory by bare name. Subdirectories are not generally
flattened. Windows name comparisons are insensitive to case. Repeated `-mod`
arguments are prepended, giving the last declared mod the first disk search root.

The resource dictionary applies these rules:

| Candidate type | Selection rule |
| --- | --- |
| Loose disk files | First matching search root wins |
| Loose file versus archive member | Loose file wins, even from a lower priority root |
| Members of different mounted archives | Most recently mounted archive wins |
| Repeated mount of the same virtual archive name | Increment its reference count and keep the original physical archive |
| Search path refresh | Rescan disk files and retain mounted archives |

Consequently, a loose `base\scripts\server.cs` can override `Server.cs` inside
`RPG\scripts.vol`. Merely moving the RPG directory ahead of base does not prevent
that. There are no such loose base scripts in the supplied PCRPG copy.

The presence of a VOL file does not mount it. Startup explicitly mounts the
standard volume names, plus enumerated skin/voice volumes and any selected
`<mod>\<mod>.vol`. Mission declarations and third-party loaders can mount others.
Most RPG world archives are introduced by mission `SimVolume` declarations.
`rpgmap5.mis`, for example, declares RPG textures, shapes and interior volumes;
it does not declare a replacement `scripts.vol`.

## What PCRPG selects at startup

`console.cs` mounts exactly one volume under the name `scripts.vol`. With
`-mod RPG`, its physical source is `RPG/scripts.vol`. It does not additionally
mount `base/scripts.vol` to fill gaps. `RPG/scripts.vol` already contains 117
entries: 80 names shared with the 82-entry base archive, plus 37 RPG additions.
Thirty shared entries have different payloads. The two base-only entries are
build metadata, `makefile.mak` and `mssccprj.scc`.

Thus, many stock-looking scripts inside an RPG session are copies deliberately
shipped inside the RPG archive. That alone does not indicate a second archive
was loaded.

| Resource | PCRPG without `-mod` | PCRPG with `-mod RPG` |
| --- | --- | --- |
| `scripts.vol` | `base/scripts.vol` | `RPG/scripts.vol` |
| `server.cs`, `client.cs`, `game.cs`, `gui.cs` | Base script archive | RPG script archive |
| `connectivity.cs` | Missing | Loose `RPG/connectivity.cs`, overriding the RPG archive member |
| `version.cs` | Missing | Loose `RPG/version.cs`, declaring version 5.005 |
| `rpghud.cs` | Outside selected roots | Visible loose file, but not executed by startup |
| `gui/play.gui` | `base/gui.vol` | `base/gui.vol` |
| `shellcommon.vol` | `base/ShellCommon.vol` | `RPG/ShellCommon.vol` |
| `autoexec.cs` | Missing | Missing |

There is no RPG `gui.vol` in this copy. The RPG `ShellCommon.vol` differs from
base in three cursor bitmaps, `CUR_Arrow.bmp`, `CUR_Centering.bmp`, and
`CUR_Hand.bmp`; it contains no HUD scripts or GUI layouts.

`LoadModVolumes` and `ExecModScripts` also look for `RPG.vol` and `RPG.cs`.
Neither exists here. The RPG archive instead supplies replacement `Server.cs`
and its gameplay script chain. Their absence is not evidence that the RPG
archive failed to load. All 56 bare-name `exec` calls in that server script
resolve with RPG selected; `connectivity.cs` and `version.cs` resolve to loose
files. Separate missing bootstrap calls include `GenericTriggers.cs`,
`badwords.cs`, and `autoexec.cs`.

## How changing servers can mix scripts

Both base and RPG `Client.cs` implement `remoteSVInfo` by assigning the server's
mod list and calling `EvalSearchPath`. That function recreates skin/voice
volumes, but does not recreate `ScriptsVolume`, `ShellCommonVolume`, or the
other standard bootstrap mounts. `ResourceManager::setSearchPath` also retains
their archive entries.

The resulting selections are counterintuitive:

| Transition | Physical lookup of `scripts.vol` after refresh | Lookup of `server.cs` after refresh |
| --- | --- | --- |
| Boot without a mod, then select RPG roots | `RPG/scripts.vol` | Still `base/scripts.vol::server.cs` |
| Boot with RPG, then select base roots | `base/scripts.vol` | Still `RPG/scripts.vol::Server.cs` |

In the first case, loose RPG files such as `connectivity.cs` become visible,
while the mounted base archive continues supplying the common scripts. In the
second, RPG's archived `connectivity.cs` becomes visible when the loose override
leaves the search path. Already evaluated functions also remain defined until
redefined; a path change does not reload them.

Calling `newObject(..., SimVolume, "scripts.vol")` again does not fix this:
`addVolume` finds the existing virtual mount and increments its reference count
before consulting the new disk search path. Every owner must release the mount
before that name can bind to a different physical archive. A fresh process with
the intended `-mod RPG` and working directory avoids this transition state.

This is a plausible explanation for the remembered workaround. Deleting or
renaming `base/scripts.vol` is unnecessary for a correct fresh RPG launch and
breaks a vanilla launch in this copy. The current base archive matches the
comparison installation byte for byte, so the supplied files do not preserve
evidence of that historical edit.

## Why the HUD packs do not load

PCRPG's readme advertises Presto and Deus, but the installed files contain:

- `config/DeusRPGPackPrefs.cs` and `BattleHUDPref.cs`, plus Deus key bindings in
  `config/config.cs`.
- No `config/autoexec.cs`, Presto installer, `Include` script definition, or
  `HUD::New` script definition, including inside all 216 VOL archives.
- A loose `RPG/rpghud.cs` whose opening comment requires Presto. No installed
  startup script executes it. Its HUD calls require the missing library. The
  RPG archive does contain the separate `RPGfetchData` network helper.
- A Deus HUD update under `Tools, Updates, and Info`, outside the selected
  resource roots. Its readme tells the user to replace an existing file under
  `config/DeusRPGPack/DeusHUD`; that installed directory is absent. The update
  itself executes six additional missing scripts: `HPMP.cs`, `ITEMS.cs`,
  `MINER.cs`, `TEAM.cs`, `SKILL.cs`, and `AMMO.cs`.

Preferences and bindings cannot supply those missing libraries. Copying the
isolated Deus update or merely executing `rpghud.cs` cannot restore the pack.
The complete original Presto/Deus client installation and its startup hook need
to be restored together. The supplied files are sufficient to identify that
gap, but not to reconstruct the original pack faithfully.

The comparison `TRIBES` installation has another incomplete client setup:
`config/autoexec.cs` calls `exec("presto\\Install.cs")` and then
`Include("Autoload.cs")`, but Presto and `Include` are absent there too.
`Autoload.cs` would mount `NewOpts.vol` and execute its installer; it cannot do
so through that broken startup chain. The volume exists, but its contents are
not automatically available merely because it is in `config`.

## GUI loading has additional state

Both bundled `GUI.CS` versions implement `loadPlayGui` by preferring a resource
named `play.gui`, then falling back to `gui\play.gui`. The comparison install
has `config/play.gui`, so it takes that override. PCRPG has no bare `play.gui`
and uses the stock layout from `base/gui.vol`.

There are two further original-engine details: `Control::load` first attempts
a direct physical file load before using the resource manager, and
`Canvas::handleGUIMessage` may reuse an existing named GUI object. A successful
path refresh therefore does not guarantee an already-created GUI will reload.
Restarting is the cleanest way to compare configurations.

## Menu artwork verification, November 4, 2026

The supplied files do not reproduce a failure to load stock menu artwork in
OpenT1. Fresh PCRPG launches with and without `-mod RPG` load and render the main
menu, Options and Create Server screens. The main menu names `DarkLogo.bmp`;
Options names `Background1.bmp`. The logos, three numbered backgrounds, grid,
`Shell.ppl`, `ShellBorderCtrl.pba` and menu button bitmaps resolve to
`base/Shell.vol` in both modes. The archive inventory contains no replacement
copies of these menu assets elsewhere in either supplied installation. This
stock-menu check does not cover all RPG styling. The RPG witch loading screen
and sword cursor are present and do not require Presto or Deus:
`RPG/loading.bmp` replaces the loading image, while `RPG/ShellCommon.vol`
replaces `CUR_Arrow.bmp`, `CUR_Centering.bmp` and `CUR_Hand.bmp`.

This follows the original `FearGuiShellBorder.cpp`: `read` restores the bitmap
name saved in the GUI, and `onAdd` loads that name plus the tagged border array.
`FearGuiUnivButton.cpp` similarly loads the saved button root with state
suffixes such as `_DF.BMP`. OpenT1 binds those resources before displaying the
GUI. Selecting a gameplay mod does not invent a background name or mount every
archive in the mod directory.

The optional `shell_artwork/installed_artwork` test now checks the decoded
resources and their physical sources, renders the startup/main/options/host
screens, and compares the main-menu interior with its selected background.
It also writes a synthetic `RPG/DarkLogo.bmp` into a temporary output overlay:
base ignores it, `-mod RPG` selects it, and its pixels reach the menu framebuffer.
The installation is never modified. At 1280x720, Direct3D 12 and Vulkan readbacks
matched the software frame byte for byte in both PCRPG launch modes, including
the synthetic replacement. Earlier software captures also verified 640x480.
The comparison TRIBES installation also rendered stock menus successfully;
its startup reports `Include: Unknown command`, confirming the broken custom
UI startup chain described above. The original executable was not run.

The subsequent RPG-specific check renders `gui/Loading.gui` using the actual
witch artwork and compares the selected cursor with the sword bitmap. These
assets load correctly. The earlier missing-pack diagnosis applies to the
separate scripted HUD libraries, not to these built-in RPG replacements.

Two presentation problems were found and fixed:

- Visible SDL `--mission` startup called `FearGameApp::init` before creating the
  window, and `init` synchronously loaded the entire mission. The script's
  `renderCanvas(MainWindow)` had no presentation callback at that point. The
  existing deferral flag was set only afterward and was never consulted by
  `loadMission`. The SDL host now enables deferral before initialization;
  `loadMission` stages the mod's Loading GUI and cursor, and the host presents
  that frame before `loadPendingMission` performs the blocking work. Dedicated,
  headless, hidden and embedded launches retain synchronous startup.
- The SDL GUI binding left the native OS pointer visible over the canvas's
  software cursor. It now hides the native pointer while the focused game
  canvas owns mouse focus, restoring it on mouse exit, focus loss and binding
  destruction. A cursor already hidden by another caller remains hidden.

The `shell_loading/installed_rpg_loading` regression failed before the deferral
fix and passes afterward. It checks that `--mission rpgmap5` first stages the
Loading GUI with RPG artwork and cursor, then loads exactly once. Platform
tests cover native cursor visibility ownership and restoration.

No distinct replacement border bitmap array is present in this copy: the RPG
ShellCommon border/scroll/slider arrays match base, and `ShellBorderCtrl.pba`
resolves to `base/Shell.vol`. This does not identify the historical edge styling
the user remembers. The separate `rpghud.cs` explicitly requires Presto and the
Deus client libraries are absent, as documented above.

Run the focused check after building `opent1_tests`:

```powershell
$env:OPENT1_TEST_ASSETS = (Resolve-Path ../TribesRE/PCRPG).Path
.\build\x64-release\tests\opent1_tests.exe --suite shell_artwork
.\build\x64-release\tests\opent1_tests.exe --suite shell_loading

# Optional real-device comparison; omit this variable for software-only checks.
$env:OPENT1_SHELL_GPU = 'direct3d12' # or vulkan
.\build\x64-release\tests\opent1_tests.exe --suite shell_artwork

# Optional PPM captures; create the output directory first.
New-Item -ItemType Directory -Force build/shell-captures | Out-Null
$env:OPENT1_SHELL_CAPTURE = (Resolve-Path build/shell-captures).Path
.\build\x64-release\tests\opent1_tests.exe --suite shell_artwork
```

The older optional `shell/installed_mission_selection` test separately fails
its mission-description assertion with the supplied PCRPG installation and
passes with the comparison TRIBES installation. That failure occurred before
the artwork test was added; it does not indicate a missing menu bitmap.

## Wasteland rpgmap5 compatibility, November 4, 2026

`../Wasteland` supplies loose RPG scripts, including a different `EnemyArmors.cs`,
rather than the PCRPG `RPG/scripts.vol` used in the earlier checks. Its
`MutantArmor` declares `maxJetSideForceFactor = 1.2` at line 3068. OpenT1 tried
to encode it with the strict normalized-float API and raised `Invalid quantized
float` while replicating datablocks. The original `playerUpdate.cpp` sends this
field with eight bits, and the original `bitstream.cpp` truncates the scaled
integer and writes its low bits without checking the normalized range.

Datablock serialization now preserves that encoding: `1.2 * 255` truncates to
306 and sends 50 in eight bits, while the server's original 1.2 value remains
unchanged. Other quantized datablock fields use the same legacy conversion.
Non-finite values and conversions outside the representable signed integer
range are still rejected; the general BitStream normalized-float API remains
strict. Unit tests cover the native bytes, negative and oversized finite inputs,
and rejection of unsafe conversions.

The next loading blocker was `HellsHammer.cs:132`, whose function body contains
the decimal literal `10000000000000000000000000`. The original lexer calls
Windows `atoi`, which wraps that literal to `1241513984`. Console execution now
uses defined unsigned arithmetic to preserve decimal literal wrapping, while
standalone parsing retains its strict default. This lets the entire file parse
without editing the external mod scripts.

The optional `wasteland/installed_loading` regression uses an isolated write
directory, loads the supplied `rpgmap5`, advances the local connection and
renders a frame. It also checks that MutantArmor retains 1.2 on the server and
receives the original eight-bit value on the client.

```powershell
$env:OPENT1_TEST_ASSETS = (Resolve-Path ../Wasteland).Path
.\build\x64-release\tests\opent1_tests.exe --suite wasteland
```

## Reproducing the audit

Run the read-only metadata tool from the OpenT1 root:

```powershell
python tools/audit_legacy_assets.py ../TribesRE/PCRPG --mod RPG --join-mod base --compare ../TribesRE/TRIBES --output build/legacy-asset-audit/pcrpg-rpg.json
python tools/audit_legacy_assets.py ../TribesRE/PCRPG --join-mod RPG --output build/legacy-asset-audit/pcrpg-vanilla.json
python tools/audit_legacy_assets.py ../TribesRE/TRIBES --output build/legacy-asset-audit/tribes.json
```

The tool inventories inactive archives too, records competing resource sources,
hashes uncompressed entries, and models standard/named-mod bootstrap mounts and
path changes. It does not execute scripts or model mission, voice/skin,
third-party mounts, direct GUI loads, or GUI caching. All 23,298 entries in the
422 supplied archives are uncompressed. Inventory metadata was cross-checked
against `opent1_asset_tool volume-list` for every archive, and payload hashes
against native extraction for all 281 entries in the three script archives.
Extracted reference content stays in ignored `build/` scratch files.

The original engine has a useful built-in console diagnostic:

```text
echo($modList);
echo($ConsoleWorld::DefaultSearchPath);
listVolumes();
which("scripts.vol");
which("server.cs");
which("connectivity.cs");
which("gui\\play.gui");
which("play.gui");
which("rpghud.cs");
which("presto\\Install.cs");
```

Compare `which("scripts.vol")` with `which("server.cs")`: the first identifies
the currently discoverable archive file; the second identifies the source of
the script member actually selected by the resource dictionary. `which` does
not identify where an already-evaluated function was originally defined.

## Source references

- [PCRPG console.cs](../../TribesRE/PCRPG/console.cs): lines 87, 130, 142,
  153, 160 and 216 establish paths, mounting, and client startup hooks.
- [Original main.cpp](../../TribesRE/legacy/tribes-rebirth/program/code/main.cpp):
  `FearMain::executeConsoleCS`, line 159, opens the physical bootstrap.
- [Original resManager.cpp](../../TribesRE/legacy/tribes-rebirth/darkstar/Core/code/resManager.cpp):
  `addVolume` line 211, `searchPath` line 329, `setSearchPath` line 389.
- [Original resDictionary.cpp](../../TribesRE/legacy/tribes-rebirth/darkstar/Core/code/resDictionary.cpp):
  `find` line 82 and `pushBehind` line 98 establish precedence.
- [Original console.cpp](../../TribesRE/legacy/tribes-rebirth/darkstar/console/code/console.cpp):
  `c_exec`, line 141, loads through the resource manager and purges its script
  resource after evaluation. This implementation does not use a `.dso` cache.
- [Original simResource.cpp](../../TribesRE/legacy/tribes-rebirth/darkstar/Sim/code/simResource.cpp):
  line 22 shares one resource manager between SimResource owners.
- [Original simGuiCtrl.cpp](../../TribesRE/legacy/tribes-rebirth/darkstar/SimGui/code/simGuiCtrl.cpp):
  `Control::load`, line 208, tries the direct file before resource lookup.
- [Original simGuiCanvas.cpp](../../TribesRE/legacy/tribes-rebirth/darkstar/SimGui/code/simGuiCanvas.cpp):
  `Canvas::handleGUIMessage`, line 1325, reuses named GUI objects.
- [Original FearPlugin.cpp](../../TribesRE/legacy/tribes-rebirth/program/code/FearPlugin.cpp):
  `c_which`, line 3691, prints the resource's physical source.
- [PCRPG HUD](../../TribesRE/PCRPG/RPG/rpghud.cs),
  [bundle information](<../../TribesRE/PCRPG/Information by Particle.txt>),
  [Deus update instructions](<../../TribesRE/PCRPG/Tools, Updates, and Info/PCRPG Deus Pack Update (If You Play There)/Readme.txt>),
  and [comparison autoexec](../../TribesRE/TRIBES/config/autoexec.cs).

The archived script findings above refer to `Client.cs:430`, `Client.cs:725`,
`GUI.CS:145`, and `Server.cs:141` inside `PCRPG/RPG/scripts.vol`.
PCRPG and the comparison install share these SHA-256 values:

| File | SHA-256 |
| --- | --- |
| `console.cs` | `939ca827ea1da016009d63e652dca4711fd7cc3bc6c879fe9a4c4f7d1ca0b247` |
| `base/scripts.vol` | `d0e70d42fccb48607ec3eb3833994e8134af89ed4d88961af88c648360697db8` |
| `base/gui.vol` | `895705ac8aff510677bc2fad67c80db57cb9f17447ee55ecda40aa85689f6c95` |
