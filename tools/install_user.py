# SPDX-License-Identifier: Apache-2.0
# Per-user zip deployment. Manifest-owned removal preserves foreign files; never writes HKLM.
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

MARKER = '.frost-install.json'
PROGID = 'FrostIDE.Project'
EXTENSIONS = ('.frostproj',)

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

def registration(root, remove=False):
    import winreg
    base = 'Software\\Classes\\'
    command = f'"{root / "frost.exe"}" "%1"'
    # OpenWithProgids avoids replacing the user's chosen default application.
    entries = {PROGID: ('', 'FrostIDE Project'),
               PROGID + r'\shell\open\command': ('', command),
               '.frostproj' + r'\OpenWithProgids': (PROGID, '')}
    if remove:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base + PROGID + r'\shell\open\command') as key:
                if winreg.QueryValueEx(key, '')[0] != command:
                    return
        except FileNotFoundError:
            return
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base + '.frostproj' + r'\OpenWithProgids', 0, winreg.KEY_SET_VALUE) as key:
                try: winreg.DeleteValue(key, PROGID)
                except FileNotFoundError: pass
        except FileNotFoundError: pass
        for suffix in (r'\shell\open\command', r'\shell\open', r'\shell', ''):
            try: winreg.DeleteKey(winreg.HKEY_CURRENT_USER, base + PROGID + suffix)
            except FileNotFoundError: pass
        return
    changes = []
    try:
        for key, (name, value) in entries.items():
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base + key): pass
                created = False
            except FileNotFoundError: created = True
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + key) as handle:
                try: previous = winreg.QueryValueEx(handle, name)
                except FileNotFoundError: previous = None
                changes.append((key, name, previous, created))
                winreg.SetValueEx(handle, name, 0, winreg.REG_SZ, value)
    except OSError:
        for key, name, previous, created in reversed(changes):
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base + key, 0, winreg.KEY_SET_VALUE) as handle:
                if previous is None:
                    try: winreg.DeleteValue(handle, name)
                    except FileNotFoundError: pass
                else: winreg.SetValueEx(handle, name, 0, previous[1], previous[0])
            if created:
                try:
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base + key) as handle:
                        subkeys, values, _ = winreg.QueryInfoKey(handle)
                    if subkeys == 0 and values == 0: winreg.DeleteKey(winreg.HKEY_CURRENT_USER, base + key)
                except OSError: pass
        raise

def install(stage, destination, associate=False):
    stage, destination = stage.resolve(), destination.resolve()
    if stage == destination or stage.is_relative_to(destination) or destination.is_relative_to(stage):
        raise ValueError('stage and destination must be disjoint')
    files = payload(stage)
    if destination.exists():
        raise ValueError('destination already exists; uninstall first')
    destination.mkdir(parents=True)
    try:
        for name, expected in files.items():
            target = owned_path(destination, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(stage / name, target)
            if digest(target) != expected:
                raise ValueError('payload changed while copying')
        (destination / MARKER).write_text(json.dumps({'schema': 1, 'files': files, 'associated': associate}), encoding='utf-8')
        if associate: registration(destination)
    except Exception:
        # Directory was created exclusively by this operation.
        shutil.rmtree(destination)
        raise

def uninstall(destination):
    destination = destination.resolve()
    manifest = json.loads((destination / MARKER).read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or not isinstance(manifest.get('files'), dict):
        raise ValueError('invalid install manifest')
    paths = [(owned_path(destination, name), expected) for name, expected in manifest['files'].items()]
    if os.name == 'nt' and manifest.get('associated', False): registration(destination, remove=True)
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

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('install', 'uninstall'))
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--associate', action='store_true')
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Windows per-user installer; Linux uses the staged archive')
    destination = args.destination or Path(os.environ['LOCALAPPDATA']) / 'Programs' / 'FrostIDE'
    if args.action == 'install':
        if not args.stage: parser.error('--stage is required')
        install(args.stage, destination, args.associate)
    else:
        for path in uninstall(destination): print('Preserved modified file:', path)
    return 0

if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError) as error:
        print(f'install: {error}', file=sys.stderr)
        sys.exit(1)
