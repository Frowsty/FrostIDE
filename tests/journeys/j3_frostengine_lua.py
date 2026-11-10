# SPDX-License-Identifier: Apache-2.0
#
# Journey j3_frostengine_lua (FND-7c, FEC-10b, LUA-3, LUA-4, LUA-6; PLAN.md section 7 R4): the real
# FrostIDE (Debug, offscreen, on a fresh profile) driven through its automation endpoint
# (src/app/src/automation_endpoint.hpp, ADR 0080) against the pinned FrostEngine corpus with the
# pinned lua-language-server and the corpus's built FrostEditor binary. Every expected value comes
# from an oracle run here, never from a hardcoded constant:
#   1. FrostEditor schema.export runs on the pinned clone and provides the oracle schema (component
#      fields and methods, Lua API entries and C++ source_location file:line);
#   2. FrostIDE opens and trusts the corpus and starts the pinned lua-language-server with the
#      generated frost_schema.lua meta stub outside the corpus tree;
#   3. Engine-API completion items on a component variable equal the entries schema.export lists
#      for that component (Transform and Rigidbody2D);
#   4. Go to Definition on an engine call (Physics.Raycast) opens the generated frost_schema.lua
#      meta stub at its declaration;
#   5. Go to C++ Binding on Physics.Raycast opens the exact C++ file and 1-based line from
#      schema.export's source_location;
#   6. Creating a new Lua module in the clone makes require() resolve to it via Go to Definition
#      without restarting LuaLS, and changing lua.runtimeVersion sends workspace/didChangeConfiguration
#      live without a restart;
#   7. Inspect Code on TestProject/Assets/Scripts equals the output of running the pinned
#      lua-language-server --check directly on that folder;
#   8. Freeze reports over the journey show no UI-thread stall above 250 ms (FND-9b).

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import j1_cmake as j1  # noqa: E402  (the shared journey machinery, beside this file)
from j1_cmake import REPO, WINDOWS, corpora, is_under, same_path, spawn_records  # noqa: E402

JOURNEY = "j3_frostengine_lua"
j1.JOURNEY = JOURNEY

LUA_WAIT_SECONDS = 120
COMPLETE_SECONDS = 60
NAVIGATE_SECONDS = 60
POLL_SECONDS = 0.25


def sanitize_identifier(raw: str, fallback: str = "_") -> str:
    """Sanitizes `raw` to a valid Lua identifier, matching frost::engine::sanitizeIdentifier."""
    out = "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in raw)
    if not out:
        return fallback
    if out[0].isdigit():
        out = "_" + out
    return out


def schema_component_entry_names(schema: dict[str, Any], component_name: str) -> list[str]:
    """Returns the sorted unique property and method names `schema` declares for `component_name`."""
    sanitized_target = sanitize_identifier(component_name)
    components = schema.get("components", [])
    lua_entries = (schema.get("lua") or {}).get("entries", [])
    comp = next(
        (
            c
            for c in components
            if c.get("name") == component_name
            or sanitize_identifier(c.get("name", ""), "Component") in (component_name, sanitized_target)
        ),
        None,
    )
    if comp is None:
        return []
    comp_name = comp.get("name", "")
    sanitized_comp = sanitize_identifier(comp_name, "Component")

    def matches_owner(entry: dict[str, Any]) -> bool:
        owner = entry.get("owner", "")
        scope = entry.get("scope", "")
        if scope != "Component" and owner not in (comp_name, sanitized_comp):
            return False
        return owner in (comp_name, sanitized_comp) or sanitize_identifier(owner) == sanitized_comp

    names: list[str] = []
    seen_props: set[str] = set()
    for field in comp.get("fields", []):
        fname = field.get("name", "")
        if fname and fname not in seen_props:
            seen_props.add(fname)
            names.append(fname)
    for entry in lua_entries:
        if not matches_owner(entry):
            continue
        kind = entry.get("kind", "")
        ename = entry.get("name", "")
        if kind == "Property" and ename and ename not in seen_props:
            seen_props.add(ename)
            names.append(ename)
        elif kind == "Method" and ename:
            names.append(ename)
    return sorted(set(names))


