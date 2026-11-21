#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Builds the opt-in Lua coroutine provider from an admitted local package.
"""Prepare a fresh private Lua Debug 2.3.0 coroutine provider from a local pinned VSIX."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import tempfile
import zipfile


SOURCE = Path(__file__).resolve().parent / "lua-debug-coroutines"
MAX_ARCHIVE = 32 * 1024 * 1024
MAX_INSTALLED = 128 * 1024 * 1024


def checked_archive(source: Path, pin: str) -> bytes:
    with source.open("rb") as stream:
        magic = stream.peek(2)[:2]
        # The Marketplace can wrap the ZIP in HTTP gzip. The digest covers the real package.
        if magic == b"\x1f\x8b":
            with gzip.GzipFile(fileobj=stream) as wrapper:
                raw = wrapper.read(MAX_ARCHIVE + 1)
        else:
            raw = stream.read(MAX_ARCHIVE + 1)
    if len(raw) > MAX_ARCHIVE or hashlib.sha256(raw).hexdigest() != pin:
        raise ValueError("Lua Debug VSIX differs from the pinned package digest")
    return raw


def extract_extension(package: bytes, to: Path) -> None:
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        entries = []
        total = 0
        seen = set()
        for member in archive.infolist():
            path = PurePosixPath(member.filename)
            if not path.parts or path.parts[0] != "extension":
                continue
            mode = member.external_attr >> 16
            kind = stat.S_IFMT(mode)
            if (path.is_absolute() or any(part in ("..", ".") or "\\" in part or ":" in part for part in path.parts)
                    or kind not in (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError("unsupported Lua Debug VSIX entry")
            key = str(path).casefold()
            if key in seen:
                raise ValueError("repeated Lua Debug VSIX entry")
            seen.add(key)
            total += member.file_size
            if total > MAX_INSTALLED:
                raise ValueError("Lua Debug VSIX extraction exceeds its size boundary")
            entries.append((member, to.joinpath(*path.parts[1:]), mode))
        for member, destination, mode in entries:
            if member.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as content, destination.open("xb") as file:
                shutil.copyfileobj(content, file)
            if os.name != "nt":
                destination.chmod(0o755 if mode & 0o111 else 0o644)


def patch(root: Path, path: str, edits: list[tuple[str, str]]) -> None:
    file = root / path
    source = file.read_text(encoding="utf8")
    for before, after in edits:
        if source.count(before) != 1:
            raise ValueError(f"unexpected Lua Debug 2.3.0 script context in {path}")
        source = source.replace(before, after, 1)
    file.write_text(source, encoding="utf8", newline="\n")


def patch_extension(extension: Path) -> None:
    scripts = extension / "script" / "backend"
    worker = "worker.lua"
    patch(scripts, worker, [
        ("local disassemble = require 'backend.worker.disassemble'",
         "local disassemble = require 'backend.worker.disassemble'\n"
         "local coroutines = require 'backend.worker.frost_coroutines'"),
        ("        local ok, err = xpcall(function()\n            local f = CMD[msg.cmd]",
         "        local host = hookmgr.gethost()\n"
         "        local ok, err = xpcall(function()\n            local f = CMD[msg.cmd]"),
        ("                f(msg)\n", "                if coroutines.enabled() then\n"
         "                    coroutines.dispatch(f, msg)\n                else\n                    f(msg)\n"
         "                end\n"),
        ("        if not ok then\n            log.error(", "        if coroutines.enabled() then coroutines.restore(host) end\n"
         "        if not ok then\n            coroutines.reportCommandError(msg, err)\n            log.error("),
        ("        masterThread:push(WorkerIdent, cmd, msg)",
         "        if coroutines.wrap(cmd, msg) then masterThread:push(WorkerIdent, cmd, msg) end"),
        ("    ev.emit('initializing', pkg.config)", "    ev.emit('initializing', pkg.config)\n"
         "    if not pkg.config.noDebug then coroutines.initialize(sendToMaster) end"),
        ("function CMD.stackTrace(pkg)\n", "function CMD.stackTrace(pkg)\n"
         "    if coroutines.enabled() then return coroutines.stack(pkg, stackTrace, skipFrame) end\n"),
        ("    hookmgr.sethost(assert(findFrame(coid)))",
         "    if not coroutines.enabled() then hookmgr.sethost(assert(findFrame(coid))) end"),
        ("    baseL = hookmgr.gethost()\n    sendToMaster 'eventStop' (reason)",
         "    baseL = hookmgr.gethost()\n"
         "    if coroutines.enabled() then\n        cleanFrame()\n"
         "        if not coroutines.stop(baseL, reason) then return end\n    end\n"
         "    sendToMaster 'eventStop' (reason)"),
        ("function event.update()\n    debuggeeReady()\n    workerThreadUpdate()",
         "function event.update()\n    debuggeeReady()\n    workerThreadUpdate()\n    coroutines.update()"),
        ("function CMD.run()\n", "function CMD.run()\n    coroutines.resume()\n"),
        ("function CMD.stepIn()\n", "function CMD.stepIn()\n    coroutines.resume()\n"),
        ("function CMD.stepOver()\n", "function CMD.stepOver()\n    coroutines.resume()\n"),
        ("function CMD.stepOut()\n", "function CMD.stepOut()\n    coroutines.resume()\n"),
        ("function CMD.disconnect()\n", "function CMD.disconnect()\n    coroutines.resume()\n"),
    ])
    patch(scripts, "master/mgr.lua", [
        ("local channel = require \"bee.channel\"",
         "local channel = require \"bee.channel\"\nlocal coroutines = require 'backend.master.frost_coroutines'"),
        ("function mgr.workerSend(w, msg)\n    return", "function mgr.workerSend(w, msg)\n"
         "    w = coroutines.route(w, msg)\n    return"),
        ("    for w, channel in pairs(threadChannel) do\n        if w ~= exclude then",
         "    exclude = coroutines.route(exclude, {})\n    for w, channel in pairs(threadChannel) do\n"
         "        if w ~= exclude then"),
        ("    return threadChannel[w] ~= nil", "    return threadChannel[w] ~= nil or coroutines.has(w)"),
        ("function mgr.exitWorker(w)\n",
         """local function startCoroutine(id)
    threadStatus[id] = "connect"
    threadName[id] = "Coroutine " .. id
    require('backend.master.event').thread {reason = "started", threadId = id}
