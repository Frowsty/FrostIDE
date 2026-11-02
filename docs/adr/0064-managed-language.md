<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0064: Managed language service

- Status: proposed
- Date: 2026-11-02
- Plan reference: Appendix B managed language support.

## Decision under evaluation

Use the MIT-licensed csharp-ls 0.28.0 release as a supervised stdio language
server, backed by Roslyn workspaces. The pinned NuGet archive and source revision
are recorded in `sdk/sdk.lock.json`; .NET 10 is required. C# project evaluation
can execute project-supplied code, so the process and project loading require
workspace trust. C++ and Lua retain their separate language services. This is a
language-service slice, not a claim of complete Unity project modelling, build,
debugging or editor parity.

The official Microsoft roslyn-language-server preview archive was evaluated but
not adopted: its bundled dependency terms include components outside this
repository's permitted distribution policy. The server licence alone does not
establish the terms of its dependencies. The release contains two `.deps.json`
files: the server graph and the nested
BuildHost-netcore graph. The manifest records both, all 21 physical build-host
files and 57 distinct NuGet package versions, including the additional net472
assets and a second Microsoft.Build.Locator version. Each exact NuGet package
archive and nuspec was fetched from the
official v3 feed; the archive and nuspec hashes, licence declarations,
attribution and upstream notice mapping are recorded in
`sdk/managed-dependencies.json`. Notice/licence files in the packages
reduce to six byte-identical groups, retained in `third_party/managed-notices`.
Across both server and build-host graphs, 55 package versions declare MIT and
two declare Apache-2.0.
FSharp.Control.AsyncSeq 3.2.1 has an obsolete licence URL; the upstream licence
at commit `8064abd5cd8f180fb02821e38beffae6fd9be485`, preceding that release,
is Apache-2.0. The csharp-ls source imports Ionide at
`712ce3b47ee044e9ef57ad36ddbd1764390392d9`; three sampled files match upstream
Ionide `d639298b94bea594b4ffa741bc3157b6abdb0683` byte for byte. The latter's
MIT licence is retained alongside the source release's own licence. The source
release subsequently changes the imported project. Original third-party notices
are retained even when they discuss components outside the runtime graph.

The 21 build-host files match `contentFiles` in the official
Microsoft.CodeAnalysis.Workspaces.MSBuild 5.9.0 package. The 11 net472 external
DLLs and netcore locator additionally match exact individual NuGet package assets
by SHA-256; their package versions, asset paths and archive hashes are in the
manifest. `--audit-cache` verifies every original dependency archive, embedded
nuspec, notice and host-asset match.

`tools/managed_server.py` checks the pinned release hash, the full NuGet graph,
the manifest and every notice hash before staging. It copies notices with the
server, and verifies every installed file on subsequent bootstrap runs. Bootstrap
installs the server under `$FROST_SDK/managed`; an explicit configuration can
instead name the .NET executable and server DLL. The runtime requires the .NET
SDK for project loading. The Windows staged application does not bundle the
server or SDK: managed features require `FROST_SDK` and `DOTNET_ROOT` from the
installed SDK environment, or the corresponding explicit application settings.
Bootstrap uses an isolated CLI home; the workbench preserves the inherited
.NET home by default and honours `FROST_DOTNET_CLI_HOME` only when explicitly
set. Both disable telemetry, first-run certificate generation and global-tool
PATH changes. Production inherits the user?s NuGet configuration, private feeds
and caches. Tests redirect NuGet packages and profile paths to scratch. On Windows,
NuGet migration uses the actual LocalAppData Known Folder despite environment
redirection, so SDK-starting tests skip unless `NuGet/Migrations/1` already
exists there; they never create or remove the real marker. Independent licence and implementation review is required before
treating the server as redistributable.

## Development environment correction

During isolated Linux SDK setup, one .NET first-run invocation used the default
CLI home and created a development certificate, three SDK sentinels and a NuGet
migration marker in the WSL user profile. Their creation times and certificate
fingerprint were checked, and only those exact artifacts were removed. Subsequent
SDK invocations use an isolated `DOTNET_CLI_HOME`, disable telemetry and suppress
development-certificate generation and global-tool PATH changes.
