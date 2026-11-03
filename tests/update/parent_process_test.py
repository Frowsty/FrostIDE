# SPDX-License-Identifier: Apache-2.0
# Actual Windows child exit and stale process identity, without starting an installer.
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

if sys.platform != 'win32':
    raise SystemExit(77)
binary = str(Path(sys.argv[1]).resolve(strict=True))
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
kernel.GetProcessTimes.restype = wintypes.BOOL
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.CloseHandle.restype = wintypes.BOOL
child = subprocess.Popen([sys.executable, '-I', '-c', 'import sys; sys.stdin.read()'], stdin=subprocess.PIPE)
handle = None
probe = None
reader = None
try:
    handle = kernel.OpenProcess(0x1000, False, child.pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    created, exited, cpu, user = (wintypes.FILETIME() for _ in range(4))
    if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(cpu), ctypes.byref(user)):
        raise ctypes.WinError(ctypes.get_last_error())
    env = os.environ.copy()
    env['FROST_UPDATE_PARENT_PID'] = str(child.pid)
    env['FROST_UPDATE_PARENT_CREATED'] = str((created.dwHighDateTime << 32) | created.dwLowDateTime)
    env.pop('FROST_UPDATE_PARENT_ALREADY_EXITED', None)
    probe = subprocess.Popen([binary, '--gtest_filter=UpdateParent.RetainedProcessProbe'], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = queue.Queue()
    output = []

    def read_output():
        for line in probe.stdout:
            output.append(line)
            lines.put(line)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    deadline = time.monotonic() + 10
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError('Guard did not retain the live child')
        line = lines.get(timeout=remaining)
        if line.strip() == 'PARENT_GUARD_READY':
            break
    if child.poll() is not None:
        raise RuntimeError('Child exited before the retained-handle handshake')
    child.stdin.close()
    if child.wait(timeout=10) != 0 or probe.wait(timeout=10) != 0:
        raise RuntimeError('Child exit observation failed')
    reader.join(timeout=2)
    print(''.join(output))
    env['FROST_UPDATE_PARENT_ALREADY_EXITED'] = '1'
    rejected = subprocess.run([binary, '--gtest_filter=UpdateParent.RetainedProcessProbe'], env=env,
                              capture_output=True, text=True, timeout=10)
    print(rejected.stdout, rejected.stderr)
    if rejected.returncode:
        raise RuntimeError('Exited parent was not refused before admission')
    print('Retained process: live barrier, clean exit observation, and already-exited refusal passed')
finally:
    for process in (probe, child):
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
    if reader is not None:
        reader.join(timeout=2)
    if probe is not None and probe.stdout is not None:
        probe.stdout.close()
    if handle:
        kernel.CloseHandle(handle)
