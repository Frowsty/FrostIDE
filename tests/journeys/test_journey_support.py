# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the journey helpers in j1_cmake.py (FND-7a) that need no frost, corpus or tool: the
# evidence file (format, verdicts, atomic write), the spawn-log reading the evidence cites (tools and
# their sha256, an unparsable line kept as such), the `cmake --list-presets` parser behind the
# presets oracle, the caret search and file URIs. The journey itself runs in the corpus tier.

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import j1_cmake  # noqa: E402


class EvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="frost-journey-")
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_the_evidence_records_each_step_and_is_ok_only_when_every_step_is(self) -> None:
        evidence = j1_cmake.Evidence(self.tmp / "out" / "j1.json")
        self.assertTrue(evidence.step("first", ok=True, oracle={"line": 3}, observed={"line": 3}, seconds=0.25))
        self.assertFalse(evidence.step("second", ok=False, needs="CPP-3", note="why"))
        evidence.write()
        data = json.loads((self.tmp / "out" / "j1.json").read_text(encoding="utf-8"))
        self.assertEqual(data["format"], j1_cmake.EVIDENCE_FORMAT)
        self.assertEqual(data["journey"], "j1_cmake")
        self.assertFalse(data["ok"])
        self.assertEqual([s["name"] for s in data["steps"]], ["first", "second"])
        self.assertEqual(data["steps"][0]["durationMs"], 250)
        self.assertEqual(data["steps"][0]["oracle"], {"line": 3})
        self.assertEqual(data["steps"][1]["needsEpic"], "CPP-3")
        self.assertNotIn("needsEpic", data["steps"][0])
        self.assertFalse((self.tmp / "out" / "j1.partial").exists(), "written through a temporary file")

    def test_an_evidence_file_without_steps_is_not_ok(self) -> None:
        evidence = j1_cmake.Evidence(self.tmp / "j1.json")
        evidence.write()
        self.assertFalse(json.loads((self.tmp / "j1.json").read_text(encoding="utf-8"))["ok"])

    def test_tools_come_from_the_spawn_log_start_records(self) -> None:
        log = self.tmp / "spawn-20261108T000000.000-1-1.jsonl"
        records = [
            {"event": "start", "id": 1, "executable": "C:\\sdk\\bin\\clangd.exe", "sha256": "aa", "argv": []},
            {"event": "exit", "id": 1, "exitCode": 0},
            {"event": "start", "id": 2, "executable": "C:\\sdk\\bin\\CLANGD.exe", "sha256": "aa", "argv": []},
            {"event": "launch-failed", "id": 3, "executable": "C:\\x\\cmake.exe"},
        ]
        log.write_text("\n".join(json.dumps(r) for r in records) + "\n{broken\n", encoding="utf-8")
        read = j1_cmake.spawn_records(self.tmp)
        self.assertEqual(len(read), 5)
        self.assertEqual(read[-1]["event"], "unparsable")
        tools = j1_cmake.tools_seen(read)
        self.assertEqual(list(tools), ["clangd.exe"])
        self.assertEqual(tools["clangd.exe"]["starts"], 2)
        self.assertEqual(tools["clangd.exe"]["sha256"], "aa")


class OracleParsingTest(unittest.TestCase):
    def test_list_presets_output_is_split_into_its_sections(self) -> None:
        text = ('Available configure presets:\n\n  "windows-debug"   - Windows Debug\n  "windows-release"\n\n'
                'Available build presets:\n\n  "windows-debug" - Debug\n\nAvailable test presets:\n\n')
        self.assertEqual(j1_cmake.parse_listed_presets(text), {
            "configure": ["windows-debug", "windows-release"], "build": ["windows-debug"], "test": []})
        self.assertEqual(j1_cmake.parse_listed_presets(""), {})

    def test_the_caret_is_the_first_occurrence_one_based(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "a.cpp"
            source.write_text("int a;\n\tFoo<Bar> x; Bar y;\n", encoding="utf-8")
            self.assertEqual(j1_cmake.caret_of(source, "Bar"), (2, 6))
            with self.assertRaises(j1_cmake.EndpointError):
                j1_cmake.caret_of(source, "Missing")

    def test_file_uris_become_native_paths(self) -> None:
        path = Path(__file__).resolve()
        self.assertTrue(j1_cmake.same_path(j1_cmake.uri_to_path(path.as_uri()), path))
        self.assertTrue(j1_cmake.is_under(path, path.parent))
        self.assertFalse(j1_cmake.is_under(path.parent, path))


if __name__ == "__main__":
    unittest.main()
