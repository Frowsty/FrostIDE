# SPDX-License-Identifier: Apache-2.0
# Exercise Unity scratch assemblies through the actual managed language server.
import argparse
import ctypes
import json
import os
import queue
import subprocess
import threading
import time
from ctypes import wintypes
from pathlib import Path


def require_existing_windows_migration_marker() -> None:
    if os.name != "nt":
        return
    class Guid(ctypes.Structure):
        _fields_ = [("data1", wintypes.DWORD), ("data2", wintypes.WORD), ("data3", wintypes.WORD),
                    ("data4", ctypes.c_ubyte * 8)]
    local_id = Guid(0xF1B32785, 0x6FBA, 0x4FCF, (ctypes.c_ubyte * 8)(0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91))
    path = ctypes.c_wchar_p()
    if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(local_id), 0x4000, None, ctypes.byref(path)) != 0:
        raise RuntimeError("cannot inspect Windows LocalAppData")
    local = Path(path.value)
    ctypes.windll.ole32.CoTaskMemFree(path)
    if not (local / "NuGet" / "Migrations" / "1").is_file():
        raise RuntimeError("NuGet migration marker is absent; refusing to modify the user's profile")


def run(project: Path, dotnet: Path, server: Path, profile: Path, mode: str, special_paths: bool) -> None:
    require_existing_windows_migration_marker()
    for path in (project, dotnet, server, profile):
        if not path.is_absolute():
            raise ValueError("all paths must be absolute")
    unity = project / ".frost" / "unity"
    descriptor = (unity / "revision").read_text(encoding="ascii")
    import re
    if not re.fullmatch(r"1:[0-9a-f]{32}:[0-9a-f]{32}", descriptor):
        raise ValueError("invalid generated Unity descriptor")
    solution = unity / "generations" / descriptor[2:34] / "FrostUnity.sln"
    if mode == "player":
        source = project / "Assets" / "Probe" / "FrostUnityProbe.cs"
        text = "using UnityEngine;\npublic class Probe : MonoBehaviour { void X() { var x = Vector3. } }\n"
        token, expected = "Vector3.", "zero"
    else:
        source = project / "Assets" / "Editor" / "FrostBridgeScratchProbe.cs"
        text = "using UnityEditor;\npublic class Probe { FrostUnityExtra Value; void X() { var x = EditorApplication. } }\n"
        token, expected = "EditorApplication.", "isPlaying"
    env = os.environ.copy()
    env.update(DOTNET_ROOT=str(dotnet.parent), DOTNET_CLI_HOME=str(profile / "dotnet-home"),
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1", DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1",
               DOTNET_GENERATE_ASPNET_CERTIFICATE="0", DOTNET_ADD_GLOBAL_TOOLS_TO_PATH="0")
    for name in ("LOCALAPPDATA", "APPDATA", "HOME", "USERPROFILE", "XDG_DATA_HOME", "XDG_CONFIG_HOME",
                 "NUGET_PACKAGES", "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH"):
        path = profile / name
        path.mkdir(parents=True, exist_ok=True)
        env[name] = str(path)
    log = (profile / (mode + "-server.log")).open("wb")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    process = subprocess.Popen([str(dotnet), str(server), "--loglevel", "warning", "--solution", str(solution)],
                               cwd=project, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=log, creationflags=flags, start_new_session=os.name != "nt")
    messages: queue.Queue = queue.Queue()
    diagnostics = []
    def receive() -> None:
        while True:
            headers = {}
            while True:
                line = process.stdout.readline()
                if not line:
                    return
                if line in (b"\r\n", b"\n"):
                    break
                key, value = line.decode("ascii").split(":", 1)
                headers[key.lower()] = value.strip()
            messages.put(json.loads(process.stdout.read(int(headers["content-length"]))))
    threading.Thread(target=receive, daemon=True).start()
    def send(value: dict) -> None:
        data = json.dumps(value, ensure_ascii=False).encode("utf-8")
        process.stdin.write(b"Content-Length: " + str(len(data)).encode() + b"\r\n\r\n" + data)
        process.stdin.flush()
    def wait_for(identifier: int, seconds: int):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                message = messages.get(timeout=max(.01, deadline - time.monotonic()))
            except queue.Empty:
                return None
            if message.get("method") == "textDocument/publishDiagnostics":
                diagnostics.append(message["params"])
            if message.get("id") == identifier and "method" not in message:
                return message
            if "method" in message and "id" in message:
                result = [None for _ in message.get("params", {}).get("items", [])] if message["method"] == "workspace/configuration" else None
                send({"jsonrpc": "2.0", "id": message["id"], "result": result})
        return None
    def request(identifier: int, method: str, params: dict) -> None:
        send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
    def notify(method: str, params: dict) -> None:
        send({"jsonrpc": "2.0", "method": method, "params": params})
    try:
        uri = source.as_uri()
        request(1, "initialize", {"processId": None, "rootUri": project.as_uri(), "rootPath": str(project),
                                  "capabilities": {"workspace": {"configuration": True, "workspaceFolders": True}},
                                  "workspaceFolders": [{"uri": project.as_uri(), "name": "Unity"}]})
        response = wait_for(1, 30)
        if not response or "result" not in response:
            raise RuntimeError("language-server initialization failed")
        notify("initialized", {})
        wait_for(-1, 15)
        notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "csharp", "version": 1, "text": text}})
        for identifier in range(2, 17):
            time.sleep(5)
            request(identifier, "textDocument/completion", {"textDocument": {"uri": uri},
                    "position": {"line": 1, "character": text.splitlines()[1].index(token) + len(token)}})
            response = wait_for(identifier, 20)
            result = response.get("result") if response else None
            items = result.get("items", []) if isinstance(result, dict) else result or []
            if any(item.get("label") == expected for item in items):
                break
        else:
            raise RuntimeError(f"Unity completion {expected} was not returned")
        if mode == "editor":
            from urllib.parse import urlsplit
            from urllib.request import url2pathname
            expected_path = os.path.normcase(os.path.normpath(str(project / "Assets" / "Probe" / "FrostUnityExtra.cs")))
            def same_document(item):
                uri = item.get("uri", item.get("targetUri", ""))
                parsed = urlsplit(uri)
                if parsed.scheme != "file" or parsed.netloc not in ("", "localhost"):
                    return False
                actual = os.path.normcase(os.path.normpath(url2pathname(parsed.path)))
                return actual == expected_path
            for identifier in range(90, 102):
                request(identifier, "textDocument/definition", {"textDocument": {"uri": uri},
                        "position": {"line": 1, "character": text.splitlines()[1].index("FrostUnityExtra") + 2}})
                definition = wait_for(identifier, 20)
                locations = definition.get("result") if definition else None
                if isinstance(locations, dict):
                    locations = [locations]
                if locations and any(same_document(item) for item in locations):
                    break
                time.sleep(5)
            else:
                raise RuntimeError("editor-to-player definition was not resolved to the expected source")
            changed = "using UnityEditor; public class Probe { FrostUnityExtra Value; void X() { MissingEditorSymbol(); } }\n"
            expected_error, forbidden_error = "MissingEditorSymbol", "FrostUnityExtra"
        else:
            special = "FrostEscapedPath Source; FrostSpecialDll Plugin; WrongPlugin Other; " if special_paths else ""
            changed = "public class Probe : UnityEngine.MonoBehaviour { " + special + "System.Data.Odbc.OdbcConnection Value; }\n"
            expected_error, forbidden_error = "Odbc", None
        notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": changed}]})
        latest = ""
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            wait_for(-2, 1)
            matching = [item for item in diagnostics if item.get("uri") == uri and item.get("version") in (None, 2)]
            for item in reversed(matching):
                candidate = json.dumps(item.get("diagnostics", []))
                if expected_error in candidate:
                    latest = candidate
                    break
            if latest:
                break
        forbidden = [forbidden_error] if forbidden_error else []
        if mode == "player" and special_paths:
            forbidden.extend(["FrostEscapedPath", "FrostSpecialDll", "WrongPlugin"])
        if not latest or any(value in latest for value in forbidden):
            raise RuntimeError("expected unsaved diagnostic or project reference was not resolved: " + latest)
        print(f"{mode}: Unity completion, project context and unsaved diagnostics passed")
    finally:
        if os.name == "nt":
            subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32" / "taskkill.exe"), "/PID", str(process.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
        else:
            import signal
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        log.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("dotnet", type=Path)
    parser.add_argument("server", type=Path)
    parser.add_argument("profile", type=Path)
    parser.add_argument("mode", choices=("player", "editor"))
    parser.add_argument("--special-path-fixture", action="store_true")
    args = parser.parse_args()
    run(args.project, args.dotnet, args.server, args.profile, args.mode, args.special_path_fixture)


if __name__ == "__main__":
    main()
