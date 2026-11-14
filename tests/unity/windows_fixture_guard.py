# SPDX-License-Identifier: Apache-2.0
# Windows-only owned jobs and durable typed EditorPrefs preservation for the legacy Unity fixtures.
# Pure snapshot/guard policy is injectable for portable unit tests. No Unity is started on import.
from __future__ import annotations

import base64
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from typing import Callable

PREFS_KEY = r"Software\Unity Technologies\Unity Editor 5.x"
KNOWN_WRITERS = frozenset({"unity.exe", "unity hub.exe", "unityhub.exe", "unitypackagemanager.exe"})


class GuardError(RuntimeError):
    """A bounded guard failure whose text contains no preference values."""


@dataclass(frozen=True)
class TypedValue:
    kind: int
    raw: bytes


@dataclass(frozen=True)
class Snapshot:
    keys: frozenset[str]
    values: dict[tuple[str, str], TypedValue]

    def encode(self) -> bytes:
        data = {
            "version": 1,
            "keys": sorted(self.keys),
            "values": [
                [key, name, value.kind, base64.b64encode(value.raw).decode("ascii")]
                for (key, name), value in sorted(self.values.items())
            ],
        }
        return json.dumps(data, ensure_ascii=True, separators=(",", ":")).encode("ascii")

    @staticmethod
    def decode(data: bytes) -> Snapshot:
        value = json.loads(data)
        if value["version"] != 1:
            raise GuardError("Unsupported preference snapshot format")
        values = {}
        for key, name, kind, raw in value["values"]:
            if key not in value["keys"] or (key, name) in values:
                raise GuardError("Invalid preference snapshot structure")
            values[key, name] = TypedValue(kind, base64.b64decode(raw, validate=True))
        return Snapshot(frozenset(value["keys"]), values)


def durable_write(path: Path, data: bytes) -> None:
    """Create a fresh file and flush its complete contents before returning."""
    with path.open("xb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def windows_only() -> None:
    if os.name != "nt":
        raise GuardError("The legacy Unity fixture guard requires Windows")


def restrict_current_user(path: Path) -> None:
    """Protect an owned fixture directory/file with the current user's explicit DACL."""
    windows_only()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.LocalFree.argtypes = [wintypes.HLOCAL]
    security.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    security.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                            wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    security.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.LPWSTR)]
    security.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    security.SetFileSecurityW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p]
    token = wintypes.HANDLE()
    if not security.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)):
        raise GuardError("Cannot identify the fixture owner")
    try:
        size = wintypes.DWORD()
        security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
        information = ctypes.create_string_buffer(size.value)
        if not security.GetTokenInformation(token, 1, information, size, ctypes.byref(size)):
            raise GuardError("Cannot query the fixture owner")
        sid = ctypes.cast(information, ctypes.POINTER(ctypes.c_void_p))[0]
        text = wintypes.LPWSTR()
        if not security.ConvertSidToStringSidW(sid, ctypes.byref(text)):
            raise GuardError("Cannot format the fixture owner")
        try:
            inheritance = "OICI" if path.is_dir() else ""
            descriptor = ctypes.c_void_p()
            if not security.ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    f"D:P(A;{inheritance};FA;;;{text.value})", 1, ctypes.byref(descriptor), None):
                raise GuardError("Cannot create the fixture security descriptor")
            try:
                if not security.SetFileSecurityW(str(path), 0x80000004, descriptor):
                    raise GuardError("Cannot restrict fixture access to its current user")
            finally:
                kernel.LocalFree(descriptor)
        finally:
            kernel.LocalFree(text)
    finally:
        kernel.CloseHandle(token)


