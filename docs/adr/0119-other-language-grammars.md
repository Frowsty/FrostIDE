# ADR 0119: Additional real tree-sitter language profiles

Status: audited source recipes and implementation authored; independent source review cleared (575333a1); execution deferred.

The parent explicitly authorized modest read-only upstream metadata/license/source archive auditing and immutable recipes for a later user-authorized build. No package installation, parser generation, configure, compilation or test execution occurs during this implementation.

LTR-18 extends the existing tree-sitter runtime with actual Python, JavaScript/JSX, TypeScript, TSX, YAML, TOML and PowerShell generated parsers. These are not renamed C++ parsers or lexical substitutes. Current repository runtime ABI support includes the inspected generated ABI14/15 profiles.

| Recipe | Version | Immutable commit | Source archive SHA256 |
|---|---|---|---|
| python | 0.25.0 | `293fdc02038ee2bf0e2e206711b69c90ac0d413f` | `b74d8ba6730535354175436208fe1452fd464f235263959ca40b8bf32cb23dc6` |
| javascript | 0.25.0 | `44c892e0be055ac465d5eeddae6d3e194424e7de` | `a13fa28148e41bb939b2c2b8cd73c4b4295273b70cbac55a7a589020f52b9611` |
| typescript | 0.23.2 | `f975a621f4e7f532fe322e13c4f79495e0a7b2e7` | `4de2e82e557810eecb93cb3b31fbb3bd28ba4c91ba9a6c6164bcaa074125b7b5` |
| yaml | 0.7.2 | `7708026449bed86239b1cd5bce6e3c34dbca6415` | `6e8cb69a4f7a05478b1143d38bd10df2163170a6678df1848f3be89c8b7c5f3b` |
| toml | 0.7.0 | `64b56832c2cffe41758f28e05c756a3a98d16f41` | `feeb2e1cf531588cdcfb9c57292620151a08597f18a98dad26cc17fd4b544dcb` |
| powershell | 0.26.6 | `65ea6919e816a7ac142acec7ee72757f89db8439` | `ce077de1a8537476750152afa984346a65519b7b795348cc8fb7f1e0e28e4a4a` |

The six upstream projects are the tree-sitter organization Python/JavaScript/TypeScript repositories, tree-sitter-grammars YAML/TOML, and the maintained Airbus CERT PowerShell grammar (the older PowerShell organization grammar is archived). Each original upstream LICENSE was read and retained byte-exact under third_party/language-grammar-licenses. All six declare MIT and contain the original permission/copyright notices. MIT is already on the repository allowlist; no exception or new runtime package manager is introduced.

third_party/language-grammars.lock.json retains exact source URLs, archive roots, commit/archive hashes and per-file hashes for generated C parsers, scanners, shared headers/schema tables and node-types metadata. The initial audit read about 2.15 MiB of source archives, bounded individually below16 MiB; these are source archives, not executable packages. Generated parser headers were inspected independently from the pinned raw upstream commit to establish ABI14/15.

cmake/FrostLanguageGrammars.cmake is a future-configure recipe. It validates original license evidence, fetches only the exact HTTPS commit archive with SHA256 when the archive cache is missing and fetching is enabled, checks the cached archive, extracts under the build tree and checks every retained generated-source member hash. It never runs upstream CMake, npm, Python packaging hooks, tree-sitter generate or other upstream generators. FROST_LANGUAGE_GRAMMAR_CACHE supports prepopulated offline archives; FROST_FETCH_LANGUAGE_GRAMMARS=OFF refuses missing source instead of silently dropping language support.

The normal language table/registry, incremental SyntaxSession, query capture checks and compiler/platform flags remain the existing implementation. Original FrostIDE highlight/outline queries are authored from the exact audited node-types metadata. YAML/TOML source highlighting has no inferred semantic symbol oracle; actual LSP capabilities stay with authenticated local registry providers. Existing all-language ABI/query/span/incremental tests now include the new grammars, with authored representative source samples and explicit tests for filename selection, highlighting and source outline.

tools/licensecheck.py gains an offline recipe gate for exact upstream commit URLs, archive/member pins, safe paths, original MIT evidence and required ADR, and contributes all six source recipes to the SBOM. It does not demand fetched build sources during source-only work or weaken the existing vendored-file gate. NOTICE and REUSE preserve the audited notices. Tests for changed license/URL and committed recipe evidence are authored only.

No build, configure, source generator, grammar query compilation, parser execution, test runner, WSL or performance acceptance ran. ABI compatibility, authored query validity and real-platform behavior require the later user-authorized verification stage.
