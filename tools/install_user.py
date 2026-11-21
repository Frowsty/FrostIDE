# SPDX-License-Identifier: Apache-2.0
# Per-user zip deployment. Manifest-owned removal preserves foreign files; never writes HKLM.
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys

MARKER = '.frost-install.json'
PROGID = 'FrostIDE.Project'
EXTENSIONS = ('.frostproj', '.sln', '.slnx', '.lua')
CLASSES = 'Software\\Classes\\'
OPEN_COMMAND = r'\shell\open\command'
CMAKE_VERB = r'SystemFileAssociations\.txt\shell\FrostIDE.CMakeLists'
CMAKE_APPLIES_TO = 'System.FileName:="CMakeLists.txt"'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def payload(root):
    files = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('payload must not contain symbolic links')
        if path.is_file() and path.name != MARKER:
            files[path.relative_to(root).as_posix()] = digest(path)
    required = {'frost.exe', 'frost_app.dll', 'LICENSE', 'NOTICE'}
    if not required.issubset(files):
        raise ValueError('payload is not a complete Windows stage')
    return files

def owned_path(root, name):
    relative = Path(name)
    if relative.is_absolute() or '..' in relative.parts or ':' in name or '\\' in name:
        raise ValueError('invalid install manifest path')
    path = root / relative
    if not path.resolve().is_relative_to(root.resolve()) or path.is_symlink():
        raise ValueError('install manifest escapes its root')
    return path

def association_values(root):
    command = f'"{root / "frost.exe"}" "%1"'
    # Separate ownership groups let uninstall preserve a verb transferred to another install.
    project_command = PROGID + OPEN_COMMAND
    cmake_command = CMAKE_VERB + r'\command'
    values = {(PROGID, ''): ('FrostIDE Project', project_command),
              (project_command, ''): (command, project_command)}
    for extension in EXTENSIONS:
        values[(extension + r'\OpenWithProgids', PROGID)] = ('', project_command)
    values[(cmake_command, '')] = (command, cmake_command)
    values[(CMAKE_VERB, 'MUIVerb')] = ('Open CMake project in FrostIDE', cmake_command)
    values[(CMAKE_VERB, 'AppliesTo')] = (CMAKE_APPLIES_TO, cmake_command)
    return values


def key_prefixes(path):
    parts = path.split('\\')
    return ['\\'.join(parts[:index]) for index in range(1, len(parts) + 1)]


def owned_registration(values, receipt):
    if receipt is None:
        # Schema 1 installations registered only the original three values.
        paths = [(PROGID, ''), (PROGID + OPEN_COMMAND, ''), (r'.frostproj\OpenWithProgids', PROGID)]
        return paths, key_prefixes(PROGID + OPEN_COMMAND)
    if not isinstance(receipt, dict):
        raise ValueError('invalid association receipt')
    paths = receipt.get('values')
    keys = receipt.get('keys')
    if not isinstance(paths, list) or not isinstance(keys, list):
        raise ValueError('invalid association receipt')
    known_keys = {prefix for path, _ in values for prefix in key_prefixes(path)}
    if (any(not isinstance(item, list) or len(item) != 2 or
            any(not isinstance(part, str) for part in item) or tuple(item) not in values for item in paths)
            or any(not isinstance(key, str) or key not in known_keys for key in keys)):
        raise ValueError('association receipt contains an unrecognized entry')
    return [tuple(path) for path in paths], keys