class Dpapi:
    def __init__(self) -> None:
        windows_only()
        self.api = ctypes.WinDLL("crypt32", use_last_error=True)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)

    class Blob(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("data", ctypes.POINTER(ctypes.c_ubyte))]

    def _convert(self, data: bytes, protect: bool) -> bytes:
        storage = ctypes.create_string_buffer(data)
        source = self.Blob(len(data), ctypes.cast(storage, ctypes.POINTER(ctypes.c_ubyte)))
        result = self.Blob()
        if protect:
            ok = self.api.CryptProtectData(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(result))
        else:
            ok = self.api.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(result))
        if not ok:
            raise GuardError("Cannot protect or read the current-user preference snapshot")
        self.kernel.LocalFree.argtypes = [wintypes.HLOCAL]
        self.kernel.LocalFree.restype = wintypes.HLOCAL
        try:
            return ctypes.string_at(result.data, result.size)
        finally:
            self.kernel.LocalFree(result.data)

    def protect(self, data: bytes) -> bytes:
        return self._convert(data, True)

    def unprotect(self, data: bytes) -> bytes:
        return self._convert(data, False)


class WindowsPreferences:
    """Capture raw registry types/bytes, including absent and empty keys. Main-thread only."""

    def __init__(self) -> None:
        windows_only()
        import winreg
        self.registry = winreg
        self.api = ctypes.WinDLL("advapi32", use_last_error=True)
        self.api.RegQueryValueExW.argtypes = [wintypes.HKEY, wintypes.LPCWSTR, ctypes.c_void_p,
                                            ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
                                            ctypes.POINTER(wintypes.DWORD)]
        self.api.RegSetValueExW.argtypes = [wintypes.HKEY, wintypes.LPCWSTR, wintypes.DWORD,
                                          wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]

    def capture(self) -> Snapshot:
        keys = set()
        values = {}
        registry = self.registry

        def walk(relative: str) -> None:
            path = PREFS_KEY + ("\\" + relative if relative else "")
            try:
                handle = registry.OpenKey(registry.HKEY_CURRENT_USER, path, 0, registry.KEY_READ)
            except FileNotFoundError:
                return
            with handle:
                keys.add(relative)
                subkeys, count, _ = registry.QueryInfoKey(handle)
                for index in range(count):
                    name = registry.EnumValue(handle, index)[0]
                    kind = wintypes.DWORD()
                    size = wintypes.DWORD()
                    if self.api.RegQueryValueExW(int(handle), name, None, ctypes.byref(kind), None,
                                                ctypes.byref(size)) != 0:
                        raise GuardError("Cannot query a preference value")
                    data = ctypes.create_string_buffer(max(1, size.value))
                    if self.api.RegQueryValueExW(int(handle), name, None, ctypes.byref(kind), data,
                                                ctypes.byref(size)) != 0:
                        raise GuardError("Preference changed during capture")
                    values[relative, name] = TypedValue(kind.value, data.raw[:size.value])
                for index in range(subkeys):
                    name = registry.EnumKey(handle, index)
                    walk(relative + "\\" + name if relative else name)

        walk("")
        return Snapshot(frozenset(keys), values)

    def restore(self, expected: Snapshot, baseline: Snapshot) -> None:
        if self.capture() != expected:
            raise GuardError("Preferences changed before restoration; encrypted recovery evidence retained")
        registry = self.registry
        for relative in sorted(baseline.keys - expected.keys, key=lambda key: key.count("\\")):
            path = PREFS_KEY + ("\\" + relative if relative else "")
            with registry.CreateKeyEx(registry.HKEY_CURRENT_USER, path, 0, registry.KEY_WRITE):
                pass
        for (relative, name), value in baseline.values.items():
            if expected.values.get((relative, name)) == value:
                continue
            path = PREFS_KEY + ("\\" + relative if relative else "")
            with registry.OpenKey(registry.HKEY_CURRENT_USER, path, 0, registry.KEY_WRITE) as handle:
                data = ctypes.create_string_buffer(value.raw)
                if self.api.RegSetValueExW(int(handle), name, 0, value.kind, data, len(value.raw)) != 0:
                    raise GuardError("Cannot restore a typed preference value")
                registry.FlushKey(handle)
        for relative, name in expected.values.keys() - baseline.values.keys():
            path = PREFS_KEY + ("\\" + relative if relative else "")
            with registry.OpenKey(registry.HKEY_CURRENT_USER, path, 0, registry.KEY_WRITE) as handle:
                registry.DeleteValue(handle, name)
                registry.FlushKey(handle)
        for relative in sorted(expected.keys - baseline.keys, key=lambda key: key.count("\\"), reverse=True):
            registry.DeleteKey(registry.HKEY_CURRENT_USER, PREFS_KEY + ("\\" + relative if relative else ""))


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    created: int
    name: str


