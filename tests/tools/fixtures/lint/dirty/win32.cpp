// SPDX-License-Identifier: Apache-2.0
// Narrow Win32 calls for the frost-lint tests.
void f() {
    CreateFileA(0, 0, 0, 0, 0, 0, 0);
    ::LoadLibraryA("x.dll");
    GetModuleFileNameA(0, 0, 0);
    CreateFileW(0, 0, 0, 0, 0, 0, 0);
}
