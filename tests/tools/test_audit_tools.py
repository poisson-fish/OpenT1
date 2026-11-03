"""Asset-free regression tests for the diagnostic Python command-line tools."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


TOOLS = Path(__file__).resolve().parents[2] / "tools"


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


compare = load_tool("compare_render_readbacks")
runner = load_tool("run_memory_audit")
summarizer = load_tool("summarize_memory_audit")


def invoke(module, arguments):
    with mock.patch.object(sys, "argv", [module.__file__, *map(str, arguments)]), \
         contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        module.main()


class AuditToolsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def readbacks(self):
        reference, candidate = self.root / "reference", self.root / "candidate"
        reference.mkdir()
        candidate.mkdir()
        return reference, candidate

    def test_empty_reference_rejected(self):
        reference, candidate = self.readbacks()
        with self.assertRaises(SystemExit) as error:
            invoke(compare, [reference, candidate])
        self.assertNotEqual(error.exception.code, 0)
        self.assertFalse((candidate / "readback-comparisons.json").exists())

    def test_missing_candidate_rejected_before_partial_report(self):
        for matching_first in (False, True):
            with self.subTest(matching_first=matching_first):
                root = self.root / str(matching_first)
                reference, candidate = root / "reference", root / "candidate"
                reference.mkdir(parents=True)
                candidate.mkdir()
                (reference / "missing.json.ppm").write_bytes(b"incomplete")
                if matching_first:
                    (reference / "first.json.ppm").write_bytes(b"incomplete")
                    (candidate / "first.json.ppm").write_bytes(b"incomplete")
                with self.assertRaises(SystemExit) as error:
                    invoke(compare, [reference, candidate])
                self.assertNotEqual(error.exception.code, 0)
                self.assertFalse((candidate / "readback-comparisons.json").exists())

    def test_complete_readbacks_report_pixel_differences(self):
        reference, candidate = self.readbacks()
        pixels = bytes(800 * 600 * 3)
        header = b"P6\n800 600\n255\n"
        for name in ("same", "changed"):
            (reference / (name + ".json.ppm")).write_bytes(header + pixels)
            other = pixels if name == "same" else b"\3" + pixels[1:]
            (candidate / (name + ".json.ppm")).write_bytes(header + other)
        invoke(compare, [reference, candidate])
        report = json.loads((candidate / "readback-comparisons.json").read_text())
        self.assertEqual(len(report), 2)
        self.assertEqual(report["same.json.ppm"]["changed_pixels"], 0)
        self.assertEqual(report["changed.json.ppm"]["changed_pixels"], 1)
        self.assertEqual(report["changed.json.ppm"]["pixels_over_2"], 1)
        self.assertTrue((candidate / "changed.json.difference.png").is_file())

    def test_failed_partial_captures_continue_and_write_provenance(self):
        base, rpg, output = (self.root / name for name in ("base", "rpg", "output"))
        (base / "base").mkdir(parents=True)
        (base / "base" / "Good.mis").touch()
        executable = self.root / "audit.exe"
        executable.write_bytes(b"fake executable")
        calls = []

        def capture(command, **kwargs):
            calls.append(command)
            path = Path(command[3] if command[1] == "catalog" else command[4])
            if len(calls) < 3:
                path.write_text('{"kind": "snapshot",')
                kwargs["stderr"].write("capture failed")
                if len(calls) == 2:
                    raise subprocess.TimeoutExpired(command, 120)
                return subprocess.CompletedProcess(command, 1)
            path.write_text(json.dumps({"kind": "snapshot", "stage": "simulation_warm"}) + "\n")
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner.subprocess, "run", side_effect=capture):
            invoke(runner, ["--executable", executable, "--base", base, "--rpg", rpg,
                            "--output", output, "--all-missions"])
        cases = json.loads((output / "runs.json").read_text())
        self.assertEqual([c["returncode"] for c in cases], [1, "timeout", 0])
        self.assertNotIn("snapshots", cases[0])
        self.assertNotIn("snapshots", cases[1])
        self.assertEqual(cases[2]["snapshots"][0]["stage"], "simulation_warm")
        self.assertEqual((output / "rpg-catalog.stderr.log").read_text(), "capture failed")
        self.assertEqual(len(json.loads((output / "provenance.json").read_text())["executable_sha256"]), 64)

    def summarize(self, extra_cases=()):
        cases = [{"label": label + "-catalog", "returncode": 0} for label in ("base", "rpg")]
        for case in cases:
            (self.root / (case["label"] + ".jsonl")).write_text("")
        (self.root / "runs.json").write_text(json.dumps([*cases, *extra_cases]))
        (self.root / "provenance.json").write_text(json.dumps({"base": "private", "rpg": "private"}))
        output = self.root / "summary.json"
        invoke(summarizer, [self.root, output, "--date", "2026-11-03"])
        return json.loads(output.read_text())

    def assert_empty_aggregates(self, summary):
        for key in ("minimum_cpp_warm_bytes", "median_cpp_warm_bytes", "maximum_cpp_warm_bytes"):
            self.assertIsNone(summary["base_missions"][key])

    def test_catalog_only_summary(self):
        summary = self.summarize()
        self.assert_empty_aggregates(summary)
        self.assertEqual(summary["base_missions"]["attempted"], 0)
        self.assertEqual(set(summary["catalogs"]), {"base", "rpg"})
        self.assertEqual(summary["provenance"], {})

    def test_gpu_only_summary(self):
        case = {"label": "base-Raindance-gpu", "returncode": 0, "devices": [],
                "snapshots": [{"stage": "before_shutdown", "cpp_live_bytes": 10,
                               "cpp_peak_bytes": 20, "process_private_bytes": 30}],
                "gpu": [{"rgba_bytes": 40, "textures": 1, "scene_polygons": 2}]}
        summary = self.summarize([case])
        self.assert_empty_aggregates(summary)
        self.assertTrue(summary["missions"][0]["gpu"]["completed"])
        self.assertEqual(summary["missions"][0]["gpu"]["maximum_rgba_bytes"], 40)

    def test_all_failed_missions_preserve_errors(self):
        cases = [{"label": "base-Broken", "returncode": 1},
                 {"label": "base-Timeout", "returncode": "timeout"}]
        for case in cases:
            (self.root / (case["label"] + ".stderr.log")).write_text("capture failed")
            (self.root / (case["label"] + ".jsonl")).write_text('{"kind":')
        summary = self.summarize(cases)
        self.assert_empty_aggregates(summary)
        self.assertEqual(summary["base_missions"]["attempted"], 2)
        self.assertEqual(summary["base_missions"]["loaded"], 0)
        self.assertTrue(all(c["error"] == "capture failed" for c in summary["missions"]))

    def test_failed_catalog_preserves_error_without_parsing_partial_jsonl(self):
        self.summarize()
        cases = json.loads((self.root / "runs.json").read_text())
        cases[0]["returncode"] = "timeout"
        (self.root / "runs.json").write_text(json.dumps(cases))
        (self.root / "base-catalog.jsonl").write_text('{"kind":')
        (self.root / "base-catalog.stderr.log").write_text("capture failed")
        output = self.root / "summary.json"
        invoke(summarizer, [self.root, output, "--date", "2026-11-03"])
        summary = json.loads(output.read_text())
        self.assert_empty_aggregates(summary)
        self.assertEqual(summary["catalogs"]["base"]["returncode"], "timeout")
        self.assertEqual(summary["catalogs"]["base"]["error"], "capture failed")
        self.assertEqual(summary["catalogs"]["rpg"]["asset_entries"], 0)


if __name__ == "__main__":
    unittest.main()
