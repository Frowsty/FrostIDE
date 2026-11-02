# SPDX-License-Identifier: Apache-2.0
# Real sccache /Z7 regression: objects from different source roots must retain their own paths.
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile

repo = Path(sys.argv[1]).resolve()
work = Path(sys.argv[2]).resolve()
work.mkdir(parents=True, exist_ok=True)
sdk = Path(os.environ["FROST_SDK"])
clang = Path(os.environ["FROST_LLVM_ROOT"]) / "bin" / "clang-cl.exe"
readobj = clang.with_name("llvm-readobj.exe")
version = json.loads((repo / "sdk/sdk.lock.json").read_text(encoding="utf-8"))["windows"]["sccache"]["version"]
sccache = sdk / f"sccache-{version}" / "sccache.exe"
fixture = repo / "tests/cmake/compiler_cache_identity_fixture"
cmake = shutil.which("cmake")
ninja = shutil.which("ninja")
assert all((cmake, ninja, clang.exists(), readobj.exists(), sccache.exists()))

def run(*args, env, cwd=None, timeout=30):
    p = subprocess.Popen(args, env=env, cwd=cwd, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    try:
        output, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        # CMake may have started ninja and a compiler; terminate the whole test-owned tree.
        subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=5, check=False)
        if p.poll() is None:
            p.kill()
        p.communicate(timeout=5)
        raise AssertionError(f"command {args!r} timed out after {timeout}s") from error
    if p.returncode:
        raise AssertionError(f"command {args!r} failed ({p.returncode}):\n{output}")
    return output

with tempfile.TemporaryDirectory(prefix="cache-identity-", dir=work) as scratch:
    base = Path(scratch)
    env = os.environ.copy()
    env["SCCACHE_DIR"] = str(base / "cache")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        env["SCCACHE_SERVER_PORT"] = str(sock.getsockname()[1])
    env.pop("SCCACHE_C_CUSTOM_CACHE_BUSTER", None)
    def stats():
        return json.loads(run(str(sccache), "--show-stats", "--stats-format=json", env=env))["stats"]
    def count(stats, key):
        return stats[key]["counts"].get("C/C++", 0)
    def configure(name, buster=None):
        source = base / name / "src"
        build = base / name / "build"
        shutil.copytree(fixture, source, dirs_exist_ok=True)
        conf_env = env.copy()
        if buster is not None:
            conf_env["SCCACHE_C_CUSTOM_CACHE_BUSTER"] = buster
        run(cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DFROST_REPO={repo.as_posix()}",
            f"-DFROST_CLANG_CL={clang.as_posix()}", env=conf_env)
        return source, build
    try:
        source_a, build_a = configure("a")
        source_b, build_b = configure("b")
        # Compiler argument boundaries must survive the wrapper, including empty strings,
        # semicolons, unmatched brackets and a trailing backslash before another argument.
        arguments = ["ordinary", "with spaces", "semi;colon", "back\\;semi", "tail\\", "",
                     "[", "after", "]", "${HOME}", '"quoted"', "line\nbreak", "end"]
        recorded = base / "recorded.json"
        run(cmake, "-P", str(build_a / "frost-sccache-launcher.cmake"), "--", sys.executable,
            str(fixture / "record_argv.py"), str(recorded), *arguments, env=env)
        assert json.loads(recorded.read_text(encoding="utf-8")) == arguments
        for source, build in ((source_a, build_a), (source_b, build_b)):
            before = stats()
            run(cmake, "--build", str(build), env=env)
            after = stats()
            assert count(after, "cache_misses") > count(before, "cache_misses"), (before, after)
            dump = run(str(readobj), "--codeview", str(build / "unit.obj"), env=env).lower().replace("/", "\\")
            assert str(source / "unit.cpp").lower() in dump, dump
        original = (build_a / "unit.obj").read_bytes()
        (build_a / "unit.obj").unlink()
        before = stats()
        run(cmake, "--build", str(build_a), env=env)
        after = stats()
        assert count(after, "cache_hits") > count(before, "cache_hits"), (before, after)
        assert (build_a / "unit.obj").read_bytes() == original
        # Unset and empty are equivalent; a nonempty caller buster set at build time
        # separates entries without reconfiguration, and remains stable on repeat builds.
        def rebuild_with(value, expected):
            (build_a / "unit.obj").unlink()
            build_env = env.copy()
            if value is not None:
                build_env["SCCACHE_C_CUSTOM_CACHE_BUSTER"] = value
            before = stats()
            run(cmake, "--build", str(build_a), env=build_env)
            after = stats()
            assert count(after, expected) > count(before, expected), (value, before, after)
            dump = run(str(readobj), "--codeview", str(build_a / "unit.obj"), env=env).lower().replace("/", "\\")
            assert str(source_a / "unit.cpp").lower() in dump
        rebuild_with("", "cache_hits")
        rebuild_with("user-one", "cache_misses")
        rebuild_with("user-one", "cache_hits")
        rebuild_with("user-two", "cache_misses")
        rebuild_with(None, "cache_hits")

        other_build = base / "other-build"
        run(cmake, "-S", str(source_a), "-B", str(other_build), "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DFROST_REPO={repo.as_posix()}",
            f"-DFROST_CLANG_CL={clang.as_posix()}", env=env)
        before = stats()
        run(cmake, "--build", str(other_build), env=env)
        assert count(stats(), "cache_misses") > count(before, "cache_misses")

        custom_build = base / "custom-build"
        run(cmake, "-S", str(source_a), "-B", str(custom_build), "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DFROST_REPO={repo.as_posix()}",
            f"-DFROST_CLANG_CL={clang.as_posix()}", "-DCMAKE_CXX_COMPILER_LAUNCHER=user-launcher", env=env)
        assert (custom_build / "launcher.txt").read_text() == "user-launcher"
        assert not (custom_build / "frost-sccache-launcher.cmake").exists()
        print("distinct worktrees retain debug sources; same-tree cache hits and caller busters are preserved")
    finally:
        prior_failure = sys.exc_info()[0] is not None
        try:
            run(str(sccache), "--stop-server", env=env, timeout=10)
        except (AssertionError, OSError, subprocess.TimeoutExpired) as cleanup_error:
            if not prior_failure:
                raise
            print(f"sccache cleanup also failed: {cleanup_error}", file=sys.stderr)
