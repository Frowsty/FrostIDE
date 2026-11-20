# SPDX-License-Identifier: Apache-2.0
# Exercises Windows installer registration with HKCU redirected for this process.
"""Windows installer contracts against a process-local redirect of HKCU."""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid
if os.name == 'nt':
    import winreg

from test_delivery import installer


@unittest.skipUnless(os.name == 'nt', 'Windows registry test')
class InstallerRegistryTest(unittest.TestCase):
    def setUp(self):
        self.private_key = 'Software\\FrostIDE-Installer-Test-' + uuid.uuid4().hex
        self.hive = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, self.private_key, 0, winreg.KEY_ALL_ACCESS)
        advapi = c.WinDLL('advapi32')
        self.override = advapi.RegOverridePredefKey
        self.override.argtypes = (w.HKEY, w.HKEY)
        self.override.restype = w.LONG
        self.delete_tree = advapi.RegDeleteTreeW
        self.delete_tree.argtypes = (w.HKEY, w.LPCWSTR)
        self.delete_tree.restype = w.LONG
        self.hkcu = w.HKEY(c.c_int32(int(winreg.HKEY_CURRENT_USER)).value)
        self.addCleanup(self.remove_private_tree)
        self.assertEqual(self.override(self.hkcu, int(self.hive)), 0)
        self.addCleanup(self.restore_hive)
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.stage = Path(scratch.name) / 'stage'
        self.stage.mkdir()
        self.destination = Path(scratch.name) / 'FrostIDE Install å'
        for name in ('frost.exe', 'frost_app.dll', 'LICENSE', 'NOTICE'):
            (self.stage / name).write_text(name, encoding='utf-8')

    def restore_hive(self):
        self.assertEqual(self.override(self.hkcu, None), 0)

    def remove_private_tree(self):
        self.hive.Close()
        self.assertEqual(self.delete_tree(self.hkcu, self.private_key), 0)

    def read(self, subkey, name=''):
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, installer.CLASSES + subkey) as key:
            return winreg.QueryValueEx(key, name)[0]

    def set_value(self, subkey, name, value):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, installer.CLASSES + subkey) as key:
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)

    def assert_missing(self, path, name=''):
        with self.assertRaises(FileNotFoundError):
            self.read(path, name)

    def test_associations_offer_open_with_and_preserve_existing_defaults(self):
        extensions = ('.frostproj', '.sln', '.slnx', '.lua')
        for extension in (*extensions, '.txt'):
            self.set_value(extension, '', 'Foreign.Default')
            self.set_value(extension + r'\OpenWithProgids', 'Foreign.Choice', '')
        choice = r'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.lua\UserChoice'
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, choice) as user_choice:
            winreg.SetValueEx(user_choice, 'ProgId', 0, winreg.REG_SZ, 'User.Lua')
        installer.install(self.stage, self.destination, associate=True)
        for extension in extensions:
            self.assertEqual(self.read(extension + r'\OpenWithProgids', installer.PROGID), '')
        self.assert_missing(r'.txt\OpenWithProgids', installer.PROGID)
        command = self.read(installer.CMAKE_VERB + r'\command')
        self.assertEqual(command, self.read(installer.PROGID + installer.OPEN_COMMAND))
        self.assertEqual(command, f'"{self.destination / "frost.exe"}" "%1"')
        self.assertEqual(self.read(installer.CMAKE_VERB, 'AppliesTo'), 'System.FileName:="CMakeLists.txt"')
        self.assert_missing(r'SystemFileAssociations\.txt\shell')
        item = self.stage / 'CMake project' / 'CMakeLists.txt'
        self.assertEqual(shell_argv(command.replace('%1', str(item))), [str(self.destination / 'frost.exe'), str(item)])
        self.assertEqual(installer.uninstall(self.destination), [])
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, choice) as user_choice:
            self.assertEqual(winreg.QueryValueEx(user_choice, 'ProgId')[0], 'User.Lua')
        for extension in (*extensions, '.txt'):
            self.assertEqual(self.read(extension), 'Foreign.Default')
            self.assertEqual(self.read(extension + r'\OpenWithProgids', 'Foreign.Choice'), '')
            self.assert_missing(extension + r'\OpenWithProgids', installer.PROGID)
        self.assert_missing(installer.CMAKE_VERB, 'AppliesTo')

    def test_conflicting_command_is_never_overwritten(self):
        command_key = installer.PROGID + installer.OPEN_COMMAND
        self.set_value(command_key, '', '"C:\\Different Installation\\frost.exe" "%1"')
        with self.assertRaisesRegex(ValueError, 'conflicting Windows association'):
            installer.install(self.stage, self.destination, associate=True)
        self.assertEqual(self.read(command_key), '"C:\\Different Installation\\frost.exe" "%1"')
        self.assert_missing(r'.sln\OpenWithProgids', installer.PROGID)
        self.assertFalse(self.destination.exists())

    def test_borrowed_values_and_transferred_verb_survive_uninstall(self):
        self.set_value(r'.frostproj\OpenWithProgids', installer.PROGID, '')
        installer.install(self.stage, self.destination, associate=True)
        self.set_value(r'.sln\OpenWithProgids', installer.PROGID, 'modified')
        self.set_value(installer.CMAKE_VERB + r'\command', '', 'Other application')
        self.set_value(installer.PROGID, 'ForeignMetadata', 'keep me')
        installer.uninstall(self.destination)
        self.assertEqual(self.read(r'.frostproj\OpenWithProgids', installer.PROGID), '')
        self.assertEqual(self.read(r'.sln\OpenWithProgids', installer.PROGID), 'modified')
        self.assertEqual(self.read(installer.CMAKE_VERB + r'\command'), 'Other application')
        self.assertEqual(self.read(installer.CMAKE_VERB, 'AppliesTo'), installer.CMAKE_APPLIES_TO)
        self.assertEqual(self.read(installer.PROGID, 'ForeignMetadata'), 'keep me')
        self.assert_missing(r'.slnx\OpenWithProgids', installer.PROGID)

    def test_partial_write_failure_rolls_back_only_owned_entries(self):
        self.set_value(r'.frostproj\OpenWithProgids', 'Foreign.Choice', '')
        original = winreg.SetValueEx

        def fail_last(key, name, reserved, kind, value):
            if name == 'AppliesTo':
                raise PermissionError('injected registry write failure')
            original(key, name, reserved, kind, value)

        with patch.object(winreg, 'SetValueEx', fail_last), self.assertRaisesRegex(PermissionError, 'injected'):
            installer.install(self.stage, self.destination, associate=True)
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.read(r'.frostproj\OpenWithProgids', 'Foreign.Choice'), '')
        for path in (installer.PROGID, '.sln', '.slnx', '.lua', 'SystemFileAssociations'):
            with self.assertRaises(FileNotFoundError):
                winreg.OpenKey(winreg.HKEY_CURRENT_USER, installer.CLASSES + path)

    def test_legacy_uninstall_removes_only_original_frostproj_registration(self):
        installer.install(self.stage, self.destination)
        marker_path = self.destination / installer.MARKER
        marker = json.loads(marker_path.read_text(encoding='utf-8'))
        marker_path.write_text(json.dumps(marker | {'associated': True}), encoding='utf-8')
        self.set_value(installer.PROGID, '', 'FrostIDE Project')
        self.set_value(installer.PROGID + installer.OPEN_COMMAND, '', f'"{self.destination / "frost.exe"}" "%1"')
        self.set_value(r'.frostproj\OpenWithProgids', installer.PROGID, '')
        self.set_value(r'.lua\OpenWithProgids', installer.PROGID, 'other registration')
        installer.uninstall(self.destination)
        self.assert_missing(r'.frostproj\OpenWithProgids', installer.PROGID)
        self.assertEqual(self.read(r'.lua\OpenWithProgids', installer.PROGID), 'other registration')

    def test_manifest_write_failure_rolls_back_registration(self):
        original = Path.write_text

        def fail_marker(path, *args, **kwargs):
            if path.name == installer.MARKER:
                raise OSError('injected marker failure')
            return original(path, *args, **kwargs)

        with patch.object(Path, 'write_text', fail_marker), self.assertRaisesRegex(OSError, 'injected marker'):
            installer.install(self.stage, self.destination, associate=True)
        self.assertFalse(self.destination.exists())
        self.assert_missing(installer.PROGID + installer.OPEN_COMMAND)
        self.assert_missing(installer.CMAKE_VERB, 'AppliesTo')


