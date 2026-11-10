# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the journey helpers in j1_cmake.py (FND-7a) that need no frost, corpus or tool: the
# evidence file (format, verdicts, atomic write), the spawn-log reading the evidence cites (tools and
# their sha256, an unparsable line kept as such), the `cmake --list-presets` parser behind the
# presets oracle, the caret search and file URIs; and j2_msvc.py's (FND-7b) parsers of MSBuild's
# file-logger lines and of the .sln/.slnx project configuration mappings. The journeys themselves
# run in the integration and corpus tiers.

from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import j1_cmake  # noqa: E402
import j2_msvc  # noqa: E402


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

    def test_tool_names_split_at_either_separator_on_any_os(self) -> None:
        # A Windows log read on Linux and a Linux log read on Windows give the same base names.
        records = [
            {"event": "start", "id": 1, "executable": "/home/u/sdk/bin/ninja", "sha256": "bb"},
            {"event": "start", "id": 2, "executable": "F:/tools/cmake/bin/cmake.exe", "sha256": "cc"},
            {"event": "start", "id": 3, "executable": "F:\\tools\\cmake\\bin\\ctest.exe", "sha256": "dd"},
        ]
        self.assertEqual(list(j1_cmake.tools_seen(records)), ["ninja", "cmake.exe", "ctest.exe"])


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

    def test_the_preset_oracle_compares_every_preset_variable_and_the_generator(self) -> None:
        # The cache `cmake --preset` writes before the project runs (shape of a real one, FrostEngine's
        # windows-debug) against a configure command line from the spawn log.
        cache = j1_cmake.parse_cache(
            "# This is the CMakeCache file.\n// comment\nCMAKE_BUILD_TYPE:UNINITIALIZED=Debug\n"
            "CMAKE_CXX_COMPILER:UNINITIALIZED=cl\nCPM_SOURCE_CACHE:UNINITIALIZED=E:/c/.cache/cpm\n"
            "CMAKE_EXPORT_COMPILE_COMMANDS:UNINITIALIZED=ON\nCMAKE_COMMAND:INTERNAL=F:/tools/cmake.exe\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_GENERATOR_TOOLSET:INTERNAL=\nCMAKE_GENERATOR_PLATFORM:INTERNAL=\n")
        self.assertEqual(cache["CMAKE_BUILD_TYPE"], ("UNINITIALIZED", "Debug"))
        self.assertEqual(cache["CMAKE_GENERATOR"], ("INTERNAL", "Ninja"))
        argv = ["cmake.exe", "-S", "E:\\c", "-B", "E:\\c\\build\\windows-debug", "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_MAKE_PROGRAM:FILEPATH=F:\\tools\\ninja.exe", "-D",
                "CMAKE_CXX_COMPILER=cl", "-DCPM_SOURCE_CACHE=E:/c/.cache/cpm", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        switches = j1_cmake.configure_switches(argv)
        self.assertEqual(switches["binaryDir"], "E:\\c\\build\\windows-debug")
        self.assertEqual(switches["-DCMAKE_MAKE_PROGRAM"], "F:\\tools\\ninja.exe")
        self.assertEqual(switches["-DCMAKE_CXX_COMPILER"], "cl")
        self.assertEqual(j1_cmake.preset_differences(cache, switches), {})
        # A preset variable left out, a different value, another generator or a toolset are differences;
        # extra switches (the pinned Ninja) are not.
        missing = j1_cmake.preset_differences(cache, j1_cmake.configure_switches(argv[:-2] + argv[-1:]))
        self.assertEqual(list(missing), ["-DCPM_SOURCE_CACHE"])
        changed = j1_cmake.configure_switches([a.replace("=Debug", "=Release") for a in argv])
        self.assertEqual(j1_cmake.preset_differences(cache, changed)["-DCMAKE_BUILD_TYPE"],
                         {"cmake --preset": "Debug", "FrostIDE": "Release"})
        other = j1_cmake.configure_switches([("Unix Makefiles" if a == "Ninja" else a) for a in argv] + ["-T", "v143"])
        self.assertEqual(set(j1_cmake.preset_differences(cache, other)), {"generator", "toolset"})

    def test_ninja_edges_drop_the_counters_and_the_housekeeping_and_sort(self) -> None:
        output = ("[1/3] Re-checking globbed directories...\n[2/3] Linking CXX executable bin\\Child.exe\n"
                  "ninja: no work to do.\n[3/3] Building CXX object Main.cpp.obj   \n[1/1] Re-running CMake...\n")
        self.assertEqual(j1_cmake.ninja_edges(output),
                         ["Building CXX object Main.cpp.obj", "Linking CXX executable bin\\Child.exe"])
        self.assertEqual(j1_cmake.ninja_edges("ninja: no work to do.\n"), [])


class MsvcOracleParsingTest(unittest.TestCase):
    FIXTURE = j1_cmake.REPO / "tests" / "project" / "fixtures" / "msvc_generated"

    def test_a_file_logger_line_becomes_the_error_lists_row(self) -> None:
        line = (r"  C:\w\App\app.cpp(1,31): error C2065: 'frost_journey_undeclared_identifier': undeclared "
                r"identifier [C:\w\App\App.vcxproj]")
        self.assertEqual(j2_msvc.parse_logged(line),
                         (j2_msvc.path_key(r"C:\w\App\app.cpp"), 1, 31, "error", "C2065",
                          "'frost_journey_undeclared_identifier': undeclared identifier"))
        warning = r"C:\w\Renderer\Renderer.vcxproj : warning MSB8065: Custom build for item ... [C:\w\x.vcxproj]"
        parsed = j2_msvc.parse_logged(warning)
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed[3:5], ("warning", "MSB8065"))
        self.assertIsNone(j2_msvc.parse_logged("Build succeeded."))
        # A Problems row in the endpoint's shape (src/app/src/automation_endpoint.cpp) compares equal;
        # live (clangd) rows are not the build's.
        row = {"source": "build", "severity": "error", "file": r"C:\w\App\app.cpp", "line": 1, "column": 31,
               "code": "C2065", "message": "'frost_journey_undeclared_identifier': undeclared identifier"}
        live = dict(row, source="live")
        self.assertEqual(j2_msvc.shown_rows([row, live]), [j2_msvc.parse_logged(line)])

    def test_the_sln_and_its_slnx_twin_map_every_configuration_alike(self) -> None:
        # Generated.slnx is Generated.sln's twin (SLN-0): both parsers must give each project the
        # same project configuration for every solution configuration the .sln lists.
        sln, slnx = self.FIXTURE / "Generated.sln", self.FIXTURE / "Generated.slnx"
        projects = j2_msvc.sln_projects(sln)
        self.assertEqual(sorted(projects), ["App", "Core", "Renderer"])
        text = sln.read_text(encoding="utf-8-sig")
        configurations = sorted(set(re.findall(r"^\s*([A-Za-z]+\|[A-Za-z0-9]+) = \1\s*$", text, re.M)))
        self.assertIn("Debug|x64", configurations)
        for configuration in configurations:
            twin = j2_msvc.slnx_projects(slnx, configuration)
            for name, info in projects.items():
                with self.subTest(configuration=configuration, project=name):
                    self.assertTrue(j1_cmake.same_path(twin[name]["path"], info["path"]))
                    self.assertEqual(twin[name]["mapping"], j2_msvc.sln_mapping(sln, info["guid"], configuration))
        self.assertEqual(j2_msvc.sln_mapping(sln, projects["Core"]["guid"], "Profile|x64"), ("Release", "x64"))

    def test_the_debuggable_copy_links_fully_and_compiles_od_before_the_targets_import(self) -> None:
        # j2_msvc's scratch App (ADR 0081 item 8): the overrides sit just before Microsoft.Cpp.targets,
        # which reads them, and the file keeps its line endings and everything else.
        original = (self.FIXTURE / "App" / "App.vcxproj").read_bytes()
        with tempfile.TemporaryDirectory(prefix="frost-journey-") as tmp:
            for newline in (b"\n", b"\r\n"):
                project = Path(tmp) / "App.vcxproj"
                project.write_bytes(original.replace(b"\r\n", b"\n").replace(b"\n", newline))
                before = project.read_bytes()
                j2_msvc.debuggable(project)
                after = project.read_bytes()
                targets = after.index(b"Microsoft.Cpp.targets")
                for setting in (b"<LinkIncremental>false</LinkIncremental>", b"<Optimization>Disabled</Optimization>",
                                b"<InlineFunctionExpansion>Disabled</InlineFunctionExpansion>"):
                    self.assertLess(after.index(setting), targets)
                self.assertEqual(after.count(b"\r\n"), after.count(b"\n") if newline == b"\r\n" else 0)
                start = after.index(b"  <PropertyGroup>" + newline + b"    <LinkIncremental>")
                end = after.index(b'  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets')
                self.assertEqual(after[:start] + after[end:], before)  # Only the block was added.


if __name__ == "__main__":
    unittest.main()
