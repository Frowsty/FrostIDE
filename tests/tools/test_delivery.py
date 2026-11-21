# SPDX-License-Identifier: Apache-2.0
# Delivery tests use temporary local fixtures; authored for the resumed testing phase.
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

installer = load('install_user')
gate = load('delivery_gate')

class DeliveryTest(unittest.TestCase):
    def test_install_applies_default_and_opt_out_before_copying(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            stage = root / 'stage'
            (stage / 'bin').mkdir(parents=True)
            for name in ('frost.exe', 'frost_app.dll', 'LICENSE', 'NOTICE', 'bin/helper.dll'):
                (stage / name).write_bytes(name.encode('utf-8'))

            def configure(directory, enabled):
                self.assertTrue(directory.is_dir())
                self.assertEqual(list(directory.iterdir()), [])
                return True

            with mock.patch.object(installer, 'configure_compression', side_effect=configure) as compression:
                installer.install(stage, root / 'compressed')
                installer.install(stage, root / 'uncompressed', compress=False)
                self.assertEqual(compression.call_args_list, [mock.call(root / 'compressed', True),
                                                              mock.call(root / 'uncompressed', False)])
            self.assertEqual((root / 'compressed/bin/helper.dll').read_bytes(), b'bin/helper.dll')
            self.assertEqual((root / 'uncompressed/bin/helper.dll').read_bytes(), b'bin/helper.dll')

    def test_compression_preparation_failure_rolls_back_fresh_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stage, target = root / 'stage', root / 'installed'
            stage.mkdir()
            for name in ('frost.exe', 'frost_app.dll', 'LICENSE', 'NOTICE'):
                (stage / name).write_bytes(name.encode('utf-8'))
            with mock.patch.object(installer, 'configure_compression', side_effect=OSError('native failure')):
                with self.assertRaisesRegex(OSError, 'native failure'):
                    installer.install(stage, target, associate=True)
            self.assertFalse(target.exists())
            self.assertTrue((stage / 'frost.exe').is_file())

    def test_uninstall_preserves_foreign_and_modified_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stage, target = root / 'stage', root / 'installed'
            stage.mkdir()
            for name in ('frost.exe', 'frost_app.dll', 'LICENSE', 'NOTICE'):
                (stage / name).write_bytes(name.encode('utf-8'))
            installer.install(stage, target)
            modified = b'user-modified\r\n\x00\xff'
            (target / 'frost.exe').write_bytes(modified)
            (target / 'foreign-empty').mkdir()
            (target / 'foreign.txt').write_bytes(b'foreign\r\n')
            # No association was requested; avoid platform registry state in this pure fixture.
            original_registration = installer.registration
            installer.registration = lambda *args, **kwargs: None
            try: preserved = installer.uninstall(target)
            finally: installer.registration = original_registration
            self.assertEqual(len(preserved), 1)
            self.assertTrue(Path(preserved[0]).samefile(target / 'frost.exe'))
            self.assertEqual((target / 'frost.exe').read_bytes(), modified)
            self.assertTrue((target / 'foreign-empty').is_dir())
            self.assertEqual((target / 'foreign.txt').read_bytes(), b'foreign\r\n')
            self.assertFalse((target / 'frost_app.dll').exists())

    def test_manifest_cannot_escape_install_root(self):
        with tempfile.TemporaryDirectory() as temp:
            for name in ('../outside', '/absolute', 'C:/absolute', 'nested/../../outside'):
                with self.assertRaises(ValueError): installer.owned_path(Path(temp), name)

    def test_upgrade_accepts_real_lock_structure_and_rejects_erased_pins(self):
        import copy
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            current = TOOLS.parent / 'sdk/sdk.lock.json'
            candidate, adr = root / 'candidate.json', root / 'decision.md'
            adr.write_text('Reviewed upgrade decision', encoding='utf-8')
            new = json.loads(current.read_text(encoding='utf-8'))
            new['description'] += ' candidate'
            candidate.write_text(json.dumps(new, ensure_ascii=False), encoding='utf-8')
            self.assertEqual(gate.upgrade_plan(current, candidate, adr)['status'], 'runtime-verification-pending')
            malformed = copy.deepcopy(new)
            malformed['windows']['llvm'] = True
            candidate.write_text(json.dumps(malformed, ensure_ascii=False), encoding='utf-8')
            with self.assertRaises(ValueError): gate.upgrade_plan(current, candidate, adr)
            malformed = copy.deepcopy(new)
            malformed['qt']['linux']['archives'] = []
            candidate.write_text(json.dumps(malformed, ensure_ascii=False), encoding='utf-8')
            with self.assertRaises(ValueError): gate.upgrade_plan(current, candidate, adr)

    def test_release_requires_each_gate_and_exact_evidence_hash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock, evidence, manifest = root / 'lock.json', root / 'evidence.txt', root / 'release.json'
            lock.write_text('{}', encoding='utf-8')
            gates = {}
            for name in gate.RELEASE_GATES:
                evidence = root / (name + '.json')
                evidence.write_text(json.dumps({'gate': name, 'commit': 'a' * 40,
                    'lockSha256': gate.sha256(lock), 'status': 'passed',
                    'observation': '確認済み'}, ensure_ascii=False), encoding='utf-8')
                gates[name] = {'status': 'passed', 'file': evidence.name,
                    'sha256': gate.sha256(evidence), 'reviewer': '確認担当者'}
            record = {'schema': 1, 'commit': 'a' * 40, 'lockSha256': gate.sha256(lock), 'gates': gates}
            manifest.write_text(json.dumps(record, ensure_ascii=False), encoding='utf-8')
            self.assertEqual(gate.release_gate(manifest, 'a' * 40, lock)['status'], 'recorded-evidence-valid')
            gates['user-signoff']['status'] = 'pending'
            manifest.write_text(json.dumps(record, ensure_ascii=False), encoding='utf-8')
            with self.assertRaises(ValueError): gate.release_gate(manifest, 'a' * 40, lock)
            gates['user-signoff']['status'] = 'passed'
            manifest.write_text(json.dumps(record, ensure_ascii=False), encoding='utf-8')
            evidence.write_text('changed', encoding='utf-8')
            with self.assertRaises(ValueError): gate.release_gate(manifest, 'a' * 40, lock)

if __name__ == '__main__': unittest.main()