def find_frost_editor_binary(corpus: Path, preset: str) -> Path | None:
    """Locates the built FrostEditor CLI binary under `<corpus>/build/<preset>/bin`."""
    bin_dir = corpus / "build" / preset / "bin"
    candidates = ("FrostEditor.com", "FrostEditor.exe") if WINDOWS else ("FrostEditor",)
    for name in candidates:
        candidate = bin_dir / name
        if candidate.is_file():
            return candidate
    return None


def ensure_frost_editor(corpus: Path, preset: str, cmake: str) -> Path:
    """Returns the built FrostEditor binary in `<corpus>/build/<preset>/bin`, building it if needed."""
    existing = find_frost_editor_binary(corpus, preset)
    if existing is not None:
        return existing
    build_dir = corpus / "build" / preset
    env = corpora.msvc_environment(REPO) if WINDOWS else dict(os.environ)
    result = subprocess.run(
        [cmake, "--build", str(build_dir), "--target", "FrostEditor"],
        cwd=str(corpus),
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=3 * 3600,
        check=False,
    )
    if result.returncode != 0:
        tail = (result.stdout + "\n" + result.stderr)[-4000:]
        raise RuntimeError(f"cmake --build {build_dir} --target FrostEditor failed ({result.returncode}):\n{tail}")
    built = find_frost_editor_binary(corpus, preset)
    if built is None:
        raise RuntimeError(f"FrostEditor binary not found under {build_dir / 'bin'} after build")
    return built


def run_schema_export_oracle(editor_binary: Path, scratch_dir: Path) -> dict[str, Any]:
    """Runs `FrostEditor schema.export --params {} --log-level error` and returns its `result` object."""
    out = subprocess.run(
        [str(editor_binary), "schema.export", "--params", "{}", "--log-level", "error"],
        cwd=str(scratch_dir),
        capture_output=True,
        text=True,
        errors="replace",
        timeout=120,
        check=False,
    )
    if out.returncode != 0:
        raise RuntimeError(
            f"{editor_binary} schema.export exited with {out.returncode}: {(out.stdout + out.stderr)[:1000]}"
        )
    payload = json.loads(out.stdout)
    if not payload.get("ok") or not isinstance(payload.get("result"), dict):
        raise RuntimeError(f"schema.export did not return ok result: {out.stdout[:500]}")
    return payload["result"]


def find_pinned_luals() -> Path:
    """Locates the pinned lua-language-server binary from FROST_SDK (`luals-<version>/bin`) or PATH."""
    exe_name = "lua-language-server.exe" if WINDOWS else "lua-language-server"
    sdk_lock = json.loads((REPO / corpora.SDK_LOCK_FILE).read_text(encoding="utf-8"))
    leg = "windows" if WINDOWS else "linux"
    version = sdk_lock[leg]["luals"]["version"]
    sdk_root = os.environ.get("FROST_SDK", "")
    if sdk_root:
        for sub in (f"luals-{version}/bin", "luals/bin"):
            candidate = Path(sdk_root) / sub / exe_name
            if candidate.is_file():
                return candidate.resolve()
    which = shutil.which(exe_name)
    if which:
        return Path(which).resolve()
    raise RuntimeError(f"{exe_name} {version} not found in FROST_SDK ({sdk_root}) or PATH")


def uri_to_norm_path(uri: str) -> str:
    """Converts a `file://` URI from LuaLS `--check` into a normalized case-folded path string."""
    parsed = urllib.parse.urlparse(uri)
    raw_path = urllib.parse.unquote(parsed.path)
    if WINDOWS and re.match(r"^/[A-Za-z]:", raw_path):
        raw_path = raw_path[1:]
    return os.path.normcase(os.path.realpath(raw_path))