end

local function exitCoroutine(id)
    threadStatus[id], threadName[id] = nil, nil
    require('backend.master.event').thread {reason = "exited", threadId = id}
end

function mgr.coroutineEvents(worker, events)
    coroutines.consume(worker, events, genThreadId, startCoroutine, exitCoroutine)
end

function mgr.coroutineStop(worker, native)
    return coroutines.noteStop(worker, native)
end

mgr.acceptsStep = coroutines.acceptsStep
mgr.isCoroutine = coroutines.has

function mgr.exitWorker(w)
    coroutines.clear(w, exitCoroutine)
"""),
    ])
    patch(scripts, "master/threads.lua", [
        ("function CMD.eventStop(w, req)\n    req.threadId = w",
         r"""function CMD.coroutineEvents(w, req)
    mgr.coroutineEvents(w, req.events)
end

function CMD.coroutineFailure(w, req)
    event.output {category = "console", output = req.message .. "\n"}
    mgr.exitWorker(w)
end

function CMD.eventStop(w, req)
    req.threadId = mgr.coroutineStop(w, req.native)
    req.native = nil"""),
        ("function CMD.scopes(w, req)\n", "function CMD.scopes(w, req)\n"
         "    if req.success == false then return response.error(req, req.message) end\n"),
        ("function CMD.setExpression(w, req)\n    if not req.success then\n        response.error(req, req.message)\n        return\n    end\n    local var = req.body",
         "function CMD.setExpression(w, req)\n    if not req.success then\n"
         "        response.error(req, req.message)\n        return\n    end\n    local var = req.body\n"
         "    if var.memoryReference then var.memoryReference = \"memory_\" .. w .. \"x\" .. var.memoryReference end"),
    ])
    for step in ("stepIn", "stepOut", "stepOver"):
        patch(scripts, "master/request.lua", [
            (f"    mgr.workerSend(threadId, {{\n        cmd = '{step}',",
             "    if not mgr.acceptsStep(threadId) then\n"
             "        return response.error(req, 'The selected coroutine is suspended. Step the stopped coroutine.')\n"
             f"    end\n    mgr.workerSend(threadId, {{\n        cmd = '{step}',"),
        ])
    patch(scripts, "master/request.lua", [
        ("function request.terminateThreads(req)\n    local args = req.arguments\n    response.success(req)",
         "function request.terminateThreads(req)\n    local args = req.arguments\n"
         "    for _, id in ipairs(args.threadIds) do\n        if mgr.isCoroutine(id) then\n"
         "            return response.error(req, 'An individual Lua coroutine cannot be terminated by the debugger')\n"
         "        end\n    end\n    response.success(req)"),
        ("""function request.restartFrame(req)
    local args = req.arguments
    local threadId = args.frameId >> 24
    local frameId = args.frameId & 0x00FFFFFF
    if not checkThreadId(req, threadId) then
        return
    end
    response.success(req)
    mgr.workerSend(threadId, {
        cmd = 'restartFrame',
        frameId = frameId,
    })
