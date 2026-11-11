# SPDX-License-Identifier: Apache-2.0
#
# Negative fixtures for the COFF static-initializer recognizer. Bytes and relocation offsets below
# are captured from the pinned Release reactor object, independently of the recognizer's constants.

from __future__ import annotations

import unittest

from static_init_coff import recognize


TARGET = "?startWinsockManually@?A0x8DBA2A05@io@frost@@YAXXZ"
GUARD_RECORD = f"reactor|.CRT$XCL|IMAGE_REL_AMD64_ADDR64|0000000000000000|{TARGET}"
INIT_RECORD = "reactor|.CRT$XCU|IMAGE_REL_AMD64_ADDR64|0000000000000000|_GLOBAL__sub_I_reactor.cpp"
INITIALIZER = """
0000000000000000 <_GLOBAL__sub_I_reactor.cpp>:
       0: 48 81 ec b8 01 00 00          subq $0x1b8, %rsp
       7: b8 01 00 00 00                movl $1, %eax
       c: f0                            lock
       d: 0f c1 05 00 00 00 00          xaddl %eax, (%rip)
        0000000000000010: IMAGE_REL_AMD64_REL32 ?data_@?$winsock_init@$01$01@detail@asio@@0Udata@winsock_init_base@23@A
      14: 85 c0                         testl %eax, %eax
      16: 74 15                         je 0x2d
      18: 48 8d 0d 00 00 00 00          leaq (%rip), %rcx
        000000000000001b: IMAGE_REL_AMD64_REL32 ??__Fwinsock_init_instance@detail@asio@@YAXXZ
      1f: e8 00 00 00 00                callq atexit
        0000000000000020: IMAGE_REL_AMD64_REL32 atexit
      24: 90                            nop
      25: 48 81 c4 b8 01 00 00          addq $0x1b8, %rsp
      2c: c3                            retq
      2d: 48 8d 54 24 20                leaq 0x20(%rsp), %rdx
      32: 66 b9 02 02                   movw $0x202, %cx
      36: ff 15 00 00 00 00             callq *(%rip)
        0000000000000038: IMAGE_REL_AMD64_REL32 __imp_WSAStartup
      3c: 87 05 04 00 00 00             xchgl %eax, 0x4(%rip)
        000000000000003e: IMAGE_REL_AMD64_REL32 ?data_@?$winsock_init@$01$01@detail@asio@@0Udata@winsock_init_base@23@A
      42: 48 8d 0d 00 00 00 00          leaq (%rip), %rcx
        0000000000000045: IMAGE_REL_AMD64_REL32 ??__Fwinsock_init_instance@detail@asio@@YAXXZ
      49: e8 00 00 00 00                callq atexit
        000000000000004a: IMAGE_REL_AMD64_REL32 atexit
      4e: 90                            nop
      4f: 48 81 c4 b8 01 00 00          addq $0x1b8, %rsp
      56: c3                            retq
"""
MANUAL_GUARD = f"""
0000000000000000 <{TARGET}>:
       0: b8 01 00 00 00                movl $1, %eax
       5: f0                            lock
       6: 0f c1 05 00 00 00 00          xaddl %eax, (%rip)
        0000000000000009: IMAGE_REL_AMD64_REL32 ?data_@?$winsock_init@$01$01@detail@asio@@0Udata@winsock_init_base@23@A
       d: 85 c0                         testl %eax, %eax
       f: 74 01                         je 0x12
      11: c3                            retq
      12: 31 c0                         xorl %eax, %eax
      14: 87 05 04 00 00 00             xchgl %eax, 0x4(%rip)
        0000000000000016: IMAGE_REL_AMD64_REL32 ?data_@?$winsock_init@$01$01@detail@asio@@0Udata@winsock_init_base@23@A
      1a: c3                            retq
"""


def member(name: str, body: str) -> str:
    return f"frost_io.lib(src/{name}.cpp.obj): file format coff-x86-64\nDisassembly of section .text:\n{body}"


FIXTURE = member("reactor", INITIALIZER + MANUAL_GUARD)


