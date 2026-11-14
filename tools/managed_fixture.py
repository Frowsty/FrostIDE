#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Supply verified local managed tools to the five existing real-provider integration cases.
"""CTest launcher for configured managed fixtures. Never installs or downloads tools."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import managed_server


def matches(name: str, pattern: str) -> bool:
    """Google Test patterns support only '*' and '?', with case-sensitive literal remaining bytes."""
    expression = re.escape(pattern).replace(r"\*", ".*").replace(r"\?", ".")
    return re.fullmatch(expression, name) is not None


def requirements(arguments: list[str], environment: dict[str, str], cases: dict[str, list[str]]) -> set[str]:
    """Discovery and unrelated cases pass through without inspecting managed installations."""
    discovery = False
    selection = environment.get("GTEST_FILTER", "*")
    for argument in arguments:
        # Pinned GTest's ParseFlag bool uses the first value byte and the last occurrence wins.
        # ParseGoogleTestFlagsOnlyImpl loads flagfiles inline, which could change either flag.
        if argument == "--gtest_flagfile" or argument.startswith("--gtest_flagfile="):
            raise ValueError("--gtest_flagfile is unsupported by managed fixture selection; use direct flags")
        if argument == "--gtest_list_tests":
            discovery = True
        elif argument.startswith("--gtest_list_tests="):
            discovery = not argument.partition("=")[2].startswith(("0", "f", "F"))
        elif argument.startswith("--gtest_filter="):
            selection = argument.partition("=")[2]
    if discovery:
        return set()
    positive, separator, negative = selection.partition("-")
    if separator and not positive:
        positive = "*"
    selected: set[str] = set()
    for name, needed in cases.items():
        included = any(matches(name, pattern) for pattern in positive.split(":"))
        excluded = separator and any(matches(name, pattern) for pattern in negative.split(":"))
        if included and not excluded:
            selected.update(needed)
    return selected


def configured_value(configuration: dict, environment: dict[str, str], name: str) -> str:
    """An explicit runtime setting takes precedence, including an explicitly empty value."""
    return environment[name] if name in environment else configuration["tools"].get(name, "")


def absolute_file(value: str, label: str) -> Path:
    path = Path(value)
    if not value or not path.is_absolute() or not path.is_file():
        raise ValueError(f"{label} requires an existing absolute file path")
    return path


def verify_debug_payload(payload: Path, expected_digest: str) -> None:
    """Reuse the reviewed installer verifier with the independently supplied build-record digest."""
    directory = Path(__file__).resolve().parent / "managed-debug"
    spec = importlib.util.spec_from_file_location("managed_fixture_payload", directory / "install_payload.py")
    if spec is None or spec.loader is None:
        raise ValueError("managed payload verifier is unavailable")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(directory))
    try:
        spec.loader.exec_module(module)
        module.verify(payload, expected_digest)
    finally:
        sys.path.pop(0)


def fixture_environment(configuration: dict, environment: dict[str, str], needed: set[str],
                        executable: Path) -> dict[str, str]:
    """Validate requested fixture inputs before starting the test; never run a provider probe."""
    result = dict(environment)
    if not needed:
        return result
    root = Path(configuration["sourceRoot"])
    lock = json.loads((root / "sdk/sdk.lock.json").read_text(encoding="utf-8"))
    platform = "windows" if os.name == "nt" else "linux"
    dotnet = absolute_file(configured_value(configuration, environment, "FROST_TEST_CSHARP_DOTNET"),
                           "FROST_TEST_CSHARP_DOTNET")
    version = lock[platform]["dotnet"]["version"]
    if not (dotnet.parent / "sdk" / version / "dotnet.dll").is_file():
        raise ValueError(f"managed SDK lacks pinned sdk/{version}/dotnet.dll")
    result["FROST_TEST_CSHARP_DOTNET"] = str(dotnet)
    if "server" in needed:
        server = absolute_file(configured_value(configuration, environment, "FROST_TEST_CSHARP_SERVER"),
                               "FROST_TEST_CSHARP_SERVER")
        archive = absolute_file(configured_value(configuration, environment, "FROST_TEST_CSHARP_ARCHIVE"),
                                "FROST_TEST_CSHARP_ARCHIVE (the pinned offline NuGet archive)")
        if server.name != "CSharpLanguageServer.dll":
            raise ValueError("managed server entry must be CSharpLanguageServer.dll")
        managed_server.verify_installed(archive, server.parent, root)
        result["FROST_TEST_CSHARP_SERVER"] = str(server)
    if "debugger" in needed:
        bundled = environment.get("FROST_TEST_MANAGED_USE_BUNDLE") == "1"
        adapter_name = "netcoredbg.exe" if os.name == "nt" else "netcoredbg"
        if bundled:
            payload = executable.parent / "FrostManagedDebug"
            adapter = absolute_file(str(payload / "adapter" / adapter_name), "bundled managed adapter")
            guard = absolute_file(str(payload / "PdbGuard/PdbGuard.dll"), "bundled PdbGuard")
        else:
            adapter = absolute_file(configured_value(configuration, environment, "FROST_TEST_MANAGED_DEBUG_ADAPTER"),
                                    "FROST_TEST_MANAGED_DEBUG_ADAPTER")
            guard = absolute_file(configured_value(configuration, environment, "FROST_TEST_MANAGED_DEBUG_GUARD"),
                                  "FROST_TEST_MANAGED_DEBUG_GUARD")
            payload = adapter.parent.parent
            if adapter.name != adapter_name or guard.resolve() != (payload / "PdbGuard/PdbGuard.dll").resolve():
                raise ValueError("adapter and PdbGuard must belong to one reviewed payload")
        expected = configured_value(configuration, environment, "FROST_MANAGED_DEBUG_MANIFEST_SHA256")
        if not expected:
            raise ValueError("FROST_MANAGED_DEBUG_MANIFEST_SHA256 requires the independently reviewed build digest")
        verify_debug_payload(payload, expected)
        result["FROST_TEST_MANAGED_DEBUG_ADAPTER"] = str(adapter)
        result["FROST_TEST_MANAGED_DEBUG_GUARD"] = str(guard)
    return result


def run_test(arguments: list[str], environment: dict[str, str]) -> int:
    """Keep inherited stdio and wait for the original test; clean up an interrupted child."""
    child = subprocess.Popen(arguments, env=environment)
    try:
        return child.wait()
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        if not command:
            raise ValueError("missing test executable")
        if args.config.stat().st_size > 64 * 1024:
            raise ValueError("managed fixture configuration exceeds 64 KiB")
        configuration = json.loads(args.config.read_text(encoding="utf-8"))
        if configuration.get("schemaVersion") != 1:
            raise ValueError("unsupported managed fixture configuration")
        needed = requirements(command[1:], dict(os.environ), configuration["cases"])
        environment = fixture_environment(configuration, dict(os.environ), needed, Path(command[0]))
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"managed fixture unavailable: {error}", file=sys.stderr)
        return 1
    return run_test(command, environment)


if __name__ == "__main__":
    sys.exit(main())
