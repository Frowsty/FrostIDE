# SPDX-License-Identifier: Apache-2.0
# Stage pinned Linux Qt shared libraries/plugins with relocatable runtime lookup.
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from package_notices import copy_qt_source

def stage_qt(repo, sdk, destination, archive=None, debug=False, check_only=False):
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
    icu_notice = next((path for path in (root / 'licenses/ICU.txt', root / 'licenses/icu-LICENSE',
        root / 'doc/icu/LICENSE') if path.is_file()), None)
    if icu and icu_notice is None: raise ValueError('bundled ICU requires SDK licenses/ICU.txt (upstream notice)')
    for path in libraries + [root / 'plugins' / name for name in plugins]:
        if not path.resolve().is_relative_to(root.resolve()): raise ValueError('Qt input escapes SDK Qt root')
    if check_only: return
    if archive is not None: copy_qt_source(archive, destination, pin['sources']['qtbase'])
    qt = destination / 'qt'
    (qt / 'lib').mkdir(parents=True)
    for path in libraries:
        if not path.resolve().is_relative_to(root.resolve()): raise ValueError('Qt library escapes SDK Qt root')
        shutil.copy2(path.resolve(), qt / 'lib' / path.name)
    for name in plugins:
        target = qt / 'plugins' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2((root / 'plugins' / name).resolve(), target)
    if icu_notice:
        (destination / 'licenses').mkdir(exist_ok=True)
        shutil.copy2(icu_notice, destination / 'licenses/ICU.txt')
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
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    stage_qt(args.repo, args.sdk, args.destination, args.qt_source, args.debug, args.check_only)

if __name__ == '__main__': main()
