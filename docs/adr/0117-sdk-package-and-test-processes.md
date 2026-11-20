# ADR 0117: SDK package proposals and .NET test reports

Status: source implementation and authored regressions; execution deliberately deferred in R17.

NuGet search, exact-version enumeration, installed references, update, vulnerability and deprecation
queries run the configured user .NET SDK. Search uses `dotnet package search --format json`; package
inspection uses .NET 10 `dotnet package list --project ... --format json --output-version 1 --no-restore`.
The project working directory preserves NuGet's configuration hierarchy and credential-provider behavior.
The IDE never saves feed credentials. Feed problems remain visible alongside partial results. The SDK's
reported project identity and per-framework resolved versions are retained. Vulnerability audit sources
come from NuGet configuration, not an invented safe/unsafe flag. Restore is a separate user action and
uses the selected configuration/framework. Installed rows reflect the current restored assets; switching
configuration does not silently restore them.

The ordinary NuGet action, project context menu and NuGet tool window use the real provider. The user
selects an explicit project, configuration and TFM. Trusted Save All precedes SDK actions; workspace,
project-generation, SDK and trust revisions fence queued results. Cancellation owns the entire process
tree. Bounded import reads run on a worker; the actual evaluated items/properties are checked again after
capturing dependencies. Source-only work does not establish a measured responsiveness result.

Package install/update/remove runs the separately installed `NuGetPlanner.dll` through the SDK. The
read-only helper uses Microsoft.Build construction APIs with formatting preservation, validates the
projected reference/version through actual in-memory reevaluation, and emits exact
base64 pre/post images. It loads user-installed SDK assemblies and redistributes none. It saves no user definitions and executes no build/restore target. Imported property functions remain
trusted project code. The consumer admits only the selected project and its actual evaluated central package file,
checks physical workspace ownership and exact preimages, previews every image, and passes them plus
MSBuild input dependencies into native REF1. Dirty/foreign buffers, changed inputs and pending native
recovery refuse through the existing coordinator. Global Undo/Redo uses its retained native images.
Central version changes can affect other projects and are identified in preview. Removing a reference
leaves a shared PackageVersion declaration intact. Conditional, imported, wildcard/dynamic and
property-expression-owned versions require a scoped ownership edit and refuse here. No direct `dotnet
add/remove package` write bypass exists. Restore and project refresh follow explicit actions.

The .NET tab in the normal Unit Tests window verifies actual `IsTestProject` evaluation for the selected
inner TFM. VSTest discovery displays only the adapter's `--list-tests` output. Display strings never become
FullyQualifiedName filters. Run All produces a private per-invocation TRX report. Actual TestMethod identities
from TRX enable **Run Selected Methods** and **Run Failed Methods**; parameterized methods can rerun several
rows. Unrepresentable filter identities refuse. Failures, durations, output and navigable stack locations
come from that report. Stdout/stderr stream while the process runs; completed result rows publish after
its report arrives. A cancelled process or a missing/malformed report never produces invented passes.

Microsoft.Testing.Platform Run All uses its documented TRX extension and reports the real tool error when
that extension is absent. MTP discovery/selection, single-case debugging, adapter-origin gutter locations,
and semantic streamed result events are not implemented by this profile. They are not represented by
source scans or fake case identities. No claim is made that all LTR-8 Rider-parity acceptance is complete.

`tools/managed_nuget.py` is an explicit offline installation step. It uses only an installed .NET 10 SDK,
empty package feeds, private intermediate output outside the source checkout, and a user-selected output
folder. Configure the resulting DLL in the package tool field. Opening a workspace never builds or installs
that helper. Missing SDK/tool/extension prerequisites are shown as errors.

Authored evidence includes protocol, TRX, command-injection, untrusted ordinary-widget action tests, and a
mandatory real-SDK local-feed scenario that packs an authored package in a private root. Missing configured
SDK inputs fail that fixture; they do not skip. The actual adapter fixture also requires an explicitly
provisioned local feed containing its pinned MSTest/Test SDK packages and dependencies
(`FROST_TEST_DOTNET_TEST_FEED`); it never falls back to public feeds. The independent planner probe
builds only against the installed SDK in a private profile, validates exact no-write proposals, and
refuses conditional/expression/outside-root ownership. No build, configuration, test runner, feature-matrix tool or
WSL execution was performed for this source checkpoint.

Primary contracts: [SDK package search](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-package-search),
[SDK package list](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-package-list),
[NuGet JSON renderer](https://source.dot.net/NuGet.CommandLine.XPlat/ListPackage/ListPackageJsonRenderer.cs.html),
[VSTest CLI](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-test-vstest), and
[MTP TRX reporting](https://learn.microsoft.com/en-us/dotnet/core/testing/microsoft-testing-platform-test-reports).
