# ADR0122: Managed framework tooling and EventPipe diagnostics

Status: core and UI implementation independently source-reviewed; runtime acceptance pending.

LTR19 adds framework-specific workflows to the evaluated R17 SDK model. Evaluation, installed
language servers and diagnostic tools remain the evidence providers. File extensions alone never
prove an installed workload, runtime, designer, Razor semantic service or framework capability.
No new dependency or automatic tool installation is introduced.

The first slice attaches explicitly selected installed dotnet-trace, dotnet-counters and
dotnet-gcdump executables through IProcessLauncher. The selected exact version is checked and
collect help must advertise bounded attachment. Trace additionally requires its actual named
managed profiles. Older incompatible tools return Unsupported. The app must retain an authenticated
PID lifetime/image admission (coreClrProcessAdmission), selected project/configuration and trust
lease. A joined watcher revokes the owned collector when that admission changes. It never owns or
terminates the attached target. Cancellation discards publication; partial artifacts remain on disk.

Each capture uses a new directory, bounded duration/output and observed tool/artifact hashes.
These hashes identify observed files; they do not establish source-to-binary provenance. Real
trace topN output describes sampled thread time, not CPU utilization. Actual counters CSV is
preserved including provider names, units, timestamps and missing measurements; no synthetic
metrics or percentages are substituted. GC dump requires explicit consent because a full generation2
GC can suspend the target. Actual gcdump report is displayed, not a fabricated heap graph.
EventPipe permissions, target diagnostic settings and matching Linux TMPDIR remain required.
No collect-linux/root escalation, startup tracing, automatic attachment, container transport,
full memory dump analysis or designer is claimed by this adapter.

Command contracts: [trace](https://learn.microsoft.com/en-us/dotnet/core/diagnostics/dotnet-trace),
[counters](https://learn.microsoft.com/en-us/dotnet/core/diagnostics/dotnet-counters),
[gcdump](https://learn.microsoft.com/en-us/dotnet/core/diagnostics/dotnet-gcdump).
The counters duration contract is also explicit in the upstream MIT-licensed
[CLI option declaration](https://github.com/dotnet/diagnostics/blob/main/src/Tools/dotnet-counters/Program.cs).
No upstream implementation is copied.

Deferred acceptance uses explicit installed tools and a locally built allocation workload,
compares reports with independent tool invocations, validates artifact receipts and verifies the
target remains alive after attachment/refusal. This authored source is not executed evidence.
Framework-specific UI/profile/registry integration follows separately; this core slice alone does
not complete LTR19.


The framework/profile increment requests actual SDK properties (Web SDK, WPF/Forms/MAUI,
platform, Razor) and evaluated RazorComponent/RazorGenerate/Page/ApplicationDefinition/
AvaloniaXaml/MauiXaml/Content items with metadata. Missing evaluated lists remain unknown.
The existing project tree exposes these actual files; extension-based workload inference is
forbidden. Exe and WinExe DLL entry points reuse the evaluated build/run target pipeline.
Installed workloads and platform compatibility still determine whether an actual build succeeds.

Captured Properties/launchSettings.json Project profiles supply arguments, environment,
applicationUrl and working directory to generated Run configurations. Their exact bytes and
presence are revalidated before launch, along with SDK import receipts and the current selected
configuration lease. IISExpress/Docker/other command providers and unknown execution overrides
remain visible and unavailable. No implicit environment expansion or hot reload is substituted.
The profile browser button opens only explicit HTTP(S) URLs for an exact running profile, after
admission. It is user-invoked; it makes no server-readiness claim and attaches no JS debugger.
Managed debug's dedicated verified-PDB flow remains separate; profile-selected Debug is not
claimed by this increment.

Razor/Blazor and XAML semantic services/designers, browser/JS debugging, MAUI device deployment,
framework-specific inspections and specialized F#/VB tooling are not implemented by listing SDK
items. Semantic requests can use an explicitly installed pinned/licensed generic registry provider;
there is no bundled Razor/XAML/F#/VB provider and no claim that ordinary C# Roslyn handles them.
This slice adds evaluated framework visibility, supported Project profile Run/browser and owned
managed-run diagnostics; the remaining LTR19 framework semantics are still open.
