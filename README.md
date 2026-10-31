# FrostIDE

A fast C/C++ IDE for Windows and Linux: Rider-style UX, full Visual Studio solution support
(.sln, .slnx, .vcxproj), CLion-grade CMake, Clang-based analysis and a built-in Claude Code integration.

Status: milestone M0 (bootstrap). See `PLAN.md` for the roadmap and `docs/progress.md` for current state.

## Building
Windows (PowerShell):

    powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1
    . tools\env.ps1
    cmake --preset win-clangcl
    cmake --build --preset win-clangcl-debug
    ctest --preset win-clangcl-debug

Linux / WSL:

    bash tools/bootstrap-wsl.sh        # after the one-time --system phase as root
    source ~/sdk/env.sh
    cmake --preset linux-clang
    cmake --build --preset linux-clang-debug
    ctest --preset linux-clang-debug

`tools\verify.ps1` and `tools/verify.sh` run the full local verification (there is no CI).

## Licence
Apache-2.0; see `LICENSE` and `NOTICE`.
