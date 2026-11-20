# ADR0122: Managed framework tooling and EventPipe diagnostics

Status: implementation in progress; independent source review and runtime acceptance pending.

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
