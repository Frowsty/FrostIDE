// SPDX-License-Identifier: Apache-2.0
//
// The shared library of the runtime-DLL fixture (tests/cmake/runtime_dlls_fixture/shared).
// It is built but never loaded; its one export makes the linker write an import library,
// which CMake lists among the link step's outputs.
#ifdef _WIN32
__declspec(dllexport)
#endif
int fixture_shared_lib() {
    return 0;
}
