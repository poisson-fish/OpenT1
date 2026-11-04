#!/usr/bin/env python3
"""Inspect a Tribes installation without running scripts or modifying assets.

Models the standard/named-mod VOL mounts in the bundled console.cs. Skin,
voice, mission, and third-party loader mounts are outside this static model.
Archive inventories include inactive volumes so missing client packs can be
distinguished from packs that merely have not been mounted. Uses only stdlib.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


STANDARD_VOLUMES = (
    "fonts.vol", "scripts.vol", "gui.vol", "edit.vol", "editor.vol",
    "darkstar.vol", "interface.vol", "shell.vol", "shellcommon.vol", "entities.vol",
)
PROBES = (
    "scripts.vol", "shellcommon.vol", "client.cs", "server.cs", "game.cs",
    "gui.cs", "connectivity.cs", "version.cs", "rpghud.cs", "autoexec.cs",
    "presto/Install.cs", "DeusRPGPack/DeusHUD/DeusRPGHud.cs",
    "DeusRPGPack/DeusHUD/HPMP.cs", "gui/play.gui", "play.gui",
    "RPG.cs", "RPG.vol", "NewOpts.vol", "NewOpts/NewOptsInstall.cs",
    "DarkLogo.bmp", "BrightLogo.bmp", "Background0.bmp", "Background1.bmp",
    "Background2.bmp", "BackgroundGrid.bmp", "Shell.ppl", "ShellBorderCtrl.pba",
    "BTN_QuickStart_DF.bmp", "Loading.bmp", "CUR_Arrow.bmp", "HH_Icon_Green.bmp",
    "I_blaster_on.bmp",
)
FUNCTIONS = ("Include", "HUD::New", "RPGfetchData", "DeusRPGPack::func1")


def key(name):
    return name.replace("\\", "/").lower()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_volume(path):
    """Read bounded PVOL metadata; hash uncompressed payloads, never extract."""
    data = path.read_bytes()

    def block(at, expected, allow_tail=False):
        if at < 8 or at + 8 > len(data):
            raise ValueError(f"{path}: block outside archive")
        tag, flags = struct.unpack_from("<4sI", data, at)
        size = flags & 0x7FFFFFFF
        alignment = 4 if flags & 0x80000000 else 2
        end = at + 8 + size
        padded = at + 8 + (size + alignment - 1) // alignment * alignment
        if tag != expected or end > len(data) or (padded > len(data) and not (allow_tail and end == len(data))):
            raise ValueError(f"{path}: invalid {expected!r} block")
        return data[at + 8:end], min(padded, len(data))

    if len(data) < 8 or data[:4] != b"PVOL":
        raise ValueError(f"{path}: not a PVOL archive")
    strings_at = struct.unpack_from("<I", data, 4)[0]
    strings, ids_at = block(strings_at, b"vols")
    records, _ = block(ids_at, b"voli", allow_tail=True)
    if len(records) % 17:
        raise ValueError(f"{path}: incomplete dictionary record")
    entries, scripts = [], {}
    for ident, name_at, offset, size, compression in struct.iter_unpack("<IiIIB", records):
        payload, end = block(offset, b"VBLK")
        if end > strings_at or compression not in (0, 1, 2) or (compression == 0 and len(payload) != size):
            raise ValueError(f"{path}: invalid entry payload")
        if name_at == -1:
            name = None
        else:
            if not 0 <= name_at < len(strings):
                raise ValueError(f"{path}: name outside string table")
            nul = strings.find(b"\0", name_at)
            if nul == -1:
                raise ValueError(f"{path}: unterminated name")
            name = strings[name_at:nul].decode("cp1252")
        entries.append({"name": name, "id": ident, "size": size, "compression": compression,
                        "sha256": digest(payload) if compression == 0 else None})
        if name and key(name).endswith(".cs") and compression == 0:
            scripts[name] = payload.decode("cp1252")
    return {"sha256": digest(data), "entries": entries}, scripts


class Installation:
    def __init__(self, root):
        self.root = root.resolve(strict=True)
        self.files = sorted(p for p in self.root.rglob("*") if p.is_file() and not p.is_symlink())
        self.volumes, self.scripts = {}, {}
        for path in self.files:
            relative = path.relative_to(self.root).as_posix()
            if path.suffix.lower() == ".vol":
                volume, scripts = read_volume(path)
                self.volumes[relative] = volume
                self.scripts.update({relative + "::" + n: text for n, text in scripts.items()})
            elif path.suffix.lower() == ".cs":
                self.scripts[relative] = path.read_bytes().decode("cp1252")

    def index(self, mods):
        roots = ["config"]
        # console.cs prepends each -mod argument, then appends base if absent.
        active = list(reversed(mods))
        if not any(key(mod) == "base" for mod in active):
            active.append("base")
        for mod in active:
            roots.append(mod)
            roots.extend(mod + "/" + sub for sub in ("missions", "fonts", "skins", "voices", "scripts"))
        roots.extend(("recordings", "temp"))
        disk = {}
        by_name = {key(p.relative_to(self.root).as_posix()): p for p in self.files}
        # Root matching is insensitive to case, like the Windows original.
        for root in roots:
            prefix = key(root).rstrip("/") + "/"
            for name, path in by_name.items():
                if name.startswith(prefix):
                    disk.setdefault(name[len(prefix):], []).append(path.relative_to(self.root).as_posix())
        return roots, disk

    def mounts(self, mods, disk):
        names = []
        # LoadModVolumes mounts only <mod>.vol, in argument declaration order.
        physical = {key(p.relative_to(self.root).as_posix()) for p in self.files}
        for mod in mods:
            name = mod + ".vol"
            if key(mod + "/" + name) in physical:
                names.append(name)
        names.extend(STANDARD_VOLUMES)
        mounts = []
        seen = set()
        for name in names:
            # Repeated addVolume(name) retains the original physical binding.
            if key(name) not in seen and key(name) in disk:
                source = disk[key(name)][0]
                if source in self.volumes:
                    mounts.append({"name": name, "source": source})
                    seen.add(key(name))
        return mounts

    def scenario(self, mods, probes, retained_mounts=None):
        roots, disk = self.index(mods)
        mounts = self.mounts(mods, disk) if retained_mounts is None else retained_mounts
        resolution = {}
        for name in probes:
            sources = list(disk.get(key(name), ()))
            for mount in reversed(mounts):
                sources.extend(mount["source"] + "::" + entry["name"]
                               for entry in self.volumes[mount["source"]]["entries"]
                               if entry["name"] and key(entry["name"]) == key(name))
            resolution[name] = sources
        return {"mods_in_argument_order": mods, "roots": roots, "mounts": mounts,
                "resources_in_precedence_order": resolution}

    def providers(self):
        return {name: [source for source, text in self.scripts.items()
                       if re.search(r"\bfunction\s+" + re.escape(name) + r"\s*\(", text, re.I)]
                for name in FUNCTIONS}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installation", type=Path)
    parser.add_argument("--mod", action="append", default=[], help="Repeat in Tribes command-line order")
    parser.add_argument("--join-mod", action="append", default=[], help="Also model a path change retaining startup mounts")
    parser.add_argument("--resource", action="append", default=[], help="Additional virtual name to inspect")
    parser.add_argument("--compare", type=Path, help="Compare console.cs and matching base VOL bytes")
    parser.add_argument("--output", type=Path, help="Save metadata as JSON; no game script bodies are copied")
    args = parser.parse_args()
    install = Installation(args.installation)
    probes = list(dict.fromkeys(PROBES + tuple(args.resource)))
    startup = install.scenario(args.mod, probes)
    result = {"installation": str(install.root),
              "scope": "Static standard/named-mod startup mounts; excludes voice/skin/mission/third-party mounts, GUI object caching, and direct GUI filesystem loads. Compressed entry payloads are not decoded.",
              "archive_count": len(install.volumes), "archives": install.volumes,
              "script_function_providers_including_inactive_sources": install.providers(),
              "startup": startup}
    if args.join_mod:
        result["after_path_change"] = install.scenario(args.join_mod, probes, startup["mounts"])
    if args.compare:
        comparison = {}
        for relative in ["console.cs"] + [n for n in install.volumes if key(n).startswith("base/")]:
            other = args.compare / relative
            if other.is_file():
                comparison[relative] = digest((install.root / relative).read_bytes()) == digest(other.read_bytes())
        result["identical_bytes_in_comparison_installation"] = comparison
    for label in ("startup", "after_path_change"):
        if label in result:
            print(label + ":")
            for name, sources in result[label]["resources_in_precedence_order"].items():
                print(f"  {name}: {sources[0] if sources else 'MISSING'}")
    print(f"Inventoried {len(install.volumes)} archives; script function providers: {install.providers()}")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
