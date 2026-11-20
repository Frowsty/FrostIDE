# SPDX-License-Identifier: Apache-2.0
# Delivery tests use temporary local fixtures; authored for the resumed testing phase.
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

installer = load('install_user')
gate = load('delivery_gate')

class DeliveryTest(unittest.TestCase):
    def test_uninstall_preserves_foreign_and_modified_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stage, target = root / 'stage', root / 'installed'
            stage.mkdir()
            for name in ('frost.exe', 'frost_app.dll', 'LICENSE', 'NOTICE'):
                (stage / name).write_text(name)
            installer.install(stage, target)
            (target / 'frost.exe').write_text('user-modified')
            (target / 'foreign-empty').mkdir()
            (target / 'foreign.txt').write_text('foreign')
            # No association was requested; avoid platform registry state in this pure fixture.
            original_registration = installer.registration
            installer.registration = lambda *args, **kwargs: None
            try: preserved = installer.uninstall(target)
            finally: installer.registration = original_registration
            self.assertEqual(preserved, [str(target / 'frost.exe')])
            self.assertTrue((target / 'foreign-empty').is_dir())
            self.assertEqual((target / 'foreign.txt').read_text(), 'foreign')
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
            adr.write_text('Reviewed upgrade decision')
            new = json.loads(current.read_text())
            new['description'] += ' candidate'
            candidate.write_text(json.dumps(new))
            self.assertEqual(gate.upgrade_plan(current, candidate, adr)['status'], 'runtime-verification-pending')
            malformed = copy.deepcopy(new)
            malformed['windows']['llvm'] = True
            candidate.write_text(json.dumps(malformed))
            with self.assertRaises(ValueError): gate.upgrade_plan(current, candidate, adr)
            malformed = copy.deepcopy(new)
            malformed['qt']['linux']['archives'] = []
            candidate.write_text(json.dumps(malformed))
            with self.assertRaises(ValueError): gate.upgrade_plan(current, candidate, adr)

    def test_release_requires_each_gate_and_exact_evidence_hash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock, evidence, manifest = root / 'lock.json', root / 'evidence.txt', root / 'release.json'
            lock.write_text('{}')
            gates = {}
            for name in gate.RELEASE_GATES:
                evidence = root / (name + '.json')
                evidence.write_text(json.dumps({'gate': name, 'commit': 'a' * 40,
                    'lockSha256': gate.sha256(lock), 'status': 'passed'}))
                gates[name] = {'status': 'passed', 'file': evidence.name,
                    'sha256': gate.sha256(evidence), 'reviewer': 'local reviewer'}
            record = {'schema': 1, 'commit': 'a' * 40, 'lockSha256': gate.sha256(lock), 'gates': gates}
            manifest.write_text(json.dumps(record))
            self.assertEqual(gate.release_gate(manifest, 'a' * 40, lock)['status'], 'recorded-evidence-valid')
            gates['user-signoff']['status'] = 'pending'
            manifest.write_text(json.dumps(record))
            with self.assertRaises(ValueError): gate.release_gate(manifest, 'a' * 40, lock)
            gates['user-signoff']['status'] = 'passed'
            manifest.write_text(json.dumps(record))
            evidence.write_text('changed')
            with self.assertRaises(ValueError): gate.release_gate(manifest, 'a' * 40, lock)

if __name__ == '__main__': unittest.main()