def registration(root, remove=False, receipt=None):
    import winreg
    hive = winreg.HKEY_CURRENT_USER
    values = association_values(root)

    def read(path, name):
        try:
            with winreg.OpenKey(hive, CLASSES + path) as key:
                return winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            return None

    def delete_value(path, name):
        expected = (values[(path, name)][0], winreg.REG_SZ)
        try:
            with winreg.OpenKey(hive, CLASSES + path, 0, winreg.KEY_QUERY_VALUE | winreg.KEY_SET_VALUE) as key:
                if winreg.QueryValueEx(key, name) == expected:
                    winreg.DeleteValue(key, name)
        except FileNotFoundError:
            pass

    def prune(keys):
        for path in sorted(set(keys), key=lambda key: key.count('\\'), reverse=True):
            try:
                with winreg.OpenKey(hive, CLASSES + path) as key:
                    subkeys, entries, _ = winreg.QueryInfoKey(key)
                if not subkeys and not entries:
                    winreg.DeleteKey(hive, CLASSES + path)
            except FileNotFoundError:
                pass

    if remove:
        paths, keys = owned_registration(values, receipt)
        groups = {values[path][1] for path in paths}
        owned_groups = {path for path in groups if read(path, '') == (values[(path, '')][0], winreg.REG_SZ)}
        owned_keys = {prefix for path in paths if values[path][1] in owned_groups for prefix in key_prefixes(path[0])}
        for path, name in reversed(paths):
            if values[(path, name)][1] in owned_groups:
                delete_value(path, name)
        prune(set(keys) & owned_keys)
        return None

    # Choices belonging to a different executable or changed by the user are never replaced.
    for (path, name), (value, _) in values.items():
        if read(path, name) not in (None, (value, winreg.REG_SZ)):
            raise ValueError(f'conflicting Windows association: {path} ({name or "default"})')

    written, created = [], []
    try:
        for (path, name), (value, _) in values.items():
            for prefix in key_prefixes(path):
                try:
                    with winreg.OpenKey(hive, CLASSES + prefix):
                        continue
                except FileNotFoundError:
                    with winreg.CreateKeyEx(hive, CLASSES + prefix, 0, winreg.KEY_READ | winreg.KEY_WRITE):
                        created.append(prefix)
            with winreg.OpenKey(hive, CLASSES + path, 0, winreg.KEY_QUERY_VALUE | winreg.KEY_SET_VALUE) as key:
                try:
                    existing = winreg.QueryValueEx(key, name)
                except FileNotFoundError:
                    written.append((path, name))
                    winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
                else:
                    if existing != (value, winreg.REG_SZ):
                        raise ValueError(f'conflicting Windows association: {path} ({name or "default"})')
    except (OSError, ValueError):
        for path, name in reversed(written):
            delete_value(path, name)
        prune(created)
        raise
    return {'values': [list(path) for path in written], 'keys': created}


