# ADR 0058: Lua workbench routing

Status: proposed, 2026-11-02.

Lua files in a project use the supervised LuaLS service from ADR 0055. The workbench opens a
`.frostproj` at its containing folder without reinterpreting it as an MSVC solution. The session
index and recents preserve that explicit project file; tab state remains keyed by its directory.
Opening an ordinary folder keeps the existing solution discovery. FrostIDE leaves
`.luarc.json` and the generated `.frost/lua-api` definitions under engine control. LuaLS executable
discovery uses the application-only `tools.luaLanguageServer.path` setting or absolute PATH entries.
The server is started only for trusted roots; opening a Lua file offers the existing trust dialog.
Trust changes are watched, and revoked trust withdraws the server and asynchronously stops it.

The language bridge chooses a server by the document URI, not the currently selected tab, while
URI-less requests use the active editor's server. Tab switches cancel outstanding controller
requests and dismiss document-bound popups. Requests retain the selected server; restarting or
closing a project cannot transfer a pending request to another server. Both services contribute
to the Live Problems list, and Lua diagnostics decorate their corresponding editor. C++ semantic
insight bindings follow the active C++ tab so their negotiated encoding and capabilities match the
server for that document. Each service receives document changes only for its own language. Asynchronous server retirement is joined before
the reactor and scheduler are destroyed.

The script editor integration is deliberately limited to LSP and project opening. Engine automation,
CLI cursor forwarding, Lua runtime debugging, engine-specific run configuration, distribution of
LuaLS and Unity integration remain separate work. No new dependency or downloaded binary is added.