def known_writers() -> set[ProcessIdentity]:
    """Read OS process names and creation times. Refuse inaccessible known writers."""
    windows_only()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)

    class Entry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("name", wintypes.WCHAR * 260)]

    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == wintypes.HANDLE(-1).value:
        raise GuardError("Cannot enumerate existing preference writers")
    result = set()
    try:
        entry = Entry()
        entry.size = ctypes.sizeof(entry)
        ok = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            if entry.name.lower() in KNOWN_WRITERS:
                process = kernel.OpenProcess(0x1000, False, entry.pid)
                if not process:
                    raise GuardError("Cannot establish the identity of a known preference writer")
                try:
                    times = [wintypes.FILETIME() for _ in range(4)]
                    if not kernel.GetProcessTimes(process, *(ctypes.byref(item) for item in times)):
                        raise GuardError("Cannot query a known preference writer")
                    created = times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)
                    result.add(ProcessIdentity(entry.pid, created, entry.name.lower()))
                finally:
                    kernel.CloseHandle(process)
            ok = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:
            raise GuardError("Preference writer enumeration did not complete")
    finally:
        kernel.CloseHandle(snapshot)
    return result


class PreferenceGuard:
    """Retain a fresh encrypted baseline, own jobs, and refuse unsafe restoration.

    Writer detection is polling. Short-lived writers and same-value concurrent edits remain the
    explicit ADR0071 attribution limits. The guard proves final equality when successful.
    """

    def __init__(self, directory: Path, preferences, cipher, inventory: Callable = known_writers) -> None:
        self.directory = directory
        self.preferences = preferences
        self.cipher = cipher
        self.inventory = inventory
        self.jobs = []
        self.baseline = None
        self.baseline_durable = False
        self.unsafe = False
        self.monitor_failure = False
        self.finished = threading.Event()
        self.thread = None
        self.lock = threading.RLock()

    def begin(self) -> None:
        self.check_writers()
        self.baseline = self.preferences.capture()
        durable_write(self.directory / "preferences-before.dpapi", self.cipher.protect(self.baseline.encode()))
        saved = Snapshot.decode(self.cipher.unprotect((self.directory / "preferences-before.dpapi").read_bytes()))
        if saved != self.baseline:
            raise GuardError("Cannot verify the current-run durable preference baseline")
        self.baseline_durable = True
        self.check_writers()
        self.thread = threading.Thread(target=self._monitor, name="UnityPreferenceGuard", daemon=True)
        self.thread.start()

    def add_job(self, job) -> None:
        with self.lock:
            if not self.baseline_durable:
                raise GuardError("A durable preference baseline must precede child creation")
            self.jobs.append(job)

    def check_writers(self) -> None:
        with self.lock:
            if any(not any(job.owns(process) for job in self.jobs) for process in self.inventory()):
                self.unsafe = True
            if self.unsafe or self.monitor_failure:
                raise GuardError("An unowned preference writer was observed; automatic restoration refused")

    def _monitor(self) -> None:
        while not self.finished.wait(0.05):
            try:
                self.check_writers()
            except Exception:
                self.monitor_failure = True
                return

    def restore(self) -> dict:
        try:
            with self.lock:
                if not self.baseline_durable:
                    raise GuardError("No durable preference baseline exists")
                if any(not job.empty() for job in self.jobs):
                    raise GuardError("Owned jobs must be verified empty before preference restoration")
                self.check_writers()
                after = self.preferences.capture()
                durable_write(self.directory / "preferences-after.dpapi", self.cipher.protect(after.encode()))
                # Read back the exact current-run durable baseline. No historical export is accepted.
                saved = (self.directory / "preferences-before.dpapi").read_bytes()
                baseline = Snapshot.decode(self.cipher.unprotect(saved))
                if baseline != self.baseline:
                    raise GuardError("Durable preference baseline differs from this run")
                self.preferences.restore(after, baseline)
                final = self.preferences.capture()
                if final != baseline:
                    raise GuardError("Preference restoration left differences; encrypted evidence retained")
                changed = sum(after.values.get(key) != baseline.values.get(key)
                              for key in after.values.keys() | baseline.values.keys())
                self.finished.set()
                return {"changedValues": changed, "finalDifferences": 0,
                        "baselineSha256": hashlib.sha256(baseline.encode()).hexdigest()}
        finally:
            self.finished.set()
            if self.thread is not None:
                self.thread.join()


