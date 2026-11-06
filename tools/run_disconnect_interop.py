"""Opt-in live 1.11/OpenT1 lifecycle probe; no game binaries are distributed.

The minimal scripts exercise real engine delegates over UDP without a mission
or a video device. Packet traces contain headers only, never player payloads.
"""
import argparse
import hashlib
import json
import shutil
import socket
import subprocess
import threading
import time
import platform
import re
from pathlib import Path


def renamed_function(source, name, alias):
    """Extract an installed function for a logging wrapper, preserving its body."""
    matches = list(re.finditer(r"\bfunction\s+(" + re.escape(name) + r")\s*\([^)]*\)\s*\{",
                               source, re.IGNORECASE))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one installed definition of {name}")
    match = matches[0]
    position, depth, state = match.end(), 1, "code"
    while position < len(source) and depth:
        char, pair = source[position], source[position:position + 2]
        if state == "string":
            if char == "\\":
                position += 2
                continue
            if char == '"':
                state = "code"
        elif state == "line":
            if char in "\r\n":
                state = "code"
        elif state == "block":
            if pair == "*/":
                state = "code"
                position += 2
                continue
        elif char == '"':
            state = "string"
        elif pair in ("//", "/*"):
            state = "line" if pair == "//" else "block"
            position += 2
            continue
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
        position += 1
    if depth:
        raise ValueError(f"unterminated installed function {name}")
    return source[match.start():match.start(1)] + alias + source[match.end(1):position]


