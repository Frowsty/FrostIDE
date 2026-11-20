<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0112: Verified managed library sources and an existing MIT decompiler

- Status: bounded implementation independently source-reviewed; compiled API and runtime acceptance pending
- Date: 2026-11-20
- Plan: R17 LTR-6; builds on ADRs 0073 and 0109

## Dependency and provenance decision

Use ICSharpCode.Decompiler 9.1.0.7988 from the already reviewed managed dependency payload. The
repository's `sdk/managed-dependencies.json` records its MIT expression, package archive SHA-256
`ccf2e02cd3b8702aed37d051f71e97f96d0c36443bd67003228a4e0b55418404` and nuspec SHA-256
`02f428ab69d1fef6cd866588c0240d914c982c073700ab34830749c0358fc89c`. Its runtime graph already
includes the package; this slice does not introduce a new package restore or arbitrary DLL loader.
The host builder first uses `managed_server.verify_installed` against the pinned offline archive
and graph, requires the recorded version/licence, directly references the verified DLL and confirms
that publication copied that exact DLL. The host source/payload receipt covers the new provider.

The [official NuGet metadata](https://www.nuget.org/packages/ICSharpCode.Decompiler/9.1.0.7988) describes
a netstandard2.0 engine and System.Collections.Immutable/System.Reflection.Metadata dependencies
with minimum version 6.0.0. The [upstream package project](https://github.com/icsharpcode/ILSpy/blob/v9.1/ICSharpCode.Decompiler/ICSharpCode.Decompiler.csproj)
declares MIT and no package licence acceptance requirement. Those runtime dependencies are resolved
by the existing audited payload and pinned net10.0 host/runtime, rather than adding a new 6.0.0
restore graph. This compatibility is an authored build/runtime acceptance condition, not a result
obtained during this slice. The [upstream licence attribution](https://github.com/icsharpcode/ILSpy/blob/v9.1/doc/ILSpyAboutPage.txt)
identifies the ILSpy contributors; original managed-payload notices and metadata remain packaged.
No ILSpy GUI, GPL IDE source, proprietary decompiler, vsdbg, or licence acceptance is used.

An independent official catalogue receipt is
[ICSharpCode.Decompiler 9.1.0.7988](https://api.nuget.org/v3/catalog0/data/2025.04.22.07.33.02/icsharpcode.decompiler.9.1.0.7988.json):
SHA-512 base64 `mNOUtNVxuqe05TkozPsmXis/Bv3V2O5kaDlpe1pzwASe34wvmD8AicK4Eoykg/I6QNLGz4S1uoG/e/LAHm0AEA==`,
package size 1,361,834 bytes. The existing repository archive pin remains authoritative for the
actual local asset verifier. Reading public metadata did not download or install a binary payload.

## Source and symbol fidelity

`frost/managed/librarySource` is an owned RoslynHost semantic extension. It starts from the current
evaluated project/TFM and overlay position, obtains the actual metadata symbol and uniquely matching
compiler reference, and binds the response to current PE bytes, module identity, source bytes and
provider kind. It never executes the target assembly. Decompiled output is real ILSpy output for
the declaring type and explicitly labelled reconstructed C#, not original source or an exact member
line. Reference assemblies can supply declaration/stub bodies only; they are not silently replaced
with an unrelated runtime implementation. Raw IL and Assembly Metadata views expose the actual PE
method bytes and TypeDef identities, including an explicit no-IL-body result.

Offline F12 first attempts portable-PDB embedded source, then real decompilation. PDB CodeView
identity must match the PE; an adjacent external PDB also needs a PE PDB-checksum match with its ID
bytes zeroed, following the existing guard policy. Embedded PDB expansion and embedded source
expansion are bounded before use. Source bytes require the exact document SHA-256 checksum and
strict UTF-8 decoding. Unsupported hashes, unavailable sequence points or an ambiguous member do
not acquire guessed original-source locations. The bounded original-source selection currently
requires one matching method name/parameter count; overloaded same-count methods and multi-method
type declarations use decompilation. Invalid supplied PDB/source identities fail visibly.

SourceLink follows the [portable PDB custom metadata contract](https://github.com/dotnet/runtime/blob/main/docs/design/specs/PortablePdb-Metadata.md).
It uses the document's actual source map, escaped path segments and verified SHA-256. Only the
explicit **Fetch Original Library Source (SourceLink)** action enables network; ordinary navigation,
decompilation, IL and metadata views remain offline. The initial public origin policy permits only
HTTPS port 443 on raw.githubusercontent.com, with no credentials, query/fragment, proxy, cookies or
redirects, and a bounded response and timeout. Other/public-private source providers and other map
forms remain visibly unsupported. This opt-in reads source; it grants no execution, workspace trust,
credential forwarding, package installation or source redistribution authority. Source bytes and
existing licence comments are preserved exactly.

## Cache and UI boundary

Provider paths never become cache output paths. Output uses validated content/assembly/provider
identity digests and a fixed .cs/.il suffix in the profile metadata cache outside the captured
physical workspace. Existing ancestor/cache aliases and non-regular entries refuse. Cache count,
total size, individual output size and metadata/assembly sizes are bounded. A cached entry whose
bytes changed is refused rather than overwritten or presented as matching evidence. Navigation uses
the existing version/cancellation slot and the workbench's forced-read-only editor entry point.
These filesystem observations do not claim a retained-handle lease against hostile concurrent
replacement; external writers can race them. No cache output is intentionally placed in the project.

## Verification and remaining limits

Authored editor entry-point regressions cover actual provider bytes, receipt/cached-content
validation, read-only opening, offline versus explicit SourceLink requests and IL routing. The real
managed semantic test asks the actual host for an external reference symbol. A separately registered
real-provider probe emits a real PE/portable PDB/embedded document with Roslyn, compares original
source byte-for-byte, invokes the actual audited decompiler, compares raw IL against the PE reader,
and checks source-map/path/expansion/cancellation policies. Missing reviewed SDK/host inputs fail
that mandatory probe rather than count as a pass.

No build, configuration, test, feature execution, WSL, installation or payload download was run.
Compiled API compatibility, real provider output, end-to-end UI behavior and Windows/Linux acceptance
remain deferred under the user's implementation-first instruction. This slice does not claim an
editable recovered project, full assembly explorer tree, source-server authentication, arbitrary
SourceLink origins, all symbol layouts, native PDBs, non-UTF-8 sources or exact decompiled member
sequence points. Those are concrete limits, not runtime verification successes.