def run_luals_check_oracle(luals: Path, directory: Path, scratch_dir: Path) -> list[dict[str, Any]]:
    """Runs `lua-language-server --check` directly on `directory` and returns normalized findings."""
    log_dir = scratch_dir / "oracle-luals-log"
    meta_dir = scratch_dir / "oracle-luals-meta"
    out_file = scratch_dir / "oracle-check.json"
    log_dir.mkdir(parents=True, exist_ok=True)
    meta_dir.mkdir(parents=True, exist_ok=True)
    out_file.unlink(missing_ok=True)
    cmd = [
        str(luals),
        f"--logpath={log_dir}",
        f"--metapath={meta_dir}",
        f"--check={directory}",
        "--checklevel=Hint",
        "--check_format=json",
        f"--check_out_path={out_file}",
    ]
    completed = subprocess.run(
        cmd,
        cwd=str(directory),
        capture_output=True,
        text=True,
        errors="replace",
        timeout=180,
        check=False,
    )
    if not out_file.is_file():
        if completed.returncode == 0:
            return []
        raise RuntimeError(
            f"LuaLS --check failed ({completed.returncode}): {(completed.stdout + completed.stderr)[:1000]}"
        )
    text = out_file.read_text(encoding="utf-8").strip()
    if not text:
        return []
    raw = json.loads(text)
    if isinstance(raw, list):
        return []
    findings: list[dict[str, Any]] = []
    for uri, diags in raw.items():
        if not isinstance(diags, list) or not diags:
            continue
        file_key = uri_to_norm_path(uri)
        for diag in diags:
            start = (diag.get("range") or {}).get("start") or {}
            line = int(start.get("line", 0)) + 1
            col = int(start.get("character", 0)) + 1
            code = str(diag.get("code", ""))
            msg = str(diag.get("message", ""))
            findings.append({"file": file_key, "line": line, "column": col, "code": code, "message": msg})
    findings.sort(key=lambda d: (d["file"], d["line"], d["column"], d["code"], d["message"]))
    return findings