def installed_probe(assets, side, relay_port, server_port, mod=None):
    # Imported lazily: the asset-free header/relay tests need no installation.
    import audit_legacy_assets
    _, content = audit_legacy_assets.read_volume(assets / "base/scripts.vol")
    content = {name.lower(): source for name, source in content.items()}

    def original(filename, name, alias):
        source = content[filename]
        if mod:
            overrides = [path.read_text(encoding="cp1252") for path in (assets / mod).rglob("*.cs")]
            matches = [text for text in overrides if re.search(r"\bfunction\s+" + re.escape(name) + r"\s*\(", text, re.I)]
            if len(matches) > 1:
                raise ValueError(f"ambiguous mod definitions of {name}")
            if matches:
                source = matches[0]
        return renamed_function(source, name, alias)
    common = f'''$Console::logMode=1;
$Server::Port={server_port};
$Server::HostPublicGame=false;
$Server::Heartbeats=false;
$Server::numMasters=0;
$pref::lanOnly=true;
$pref::skipIntro=true;
$pref::AutoRefresh=false;
$pref::VideoFullScreen=false;
$pref::cdMusic=false;
$PCFG::Name="LifecycleProbe";
$PCFG::Gender="Male";
$PCFG::Voice="male1";
$PCFG::SkinBase="base";
$PCFG::Script="";
$PCFG::Info="lifecycle-probe-password";
function IRCLogin::AutoConnect() {{}}
'''
    if side == "server":
        common += original("server.cs", "Server::onClientConnect", "ProbeOriginalConnect")
        common += original("server.cs", "Server::onClientDisconnect", "ProbeOriginalDisconnect")
        return common + '''
function ReportRoster() {
    echo("LIFECYCLE ROSTER=", getNumClients(), " DROPS=", $Drops, " STREAM=", isObject($LastPeer));
}
function ProbeServerReady() { echo("LIFECYCLE READY"); }
function Server::onClientConnect(%id) {
    $Joins++;
    $Probe::Joins++;
    export("Probe::*", "config\\\\lifecycle-state.cs", false);
    echo("LIFECYCLE JOIN=", $Joins, " ID=", %id, " NAME=", Client::getName(%id));
    ProbeOriginalConnect(%id);
}
function Server::onClientDisconnect(%id) {
    $Drops++;
    $Probe::Drops++;
    export("Probe::*", "config\\\\lifecycle-state.cs", false);
    $LastPeer=%id;
    echo("LIFECYCLE DROP=", $Drops, " ID=", %id, " NAME=", Client::getName(%id));
    echo("LIFECYCLE DEPARTING_BODY=", Client::getOwnedObject(%id), " CAMERA=", Client::getObserverCamera(%id));
    ProbeOriginalDisconnect(%id);
    schedule("ReportRoster();", 0.1);
}
'''
    common += original("client.cs", "onConnection", "ProbeOriginalConnection")
    common += original("client.cs", "onClientGhostAlwaysDone", "ProbeOriginalGhostsDone")
    return common + f'''
function onConnection(%state) {{
    echo("LIFECYCLE STATE=", %state, " MANAGER=", getManagerId());
    if (%state == "Accepted") {{
        $Accepted++;
        $Probe::Accepted++;
        export("Probe::*", "config\\\\lifecycle-state.cs", false);
    }}
    ProbeOriginalConnection(%state);
}}
function onClientGhostAlwaysDone() {{
    $Probe::Ghosts++;
    export("Probe::*", "config\\\\lifecycle-state.cs", false);
    echo("LIFECYCLE GHOSTS_DONE");
    ProbeOriginalGhostsDone();
    schedule("ProbeDepart();", 1);
}}
function ProbeDepart() {{
    EndGame();
    $Probe::Endgames++;
    export("Probe::*", "config\\\\lifecycle-state.cs", false);
    echo("LIFECYCLE ENDGAME_RETURNED");
    if ($Accepted < 2) schedule("ProbeJoin();", 0.2);
    else echo("LIFECYCLE DONE");
}}
function ProbeJoin() {{
    $quitOnDisconnect=false;
    $pref::PlayGameMode="JOIN";
    GuiLoadContentCtrl(MainWindow,"gui\\\\JoinGame.gui");
    $Server::Address="IP:127.0.0.1:{relay_port}";
    JoinGame();
}}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def content_digest(root):
    checksum = hashlib.sha256()
    files = sorted(path for path in root.rglob("*") if path.is_file())
    for path in files:
        checksum.update(path.relative_to(root).as_posix().encode("utf-8"))
        checksum.update(b"\0")
        checksum.update(bytes.fromhex(digest(path)))
    return {"sha256": checksum.hexdigest(), "files": len(files)}


def header(data):
    position = 0

    def read(count):
        nonlocal position
        if position + count > len(data) * 8:
            raise ValueError("truncated header")
        value = sum(((data[(position + n) // 8] >> ((position + n) % 8)) & 1) << n
                    for n in range(count))
        position += count
        return value

    try:
        if not read(1):
            return {"type": "Raw", "size": len(data)}
        parity, sequence, high_ack = read(1), read(9), read(5)
        runs = []
        for _ in range(17):
            count = read(3)
            if not count:
                break
            runs.append([read(5), count])
        else:
            raise ValueError("too many ack runs")
        flags = read(5)
        base = flags & ~24
        result = {"type": {0: "Data", 2: "Disconnect", 3: "Request", 4: "Accept",
                           5: "Reject", 7: "Ping"}.get(base, "Invalid"),
                  "flags": flags, "parity": parity, "sequence": sequence,
                  "high_ack": high_ack, "acks": runs, "size": len(data)}
        if base in (2, 3, 4, 5):
            result["generation"] = read(32)
        return result
    except ValueError:
        return {"type": "Invalid", "size": len(data)}


class Relay:
    def __init__(self, port, scenario):
        self.server = ("127.0.0.1", port)
        self.scenario = scenario
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(.05)
        self.port = self.socket.getsockname()[1]
        self.stop = threading.Event()
        self.trace = []
        self.initial = None
        self.held = []
        self.cancelled = False
        self.client = None
        self.blackhole = False
        self.start = time.monotonic()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def record(self, direction, data, action):
        self.trace.append({"ms": round((time.monotonic() - self.start) * 1000, 2),
                           "direction": direction, "action": action, **header(data)})

    def send(self, direction, data, destination, action="forward"):
        self.record(direction, data, action)
        if destination:
            self.socket.sendto(data, destination)

    def run(self):
        while not self.stop.is_set():
            try:
                data, address = self.socket.recvfrom(2048)
            except (socket.timeout, ConnectionResetError):
                continue
            direction = "s2c" if address == self.server else "c2s"
            if direction == "c2s":
                self.client = address
            destination = self.client if direction == "s2c" else self.server
            h = header(data)
            if self.blackhole:
                self.record(direction, data, "blackhole")
                continue
            if direction == "c2s" and h["type"] == "Request":
                if self.initial is None:
                    self.initial = h["generation"]
                elif h["generation"] != self.initial and self.cancelled:
                    self.send(direction, data, destination)
                    for old_direction, old_data in self.held:
                        target = self.client if old_direction == "s2c" else self.server
                        self.send(old_direction, old_data, target, "release_old")
                    self.held.clear()
                    continue
            if self.scenario == "cancel_request" and direction == "c2s" and (
                    h["type"] == "Request" and h.get("generation") == self.initial):
                self.held.append((direction, data))
                self.record(direction, data, "hold_request")
                continue
            if self.scenario == "cancel_accept" and direction == "s2c" and (
                    h["type"] == "Accept" and h.get("generation") == self.initial):
                self.held.append((direction, data))
                self.record(direction, data, "hold_accept")
                continue
            if self.scenario == "cancel_ack" and direction == "c2s" and not self.cancelled and (
                    h["type"] not in ("Request", "Disconnect")):
                self.record(direction, data, "drop_before_disconnect")
                continue
            if direction == "c2s" and h["type"] == "Disconnect":
                self.cancelled = True
            self.send(direction, data, destination)
            if (self.scenario == "timeout" and direction == "c2s" and
                    h.get("flags", 0) & 16 and h["type"] not in ("Disconnect", "Request")):
                self.blackhole = True

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.socket.close()


def scripts(server_kind, client_kind, port, relay_port, scenario):
    common = f'''$Console::logMode = 1;