class _SecurityAttributes(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("descriptor", ctypes.c_void_p), ("inherit", wintypes.BOOL)]


class _StartupInfo(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("reserved", wintypes.LPWSTR), ("desktop", wintypes.LPWSTR),
                ("title", wintypes.LPWSTR), ("x", wintypes.DWORD), ("y", wintypes.DWORD),
                ("width", wintypes.DWORD), ("height", wintypes.DWORD), ("columns", wintypes.DWORD),
                ("rows", wintypes.DWORD), ("fill", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("show", wintypes.WORD), ("reservedSize", wintypes.WORD), ("reservedData", ctypes.c_void_p),
                ("input", wintypes.HANDLE), ("output", wintypes.HANDLE), ("error", wintypes.HANDLE)]


class _StartupInfoEx(ctypes.Structure):
    _fields_ = [("startup", _StartupInfo), ("attributes", ctypes.c_void_p)]


class _ProcessInfo(ctypes.Structure):
    _fields_ = [("process", wintypes.HANDLE), ("thread", wintypes.HANDLE),
                ("pid", wintypes.DWORD), ("threadId", wintypes.DWORD)]


class _BasicLimits(ctypes.Structure):
    _fields_ = [("processTime", ctypes.c_int64), ("jobTime", ctypes.c_int64), ("flags", wintypes.DWORD),
                ("minimum", ctypes.c_size_t), ("maximum", ctypes.c_size_t), ("active", wintypes.DWORD),
                ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [("basic", _BasicLimits), ("io", ctypes.c_uint64 * 6),
                ("processMemory", ctypes.c_size_t), ("jobMemory", ctypes.c_size_t),
                ("peakProcess", ctypes.c_size_t), ("peakJob", ctypes.c_size_t)]


class OwnedJob:
    """A process/descendant job assigned while suspended, before any child code executes.

    Only the private log and NUL input handles are inherited. Closing a live job kills its owned
    processes. Keep this object open until PreferenceGuard has verified every job empty.
    """

    def __init__(self, guard: PreferenceGuard) -> None:
        windows_only()
        self.guard = guard
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.api.CreateJobObjectW.restype = wintypes.HANDLE
        self.api.CloseHandle.argtypes = [wintypes.HANDLE]
        self.api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                       ctypes.POINTER(_SecurityAttributes), wintypes.DWORD,
                                       wintypes.DWORD, wintypes.HANDLE]
        self.api.CreateFileW.restype = wintypes.HANDLE
        self.api.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        self.api.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                       wintypes.DWORD, ctypes.c_void_p]
        self.api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.api.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self.api.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self.api.ResumeThread.argtypes = [wintypes.HANDLE]
        self.api.ResumeThread.restype = wintypes.DWORD
        self.api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.api.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        self.api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.api.OpenProcess.restype = wintypes.HANDLE
        self.api.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
        self.api.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        self.api.InitializeProcThreadAttributeList.argtypes = [ctypes.c_void_p, wintypes.DWORD,
                                                              wintypes.DWORD, ctypes.POINTER(ctypes.c_size_t)]
        self.api.UpdateProcThreadAttribute.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t,
                                                      ctypes.c_void_p, ctypes.c_size_t,
                                                      ctypes.c_void_p, ctypes.c_void_p]
        self.api.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
        self.api.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p,
                                          ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p,
                                          wintypes.LPCWSTR, ctypes.POINTER(_StartupInfoEx),
                                          ctypes.POINTER(_ProcessInfo)]
        self.handle = self.api.CreateJobObjectW(None, None)
        self.process = None
        self.pid = 0
        self.assigned = False
        if not self.handle:
            raise GuardError("Cannot create an owned fixture job")
        limits = _ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, no breakaway.
        if not self.api.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.api.CloseHandle(self.handle)
            self.handle = None
            raise GuardError("Cannot contain fixture descendants")
        try:
            guard.add_job(self)
        except Exception:
            self.api.CloseHandle(self.handle)
            self.handle = None
            raise

    def pids(self) -> set[int]:
        if self.handle is None:
            raise GuardError("Cannot inspect a closed fixture job")
        capacity = 64
        while capacity <= 65536:
            data = ctypes.create_string_buffer(8 + capacity * ctypes.sizeof(ctypes.c_size_t))
            if self.api.QueryInformationJobObject(self.handle, 3, data, len(data), None):
                count = ctypes.c_uint32.from_buffer(data, 4).value
                if count > capacity:
                    raise GuardError("Invalid fixture job process list")
                members = (ctypes.c_size_t * count).from_buffer(data, 8)
                return set(members)
            if ctypes.get_last_error() != 234:
                raise GuardError("Cannot verify the owned fixture job")
            capacity *= 2
        raise GuardError("Fixture job exceeds its bounded process inventory")

    def empty(self) -> bool:
        if self.process is not None and not self.assigned and self.poll() is None:
            return False
        return not self.pids()

    def owns(self, identity: ProcessIdentity) -> bool:
        # Check the held OS process object and creation time, avoiding PID-reuse attribution.
        process = self.api.OpenProcess(0x1000, False, identity.pid)
        if not process:
            raise GuardError("Cannot establish preference-writer ownership")
        try:
            times = [wintypes.FILETIME() for _ in range(4)]
            member = wintypes.BOOL()
            if not self.api.GetProcessTimes(process, *(ctypes.byref(item) for item in times)):
                raise GuardError("Cannot establish preference-writer lifetime")
            created = times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)
            if not self.api.IsProcessInJob(process, self.handle, ctypes.byref(member)):
                raise GuardError("Cannot establish preference-writer job membership")
            return created == identity.created and bool(member.value)
        finally:
            self.api.CloseHandle(process)

    def start(self, arguments: list[str], environment: dict[str, str], directory: Path, log: Path) -> None:
        if self.process is not None or not Path(arguments[0]).is_absolute():
            raise GuardError("Fixture launch requires a fresh job and absolute executable")
        attributes = _SecurityAttributes(ctypes.sizeof(_SecurityAttributes), None, True)
        output = self.api.CreateFileW(str(log), 0x40000000, 1, ctypes.byref(attributes), 1, 0x80, None)
        input_handle = self.api.CreateFileW("NUL", 0x80000000, 3, ctypes.byref(attributes), 3, 0x80, None)
        invalid = wintypes.HANDLE(-1).value
        if output == invalid or input_handle == invalid:
            for handle in (output, input_handle):
                if handle != invalid:
                    self.api.CloseHandle(handle)
            raise GuardError("Cannot create private fixture process streams")
        size = ctypes.c_size_t()
        self.api.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
        storage = ctypes.create_string_buffer(size.value)
        initialized = False
        information = _ProcessInfo()
        try:
            if not self.api.InitializeProcThreadAttributeList(storage, 1, 0, ctypes.byref(size)):
                raise GuardError("Cannot initialize fixture handle inheritance")
            initialized = True
            handles = (wintypes.HANDLE * 2)(input_handle, output)
            if not self.api.UpdateProcThreadAttribute(storage, 0, 0x20002, handles, ctypes.sizeof(handles), None, None):
                raise GuardError("Cannot restrict fixture handle inheritance")
            startup = _StartupInfoEx()
            startup.startup.cb = ctypes.sizeof(startup)
            startup.startup.flags = 0x101  # STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW.
            startup.startup.show = 0
            startup.startup.input = input_handle
            startup.startup.output = output
            startup.startup.error = output
            startup.attributes = ctypes.cast(storage, ctypes.c_void_p)
            command = ctypes.create_unicode_buffer(subprocess.list2cmdline(arguments))
            block = ctypes.create_unicode_buffer("\0".join(f"{key}={value}" for key, value in sorted(
                environment.items(), key=lambda item: item[0].upper())) + "\0\0")
            # Hold the inventory lock across creation/assignment, avoiding a false unowned-writer gap.
            with self.guard.lock:
                self.guard.check_writers()
                if not self.api.CreateProcessW(arguments[0], command, None, None, True,
                                               0x80404, block, str(directory), ctypes.byref(startup),
                                               ctypes.byref(information)):
                    raise GuardError("Cannot create the suspended fixture process")
                self.process = information.process
                self.pid = information.pid
                if not self.api.AssignProcessToJobObject(self.handle, self.process):
                    self.terminate()
                    raise GuardError("Cannot assign the suspended fixture process to its owned job")
                self.assigned = True
                if self.api.ResumeThread(information.thread) == 0xFFFFFFFF:
                    self.terminate()
                    raise GuardError("Cannot resume the contained fixture process")
        finally:
            if information.thread:
                self.api.CloseHandle(information.thread)
            if initialized:
                self.api.DeleteProcThreadAttributeList(storage)
            self.api.CloseHandle(output)
            self.api.CloseHandle(input_handle)

    def poll(self) -> int | None:
        if self.process is None:
            raise GuardError("Fixture job has no launched process")
        state = self.api.WaitForSingleObject(self.process, 0)
        if state == 258:
            return None
        if state != 0:
            raise GuardError("Cannot observe fixture process exit")
        code = wintypes.DWORD()
        if not self.api.GetExitCodeProcess(self.process, ctypes.byref(code)):
            raise GuardError("Cannot read fixture process exit status")
        return code.value

    def wait(self, timeout: float) -> int:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.guard.check_writers()
            code = self.poll()
            if code is not None:
                return code
            time.sleep(0.02)
        raise GuardError("Owned fixture process exceeded its deadline")

    def wait_empty(self, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.empty():
                return
            time.sleep(0.02)
        raise GuardError("Owned fixture descendants did not exit")

    def terminate(self) -> None:
        if self.process is not None and not self.assigned and self.poll() is None:
            if not self.api.TerminateProcess(self.process, 1):
                raise GuardError("Cannot terminate an unassigned suspended fixture process")
            if self.api.WaitForSingleObject(self.process, 10000) != 0:
                raise GuardError("The suspended fixture process did not exit")
        if not self.empty():
            if not self.api.TerminateJobObject(self.handle, 1):
                raise GuardError("Cannot terminate the owned fixture job")
        self.wait_empty(10)

    def close(self) -> None:
        if not self.empty():
            raise GuardError("Cannot release a nonempty fixture job")
        if self.process is not None:
            self.api.CloseHandle(self.process)
            self.process = None
        self.api.CloseHandle(self.handle)
        self.handle = None