def normalize_ide_inspect_files(files: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Normalizes the `{files: [{file, diagnostics: [...]}]}` reply of `lua.inspect` for comparison."""
    findings: list[dict[str, Any]] = []
    for entry in files:
        file_key = os.path.normcase(os.path.realpath(entry.get("file", "")))
        for diag in entry.get("diagnostics", []):
            findings.append(
                {
                    "file": file_key,
                    "line": int(diag.get("line", 0)),
                    "column": int(diag.get("column", 0)),
                    "code": str(diag.get("code", "")),
                    "message": str(diag.get("message", "")),
                }
            )
    findings.sort(key=lambda d: (d["file"], d["line"], d["column"], d["code"], d["message"]))
    return findings


class Journey(j1.Journey):
    """Automated journey j3_frostengine_lua on the pinned FrostEngine clone."""

    def _main(self) -> int:
        args = self.args
        if args.config != "Debug":
            self.evidence.step(
                "setup", ok=False, note=f"the automation endpoint is in Debug builds only, not {args.config}"
            )
            return 1
        corpora_root = os.environ.get("FROST_CORPORA_ROOT", "")
        if not corpora_root:
            self.evidence.step("setup", ok=False, note="FROST_CORPORA_ROOT does not name the corpora root")
            return 1
        root = Path(corpora_root)
        lock = corpora.load_lock(REPO / corpora.LOCK_FILE)
        entry = next(item for item in lock["corpora"] if item["id"] == "frostengine")
        corpus = corpora.corpus_dir(root, entry)
        preset = corpora.engine_preset(entry, None)
        self.evidence.data["corpus"] = {
            "id": entry["id"],
            "sha": entry["sha"],
            "path": str(corpus),
            "preset": preset,
        }
        frost = Path(args.frost)
        self.evidence.data["frost"] = {
            "image": str(frost),
            "sha256": j1.sha256_file(frost) if frost.is_file() else None,
        }
        try:
            cmake = corpora.pinned_tool("cmake", REPO)
            ninja = corpora.pinned_tool("ninja", REPO)
            luals = find_pinned_luals()
        except RuntimeError as error:
            self.evidence.step("setup", ok=False, note=str(error))
            return 1
        problem = corpora.problem_or_none(entry, root) or corpora.engine_problem(
            corpus, entry, preset, {"cmake": cmake, "ninja": ninja}
        )
        if problem:
            self.evidence.step(
                "setup",
                ok=False,
                note=f"the pinned engine is not ready: {problem} (tools/corpora.py fetch; engine --configure)",
            )
            return 1

        self.scratch_files: list[Path] = []
        with corpora.BuildLock(corpora.build_lock_path(root, entry)):
            self.frost_dir_before = (corpus / ".frost").exists()
            try:
                editor_binary = ensure_frost_editor(corpus, preset, cmake)
            except RuntimeError as error:
                self.evidence.step("setup", ok=False, note=str(error))
                return 1
            self._launch(corpus)
            try:
                self._run_journey(corpus, preset, editor_binary, luals)
            finally:
                self._remove_scratch_files()
                self._quit()
        return 0 if all(step["ok"] for step in self.evidence.data["steps"]) else 1

    def _remove_scratch_files(self) -> None:
        """Removes any temporary Lua scripts written into the corpus during the journey."""
        for path in getattr(self, "scratch_files", []):
            path.unlink(missing_ok=True)
        self.scratch_files = []

    def _cleanup(self) -> None:
        self._remove_scratch_files()
        super()._cleanup()

    def _run_journey(self, corpus: Path, preset: str, editor_binary: Path, luals: Path) -> None:
        ep = self.endpoint
        project_root = corpus / "TestProject"
        scripts_dir = project_root / "Assets" / "Scripts"
        probe_path = scripts_dir / "frost_j3_probe.lua"
        require_path = scripts_dir / "frost_j3_require_probe.lua"
        live_module_path = scripts_dir / "frost_j3_live_module.lua"
        self.scratch_files.extend([probe_path, require_path, live_module_path])

        oracle_schema: dict[str, Any] = {}
        transform_expected: list[str] = []
        rigidbody_expected: list[str] = []
        raycast_entry: dict[str, Any] = {}

        def export_schema_oracle() -> tuple[bool, Any, Any, str]:
            nonlocal oracle_schema, transform_expected, rigidbody_expected, raycast_entry
            oracle_schema = run_schema_export_oracle(editor_binary, self.tmp)
            transform_expected = schema_component_entry_names(oracle_schema, "Transform")
            rigidbody_expected = schema_component_entry_names(oracle_schema, "Rigidbody2D")
            entries = (oracle_schema.get("lua") or {}).get("entries", [])
            raycast_entry = next((e for e in entries if e.get("qualifiedName") == "Physics.Raycast"), {})
            source = raycast_entry.get("source") or {}
            source_file = corpus / source.get("file", "") if source.get("file") else None
            ok = (
                oracle_schema.get("schemaVersion") == 1
                and bool(transform_expected)
                and bool(rigidbody_expected)
                and bool(source_file and source_file.is_file())
                and int(source.get("line", 0)) > 0
            )
            expected = {
                "schemaVersion": 1,
                "hasTransformEntries": True,
                "hasRigidbody2DEntries": True,
                "raycastSourceExists": True,
            }
            observed = {
                "schemaVersion": oracle_schema.get("schemaVersion"),
                "transformEntries": transform_expected,
                "rigidbody2DEntries": rigidbody_expected,
                "raycastSource": source,
            }
            return ok, expected, observed, "" if ok else "schema.export oracle failed validation"

        if not self.run_step("FrostEditor schema.export oracle on the pinned clone", export_schema_oracle):
            return

        def open_and_start_lua() -> tuple[bool, Any, Any, str]:
            ep.call("trust", root=str(project_root))
            ep.call("open", path=str(project_root))
            waited = ep.call("wait", **{"for": "lua", "timeoutMs": LUA_WAIT_SECONDS * 1000})
            lua_state = ep.call("lua.state")
            records = spawn_records(self.spawn_dir)
            luals_starts = [
                r
                for r in records
                if r.get("event") == "start" and same_path(r.get("executable", ""), luals)
            ]
            schema_starts = [
                r
                for r in records
                if r.get("event") == "start"
                and same_path(r.get("executable", ""), editor_binary)
                and "schema.export" in (r.get("argv") or [])
            ]
            ok = (
                not waited.get("timedOut")
                and lua_state.get("available") is True
                and lua_state.get("state") == "Running"
                and int(lua_state.get("pid") or 0) > 0
                and int(lua_state.get("restarts") or 0) == 0
                and len(luals_starts) >= 1
                and len(schema_starts) >= 1
            )
            expected = {
                "state": "Running",
                "restarts": 0,
                "lualsSpawned": True,
                "schemaExportSpawned": True,
            }
            observed = {
                "luaState": lua_state,
                "lualsStarts": len(luals_starts),
                "schemaExportStarts": len(schema_starts),
            }
            return ok, expected, observed, "" if ok else "LuaLS did not start or schema.export was not run"

        if not self.run_step("open, trust and start LuaLS with FrostEngine schema.export", open_and_start_lua):
            return

        probe_lines = [
            "---@class FrostJ3Probe : FrostScript",
            "local FrostJ3Probe = {}",
            "function FrostJ3Probe:OnUpdate(dt)",
            '    local t = self.Entity:GetComponent("Transform")',
            "    local p = t.",
            '    local rb = self.Entity:GetComponent("Rigidbody2D")',
            "    local v = rb.",
            "    local hit = Physics.Raycast(t.Translation, t.Rotation)",
            "end",
            "return FrostJ3Probe",
            "",
        ]
        probe_path.write_text("\n".join(probe_lines), encoding="utf-8")

        def component_completion() -> tuple[bool, Any, Any, str]:
            t_line = 5
            t_col = len("    local p = t.") + 1
            rb_line = 7
            rb_col = len("    local v = rb.") + 1

            ep.call("open", path=str(probe_path), line=t_line, column=t_col)
            deadline = time.monotonic() + COMPLETE_SECONDS
            t_labels: list[str] = []
            while time.monotonic() < deadline:
                res = ep.call("lua.complete", line=t_line, column=t_col, timeoutMs=10000)
                t_labels = sorted(res.get("labels") or [])
                if t_labels == transform_expected:
                    break
                time.sleep(POLL_SECONDS)

            rb_labels: list[str] = []
            while time.monotonic() < deadline:
                res = ep.call("lua.complete", line=rb_line, column=rb_col, timeoutMs=10000)
                rb_labels = sorted(res.get("labels") or [])
                if rb_labels == rigidbody_expected:
                    break
                time.sleep(POLL_SECONDS)

            ok = t_labels == transform_expected and rb_labels == rigidbody_expected
            expected = {"Transform": transform_expected, "Rigidbody2D": rigidbody_expected}
            observed = {"Transform": t_labels, "Rigidbody2D": rb_labels}
            return (
                ok,
                expected,
                observed,
                "" if ok else "component completion items differ from schema.export entries",
            )

        if not self.run_step(
            "engine-API completion items for a component equal schema.export entries", component_completion
        ):
            return

        raycast_line = 8
        raycast_col = probe_lines[raycast_line - 1].index("Raycast") + 2

        def goto_definition_opens_stub() -> tuple[bool, Any, Any, str]:
            ep.call("open", path=str(probe_path), line=raycast_line, column=raycast_col)
            deadline = time.monotonic() + NAVIGATE_SECONDS
            editor: dict[str, Any] = {}
            while time.monotonic() < deadline:
                ep.call("navigate.definition")
                wait_until = time.monotonic() + 2.0
                while time.monotonic() < wait_until:
                    editor = ep.call("editor")
                    if editor.get("available") and not same_path(editor.get("path", ""), probe_path):
                        break
                    time.sleep(POLL_SECONDS)
                if editor.get("available") and not same_path(editor.get("path", ""), probe_path):
                    break
            target_path = Path(editor.get("path") or "")
            target_line = int(editor.get("line") or 0)
            line_text = ""
            if target_path.is_file() and target_line > 0:
                lines = target_path.read_text(encoding="utf-8", errors="replace").splitlines()
                if target_line <= len(lines):
                    line_text = lines[target_line - 1]
            outside_corpus = bool(target_path.name) and not is_under(str(target_path), corpus)
            ok = (
                target_path.name == "frost_schema.lua"
                and outside_corpus
                and "Physics.Raycast" in line_text
            )
            expected = {
                "fileName": "frost_schema.lua",
                "outsideCorpus": True,
                "declares": "Physics.Raycast",
            }
            observed = {
                "path": str(target_path),
                "line": target_line,
                "outsideCorpus": outside_corpus,
                "lineText": line_text,
            }
            return ok, expected, observed, "" if ok else "Go to Definition did not open frost_schema.lua"

        if not self.run_step(
            "Go to Definition on an engine call opens the generated frost_schema.lua stub",
            goto_definition_opens_stub,
        ):
            return

        def goto_cpp_binding_opens_source() -> tuple[bool, Any, Any, str]:
            source = raycast_entry.get("source") or {}
            expected_path = corpus / source["file"]
            expected_line = int(source["line"])

            ep.call("open", path=str(probe_path), line=raycast_line, column=raycast_col)
            ep.call("navigate.cppBinding")
            editor = ep.call("editor")
            ok = (
                editor.get("available") is True
                and same_path(editor.get("path", ""), expected_path)
                and int(editor.get("line") or 0) == expected_line
            )
            expected = {"path": str(expected_path), "line": expected_line}
            observed = {"path": editor.get("path"), "line": editor.get("line")}
            return (
                ok,
                expected,
                observed,
                "" if ok else "Go to C++ Binding did not open the C++ file:line from source_location",
            )

        if not self.run_step(
            "Go to C++ Binding opens the C++ file:line from schema.export source_location",
            goto_cpp_binding_opens_source,
        ):
            return

        def live_module_and_runtime_change() -> tuple[bool, Any, Any, str]:
            before = ep.call("lua.state")
            live_module_path.write_text(
                "local M = {}\nfunction M.answer()\n    return 42\nend\nreturn M\n",
                encoding="utf-8",
            )
            require_lines = [
                'local live = require("frost_j3_live_module")',
                "local value = live.answer()",
                "return value",
                "",
            ]
            require_path.write_text("\n".join(require_lines), encoding="utf-8")
            req_col = require_lines[0].index("frost_j3_live_module") + 2
            ep.call("open", path=str(require_path), line=1, column=req_col)
            deadline = time.monotonic() + NAVIGATE_SECONDS
            editor: dict[str, Any] = {}
            while time.monotonic() < deadline:
                ep.call("open", path=str(require_path), line=1, column=req_col)
                ep.call("navigate.definition")
                wait_until = time.monotonic() + 2.0
                while time.monotonic() < wait_until:
                    editor = ep.call("editor")
                    if editor.get("available") and same_path(editor.get("path", ""), live_module_path):
                        break
                    time.sleep(POLL_SECONDS)
                if editor.get("available") and same_path(editor.get("path", ""), live_module_path):
                    break

            ep.call("lua.setRuntime", version="Lua 5.1")
            after = ep.call("lua.state")
            ep.call("lua.setRuntime", version="Lua 5.4")

            resolved_live = editor.get("available") is True and same_path(
                editor.get("path", ""), live_module_path
            )
            same_pid = after.get("pid") == before.get("pid")
            no_restarts = int(after.get("restarts") or 0) == 0
            cfg_sent = int(after.get("configurationChangesSent") or 0) > int(
                before.get("configurationChangesSent") or 0
            )
            runtime_updated = after.get("runtimeVersion") == "Lua 5.1"
            ok = resolved_live and same_pid and no_restarts and cfg_sent and runtime_updated
            expected = {
                "resolvedModule": str(live_module_path),
                "pid": before.get("pid"),
                "restarts": 0,
                "configurationChangesIncreased": True,
                "runtimeVersion": "Lua 5.1",
            }
            observed = {
                "resolvedModule": editor.get("path"),
                "pid": after.get("pid"),
                "restarts": after.get("restarts"),
                "beforeChanges": before.get("configurationChangesSent"),
                "afterChanges": after.get("configurationChangesSent"),
                "runtimeVersion": after.get("runtimeVersion"),
            }
            return (
                ok,
                expected,
                observed,
                "" if ok else "new module require() or live runtime didChangeConfiguration failed",
            )

        if not self.run_step(
            "new Lua module resolves without a restart and runtime change sends didChangeConfiguration",
            live_module_and_runtime_change,
        ):
            return

        self._remove_scratch_files()

        def inspect_code_matches_oracle() -> tuple[bool, Any, Any, str]:
            ide_reply = ep.call("lua.inspect", directory=str(scripts_dir))
            ide_findings = normalize_ide_inspect_files(ide_reply.get("files") or [])
            oracle_findings = run_luals_check_oracle(luals, scripts_dir, self.tmp)
            ok = ide_findings == oracle_findings
            expected = {"count": len(oracle_findings), "findings": oracle_findings}
            observed = {"count": len(ide_findings), "findings": ide_findings}
            return (
                ok,
                expected,
                observed,
                "" if ok else "lua.inspect results differ from lua-language-server --check oracle",
            )

        self.run_step(
            "Inspect Code on the scripts folder equals lua-language-server --check output",
            inspect_code_matches_oracle,
        )


def main() -> int:
    """Entry point for journey j3_frostengine_lua."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frost", required=True, help="the frost image (Debug)")
    parser.add_argument("--config", required=True, help="the ctest configuration")
    parser.add_argument("--evidence", required=True, help="the directory for the evidence JSON")
    args = parser.parse_args()
    try:
        return Journey(args).main()
    except SystemExit as stop:
        return int(stop.code or 1)


if __name__ == "__main__":
    sys.exit(main())