$WinConsoleEnabled = false;
$Server::Port = {port};
$Server::MaxPlayers = 8;
$Server::HostPublicGame = false;
$Server::Heartbeats = false;
function Game::EndFrame() {{}}
function onExit() {{}}
function onClientJoin(%id) {{}}
function dataFinished() {{}}
'''
    server = common + '''newServer(); focusServer();
newObject(ConsoleScheduler, SimConsoleScheduler);
function ReportRoster() { echo("LIFECYCLE ROSTER=", getNumClients(), " DROPS=", $Drops); }
function Server::onClientConnect(%id) {
    $Joins++;
    echo("LIFECYCLE JOIN=", $Joins, " ID=", %id, " NAME=", Client::getName(%id));
}
function Server::onClientDisconnect(%id) {
    $Drops++;
    echo("LIFECYCLE DROP=", $Drops, " ID=", %id, " NAME=", Client::getName(%id));
    schedule("ReportRoster();", 0.1);
}
'''
    if server_kind == "vanilla":
        server += f'echo("LIFECYCLE DELEGATE=", newObject(serverDelegate, FearCSDelegate, true, "IP", {port}));\n'
    else:
        server = server.replace("newServer();", "$OpenT1::ConsoleCSExecuted = true; newServer();")
    server += 'echo("LIFECYCLE READY");\n'
    if scenario == "kick":
        server = server.replace('$Joins++;', '$Joins++;\n    schedule("TerminateClient(" @ %id @ ");", 0.5);')
        server += '''function TerminateClient(%id) {
    Net::kick(%id, "Lifecycle kick");
}
'''
    client = common + '''newClient(); focusClient();
newObject(ConsoleScheduler, SimConsoleScheduler);
$PCFG::Name = "LifecycleProbe";
$PCFG::Gender = "Male";
$PCFG::Voice = "male1";
$PCFG::SkinBase = "base";
function Depart() {
    disconnect();
    echo("LIFECYCLE CLOSED MANAGER=", getManagerId());
    if ($Accepted < $Cycles) BeginJoin();
    else echo("LIFECYCLE DONE");
}
function CancelPending() {
    disconnect();
    echo("LIFECYCLE CANCELLED MANAGER=", getManagerId());
    schedule("BeginJoin();", 0.1);
}
function onConnection(%state) {
    echo("LIFECYCLE STATE=", %state, " MANAGER=", getManagerId());
    if (%state == "Accepted") {
        $Accepted++;
        schedule("Depart();", 0.2);
    }
}
'''
    if client_kind == "vanilla":
        client += 'newObject(clientDelegate, FearCSDelegate, false, "IP", 0);\n'
    if scenario in ("kick", "timeout"):
        client = client.replace('schedule("Depart();", 0.2);', '')
        client = client.replace('echo("LIFECYCLE STATE=", %state, " MANAGER=", getManagerId());', '''echo("LIFECYCLE STATE=", %state, " MANAGER=", getManagerId());
    if (%state == "Dropped" || %state == "TimedOut") {
        $Terminations++;
        if ($Terminations < $Cycles) schedule("BeginJoin();", 0.05);
        else echo("LIFECYCLE DONE");
    }''')
    elif scenario == "shutdown":
        client = client.replace('schedule("Depart();", 0.2);', 'schedule("ShutdownClient();", 0.2);')
        client += '''function ShutdownClient() {
    echo("LIFECYCLE SHUTDOWN");
    quit();
}
'''
    client += f'''function BeginJoin() {{ connect("IP:127.0.0.1:{relay_port}"); }}
