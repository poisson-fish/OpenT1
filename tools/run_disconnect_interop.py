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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
    client += f'''function BeginJoin() {{ connect("IP:127.0.0.1:{relay_port}"); }}
$Cycles = {1 if scenario in ('cancel_request', 'cancel_accept') else 3};
BeginJoin();
'''
    return server, client


def read_log(path):
    try:
        return path.read_text(errors="replace")
    except (FileNotFoundError, PermissionError):
        return ""


def launch(kind, directory, args):
    output = (directory / "process.log").open("w")
    if kind == "vanilla":
        command = [str(directory / "Tribes.exe"), "-dedicated"]
    else:
        command = [str(args.opent1), "--headless", "--no-audio", "--no-assets", "-dedicated",
                   "+exec", str(directory / "console.cs")]
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
    server_script, client_script = scripts(server_kind, client_kind, port, relay.port, scenario)
    for side, kind, script in (("server", server_kind, server_script),
                               ("client", client_kind, client_script)):
        target = directory / side
        target.mkdir()
        (target / "console.cs").write_text(script)
        if kind == "vanilla":
            for filename in ("Tribes.exe", "smackw32.dll"):
                shutil.copy2(args.vanilla / filename, target / filename)
            shutil.copy2(args.memory_dll or args.vanilla / "mem.dll", target / "mem.dll")
    processes = []
    start = time.monotonic()
    try:
        processes.append(launch(server_kind, directory / "server", args))
        deadline = time.monotonic() + args.timeout
        while "LIFECYCLE READY" not in read_log(directory / "server/console.log"):
            if processes[0].poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("server did not initialize")
            time.sleep(.05)
        # Native startup continues after console.cs. Let its main loop start;
        # later assertions use events, never this delay as evidence of a join.
        time.sleep(1)
        processes.append(launch(client_kind, directory / "client", args))
        cancellation_sent = False
        first_drop_ms = None
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
            if ("LIFECYCLE DONE" in client and rosters and rosters[-1] == "0" and
                    server.count("LIFECYCLE JOIN=") == server.count("LIFECYCLE DROP=")):
                break
            if any(p.poll() is not None for p in processes):
                break
            time.sleep(.05)
        client = read_log(directory / "client/console.log")
        server = read_log(directory / "server/console.log")
        rosters = re.findall(r"^LIFECYCLE ROSTER=(\d+)", server, re.MULTILINE)
        accepted = client.count("LIFECYCLE STATE=Accepted")
        joins = server.count("LIFECYCLE JOIN=")
        drops = server.count("LIFECYCLE DROP=")
        result = {"name": name, "client": client_kind, "server": server_kind,
                  "scenario": scenario, "accepted": accepted, "joins": joins, "drops": drops,
                  "empty_roster": bool(rosters and rosters[-1] == "0"),
                  "done": "LIFECYCLE DONE" in client,
                  "elapsed_s": round(time.monotonic() - start, 3)}
        departures = [packet for packet in relay.trace if packet["direction"] == "c2s" and
                       packet["type"] == "Disconnect"]
        result["disconnects"] = len(departures)
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
        cycles = 1 if scenario in ("cancel_request", "cancel_accept") else 3
        result["status"] = "pass" if (result["done"] and result["empty_roster"] and
                                         accepted == cycles and joins >= cycles and joins == drops and
                                         result["names_preserved"]) else "fail"
        return result
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
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=12)
    parser.add_argument("--scenarios", nargs="+", default=["orderly", "cancel_accept", "cancel_ack"],
                        choices=["orderly", "cancel_request", "cancel_accept", "cancel_ack"])
    parser.add_argument("--pairings", nargs="+", default=["vv", "vo", "ov", "oo"],
                        choices=["vv", "vo", "ov", "oo"],
                        help="client then server: v=vanilla, o=OpenT1")
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("live native 1.11 processes require Windows")
    if args.timeout <= 0 or args.timeout > 120:
        parser.error("--timeout must be between 0 and 120 seconds")
    for field in ("opent1", "vanilla", "memory_dll", "output"):
        if getattr(args, field) is not None:
            setattr(args, field, getattr(args, field).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = {"opent1_sha256": digest(args.opent1),
                "vanilla_sha256": digest(args.vanilla / "Tribes.exe"),
                "memory_sha256": digest(args.memory_dll or args.vanilla / "mem.dll"),
                "scope": "minimal native delegates; no mission, UI, GPU, or internet claims",
                "os": platform.platform(),
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
