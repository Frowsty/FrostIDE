# ADR 0066: Unity editor bridge and generated managed context

Status: **reviewed bridge and managed-context slice; broader Unity acceptance incomplete** (2026-11-03).

FrostIDE needs Unity script opening and C# context without deriving implementation from proprietary IDE
plugins. The original package in `integrations/unity/com.frostide.editor` uses Unity's public
`Unity.CodeEditor.IExternalCodeEditor` and `UnityEditor.Compilation.CompilationPipeline` APIs. To try it,
add its `package.json` through Unity's Package Manager (Add package from disk), then select `frost.exe`
(or `frost` on Linux) as the external editor. Windows staging includes the package under `unity/`.
`FROSTIDE_EXECUTABLE` can advertise an absolute FrostIDE executable to Unity's editor list; browsing for
the executable also works. Installing and selecting a Unity package is an explicit user action.

Unity supplies 1-based UTF-16 columns. The package passes `--column-utf16`; the receiving Workbench
converts against the live editor snapshot, including unsaved text. Existing CLI and IPC requests keep
their byte-column contract. UTF-16 IPC requests use a versioned `position` object and omit the legacy
line/column fields: an older owner rejects them instead of silently opening the wrong byte position.
The existing project-scoped native broker owns window reuse.

The bridge derives editor assemblies, source paths, assembly references, defines, unsafe/language options,
compatibility levels and compiled references from Unity's compilation metadata. It resolves package paths
through `PackageInfo` and never rewrites source, assets or `.meta` files. Explicit Unity references prevent
the host SDK from substituting a different framework. Editor and player assemblies retain their separate
Unity profiles; project references opt out of MSBuild framework negotiation because Unity accepts those
cross-profile references. Unity's default warning suppressions are preserved. Unsupported compiler
arguments and response files fail synchronization explicitly and retain the last complete generation;
analyzers, additional compiler options and other Unity build configurations remain unfinished.

MSBuild literal escaping preserves special characters in source and assembly names. Explicit Unity
reference paths containing MSBuild metacharacters use a generated design-time target after reference
resolution: this preserves the original path through the additional MSBuild unescape performed by the
compiler task, without copying or renaming the DLL. Ordinary references use normal reference resolution.
The metacharacter path behavior requires validation against both compiler arguments and the language server;
the MSBuild evaluation result alone does not establish correctness.

Generated files live under the owned `.frost/unity` directory. A versioned descriptor names a fresh
immutable generation, published only after every project is written; a failure leaves the old generation
selected. Publication uses verified control-file handles and pinned directories: descriptor-relative anchors on POSIX,
and directory handles denying write and delete sharing on Windows. Only after project trust, FrostIDE
validates regular, non-linked control files and the physical solution path before supplying csharp-ls the
solution path. The LSP workspace remains the Unity root so source files fall inside it. A changed
generation causes a trusted Workbench to restart its managed service and replay open document text.
Old generations are retained; bounded safe collection while servers may still hold them remains open.

Evidence so far: Unity 6000.6.4f1 compiled the package in a scratch project with spaces and Unicode;
actual public API calls generated source, Unity references and asmdef project references. An injected
write failure retained the old descriptor and solution, an unowned directory was left untouched, and the
original source `.meta` hash survived a source/asmdef refresh. The generated player and editor projects
matched their actual Unity compiler invocations: initially 301 and 307 exact non-project DLL paths,
then 303 and 309 after adding the two distinct metacharacter-path DLLs, plus 2 and 43 project references.
These comparisons cover the scratch project and installed Unity version.

The reviewed csharp-ls on .NET 10 returned `Vector3.zero` completion, rejected unavailable
`System.Data.Odbc` in unsaved player text, returned `EditorApplication.isPlaying` completion, navigated
from an editor assembly to a player asmdef source and diagnosed an unsaved editor error. These checks
needed no manual restore. A real Unity request invoked the built FrostIDE client, which delivered the requested
UTF-16 position to a Workbench test rig with a dirty buffer containing both an astral character and an unsaved
non-ASCII prefix. This establishes the client/broker/editor path, not a staged receiving application window. A separate
Workbench test switched generations, replayed dirty text and observed the changed diagnostics. Unity's
touched EditorPrefs values were restored and compared to their snapshots. Native Linux does not yet have
an installed Unity Editor acceptance run; user packages and full compiler-option coverage remain open.

UNITY-01 remains in progress. Live editor controls, tests, debug sessions, serialized references,
Unity-specific analysis and shader workflows are separate tracks. No Unity source, proprietary plugin or
new third-party dependency was added. This decision uses only Unity's public API documentation:
[external editor](https://docs.unity.com/en-us/engine/6000.6/script-reference/unity/codeeditor/iexternalcodeeditor),
[assemblies](https://docs.unity.com/en-us/engine/6000.6/script-reference/unityeditor/compilation/assembly),
[compiler options](https://docs.unity.com/en-us/engine/6000.6/script-reference/unityeditor/compilation/scriptcompileroptions).