class StaticInitCoffTests(unittest.TestCase):
    def accepted(self, text=FIXTURE, guards=None, initializers=None):
        return recognize(text, [GUARD_RECORD] if guards is None else guards,
                         [INIT_RECORD] if initializers is None else initializers)

    def test_exact_release_lowering_is_recognized_with_its_earlier_guard(self):
        self.assertEqual(self.accepted(), ["reactor|_GLOBAL__sub_I_reactor.cpp"])

    def test_instruction_spacing_and_hex_case_do_not_change_the_signature(self):
        self.assertEqual(self.accepted(FIXTURE.replace("48 81 ec", "48\t81 EC")), ["reactor|_GLOBAL__sub_I_reactor.cpp"])

    def test_a_direct_unguarded_winsock_call_in_an_allowed_asio_tu_is_not_recognized(self):
        unguarded = "0000000000000000 <_GLOBAL__sub_I_reactor.cpp>:\n 0: ff 15 00 00 00 00 callq *(%rip)\n 2: IMAGE_REL_AMD64_REL32 __imp_WSAStartup\n 6: c3 retq\n"
        self.assertEqual(self.accepted(member("reactor", unguarded + MANUAL_GUARD)), [])

    def test_an_extra_call_or_tail_jump_or_even_padding_is_not_recognized(self):
        for extra in (" 57: ff 15 00 00 00 00 callq *(%rip)\n 59: IMAGE_REL_AMD64_REL32 __imp_WSAStartup\n",
                      " 57: ff 25 00 00 00 00 jmpq *(%rip)\n 59: IMAGE_REL_AMD64_REL32 __imp_WSAStartup\n",
                      " 57: 90 nop\n"):
            with self.subTest(extra=extra):
                self.assertEqual(self.accepted(member("reactor", INITIALIZER + extra + MANUAL_GUARD)), [])

    def test_an_unconditional_or_reversed_or_misdirected_branch_is_not_recognized(self):
        for changed in ("eb 15", "75 15", "74 16"):
            with self.subTest(branch=changed):
                self.assertEqual(self.accepted(FIXTURE.replace("74 15", changed)), [])

    def test_changed_counter_or_destructor_or_import_is_not_recognized(self):
        for old, new in (("?data_@?", "?other_@?"), ("??__Fwinsock_init_instance", "??__Funrelated"),
                         ("__imp_WSAStartup", "__imp_LoadLibraryW"), ("IMAGE_REL_AMD64_REL32 atexit", "IMAGE_REL_AMD64_REL32 abort")):
            with self.subTest(target=new):
                self.assertEqual(self.accepted(member("reactor", INITIALIZER.replace(old, new) + MANUAL_GUARD)), [])

    def test_extra_or_wrong_type_relocations_are_not_recognized(self):
        for changed in (INITIALIZER.replace("IMAGE_REL_AMD64_REL32 __imp_WSAStartup", "IMAGE_REL_AMD64_ADDR64 __imp_WSAStartup"),
                        INITIALIZER + " 57: IMAGE_REL_AMD64_REL32 __imp_WSAStartup\n"):
            self.assertEqual(self.accepted(member("reactor", changed + MANUAL_GUARD)), [])

    def test_unparsed_or_truncated_code_is_not_recognized(self):
        self.assertEqual(self.accepted(member("reactor", INITIALIZER + "unexpected disassembler output\n" + MANUAL_GUARD)), [])
        self.assertEqual(self.accepted(FIXTURE.replace("      56: c3                            retq\n", "")), [])

    def test_the_guard_must_be_unique_and_target_the_actual_reactor_function(self):
        for records in ([], [GUARD_RECORD, GUARD_RECORD], [GUARD_RECORD.replace("|" + TARGET, "|arbitrary_function")],
                        [GUARD_RECORD.replace("reactor|", "async_process|")], [GUARD_RECORD.replace("XCL", "XCU")],
                        [GUARD_RECORD.replace("ADDR64", "ADDR32")], [GUARD_RECORD.replace("|0000000000000000|", "|0000000000000008|")]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                self.accepted(guards=records)

    def test_the_guard_must_increment_the_same_counter_and_have_no_extra_work(self):
        for guard in (MANUAL_GUARD.replace("?data_@?", "?other_@?"), MANUAL_GUARD.replace("b8 01", "b8 00"),
                      MANUAL_GUARD.replace("74 01", "eb 01"), "", MANUAL_GUARD + " 1b: e8 00 00 00 00 callq additional_work\n"):
            with self.subTest(guard=guard), self.assertRaises(ValueError):
                self.accepted(member("reactor", INITIALIZER + guard))

    def test_the_initializer_must_run_after_the_guard_from_exactly_one_xcu_entry(self):
        for records in ([], [INIT_RECORD, INIT_RECORD], [INIT_RECORD.replace("XCU", "XCA")],
                        [INIT_RECORD.replace("XCU", "XCL")], [INIT_RECORD.replace("ADDR64", "ADDR32")],
                        [INIT_RECORD.replace("|0000000000000000|", "|0000000000000008|")]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                self.accepted(initializers=records)

    def test_recognition_is_scoped_to_both_archive_member_and_function_name(self):
        self.assertEqual(self.accepted(member("other", INITIALIZER) + member("reactor", MANUAL_GUARD)), [])
        self.assertEqual(self.accepted(FIXTURE.replace("<_GLOBAL__sub_I_reactor.cpp>", "<unrelated>")), [])
        other = member("other", INITIALIZER)
        self.assertEqual(self.accepted(FIXTURE + other), ["reactor|_GLOBAL__sub_I_reactor.cpp"])

    def test_duplicate_function_definitions_fail_closed(self):
        with self.assertRaises(ValueError):
            self.accepted(FIXTURE + member("reactor", INITIALIZER))

    def test_nonmatching_debug_lowering_keeps_the_original_callee_inspection(self):
        debug = "0000000000000000 <_GLOBAL__sub_I_reactor.cpp>:\n 0: e8 00 00 00 00 callq init\n 1: IMAGE_REL_AMD64_REL32 ??__Ewinsock_init_instance@detail@asio@@YAXXZ\n 5: c3 retq\n"
        self.assertEqual(self.accepted(member("reactor", debug + MANUAL_GUARD)), [])


if __name__ == "__main__":
    unittest.main()