def configure_compression(directory, enabled):
    """Set the inheritance policy of the fresh install directory on local NTFS."""
    if os.name != 'nt':
        return False
    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    mount = ctypes.create_unicode_buffer(32768)
    kernel.GetVolumePathNameW.argtypes = (wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD)
    kernel.GetDriveTypeW.argtypes = (wintypes.LPCWSTR,)
    if not kernel.GetVolumePathNameW(str(directory), mount, len(mount)) or kernel.GetDriveTypeW(mount) not in (2, 3):
        return False  # Local removable or fixed volume only.
    flags = wintypes.DWORD()
    filesystem = ctypes.create_unicode_buffer(32)
    kernel.GetVolumeInformationW.argtypes = (wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD)
    # NTFS reports FILE_FILE_COMPRESSION only for cluster sizes up to 4 KiB (MS-FSA, Appendix A).
    if (not kernel.GetVolumeInformationW(mount, None, 0, None, None, ctypes.byref(flags), filesystem, len(filesystem))
            or filesystem.value.casefold() != 'ntfs' or not flags.value & 0x10):
        return False
    attributes = directory.stat().st_file_attributes
    if bool(attributes & stat.FILE_ATTRIBUTE_COMPRESSED) == enabled:
        return True

    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.DeviceIoControl.argtypes = (wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
    # OPEN_EXISTING, BACKUP_SEMANTICS | OPEN_REPARSE_POINT, share read/write/delete.
    handle = kernel.CreateFileW(str(directory), 0xC0000000, 7, None, 3, 0x02200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        state = wintypes.WORD(int(enabled))  # COMPRESSION_FORMAT_DEFAULT or COMPRESSION_FORMAT_NONE.
        transferred = wintypes.DWORD()
        if not kernel.DeviceIoControl(handle, 0x9C040, ctypes.byref(state), ctypes.sizeof(state),
                                      None, 0, ctypes.byref(transferred), None):  # FSCTL_SET_COMPRESSION
            error = ctypes.get_last_error()
            if enabled and error in (1, 50, 769):  # Invalid function, unsupported, or disabled by filesystem policy.
                return False
            raise ctypes.WinError(error)
    finally:
        kernel.CloseHandle(handle)
    return True


def install(stage, destination, associate=False, compress=True):
    stage, destination = stage.resolve(), destination.resolve()
    if stage == destination or stage.is_relative_to(destination) or destination.is_relative_to(stage):
        raise ValueError('stage and destination must be disjoint')
    files = payload(stage)
    if destination.exists():
        raise ValueError('destination already exists; uninstall first')
    destination.mkdir(parents=True)
    receipt = None
    try:
        inherit_compression = configure_compression(destination, compress)
        for name, expected in files.items():
            target = owned_path(destination, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            source = stage / name
            if inherit_compression:
                # Create the file under the directory's policy rather than copying source attributes.
                with source.open('rb') as contents, target.open('xb') as installed:
                    shutil.copyfileobj(contents, installed)
                shutil.copystat(source, target)
            else:
                shutil.copy2(source, target)
            if digest(target) != expected:
                raise ValueError('payload changed while copying')
        marker = {'schema': 1, 'files': files, 'associated': associate}
        if associate:
            receipt = registration(destination)
            marker['associationReceipt'] = receipt
        (destination / MARKER).write_text(json.dumps(marker), encoding='utf-8')
    except Exception:
        if receipt is not None:
            registration(destination, remove=True, receipt=receipt)
        # Directory was created exclusively by this operation.
        shutil.rmtree(destination)
        raise

def uninstall(destination):
    destination = destination.resolve()
    manifest = json.loads((destination / MARKER).read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or not isinstance(manifest.get('files'), dict):
        raise ValueError('invalid install manifest')
    paths = [(owned_path(destination, name), expected) for name, expected in manifest['files'].items()]
    if os.name == 'nt' and manifest.get('associated', False):
        registration(destination, remove=True, receipt=manifest.get('associationReceipt'))
    preserved = []
    for path, expected in paths:
        if path.exists():
            if digest(path) == expected: path.unlink()
            else: preserved.append(str(path))
    # Retain the manifest when modified files remain, allowing deliberate recovery/removal later.
    if not preserved: (destination / MARKER).unlink()
    parents = {parent for path, _ in paths for parent in path.parents if parent != destination and parent.is_relative_to(destination)}
    for path in sorted(parents, key=lambda item: len(item.parts), reverse=True):
        if path.is_dir() and not path.is_symlink():
            try: path.rmdir()
            except OSError: pass
    try: destination.rmdir()
    except OSError: pass
    return preserved

def notify_shell_associations_changed():
    import ctypes
    from ctypes import wintypes

    notify = ctypes.WinDLL('shell32').SHChangeNotify
    notify.argtypes = (wintypes.LONG, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p)
    notify.restype = None
    notify(0x08000000, 0, None, None)  # SHCNE_ASSOCCHANGED, SHCNF_IDLIST


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('install', 'uninstall'))
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--associate', action='store_true')
    parser.add_argument('--no-compression', action='store_true', help='Store a fresh NTFS installation uncompressed')
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Windows per-user installer; Linux uses the staged archive')
    destination = args.destination or Path(os.environ['LOCALAPPDATA']) / 'Programs' / 'FrostIDE'
    if args.action == 'install':
        if not args.stage: parser.error('--stage is required')
        install(args.stage, destination, args.associate, compress=not args.no_compression)
        if args.associate:
            notify_shell_associations_changed()
    else:
        for path in uninstall(destination): print('Preserved modified file:', path)
        notify_shell_associations_changed()
    return 0

if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError) as error:
        print(f'install: {error}', file=sys.stderr)
        sys.exit(1)
