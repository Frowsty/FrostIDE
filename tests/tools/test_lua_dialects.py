# SPDX-License-Identifier: Apache-2.0
# Dialect manifest generation is local evidence only and never launches a tool or requests network.
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import lua_dialects
class LuaDialects(unittest.TestCase):
    def test_manifest_pins_explicit_luau_selector_and_license_and_keeps_lua_separate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); executable=root/'luau-lsp'; license_file=root/'LICENSE.md'
            executable.write_bytes(b'unit fixture; never executed'); license_file.write_text('unit license evidence',encoding='utf-8')
            record={'version':'fixture-1','license':'MIT','licenseSha256':lua_dialects.digest(license_file)}
            with mock.patch.object(lua_dialects,'pin',return_value=record):
                manifest=lua_dialects.registry_manifest(executable,license_file)
                row=manifest['servers'][0]
                self.assertEqual(row['languageId'],'luau'); self.assertEqual(row['extensions'],['.luau'])
                self.assertEqual(row['arguments'],['lsp']); self.assertEqual(row['artifacts'][0]['sha256'],lua_dialects.digest(executable))
                license_file.write_text('changed',encoding='utf-8')
                with self.assertRaisesRegex(ValueError,'license'): lua_dialects.registry_manifest(executable,license_file)
    def test_all_source_recipes_use_immutable_commits_and_allowed_license(self):
        for name in ('luau','luajit','luau-lsp'):
            record=lua_dialects.pin(name)
            self.assertRegex(record['commit'],r'^[0-9a-f]{40}$'); self.assertRegex(record['licenseSha256'],r'^[0-9a-f]{64}$')
            self.assertIn(record['license'],lua_dialects.ALLOWLIST)
if __name__=='__main__': unittest.main()
