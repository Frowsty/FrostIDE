# Read-only NuGet package planner

The helper emits package-reference and central-version proposal images. FrostIDE previews and applies them
through its native transaction coordinator. It never writes a project, props file, package or cache.

Install explicitly with an existing .NET 10 SDK, choosing absolute private directories outside this checkout:

```text
python tools/managed_nuget.py --dotnet <absolute-dotnet> --output <absolute-tool-directory> --work <absolute-private-build-root>
```

Set **Package edit tool** in the NuGet window to `<absolute-tool-directory>/NuGetPlanner.dll`.
The SDK directory supplied by the IDE is the selected project's actual MSBuildToolsPath. SDK assemblies
remain in that installed SDK; the helper loads them through a scoped resolver and copies none.

Conditional/imported/dynamic ownership and version expressions refuse. Central updates show the shared
props change in preview. Restore is separate. Source tests are authored; installation and acceptance have
not been executed under the R17 source-only instruction.
