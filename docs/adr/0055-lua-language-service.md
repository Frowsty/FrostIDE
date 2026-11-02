# ADR 0055: Lua language service backend

Status: proposed, 2026-11-02.

Lua intelligence uses the existing LSP transport, document and diagnostic models with a separate
supervised LuaLS process. Launch uses an absolute executable path and no clangd arguments. It is
refused until the workspace is trusted, because LuaLS can read project configuration and extensions.
Discovery searches a configured executable first, then explicit installation directories and
absolute PATH entries; a broken explicit setting is reported. No executable is downloaded.

Lua 5.4 is the client default and may be changed by the caller. FrostEngine mode supplies its
sandbox builtins, require path and generated API directory as client defaults. LuaLS gives the
project's `.luarc.json` priority over client settings ([LuaLS configuration](https://luals.github.io/wiki/configuration/)).
The engine owns `.frost/lua-api/frost.lua` and its shared `.luarc.json`; FrostIDE neither regenerates
nor rewrites them. The project root passed to the service must be the directory containing these
files. Server requests, including completion and navigation, use `ILspRequester`; document changes
and diagnostics use the same snapshot and encoding models as clangd. Server crashes restart with
backoff and open documents are replayed.

This is a backend slice. Application selection, settings UI, process status and editor wiring,
server packaging, plugin policy and broader Lua features remain future work. The Lua and clangd
supervisors currently have separate implementations to avoid coupling their concurrent lifecycle
changes; extracting the shared lifecycle is deferred. Integration with generated engine definitions
must be checked against the packaged server and project, in addition to scripted transport tests.
