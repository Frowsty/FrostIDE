# SPDX-License-Identifier: Apache-2.0
# Stage pinned Linux Qt shared libraries/plugins with relocatable runtime lookup.
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from package_notices import copy_qt_source

def stage_qt(repo, sdk, destination, archive=None, debug=False, check_only=False, *, icu_notice=None):
    pin = json.loads((repo / 'sdk/sdk.lock.json').read_text())['qt']
    root = sdk / 'qt' / pin['version'] / 'gcc_64'
    modules = ('Core', 'Gui', 'Widgets', 'Network', 'DBus', 'XcbQpa', 'OpenGL')
    libraries = [path for module in modules for path in sorted(root.joinpath('lib').glob('libQt6' + module + '.so*'))]
    libraries += sorted(root.joinpath('lib').glob('libicu*.so*'))
    plugins = ('platforms/libqxcb.so', 'platforms/libqoffscreen.so', 'tls/libqopensslbackend.so')
    if not libraries or any(not (root / 'plugins' / name).is_file() for name in plugins):
        raise ValueError('missing pinned Qt libraries or XCB platform plugin')
    if archive is None and not debug: raise ValueError('Release Qt packaging requires --qt-source')
    if archive is not None:
        if hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sources']['qtbase']['sha256']:
            raise ValueError('Qt source archive does not match sdk.lock')
    icu = [path for path in libraries if path.name.startswith('libicu')]
    notice_pin = pin['linux']['icuNotice']
    cached_notice = sdk / '_downloads' / notice_pin['file']
    if icu_notice is None and icu and cached_notice.exists():
        icu_notice = cached_notice
    if icu_notice is not None:
        notice_bytes = icu_notice.read_bytes()
        if hashlib.sha256(notice_bytes).hexdigest() != notice_pin['sha256']:
            raise ValueError('ICU notice does not match sdk.lock')
    else:
        installed_notice = next((path for path in (root / 'licenses/ICU.txt', root / 'licenses/icu-LICENSE',
            root / 'doc/icu/LICENSE') if path.is_file()), None)
        notice_bytes = installed_notice.read_bytes() if installed_notice is not None else None
    if icu and notice_bytes is None:
        raise ValueError('bundled ICU requires the pinned --icu-notice or an SDK notice '
                         '(tools/bootstrap-wsl.sh --only icu-notice)')
    sdk_root = root.resolve()
    sdk_lib = root.joinpath('lib').resolve()
    library_names = {path.name for path in libraries}
    sources = {path: path.resolve(strict=True) for path in libraries}
    for path, source in sources.items():
        if not source.is_relative_to(sdk_root):
            raise ValueError('Qt input escapes SDK Qt root')
        if path.is_symlink() and (source.parent != sdk_lib or source.name not in library_names):
            raise ValueError('Qt library alias does not resolve to a shipped library')
    for name in plugins:
        if not (root / 'plugins' / name).resolve().is_relative_to(sdk_root):
            raise ValueError('Qt input escapes SDK Qt root')
    if check_only: return
    if archive is not None: copy_qt_source(archive, destination, pin['sources']['qtbase'])
    qt = destination / 'qt'
    (qt / 'lib').mkdir(parents=True)
    for path, source in sources.items():
        target = qt / 'lib' / path.name
        if path.is_symlink():
            # Keep every runtime name while sharing one relocatable, verified payload.
            target.symlink_to(source.name)
        else:
            shutil.copy2(source, target)
    for name in plugins:
        target = qt / 'plugins' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2((root / 'plugins' / name).resolve(), target)
    if icu and notice_bytes is not None:
        (destination / 'licenses').mkdir(exist_ok=True)
        (destination / 'licenses/ICU.txt').write_bytes(notice_bytes)
    for name in ('licenses', 'LICENSES', 'sbom'):
        if (root / name).is_dir(): shutil.copytree(root / name, qt / name)
    if (root / 'translations').is_dir(): shutil.copytree(root / 'translations', qt / 'translations')
    (destination / 'qt.conf').write_text('[Paths]\nPrefix=qt\nLibraries=lib\nPlugins=plugins\nTranslations=translations\n')
    (destination / 'frost').rename(destination / 'frost.bin')
    wrapper = destination / 'frost'
    wrapper.write_text('''#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Resolve this package after relocation, including paths containing spaces.
set -e
script="$(readlink -f -- "${BASH_SOURCE[0]}")"
root="$(cd -- "$(dirname -- "$script")" && pwd)"
export LD_LIBRARY_PATH="$root/qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$root/qt/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$root/qt/plugins/platforms"
exec "$root/frost.bin" "$@"
''')
    wrapper.chmod(0o755)
    return destination / 'frost.bin'

def main():
    parser = argparse.ArgumentParser()
    for name in ('repo', 'sdk', 'destination'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--qt-source', type=Path)
    parser.add_argument('--icu-notice', type=Path)
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    stage_qt(args.repo, args.sdk, args.destination, args.qt_source, args.debug, args.check_only,
             icu_notice=args.icu_notice)

if __name__ == '__main__': main()
