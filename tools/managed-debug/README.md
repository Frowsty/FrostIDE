<!-- SPDX-License-Identifier: Apache-2.0 -->

# Managed debugger source build and local installation

This reviewed recipe builds the CoreCLR DAP adapter and the FrostIDE source
verifier from source for the bounded local workflow. It does not authorize publication or installation of the
upstream prebuilt debugger archives. The initial application workflow supports
trusted .NET 10 console projects on Windows and Linux x64; Unity Mono, IL2CPP,
attach, other target frameworks and multi-module source are outside this slice.

`inputs.json` and `packages.lock.json` list the source, runtime and NuGet
identities. Prepare a clean checkout of the exact runtime commit and a private
cache containing the named official source and package archives. Supply absolute
paths to the already provisioned pinned .NET SDK, LLVM, CMake and Ninja. The
Windows toolchain also requires the pinned MSVC toolset and Windows SDK recorded
in `sdk/sdk.lock.json`; Linux requires the pinned GCC 14 runtime. No package
sources are contacted during restore.

```text
python tools/managed-debug/build_payload.py \
  --cache ABSOLUTE_CACHE --runtime ABSOLUTE_RUNTIME_CHECKOUT \
  --sdk-root ABSOLUTE_DOTNET_SDK --llvm-root ABSOLUTE_LLVM \
  --cmake ABSOLUTE_CMAKE --ninja ABSOLUTE_NINJA \
  --work NEW_PRIVATE_WORK_DIRECTORY --output NEW_PAYLOAD_DIRECTORY --jobs 1
```

The recipe verifies the reviewed patch and verifier source, audits archives and
notices, restores the locked managed graph in isolated profiles and package
caches, builds the DAP-only adapter, verifies the adjacent platform dbgshim and
native system imports, and builds the verifier. It writes an exact file list and
hashes to `manifest.json` and prints that manifest's SHA-256. The recipe checks
tool versions and records executable hashes; it does **not** independently
revalidate every installed compiler and SDK file against its bootstrap archive.
Retain the build log, manifest digest, and trusted toolchain provenance with the
artifact. Its package and platform libraries are not a claim of release approval.

To install a payload into an existing FrostIDE application directory, provide
the manifest digest from the independently retained build record:

```text
python tools/managed-debug/install_payload.py \
  --payload ABSOLUTE_PAYLOAD --manifest-sha256 BUILD_MANIFEST_SHA256 \
  --application-dir ABSOLUTE_APPLICATION_DIRECTORY
```

The payload contains 25 manifest-listed files plus `manifest.json`. The
installer checks the digest, exact file list and pinned input identities,
rejects links and altered or additional files, copies to a new temporary
directory, checks the copy, and atomically creates `FrostManagedDebug` without
replacing an existing directory. The installed verifier is
`FrostManagedDebug/PdbGuard/PdbGuard.dll`. The .NET action defaults to the
platform debugger executable in `FrostManagedDebug/adapter`; its path can be
overridden. The adapter and shim
also require their recorded operating-system libraries and an installed .NET 10
runtime; these system dependencies are not included in the payload.

Fresh source builds, installation and relocated Workbench fixtures passed on
Windows and Linux using the reviewed local payload and default discovery. The
fixture checks a trusted console project, Save All, breakpoint and local
inspection, stepping, continuing to session end, edit cancellation, Stop and
trust revocation; it does not assert the program's exit code or stdout. The
application validates one module's PE, portable PDB and SHA-256 source documents
and presents matching immutable source snapshots. Other modules, unsupported
encodings and non-adjacent PDB layouts are outside the initial workflow; checks
do not lock files against external changes or eliminate the validation-to-launch
race.

The local payload, its dependency and notice checks, and installer received
independent review. Combined functional verification and the separately corrected
Windows formatting failure are recorded in the progress log. The recorded tool
versions and executable hashes do not constitute full bootstrap-archive
verification or a reproducible-build proof. General release or distribution
approval has not been granted. Keep direct adapter probes distinct from the
application workflow acceptance test.