def shell_argv(command):
    command_line_to_argv = c.WinDLL('shell32').CommandLineToArgvW
    command_line_to_argv.argtypes = (w.LPCWSTR, c.POINTER(c.c_int))
    command_line_to_argv.restype = c.POINTER(w.LPWSTR)
    local_free = c.WinDLL('kernel32').LocalFree
    local_free.argtypes = (c.c_void_p,)
    local_free.restype = c.c_void_p
    count = c.c_int()
    argv = command_line_to_argv(command, c.byref(count))
    if not argv:
        raise c.WinError()
    try:
        return argv[:count.value]
    finally:
        local_free(argv)


class PropertyKey(c.Structure):
    _fields_ = [('guid', c.c_ubyte * 16), ('id', w.DWORD)]


def guid(value):
    return (c.c_ubyte * 16).from_buffer_copy(uuid.UUID(value).bytes_le)


def com_call(obj, index, result, types, *args):
    table = c.cast(obj, c.POINTER(c.POINTER(c.c_void_p)))[0]
    return c.WINFUNCTYPE(result, c.c_void_p, *types)(table[index])(obj, *args)


def require_com(result):
    if result < 0:
        raise OSError(f'Windows Shell HRESULT 0x{result & 0xffffffff:08x}')


@unittest.skipUnless(os.name == 'nt', 'Windows Shell condition test')
class CMakeShellConditionTest(unittest.TestCase):
    def test_windows_parses_exact_basename_and_reads_real_shell_item_properties(self):
        # Windows SDK structuredquery.h, structuredquerycondition.h and shobjidl_core.h.
        # Parse the actual installed expression, then obtain System.FileName from real Shell items.
        ole = c.WinDLL('ole32')
        ole.CoInitializeEx.argtypes = (c.c_void_p, w.DWORD)
        ole.CoInitializeEx.restype = w.LONG
        ole.CoCreateInstance.argtypes = (c.c_void_p, c.c_void_p, w.DWORD, c.c_void_p, c.c_void_p)
        ole.CoCreateInstance.restype = w.LONG
        ole.CoTaskMemFree.argtypes = (c.c_void_p,)
        require_com(ole.CoInitializeEx(None, 2))  # COINIT_APARTMENTTHREADED
        self.addCleanup(ole.CoUninitialize)

        def own(obj):
            self.addCleanup(com_call, obj, 2, w.ULONG, ())  # IUnknown::Release

        manager, parser, solution, condition = (c.c_void_p() for _ in range(4))
        require_com(ole.CoCreateInstance(guid('5088b39a-29b4-4d9d-8245-4ee289222f66'), None, 1,
                                         guid('a879e3c4-af77-44fb-8f37-ebd1487cf920'), c.byref(manager)))
        own(manager)
        require_com(com_call(manager, 3, w.LONG, (w.LPCWSTR, w.WORD, c.c_void_p, c.c_void_p), 'SystemIndex', 0,
                             guid('2ebdee67-3505-43f8-9946-ea44abc8e5b0'), c.byref(parser)))  # CreateLoadedParser
        own(parser)
        require_com(com_call(parser, 3, w.LONG, (w.LPCWSTR, c.c_void_p, c.c_void_p), installer.CMAKE_APPLIES_TO,
                             None, c.byref(solution)))  # IQueryParser::Parse
        own(solution)
        require_com(com_call(solution, 7, w.LONG, (c.c_void_p, c.c_void_p), c.byref(condition), None))  # GetQuery
        own(condition)
        condition_type, comparison = c.c_int(), c.c_int()
        require_com(com_call(condition, 8, w.LONG, (c.c_void_p,), c.byref(condition_type)))
        self.assertEqual(condition_type.value, 3)  # CT_LEAF_CONDITION
        property_name = c.c_void_p()
        require_com(com_call(condition, 10, w.LONG, (c.c_void_p, c.c_void_p, c.c_void_p), c.byref(property_name),
                             c.byref(comparison), None))  # GetComparisonInfo
        self.addCleanup(ole.CoTaskMemFree, property_name)
        self.assertEqual(c.wstring_at(property_name), 'System.FileName')
        self.assertEqual(comparison.value, 1)  # COP_EQUAL
        value_term, term_text = c.c_void_p(), c.c_void_p()
        require_com(com_call(condition, 13, w.LONG, (c.c_void_p, c.c_void_p, c.c_void_p), None, None,
                             c.byref(value_term)))  # GetInputTerms
        self.assertTrue(value_term)
        own(value_term)
        require_com(com_call(value_term, 3, w.LONG, (c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p), None, None,
                             c.byref(term_text), None))  # IRichChunk::GetData
        self.addCleanup(ole.CoTaskMemFree, term_text)
        operand_name = c.wstring_at(term_text).strip('"')
        self.assertEqual(operand_name, 'CMakeLists.txt')

        file_name = PropertyKey(guid('41cf5ae0-f75a-4806-bd87-59c7d9248eb9'), 100)
        shell_item = c.WinDLL('shell32').SHCreateItemFromParsingName
        shell_item.argtypes = (w.LPCWSTR, c.c_void_p, c.c_void_p, c.c_void_p)
        shell_item.restype = w.LONG
        compare = c.WinDLL('kernel32').CompareStringOrdinal
        compare.argtypes = (w.LPCWSTR, c.c_int, w.LPCWSTR, c.c_int, w.BOOL)
        compare.restype = c.c_int
        with tempfile.TemporaryDirectory() as root:
            for name, expected in (('CMakeLists.txt', True), ('prefix-CMakeLists.txt', False),
                                   ('CMakeLists.txt.txt', False), ('README.txt', False)):
                with self.subTest(name=name):
                    path = Path(root) / name
                    path.touch()
                    item, actual = c.c_void_p(), c.c_void_p()
                    require_com(shell_item(str(path), None, guid('7e9fb0d3-919f-4307-ab2e-9b1860310c93'), c.byref(item)))
                    try:
                        require_com(com_call(item, 17, w.LONG, (c.c_void_p, c.c_void_p), c.byref(file_name),
                                             c.byref(actual)))  # IShellItem2::GetString
                        self.assertEqual(compare(c.wstring_at(actual), -1, operand_name, -1, True) == 2,
                                         expected)
                    finally:
                        ole.CoTaskMemFree(actual)
                        com_call(item, 2, w.ULONG, ())


if __name__ == '__main__':
    unittest.main()
