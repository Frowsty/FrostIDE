# SPDX-License-Identifier: Apache-2.0
# Workbench Windows update button and close-path integration over a local signed offer.
"""Drive the normal Workbench update dialog with an isolated local signed offer."""
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


def app_image(exe, version):
    return exe.read_bytes() + f'\nFROST_HELPER_META:version={version};selftest_exit=0;run_exit=0\n'.encode()


def wait_for_receipt(profile, receipt, final):
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        results = list((profile / 'updates').glob('inplace-*/update-result.txt'))
        if results:
            assert len(results) == 1 and results[0].read_text().strip() == 'Update installed', results
            assert receipt.read_bytes() != final
            return
        time.sleep(0.05)
    raise AssertionError(f'No native update receipt under {profile}')


def run(probe, updater, app, plugins, config):
    with tempfile.TemporaryDirectory(prefix='frost-update-ui-') as temporary:
        root = Path(temporary)
        stage = root / 'stage'
        stage.mkdir()
        old, new = app_image(app, '1.0.0'), app_image(app, '2.0.0')
        (stage / 'frost.exe').write_bytes(old)
        for filename in ('frost_app.dll', 'LICENSE', 'NOTICE'):
            (stage / filename).write_text(filename, encoding='utf-8')
        shutil.copy2(probe, stage / probe.name)
        shutil.copy2(updater, stage / updater.name)
        runtime = [*probe.parent.glob('Qt6*.dll'), *probe.parent.glob('clang_rt.asan_dynamic-*.dll')]
        assert runtime, f'No adjacent Qt runtime under {probe.parent}'
        for dependency in runtime:
            shutil.copy2(dependency, stage / dependency.name)
        platform = 'qoffscreend.dll' if config == 'Debug' else 'qoffscreen.dll'
        (stage / 'platforms').mkdir()
        shutil.copy2(plugins / 'platforms' / platform, stage / 'platforms' / platform)
        archive = root / 'candidate.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as candidate:
            candidate.writestr('frost.exe', new)
            for filename in ('frost_app.dll', 'LICENSE', 'NOTICE'):
                candidate.writestr(filename, f'candidate {filename}')
        for mode in ('approved', 'vetoed', 'tampered'):
            work = root / (root.name + '-' + mode)
            work.mkdir()
            install = root / ('installed-' + mode)
            install_user.install(stage, install, compress=False)
            marker = install / install_user.MARKER
            before = marker.read_bytes()
            env = os.environ.copy()
            env['QT_QPA_PLATFORM'] = 'offscreen'
            env['QT_PLUGIN_PATH'] = str(install)
            for name in ('APPDATA', 'LOCALAPPDATA', 'USERPROFILE', 'TEMP', 'TMP'):
                folder = work / name
                folder.mkdir()
                env[name] = str(folder)
            result = subprocess.run([install / probe.name, mode, archive, work], cwd=work, env=env,
                                    capture_output=True, text=True, timeout=45)
            profile = Path((work / 'profile-path.txt').read_text(encoding='utf-8')) if (
                work / 'profile-path.txt').is_file() else None
            try:
                assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
                assert profile is not None and profile.name == work.name
                if mode == 'approved':
                    assert 'clicked=1 window_visible=0 helper_launches=1' in result.stdout, result.stdout
                    wait_for_receipt(profile, marker, before)
                    assert (install / 'frost.exe').read_bytes() == new
                else:
                    assert marker.read_bytes() == before and (install / 'frost.exe').read_bytes() == old
                    assert not list((profile / 'updates').glob('inplace-*/update-result.txt'))
                    proof = ('helper_launches=1 veto=1' if mode == 'vetoed' else
                             'helper_launches=0 veto=0 integrity_refused=1')
                    assert 'clicked=1 window_visible=1' in result.stdout and proof in result.stdout, result.stdout
                # Restarted fixture app can hold its image for a few milliseconds after the receipt.
                for attempt in range(50):
                    try:
                        assert install_user.uninstall(install) == []
                        break
                    except PermissionError:
                        if attempt == 49:
                            raise
                        time.sleep(0.05)
                assert not install.exists()
                print(f'{mode}: {result.stdout.strip()}; uninstall passed', flush=True)
            finally:
                if (profile is not None and profile.name == work.name and
                        profile.parent.name == 'FrostIDE-Update-Tests' and
                        ('qttest' in profile.parts or profile.is_relative_to(work)) and profile.is_dir()):
                    shutil.rmtree(profile)


if __name__ == '__main__':
    run(*map(lambda item: Path(item).resolve(), sys.argv[1:5]), sys.argv[5])
