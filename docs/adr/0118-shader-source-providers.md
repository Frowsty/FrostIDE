# ADR 0118: Shader source profiles and authenticated semantic providers

Status: source implementation authored; independent review pending; execution deferred.

LTR-14 adds built-in lexical HLSL and ShaderLab source highlighting without adding a dependency
or pretending that the C++ tree-sitter grammar is an HLSL parser. `.hlsl`, `.hlsli`, `.fx`, `.cginc`
and `.compute` use the HLSL source profile; `.shader` uses ShaderLab with line-positioned
HLSLPROGRAM/HLSLINCLUDE/CGPROGRAM/CGINCLUDE and ENDHLSL/ENDCG boundaries. Editing comment tokens
follow these profiles. The scanner recognizes source keywords, builtin scalar/vector/matrix and
resource type spellings, comments, strings, numbers, punctuation and preprocessor directives.
It does not infer symbol types, compile errors, active variants or bindings.

The original implementation references the public Microsoft HLSL keyword/preprocessor contracts
and Unity ShaderLab block/include documentation, never private compiler or IDE sources:

- https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-appendix-keywords
- https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-appendix-preprocessor
- https://docs.unity.com/en-us/engine/6000.0/manual/materials-and-shaders/shaders/reference/sl-reference/sl-sub-shader-pass/shader-shaderlab-code-blocks
- https://docs.unity.com/en-us/engine/6000.5/manual/materials-and-shaders/shaders/writing-custom/shader-writing/sl/writing-shader-writing-shader-programs-hlsl/include-shader-program

Named limits are 4 MiB source bytes, 250,000 lexical spans, 4,096 include observations,
32 explicit include-root mappings and 64 physical include candidates. The editor scans immutable
snapshots on its existing background scheduler, cancels superseded jobs, and publishes only the
current snapshot/profile. Viewport clipping uses sorted spans; no full shader scan runs in paint.
A view without a scheduler has a separate 64 KiB synchronous fallback; larger content remains
plain text there. Incomplete scans never become compiler diagnostics. Large-file mode retains
its existing restricted behavior.

The builtin read-only source service supports hover, definition/declaration at an include and
document links. Original byte ranges, directive names and conditional-region evidence survive.
It returns physical literal-source candidates, visibly labelled **not compiler-resolved**.
Conditional activity, macro expansion, include precedence and variants remain unevaluated.
Comments, spliced comment/macro bodies, strings and malformed/continued include names cannot
supply an invented active include. Multiple candidates stay multiple, and ambiguous document
links have no chosen target. Navigation starts at the target file, never an invented symbol line.

Quoted relative paths are checked against the original source directory and retained physical
project. Literal Assets and physically present embedded Packages source paths are candidates
with explicit provenance. There is no Library/PackageCache folder scan or version guessing.
Package paths and angle includes requiring compiler/Unity metadata use only explicit typed
virtual-prefix/directory/physical-identity/provenance mappings; unavailable mappings remain
visible in hover. Linked external roots require such explicit authority. Physical project/source
identity and requested URI are rechecked; jobs retain immutable inputs and no GUI owner.

An actual configured HLSL/ShaderLab language server continues through ADR0117's pinned,
license-governed and trust-gated registry, and outranks the source fallback. Semantic completion,
reflection, diagnostics and compiler facts must come from that actual provider. This source
service does not claim a compiler installation, a successful Unity import, shader variant
acceptance, DXC support or full ShaderLab grammar coverage. R14 engine GLSL providers retain
their distinct source/session boundary when the branches are joined.

Authored regressions cover embedded profiles, comment/string/splice exclusions, conditional
include provenance, actual physical candidate paths, ambiguity, missing package mapping,
macro/refused source state, named limits/cancellation, provider precedence and versioned worker
editor highlighting. Builds, tests, shader compilation, engine/Unity execution, downloads,
installation, WSL and performance acceptance have not run under the implementation-first rule.