$Cycles = {1 if scenario in ('cancel_request', 'cancel_accept', 'shutdown', 'timeout') else 3};
BeginJoin();
'''
    return server, client


def read_log(path):
    try:
        return path.read_text(errors="replace")
    except (FileNotFoundError, PermissionError):
        return ""


def read_state(path):
    return {name.lower(): int(value) for name, value in re.findall(
        r'\$Probe::(\w+)\s*=\s*"?(\d+)"?\s*;', read_log(path), re.IGNORECASE)}


def launch(kind, directory, args):
    output = (directory / "process.log").open("w")
    if kind == "vanilla":
        command = [str(directory / (args.vanilla_executable.name if args.vanilla_executable else "Tribes.exe")), "-dedicated"]
    else:
        command = [str(args.opent1), "--headless", "--no-audio", "--no-assets", "-dedicated",
                   "+exec", str(directory / "console.cs")]
    if args.installed_assets:
        if kind == "vanilla":
            command = [str(directory / (args.vanilla_executable.name if args.vanilla_executable else "Tribes.exe"))]
        else:
            command = [str(args.opent1), "--headless", "--no-audio", "--assets", str(directory)]
        if directory.name == "server":
            command += ["-dedicated", args.mission]
        if args.mod:
            command += ["-mod", args.mod]
        command += ["+exec", "lifecycle-probe.cs"]
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen(command, cwd=directory, stdin=subprocess.PIPE,
                               stdout=output, stderr=output, startupinfo=startup,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    output.close()
    return process


def run_case(args, server_kind, client_kind, scenario):
    name = f"{client_kind}-to-{server_kind}-{scenario}"
    if client_kind == "vanilla" and scenario in ("cancel_request", "cancel_accept"):
        return {"name": name, "client": client_kind, "server": server_kind,
                "scenario": scenario, "status": "not_run",
                "reason": "Vanilla pending UI cancellation is not exposed by disconnect(); no UI automation is used."}
    directory = args.output / name
    directory.mkdir(parents=True, exist_ok=False)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    relay = Relay(port, scenario)
    processes = []
    start = time.monotonic()
    phase = "preparation"
    try:
        server_script, client_script = scripts(server_kind, client_kind, port, relay.port, scenario)
        for side, kind, script in (("server", server_kind, server_script),
                                   ("client", client_kind, client_script)):
            target = directory / side
            target.mkdir()
            if args.installed_assets:
                shutil.copytree(args.installed_assets / "base", target / "base")
                if args.mod:
                    shutil.copytree(args.installed_assets / args.mod, target / args.mod)
                (target / "config").mkdir()
                (target / "temp").mkdir()
                script = (args.installed_assets / "console.cs").read_text()
                script += ('\nexec("lifecycle-probe.cs");\nschedule("ProbeServerReady();", 1);\n' if side == "server" else
                           '\nschedule("ProbeJoin();", 1);\n')
                (target / "config/lifecycle-probe.cs").write_text(
                    installed_probe(args.installed_assets, side, relay.port, port, args.mod),
                    encoding="cp1252", newline="\n")
            (target / "console.cs").write_text(script)
            if kind == "vanilla":
                native = args.vanilla_executable or args.vanilla / "Tribes.exe"
                shutil.copy2(native, target / native.name)
                shutil.copy2(args.vanilla / "smackw32.dll", target / "smackw32.dll")
                shutil.copy2(args.memory_dll or args.vanilla / "mem.dll", target / "mem.dll")
        phase = "server_startup"
        processes.append(launch(server_kind, directory / "server", args))
        deadline = time.monotonic() + args.timeout
        while "LIFECYCLE READY" not in read_log(directory / "server/console.log"):
            if processes[0].poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("server did not initialize")
            time.sleep(.05)
        # Native startup continues after console.cs. Let its main loop start;
        # later assertions use events, never this delay as evidence of a join.
        time.sleep(1)
        phase = "client_startup"
        processes.append(launch(client_kind, directory / "client", args))
        cancellation_sent = False
        first_drop_ms = None
        phase = "lifecycle"
        while time.monotonic() < deadline:
            client = read_log(directory / "client/console.log")
            server = read_log(directory / "server/console.log")
            if first_drop_ms is None and "LIFECYCLE DROP=" in server:
                first_drop_ms = round((time.monotonic() - relay.start) * 1000, 2)
            if scenario in ("cancel_request", "cancel_accept") and not cancellation_sent:
                gate = "hold_request" if scenario == "cancel_request" else "hold_accept"
                if any(packet["action"] == gate for packet in relay.trace):
                    processes[1].stdin.write(b"CancelPending();\n")
                    processes[1].stdin.flush()
                    cancellation_sent = True
            rosters = re.findall(r"^LIFECYCLE ROSTER=(\d+)", server, re.MULTILINE)
            done = "LIFECYCLE DONE" in client or (scenario == "shutdown" and
                                                    "LIFECYCLE SHUTDOWN" in client and processes[1].poll() is not None)
            if (done and rosters and rosters[-1] == "0" and
                    server.count("LIFECYCLE JOIN=") == server.count("LIFECYCLE DROP=")):
                break
            if processes[0].poll() is not None or (scenario != "shutdown" and processes[1].poll() is not None):
                break
            time.sleep(.05)
        client = read_log(directory / "client/console.log")
        server = read_log(directory / "server/console.log")
        rosters = re.findall(r"^LIFECYCLE ROSTER=(\d+)", server, re.MULTILINE)
        accepted = client.count("LIFECYCLE STATE=Accepted")
        joins = server.count("LIFECYCLE JOIN=")
        drops = server.count("LIFECYCLE DROP=")
        client_state = read_state(directory / "client/config/lifecycle-state.cs")
        server_state = read_state(directory / "server/config/lifecycle-state.cs")
        if args.installed_assets:
            accepted = client_state.get("accepted", accepted)
            joins = server_state.get("joins", joins)
            drops = server_state.get("drops", drops)
        result = {"name": name, "client": client_kind, "server": server_kind,
                  "scenario": scenario, "accepted": accepted, "joins": joins, "drops": drops,
                  "empty_roster": bool(rosters and rosters[-1] == "0"),
                  "done": "LIFECYCLE DONE" in client or (scenario == "shutdown" and
                            "LIFECYCLE SHUTDOWN" in client and processes[1].poll() is not None),
                  "elapsed_s": round(time.monotonic() - start, 3)}
        departures = [packet for packet in relay.trace if packet["direction"] == "c2s" and
                       packet["type"] == "Disconnect"]
        result["disconnects"] = len(departures)
        result["first_drop_observed_ms"] = first_drop_ms
        result["first_disconnect_ms"] = departures[0]["ms"] if departures else None
        result["departure_observation_delay_ms"] = (round(first_drop_ms - departures[0]["ms"], 2)
                                                   if first_drop_ms is not None and departures else None)
        result["replacement_requests"] = len({packet.get("generation") for packet in relay.trace
                                               if packet["type"] == "Request"}) - 1
        names = re.findall(r"^LIFECYCLE (?:JOIN|DROP)=\d+ ID=\d+ NAME=(.*)", server, re.MULTILINE)
        result["names_preserved"] = bool(names) and all(name == "LifecycleProbe" for name in names)
        if scenario == "cancel_accept":
            replacements = [packet for packet in relay.trace if packet["direction"] == "c2s" and
                            packet["type"] == "Request" and packet.get("generation") != relay.initial]
            result["first_drop_observed_ms"] = first_drop_ms
            result["first_disconnect_ms"] = departures[0]["ms"] if departures else None
            result["first_replacement_request_ms"] = replacements[0]["ms"] if replacements else None
            result["drop_before_replacement"] = bool(first_drop_ms is not None and
                                                       (not replacements or first_drop_ms < replacements[0]["ms"]))
        result["client_states"] = re.findall(r"LIFECYCLE STATE=(\w+)", client)
        result["client_exit"] = processes[1].poll()
        result["ghosts_done"] = "LIFECYCLE GHOSTS_DONE" in client
        result["endgame_returned"] = "LIFECYCLE ENDGAME_RETURNED" in client
        result["ghost_batches"] = client.count("LIFECYCLE GHOSTS_DONE")
        result["endgame_count"] = client.count("LIFECYCLE ENDGAME_RETURNED")
        result["ghost_log_batches"] = result["ghost_batches"]
        if args.installed_assets:
            result["ghost_batches"] = client_state.get("ghosts", result["ghost_batches"])
            result["endgame_count"] = client_state.get("endgames", result["endgame_count"])
            result["exported_client_state"] = client_state
            result["exported_server_state"] = server_state
        streams = re.findall(r"^LIFECYCLE ROSTER=\d+ DROPS=\d+ STREAM=(\w+)", server, re.MULTILINE)
        result["stream_retired"] = streams[-1].lower() in ("false", "0") if streams else None
        result["server_disconnects"] = sum(packet["direction"] == "s2c" and packet["type"] == "Disconnect"
                                             for packet in relay.trace)
        result["blackholed_packets"] = sum(packet["action"] == "blackhole" for packet in relay.trace)
        cycles = 2 if args.installed_assets else (1 if scenario in ("cancel_request", "cancel_accept", "shutdown", "timeout") else 3)
        result["status"] = "pass" if (result["done"] and result["empty_roster"] and
                                         accepted == cycles and joins >= cycles and joins == drops and
                                         result["names_preserved"]) else "fail"
        if args.installed_assets and not (result["ghost_batches"] == cycles and result["endgame_count"] == cycles and
                                         result["stream_retired"]):
            result["status"] = "fail"
        return result
    except (OSError, RuntimeError, ValueError) as error:
        return {"name": name, "client": client_kind, "server": server_kind, "scenario": scenario,
                "status": "fail", "phase": phase, "reason": str(error),
                "elapsed_s": round(time.monotonic() - start, 3),
                "process_exits": [process.poll() for process in processes]}
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
        relay.close()
        (directory / "packets.json").write_text(json.dumps(relay.trace, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--opent1", type=Path, required=True)
    parser.add_argument("--vanilla", type=Path, required=True)
    parser.add_argument("--memory-dll", type=Path)
    parser.add_argument("--vanilla-executable", type=Path,
                        help="explicit native compatibility executable, for example an existing T1Vista.exe")
    parser.add_argument("--installed-assets", type=Path,
                        help="opt in to full installed startup and mission/EndGame validation")
    parser.add_argument("--mission", default="Raindance")
    parser.add_argument("--mod", help="installed loose-script mod directory, for example RPG")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=12)
    parser.add_argument("--scenarios", nargs="+", default=["orderly", "cancel_accept", "cancel_ack"],
                        choices=["orderly", "cancel_request", "cancel_accept", "cancel_ack",
                                 "kick", "shutdown", "timeout"])
    parser.add_argument("--pairings", nargs="+", default=["vv", "vo", "ov", "oo"],
                        choices=["vv", "vo", "ov", "oo"],
                        help="client then server: v=vanilla, o=OpenT1")
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("live native 1.11 processes require Windows")
    if args.timeout <= 0 or args.timeout > 120:
        parser.error("--timeout must be between 0 and 120 seconds")
    if args.installed_assets and args.scenarios != ["orderly"]:
        parser.error("--installed-assets currently supports --scenarios orderly")
    if args.mod and (not args.installed_assets or Path(args.mod).name != args.mod or args.mod in (".", "..")):
        parser.error("--mod requires --installed-assets and a single child directory name")
    for field in ("opent1", "vanilla", "memory_dll", "vanilla_executable", "output", "installed_assets"):
        if getattr(args, field) is not None:
            setattr(args, field, getattr(args, field).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = {"opent1_sha256": digest(args.opent1),
                "vanilla_sha256": digest(args.vanilla_executable or args.vanilla / "Tribes.exe"),
                "native_binary": args.vanilla_executable.name if args.vanilla_executable else "Tribes.exe",
                "memory_sha256": digest(args.memory_dll or args.vanilla / "mem.dll"),
                "scope": ("installed startup, mission ghosts and scripted EndGame; no native input/GPU/internet claim"
                          if args.installed_assets else
                          "minimal native delegates; no mission, UI, GPU, or internet claims"),
                "os": platform.platform(),
                "installed_content": {"scripts_sha256": digest(args.installed_assets / "base/scripts.vol"),
                                      "base_tree": content_digest(args.installed_assets / "base"),
                                      "mod_tree": content_digest(args.installed_assets / args.mod) if args.mod else None,
                                      "mod": args.mod,
                                      "mission": args.mission} if args.installed_assets else None,
                "results": []}
    for pairing in args.pairings:
        client, server = [{"v": "vanilla", "o": "opent1"}[side] for side in pairing]
        for scenario in args.scenarios:
            result = run_case(args, server, client, scenario)
            metadata["results"].append(result)
            print(json.dumps(result), flush=True)
            (args.output / "summary.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return int(any(result["status"] == "fail" for result in metadata["results"]))


if __name__ == "__main__":
    raise SystemExit(main())
