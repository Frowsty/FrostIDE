<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0073: Source-built CoreCLR debugger for .NET 10 console projects

- Status: bounded implementation and local payload recipe reviewed; combined
  functional verification is recorded in the progress log. General release and distribution are not approved.
- Date: 2026-11-03
- Plan reference: Appendix B MANAGED-02.

## Decision and boundary

Use a DAP-only source build of Samsung netcoredbg at commit
`9744e1f051866215611b8440c638042aa2aa2f72` with a separately pinned Microsoft
dbgshim and Roslyn managed component for the reviewed local payload. The
application workflow targets one trusted .NET 10 console project's module on
Windows and Linux x64. This does not establish Unity Mono, IL2CPP, attach,
managed test running, profiling, other frameworks or MANAGED-02 completion.

The upstream Windows and Linux release archives contain Roslyn 2.3 binaries
whose NuGet package licence URL currently returns 403; their binary terms and
the release dbgshim's provenance have not been established. Do not distribute
or automatically install those archives. Their SHA-256 values, used only for
local compatibility investigation, are respectively
`3c410a45fa502415203a94fcb88654af65bf8e3dac158a5527a722e7a6b9274a` and
`080eb3b2d2152465f599d3b33d1ee6e747794e11cc0a3773ec689f5e5f2c5afa`.

## Inputs and build

The upstream project licence is MIT. Its build defaults fetch a floating
runtime branch, SDK channel and NuGet versions. The reviewed recipe disables
those fetches, uses the pinned graph, and checks staged files and notices. The
runtime v10.0.14 tag
resolves to `6aa72daa24f66c01c3d7b6a32519e9d21396deec`; clean pinned Git
checkouts supplied the headers and helpers used by both scratch builds. The
reviewed DAP-only patch excludes CLI, MI and their linenoise-ng dependency,
as well as interop, hot reload and upstream tests. It retains the debugger's
warning-as-error policy. The patch SHA-256 is
`6379dd5d3fd68f4bb2cbc5ba9252e43bc8650f84bb22c9790968a4396b87a50b`.

The official Microsoft.Diagnostics.DbgShim.win-x64 and .linux-x64 10.0.750501
NuGet archives declare MIT and identify diagnostics commit
`ad724c4b85e7ae775e42f393c6e95a7557a57954`. Their archive SHA-256 values are
`7441fb0c0a14ea705cdc208a4db19a5e238e736699228691712cb90cc4dbf069` and
`788a66a8404dfc649abc2ef8765e7285e11bad963cab1dd0c662c5127c39bb5b`.
They describe themselves as internal implementation packages. A build patch
must keep shim lookup relative to the debugger executable while preventing
upstream's floating shim restore. The source build implements this relative
lookup. These packages describe themselves as internal implementation packages;
the review covers the pinned assets used here.

The official source archive retrieved for netcoredbg commit
`9744e1f051866215611b8440c638042aa2aa2f72` has SHA-256
`76015541132e982ca842facfb685a1fe5a7948f4c42ebba76986bfe324fcc59b`.
The runtime source download in scratch is incomplete and is not a pin.

The upstream managed component targeting net10.0 removes
legacy framework package references and used Microsoft.CodeAnalysis.CSharp.
Scripting 5.9.0. Its restore graph comprises Common, CSharp, CSharp.Scripting,
Scripting.Common 5.9.0 and Analyzers 5.9.0-1.26328.17, all with MIT expressions
in their nuspecs. The input, staging and installation checks gate the pinned
archives, publish files, package metadata and notices. The net10 target limits
the managed component to compatible runtimes.
The four runtime packages contain byte-identical `ThirdPartyNotices.rtf` files
(SHA-256 `ef4678850e7e3aaf9297f07e5c4060d477854e545ccf8e2ef8452d65a7b2efaf`);
the analyzer's `ThirdPartyNotices.txt` has SHA-256
`0eabe2880daf9bfb25ac160b1e779c5da95a88406dd06cdb570ec7edb5eabb76`.
These notices and the original metadata for all seven packages, including the
build-time analyzer, are included in the payload.

The locked restore archive SHA-256 values are:

| Package | Version | SHA-256 |
|---|---|---|
| Microsoft.CodeAnalysis.Common | 5.9.0 | `77efd4715c24c8cd6b36f9b92980a2f5a86e4a7b53648d4f1ca3b79a3933a429` |
| Microsoft.CodeAnalysis.CSharp | 5.9.0 | `7602c3dbaa122268f3b26c6cc0dd02eb9224dea9442ed27af5c3fe56d643993f` |
| Microsoft.CodeAnalysis.CSharp.Scripting | 5.9.0 | `61ff8463c1debf44bfc4ca4bc31a6a74dbbcb44c6163fde1b05ddb851114aa79` |
| Microsoft.CodeAnalysis.Scripting.Common | 5.9.0 | `058777a379e2bd6b004eb8bc8d81aaeb25f09443d80979f2ba46794f434b5345` |
| Microsoft.CodeAnalysis.Analyzers | 5.9.0-1.26328.17 | `a7ef4c9b2cf403bf54e4b52eb8fb89e946a286894f148ecbe7515d81c7b0f612` |

## Application workflow and source fidelity

The Workbench's **Debug .NET Project** action selects a project and explicit
SDK host, configuration, framework and optional RID, saves documents, then
builds, queries the output with matching properties and validates its symbols.
The host launches the DLL through CoreCLR DAP and the existing debug controls.
Trust and workspace state are checked before project execution and at phase
transitions; owned processes and pending validation are cancelled on Stop,
workspace changes, edits or observed trust revocation. The initial path checks
reject symlinks, junctions and ambiguous component casing.

PdbGuard validates the PE CodeView identity and PDB checksum, then SHA-256
checksums for every source document in the one module. It returns the module
identity and hashes of the exact PE, PDB and source bytes it read. The
application captures immutable matching source bytes for the read-only source
view, rejects unvalidated modules and restricts breakpoint bindings to verified
sources. Live-file validation for changed breakpoints and run-to-cursor occurs
on a worker; session generation, request order, trust and editor state gate
publication. These checks are not filesystem locks: external writers and
path replacement can race checks and launch. Dirty or changed source cannot be
presented as a matching live editor; unsupported source encodings are refused
by the snapshot viewer. Other modules and symbol layouts outside the supported
single-module, adjacent portable-PDB path are not established by this slice.

The [portable PDB document table](https://github.com/dotnet/runtime/blob/main/docs/design/specs/PortablePdb-Metadata.md#document-table-0x30)
records source byte hashes and algorithms; [PE debug directory metadata](https://github.com/dotnet/runtime/blob/main/docs/design/specs/PE-COFF.md#pdb-checksum-debug-directory-entry-type-19)
also binds the PDB to the assembly. Both relationships require validation;
checking the document hash against an unrelated PDB is insufficient.

## Payload, verification and remaining limits

The source-build recipe and installer described in the
[managed-debug README](../../tools/managed-debug/README.md) produce a payload
with 25 manifest-listed files plus the manifest. The installer verifies its
supplied manifest digest, exact file set, hashes, pinned identities and recorded
system imports before a no-replace installation. The bundle includes the
adapter, adjacent verified RID-specific shim, managed assets, PdbGuard and
notices. It does not include the required operating-system libraries or .NET
runtime. The installed adapter and verifier are the Workbench defaults; the
adapter path may be overridden explicitly.

Independent review covered the bounded source patch, package and notice inputs,
PdbGuard, manifest and installer, and checked the Windows and Linux local
payloads. Both platforms passed fresh source builds, installation and a real
Workbench fixture using the relocated bundle and default discovery. The fixture
exercised Save All, a verified breakpoint, stack and local inspection, stepping,
continue to session end, source-edit cancellation, Stop and trust revocation.
It did not assert the debuggee's exit code or stdout. Separate direct DAP and
64-bit enum probes supplied adapter-level evidence; they are not application
acceptance or general parity evidence.

The recipe records tool versions and executable hashes, but does not verify
every installed SDK and compiler file against the official bootstrap archives
or prove bit-for-bit reproducibility across machines. Combined functional verification
and the separately corrected Windows formatting failure are recorded in the progress log.
These local results do not approve general distribution or
the unverified upstream release archives.
