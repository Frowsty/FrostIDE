# SPDX-License-Identifier: Apache-2.0
"""Exercise the real Windows updater and parent-exit barrier with disposable installs."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import install_user

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.GetCurrentProcess.restype = wintypes.HANDLE
kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel.CloseHandle.argtypes = [wintypes.HANDLE]


def as_parent(helper, install, backup, archive):
    times = [wintypes.FILETIME() for _ in range(4)]
    assert kernel.GetProcessTimes(kernel.GetCurrentProcess(), *(ctypes.byref(item) for item in times))
    creation = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
    release = dict(schema=1, version='2.0.0', build=200,
                   sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
    receipt_digest = hashlib.sha256((install / install_user.MARKER).read_bytes()).hexdigest()
    with (helper.parent / 'helper-error.txt').open('w') as errors:
        child = subprocess.Popen([helper, install, backup, archive], cwd=helper.parent, text=True,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors)
        child.stdin.write(f'{os.getpid()} {creation}\n{archive.stat().st_size}\n{json.dumps(release)}\n'
                          f'{receipt_digest}\n')
        child.stdin.flush()
        assert child.stdout.readline() == 'ready\n', (helper.parent / 'helper-error.txt').read_text()
        print(child.pid, flush=True)
        if sys.stdin.readline().strip() == 'apply':
            child.stdin.write('apply\n')
        child.stdin.close()


def app_image(base, version, health):
    return base + f'\nFROST_HELPER_META:version={version};selftest_exit={health};run_exit=0\n'.encode()


def run(helper, base):
    with tempfile.TemporaryDirectory(prefix='frost-inplace-') as temporary:
        root = Path(temporary)
        stage = root / 'original'
        stage.mkdir()
        original = app_image(base, '1.0.0', 0)
        (stage / 'frost.exe').write_bytes(original)
        support = ('frost_app.dll', 'LICENSE', 'NOTICE')
        for name in support:
            (stage / name).write_text(name, encoding='utf-8')
        for case, decision, health, expected in (
            ('approved', 'apply', 0, 0),
            ('rollback', 'apply', 42, 4),
            ('vetoed', 'cancel', 0, 3),
            ('foreign-refused', 'apply', 0, 3),
            ('changed-receipt', 'apply', 0, 3),
        ):
            work = root / case
            work.mkdir()
            install = work / 'installed'
            install_user.install(stage, install, compress=False)
            marker = install / install_user.MARKER
            previous = marker.read_bytes()
            foreign = install / 'foreign.txt'
            if case == 'foreign-refused':
                foreign.write_text('keep me', encoding='utf-8')
            backup = work / 'backup'
            archive = work / 'payload.zip'
            candidate = app_image(base, '2.0.0', health)
            with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as payload:
                payload.writestr('frost.exe', candidate)
                for name in support:
                    payload.writestr(name, f'updated {name}')
            private_helper = shutil.copy2(helper, work / helper.name)
            with subprocess.Popen([sys.executable, '-B', __file__, 'parent', private_helper, install, backup,
                                   archive], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as parent:
                pid = int(parent.stdout.readline())
                process = kernel.OpenProcess(0x00100000 | 0x1000, False, pid)  # SYNCHRONIZE | query
                assert process, ctypes.get_last_error()
                try:
                    assert marker.read_bytes() == previous and not backup.exists()
                    if case == 'changed-receipt':
                        marker.write_bytes(previous + b'\n')
                    parent.communicate(decision + '\n', timeout=10)
                    assert parent.returncode == 0
                    assert kernel.WaitForSingleObject(process, 90000) == 0, case
                    code = wintypes.DWORD()
                    assert kernel.GetExitCodeProcess(process, ctypes.byref(code))
                    assert code.value == expected, (case, code.value, (work / 'helper-error.txt').read_text())
                finally:
                    kernel.CloseHandle(process)
            exe = install / 'frost.exe'
            assert exe.read_bytes() == (candidate if case == 'approved' else original)
            if case == 'changed-receipt':
                assert marker.read_bytes() == previous + b'\n'
                assert 'receipt changed' in (work / 'update-result.txt').read_text()
                marker.write_bytes(previous)
            assert (marker.read_bytes() != previous) == (case == 'approved')
            if case == 'foreign-refused':
                assert foreign.read_text(encoding='utf-8') == 'keep me'
                assert 'foreign files' in (work / 'update-result.txt').read_text()
                foreign.unlink()
            if case == 'rollback':
                assert 'rolled back' in (work / 'update-result.txt').read_text()
            # The helper starts the active version before it exits. Its disposable app exits promptly.
            for attempt in range(50):
                try:
                    assert install_user.uninstall(install) == []
                    break
                except PermissionError:
                    if attempt == 49:
                        raise
                    time.sleep(0.05)
            assert not install.exists(), case
            print(f'{case}: native helper exit {expected}, uninstall passed', flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'parent':
        as_parent(*map(Path, sys.argv[2:]))
    else:
        run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).read_bytes())
