<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0082: Safe solution and project edits through MSBuild and SolutionPersistence

- Status: accepted; implemented by SLN-6 (R3).
- Date: 2026-11-09
- Plan reference: SLN-6 (PLAN.md Appendix C, R3), ADR 0006 (ProjectHost), ADR 0078 (definition of done v2).

## Context

FrostIDE used to rewrite `.vcxproj`, `.vcxproj.filters`, `.sln` and `.csproj` files with
string and regex edits. Those edits could corrupt a definition (an item matched inside a
comment, a condition or a wildcard) and were removed (SLN-1, FND-3). Since then every rename,
move or delete left the project listing the old path, and most Solution Explorer edits were
refused with "requires SLN-6". To replace Visual Studio at work, the IDE has to edit MSVC
solutions and projects. A wrong edit must be refused, never written: a user's `.vcxproj` is
not ours to break.

## Decision

1. **Project files go through the MSBuild construction API.** The ProjectHost helper
   (`tools/project-host-spike`, net472, built against the installed Visual Studio or Build Tools)
   edits `.vcxproj` and `.vcxproj.filters` with `ProjectRootElement` opened with
   `preserveFormatting`: `AddItem`, `RemoveChild` and the `Include` setter
   (`ProjectFileEdits.cs`). Text is never searched or replaced. The saved document gets the
   original's byte order mark and line endings back; mixed line endings and invalid UTF-8 are
   refused. The edit is re-read and its items compared with the intended ones before anything
   is written.
2. **Solution files go through Microsoft.VisualStudio.SolutionPersistence** (MIT,
   https://github.com/microsoft/vs-solutionpersistence). It is Visual Studio's own reader and
   writer of `.sln` and `.slnx` (`SolutionEdits.cs`: `AddProject`, `RemoveProject`; the added
   project keeps its `ProjectGuid`). SolutionPersistence renders the whole file again. A
   line-multiset guard therefore compares the result with the original: every changed `.sln`
   line must belong to the added or removed project (its GUID, its `EndProject` line, an
   emptied `ProjectDependencies` section). For `.slnx`, the XML must be the same once the
   project's element and its `BuildDependency` entries are taken out. When SolutionPersistence
   would also change anything else, the edit is refused with that line named. One example is a
   sparse configuration matrix that it fills in (`Profile|x86 = Profile|x86`). The `.sln` header
   lines and a kept project's own `Project(...)` line (its display name) are taken from the
   original.
3. **Only edits that mean exactly one thing.** An item is edited only if MSBuild evaluates it
   exactly once in every project configuration, defined directly in the project, literally,
   without a condition, and in an unconditional top-level group without Exclude, Remove or
   Update. Every configuration is evaluated before and after the edit, with imports fenced.
   Items from wildcards, imports or conditions are refused ("edit it in Visual Studio or by
   hand"). A file that is already included, a filter that the `.filters` file does not define,
   and a missing file are refused too.
4. **Durable, admitted commits.** The existing two-phase journal protocol of
   `TransactionProbe.cs` commits one project with its filters file, or one solution file. The
   helper announces `Ready` with the journal digest. FrostIDE records a private recovery marker,
   admits the edit, and only then is the file written in place. `frost/project/project_edit.hpp`
   builds the request and runs it synchronously (tests and tools). The workbench runs it
   asynchronously, one edit at a time, with a queue (`workbench_project_edit.cpp`).
5. **Not redistributed.** `Microsoft.Build`, `Microsoft.Build.Framework` and
   `Microsoft.VisualStudio.SolutionPersistence` are resolved from the user's Visual Studio
   installation (`Private="false"`; SolutionPersistence lives in `MSBuild\Current\Bin`). Nothing
   is copied into FrostIDE's output or installer, so they are not SBOM packages.
   `tools/licensecheck.py` (`check_project_host`) checks every Reference of the helper against
   its allowlist: name, SPDX licence, admitting ADR, `Private="false"` and a path under the
   installation. Package references are refused.

## Workbench behaviour

- Add Existing Source, Add New File and drops onto a project add the files in one edit. Add New
  File and drops file the item under the filter the menu or drop was on.
- Remove from Project removes any item the project lists directly (not only `ClCompile`).
- A rename, move or delete on disk renames or removes the items in every loaded C++ project that
  lists them. When the edit cannot start (no helper, the definition open in a tab, a build
  running, an interrupted edit to recover), a warning names the projects that still list the
  old path, as before.

## Limitations

- Adding or removing a project in the solution works in the helper and in
  `frost/project/project_edit.hpp`, and is tested on the synthetic solution and R2a. The
  Solution Explorer actions for it (Add Existing Project, Remove Project), solution folders and
  filter edits are not wired yet and stay refused.
- A rename keeps the item type: renaming `a.cpp` to `a.h` stays `ClCompile`.
- `.csproj` files are not edited (SDK-style projects include their folder implicitly).
- Files added from the IDE must still lie inside the project directory. The helper itself
  accepts files anywhere that exist and are not reparse points (R2a lists `..\src`).

## Consequences

Edits need a Visual Studio or Build Tools installation with MSBuild 17. Without one the action
reports "tool unavailable", the truth rule. Tests drive the real helper and MSBuild
(`project_edit_msbuild_tests` and the R2a corpus test). Their oracles are `git diff` of the
definitions, MSBuild's evaluation and MSBuild's build of the edited solution.
