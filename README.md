# FrostIDE

A Windows and Linux IDE under development toward independently implemented Rider feature and workflow
parity, with C/C++, CMake, Lua, Unity, FrostEngine and Claude Code integration. These are product goals;
the application does not yet deliver the complete feature set.

Status: M0 is complete; the W1 application integration and subsequent feature slices are in progress.
See `PLAN.md` and ADR 0054 for scope, and `docs/progress.md` for implementation and verification evidence.

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

## Unity editor bridge (experimental)

In Unity's Package Manager, choose **Add package from disk** and select
`integrations/unity/com.frostide.editor/package.json` (in a staged Windows build, use
`unity/com.frostide.editor/package.json`). Choose FrostIDE's `frost.exe` or `frost` as the external
script editor. An absolute executable path in `FROSTIDE_EXECUTABLE` also advertises FrostIDE in Unity's
editor list. The generated C# context requires a configured managed language server and a trusted project.
The bridge reports unsupported compiler arguments and response files in Unity's Console and retains the
last successfully generated context. Unity play controls, debugging and test integration are still in progress.

## Licence
Apache-2.0; see `LICENSE` and `NOTICE`.
