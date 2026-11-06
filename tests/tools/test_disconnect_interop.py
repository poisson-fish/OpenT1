"""Check the header-only evidence decoder and live relay ordering without assets."""
import importlib.util
from pathlib import Path
import socket
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    "disconnect_interop", Path(__file__).resolve().parents[2] / "tools/run_disconnect_interop.py")
interop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(interop)


class DisconnectInteropTests(unittest.TestCase):
    def test_exported_counters_are_independent_of_console_line_delivery(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.cs"
            path.write_text('$Probe::Ghosts = "2";\n$probe::Endgames = 2;\n$Other = "99";\n')
            self.assertEqual(interop.read_state(path), {"ghosts": 2, "endgames": 2})
            self.assertEqual(interop.read_state(path.with_name("missing.cs")), {})

    def test_loose_mod_hooks_preserve_the_mod_implementation(self):
        base = '''function Server::onClientConnect(%id) { echo("base"); }
                  function Server::onClientDisconnect(%id) { echo("base"); }'''
        fake_audit = SimpleNamespace(read_volume=lambda _: ({}, {"server.cs": base}))
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.dict(sys.modules, {"audit_legacy_assets": fake_audit}):
            root = Path(directory)
            (root / "RPG").mkdir()
            (root / "RPG/connectivity.cs").write_text(base.replace('"base"', '"mod"'))
            script = interop.installed_probe(root, "server", 1, 2, "RPG")
            self.assertIn('function ProbeOriginalConnect(%id) { echo("mod"); }', script)
            self.assertNotIn('echo("base")', script)
            (root / "RPG/ambiguous.cs").write_text(base)
            with self.assertRaises(ValueError):
                interop.installed_probe(root, "server", 1, 2, "RPG")

    def test_installed_wrappers_preserve_bodies_and_ignore_quoted_braces(self):
        source = '''function Server::hook(%id) {
            echo("escaped \\\" } text"); // } ignored
            /* { ignored */
            if (%id) { echo(%id); }
        }
        function Next() { echo("unrelated"); }
        '''
        extracted = interop.renamed_function(source, "Server::hook", "OriginalHook")
        self.assertTrue(extracted.startswith("function OriginalHook(%id)"))
        self.assertIn('if (%id) { echo(%id); }', extracted)
        self.assertNotIn("function Next", extracted)
        with self.assertRaises(ValueError):
            interop.renamed_function(source + source, "Server::hook", "OriginalHook")
        with self.assertRaises(ValueError):
            interop.renamed_function("function Hook() {", "Hook", "OriginalHook")

    def test_known_control_headers_and_malformed_packets(self):
        request = interop.header(bytes.fromhex("07001801000000"))
        self.assertEqual((request["type"], request["sequence"], request["generation"]),
                         ("Request", 1, 1))
        accept = interop.header(bytes.fromhex("07000960010000000108000000080000"))
        self.assertEqual((accept["type"], accept["flags"], accept["generation"], accept["acks"]),
                         ("Accept", 12, 1, [[1, 1]]))
        for packet in (b"", b"\x01", b"\xff" * 64):
            self.assertEqual(interop.header(packet)["type"], "Invalid")
        self.assertEqual(interop.header(b"\x00")["type"], "Raw")
        self.assertNotIn("payload", accept)

    def test_old_acceptance_released_after_replacement_request(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(server.close)
        self.addCleanup(client.close)
        for endpoint in (server, client):
            endpoint.bind(("127.0.0.1", 0))
            endpoint.settimeout(2)
        relay = interop.Relay(server.getsockname()[1], "cancel_accept")
        self.addCleanup(relay.close)
        address = ("127.0.0.1", relay.port)
        request = bytes.fromhex("07001801000000")
        client.sendto(request, address)
        self.assertEqual(server.recvfrom(2048)[0], request)
        accept = bytes.fromhex("07000960010000000108000000080000")
        server.sendto(accept, address)
        disconnect = bytes.fromhex("0b001001000000")
        client.sendto(disconnect, address)
        self.assertEqual(server.recvfrom(2048)[0], disconnect)
        replacement = bytes.fromhex("07001803000000")
        client.sendto(replacement, address)
        self.assertEqual(server.recvfrom(2048)[0], replacement)
        self.assertEqual(client.recvfrom(2048)[0], accept)
        actions = [packet["action"] for packet in relay.trace]
        self.assertIn("hold_accept", actions)
        self.assertEqual(actions[-1], "release_old")
        self.assertFalse(any("payload" in packet for packet in relay.trace))


if __name__ == "__main__":
    unittest.main()
