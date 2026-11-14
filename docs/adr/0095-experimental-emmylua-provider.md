# 0095: Explicit experimental EmmyLua Rust provider

Status: source implementation; runtime comparison deferred by user.

LuaLS remains the pinned default. An application-only Lua provider setting can select an
explicit user-installed `emmylua_ls` executable. Missing or invalid experimental paths refuse;
they do not install software or silently choose another provider. The existing supervised LSP
service negotiates actual capabilities and retains its trust/stop/document/version rules.

Research basis is upstream commit
[`f7aa28df50a849d813dd52a50e15d316ed99098c`](https://github.com/EmmyLuaLs/emmylua-analyzer-rust/tree/f7aa28df50a849d813dd52a50e15d316ed99098c).
Its [crate manifest](https://github.com/EmmyLuaLs/emmylua-analyzer-rust/blob/f7aa28df50a849d813dd52a50e15d316ed99098c/crates/emmylua_ls/Cargo.toml)
declares MIT licensing. Its [argument definitions](https://github.com/EmmyLuaLs/emmylua-analyzer-rust/blob/f7aa28df50a849d813dd52a50e15d316ed99098c/crates/emmylua_ls/src/cmd_args.rs)
and [server transport](https://github.com/EmmyLuaLs/emmylua-analyzer-rust/blob/f7aa28df50a849d813dd52a50e15d316ed99098c/crates/emmylua_ls/src/server/mod.rs)
establish stdio, a private resources directory, and disabled log-file output for this profile.

The provider owns its native `.emmyrc.json` / `.luarc.json` configuration. `.emmyrc.lua`
executes Lua; like every language-server launch it requires workspace trust. FrostIDE does
not evaluate that file, translate LuaLS-only command-line arguments or generated host overrides,
or claim equivalent runtime/version/library semantics. Host configuration UI continues to edit
LuaLS configuration; experimental users configure Emmy's files directly. Changing a watched
native config restarts the experimental service. Explicit negotiated unsupported capabilities
remain unsupported in the editor.

An opt-in source comparison test is registered only when the user supplies
`FROST_EMMYLUA_TEST_EXECUTABLE`; it uses the pinned LuaLS and that exact experimental path.
Both see the same scratch source/dirty edit and comparable hover/definition/rename requests.
It retains actual capabilities, protocol responses, diagnostics and elapsed observations, without
a hard-coded claim of equivalent diagnostics or faster indexing. It exercises real supervised
processes when eventually run; it is not a substitute for large-workspace latency/RSS, cancellation,
and semantic-result review. No downloads, Rust builds, measurements, adoption decision or runtime
acceptance were performed for this source change.
