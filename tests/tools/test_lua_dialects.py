# SPDX-License-Identifier: Apache-2.0
# Dialect manifest generation is local evidence only and never launches a tool or requests network.
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
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

    def test_windows_clang_lsp_build_records_the_private_compatibility_include(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, work = root / 'source', root / 'work'
            source.mkdir()
            license_file = source / 'LICENSE.md'
            license_file.write_bytes(b'fixture license')
            args = SimpleNamespace(kind='luau-lsp', source=source, work=work,
                                   clang=root / 'clang-cl.exe', cmake=root / 'cmake.exe',
                                   ninja=root / 'ninja.exe', git=root / 'git.exe')
            for tool in (args.clang, args.cmake, args.ninja, args.git):
                tool.write_bytes(b'unit fixture; never executed')
            record = {'commit': 'a' * 40, 'licensePath': license_file.name,
                      'licenseSha256': lua_dialects.digest(license_file)}
            windows = SimpleNamespace(name='nt', pathsep=';', environ={'INCLUDE': 'existing SDK includes'})
            cmake_calls = []

            def run(command, **options):
                if command[0] == str(args.git):
                    output = record['commit'] if 'rev-parse' in command else ''
                    return SimpleNamespace(stdout=output)
                cmake_calls.append((command, options['env']['INCLUDE']))
                if '--build' in command:
                    (work / 'build').mkdir()
                    (work / 'build' / 'luau-lsp.exe').write_bytes(b'fixture output')
                return SimpleNamespace(stdout='')

            with mock.patch.object(lua_dialects, 'pin', return_value=record), \
                    mock.patch.object(lua_dialects, 'os', windows), \
                    mock.patch.object(lua_dialects, 'subprocess', SimpleNamespace(run=run)):
                lua_dialects.build(args)

            compat = work / 'clang-cl-crt'
            shim = compat / 'alloca.h'
            configure, build = cmake_calls
            self.assertIn(f'-DCMAKE_C_COMPILER={args.clang}', configure[0])
            self.assertIn(f'-DCMAKE_CXX_COMPILER={args.clang}', configure[0])
            self.assertEqual(build[0][-2:], ['--target', 'Luau.LanguageServer.CLI'])
            self.assertEqual(configure[1], f'{compat};existing SDK includes')
            self.assertEqual(configure[1], build[1])
            self.assertEqual(windows.environ['INCLUDE'], 'existing SDK includes')
            self.assertFalse((work / 'source' / 'alloca.h').exists())
            receipt = json.loads((work / 'frost-dialect-build.json').read_text(encoding='utf-8'))
            self.assertEqual(receipt['compatibilityInputs'], [{'path': str(shim), 'sha256': lua_dialects.digest(shim)}])
            self.assertEqual(receipt['compilers'],
                             [{'path': str(args.clang), 'sha256': lua_dialects.digest(args.clang)}])
if __name__=='__main__': unittest.main()
