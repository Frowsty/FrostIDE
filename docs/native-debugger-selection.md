# Native debugger selection

Settings / Build, Execution, Deployment / Debugger exposes the ordinary native
backend choice. LLDB remains the default. Linux offers user-installed GDB >=16
with Python/DAP; Windows offers Frost DbgEng. Tool paths are application-only
execution settings, so project files cannot silently select another debugger.

The Workbench passes current settings into primary and child DebugControllers.
Ordinary launch and Attach to Process/configuration paths select the real backend;
explicit managed/custom adapters retain their own factory. Private engine bootstrap
continues to require pinned LLDB. Capabilities come from each adapter's initialize
response; selecting a backend does not grant capabilities or verified OS-thread IDs.

For DbgEng, the Workbench's GUI-only Natvis discovery callback supplies explicitly
admitted files. The controller serializes those files with the actual target and
uses the same DAP session lifetime as other native backends. Attach uses a positive
`processId` and Attach ownership, so ordinary disconnect detaches the host.
TrustStore revision is captured after discovery and rechecked at actual launch or
attach transmission alongside current target-root trust. Requested file lists do
not become loaded-file claims; the backend's successful visualizer-load event is
the loaded inventory authority.

DbgEng adapter staging and discovery callback wiring belong to the Windows/Natvis
integration. Its native attach operation depends on the DBG-14 backend implementation
using IDebugClient5::AttachProcess; this selection change alone does not supply that
backend operation. Unsupported source-map serialization is reported explicitly.

Tests for setting scope/platform availability, native factory routing and trust
revocation before an attach frame are authored. No builds, tests, WSL or debugger
commands were executed during this implementation-first work; runtime remains
unverified.
