# ADR 0097: C++ platform, configuration index and tool-derived preprocessing/modules

- Status: Accepted source architecture; runtime acceptance deferred
- Date: 2026-11-14
- Scope: CPP-7, CPP-8, CPP-13, CPP-15; extends ADR 0083

The application uses the bundled `frost-clang` as its default C++ language server. An explicit
`tools.clangd.path` remains authoritative; installations without the bundled engine retain the
existing pinned stock-clangd/PATH fallback. The bundled selection must advertise schema 1 of
`frostClangFeatureModule` with the pinned LLVM version before the normal service enters Running.
Ordinary clangd protocol, synchronization, trust restrictions, cancellation, shutdown and resource
governance remain on the existing ClangdService path. The staged LLVM resource directory is passed
to clangdMain when the caller supplied no resource-directory override.

LLVM remains in the Qt-free out-of-process `/MT` helper. No LLVM objects or CRT allocations cross
into the application. The pinned noninstalled clangd SPI is unchanged; new semantic producers use
installed Clang Index/AST/PP APIs in separate translation units rather than invented SPI methods.

The configuration index will persist actual Clang symbol occurrences and relations using the
existing storage Database. A translation-unit identity includes its complete compilation command,
working directory and selected configuration; two `-D` variants of one source never share an index
generation. Publication requires unchanged captured source/dependency inputs and current project
generation. Cancellation and stale results do not publish partial generations. Queries expose the
configuration and completeness rather than claiming whole-project coverage from a partial index.

Preprocessor views will retain actual Clang PP callbacks and preprocessed output with source and
configuration locations. Module data will come from actual Clang dependency scanning output,
including provides/requires and reported artifact paths; missing tools and unsupported outputs are
reported as unavailable, never substituted with guessed module nodes. These are implementation
boundaries for the following verticals, not claims they are already implemented.

The implementation-first phase authors local correctness/oracle and scale cases without running
tools, builds, tests, journeys or benchmarks. No speed, memory or FrostEngine journey acceptance is
claimed by this decision or the default-server source checkpoint.
