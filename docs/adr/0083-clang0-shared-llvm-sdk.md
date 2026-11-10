# ADR 0083: CLANG-0 Shared LLVM SDK and `/MT` Process-Boundary Engine (`frost-clang`)

* **Status**: Accepted
* **Date**: 2026-11-10
* **Requirement**: `CLANG-0` (`PLAN.md` Appendix C, R5 exit criteria)

## Context and Problem Statement

FrostIDE's pinned LLVM 23.1.4 SDK (`sdk/sdk.lock.json`) ships static archives (`clangdMain`,
`clangDaemon`, `clangDaemonTweaks`, `clangdSupport`, `clangTooling`, `clangASTMatchers`,
`clangRewrite`, `LLVMSupport`, etc.) for both Windows (`x86_64-pc-windows-msvc`) and Linux
(`x86_64-unknown-linux-gnu`).

On Windows, the LLVM 23.1.4 static archives are compiled with:
* `RuntimeLibrary=MT_StaticRelease` (`/MT`, `_ITERATOR_DEBUG_LEVEL=0`),
* `/EHs-c- /GR-` (no C++ exceptions, no RTTI), and
* `annotate_string=0` / `annotate_vector=0` (uninstrumented STL containers).

By contrast, FrostIDE's application and test binaries use the dynamic CRT (`/MDd` in Debug,
`/MD` in Release/RelWithDebInfo) because Qt 6.12.0's prebuilt Windows libraries require `/MD`,
and the ASan preset (`win-clangcl-asan`) compiles with `-fsanitize=address` (`annotate_string=1`).
Linking the `/MT` LLVM static libraries directly into an `/MD` or ASan-instrumented target fails
at link time via COFF `/FAILIFMISMATCH` directives (`RuntimeLibrary`, `_ITERATOR_DEBUG_LEVEL`, and
`annotate_string`).

At the same time, FrostIDE's C++ refactoring and language roadmap (`CLANG-0`, `UPG-1`) needs two
capabilities backed by the pinned LLVM 23.1.4 SDK without duplicating a second LLVM build:
1. Custom `clang::clangd::FeatureModule` / `Tweak` extensions linked directly into `clangdMain`
   and exposed over the LSP JSON-RPC stream.
2. Custom Clang `LibTooling` / `ASTMatchers` / `Rewriter` tools that inspect and rewrite C++ ASTs
   using the exact same Clang frontend as `clangd`.

## Decision

### 1. Unified Qt-Free Out-of-Process Engine (`frost-clang`)

We build a single Qt-free helper executable, `frost-clang` (`src/clang/`), linked directly against
the pinned LLVM 23.1.4 static archives (`${FROST_LLVM_SDK_ROOT}/lib`):
* On Windows (`MSVC` / `clang-cl`), `frost-clang` sets `MSVC_RUNTIME_LIBRARY "MultiThreaded"`
  (`/MT`), `_ITERATOR_DEBUG_LEVEL=0`, `NDEBUG`, `/EHs-c-`, and `/GR-` in every build tree
  (including `Debug`, `Release`, `RelWithDebInfo`, `win-clangcl-asan`, and `win-clangcl-fuzz`).
* On Linux (`clang++`), `frost-clang` compiles with `-fno-exceptions -fno-rtti -DNDEBUG` against
  the static `.a` archives.
* FrostIDE communicates with `frost-clang` strictly across process boundaries (`frost::io::IProcessLauncher`
  and Content-Length JSON-RPC stdio or CLI invocations), completely isolating the `/MT` static CRT
  and LLVM global command-line registry from FrostIDE's `/MD` Qt process.

### 2. Self-Contained Compression and CRC-32 Shim

Upstream `LLVMSupport` exports `ZLIB::ZLIB` and `zstd::libzstd_static` in `LLVMExports.cmake` for
`Compression.cpp.obj` and `CRC.cpp.obj`. Because `frost-clang` must link cleanly in all presets—including
`/MT` fuzz trees where `VCPKG_MANIFEST_INSTALL=OFF`—`src/clang/src/llvm_compression_shim.cpp`
provides the `!LLVM_ENABLE_ZLIB` / `!LLVM_ENABLE_ZSTD` implementations of `llvm::compression::*`
(`isAvailable() == false`, causing `clangd` and `clangSerialization` to use their built-in
uncompressed paths) together with an IEEE 802.3 `crc32` implementation. As a result, `frost-clang`
requires no external compression libraries on either platform.

### 3. `FrostFeatureModule` Linked into `clangdMain`

In default (LSP server) mode, `frost-clang` delegates to `clang::clangd::clangdMain(argc, argv)`
with `FrostFeatureModule` registered in `llvm::Registry<clang::clangd::FeatureModule>`
(`FeatureModuleRegistry`):
* During `FeatureModule::initializeLSP(LSPBinder&, ...)`, `FrostFeatureModule` advertises
  `frostClangFeatureModule` in `ServerCapabilities` and binds custom LSP methods
  (`frost/clangInfo` and `frost/spikePing`).
* Requests to `frost/clangInfo` or `frost/spikePing` return a structured JSON object reporting the
  engine identity (`"frost-clang"`), the linked LLVM version (`"23.1.4"`), CRT linkage (`"MT"` on
  Windows, `"static-llvm"` on Linux), and the echoed request payload.

### 4. LibTooling AST Rewrite Mode (`--rewrite-symbol`)

When invoked with `--rewrite-symbol <old-symbol> <new-symbol> <file> [-- <compile-flags...>]`,
`frost-clang` runs a `clang::tooling::ClangTool` with `clang::ast_matchers::MatchFinder` and
`clang::Rewriter`:
* Matches declarations (`namedDecl`, including destructors), expression references (`declRefExpr`),
  member accesses (`memberExpr`), constructor field initializers (`cxxCtorInitializer`), and type
  locations (`typeLoc`) in the main file whose spelled token matches `<old-symbol>`.
* Rewrites only matched C++ AST tokens on disk and stdout, leaving comments, string literals, and
  longer identifiers untouched.

## Verification

* `clang_spike_tests` (`tests/clang/clang_spike_test.cpp`):
  * Launches `frost-clang` via `frost::io::realProcessLauncher()` in LSP mode, completes the
    `initialize` / `initialized` handshake, verifies `frost/clangInfo` and `frost/spikePing`
    responses, and performs a clean `shutdown` / `exit`.
  * Launches `frost-clang --rewrite-symbol` on temporary C++ fixtures (functions, namespaces,
    classes, constructors, destructors, member fields), verifies comments and string literals are
    preserved, and compiles the rewritten C++ files with the pinned Clang compiler.