end""",
         "function request.restartFrame(req)\n    return response.error(req, 'Lua frame restart is unavailable')\nend"),
    ])
    patch(extension / "script", "common/capabilities.lua", [
        ("supportsRestartFrame = true", "supportsRestartFrame = false"),
        ("supportsTerminateThreadsRequest = true", "supportsTerminateThreadsRequest = false"),
    ])
    for role in ("master", "worker"):
        shutil.copyfile(SOURCE / f"{role}.lua", scripts / role / "frost_coroutines.lua")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vsix", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    args = parser.parse_args()
    sdk = Path(__file__).resolve().parents[1] / "sdk" / "sdk.lock.json"
    pin = json.loads(sdk.read_text(encoding="utf8"))["sources"]["luaDebugCoroutines"]
    system = "windows" if os.name == "nt" else platform.system().lower()
    if system not in pin["vsixSha256"] or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("this recipe supports Windows and Linux x64")
    if not args.vsix.is_absolute() or not args.vsix.is_file() or not args.prefix.is_absolute():
        parser.error("provide an existing absolute --vsix and a fresh absolute --prefix")
    if args.prefix.exists() or args.prefix.is_symlink():
        parser.error("--prefix already exists")
    content = checked_archive(args.vsix, pin["vsixSha256"][system])
    args.prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".frost-lua-debug-", dir=args.prefix.parent) as staging:
        extension = Path(staging) / "extension"
        extract_extension(content, extension)
        metadata = json.loads((extension / "package.json").read_text(encoding="utf8"))
        if metadata.get("version") != pin["version"] or metadata.get("publisher") != "actboy168":
            raise ValueError("Lua Debug package identity does not match the pinned provider")
        patch_extension(extension)
        license_dir = extension / "frostide-license"
        license_dir.mkdir()
        shutil.copyfile(sdk.parents[1] / "LICENSE", license_dir / "Apache-2.0.txt")
        shutil.copyfile(SOURCE / "NOTICE.txt", license_dir / "NOTICE.txt")
        args.prefix.mkdir(exist_ok=False)
        try:
            extension.rename(args.prefix / "extension")
        except OSError:
            args.prefix.rmdir()
            raise


if __name__ == "__main__":
    main()
