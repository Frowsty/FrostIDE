<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0068: Lua editing and position encoding

## Decision

Formatting requests use the active editor's effective EditorConfig settings. For spaces, LSP tabSize carries the indentation width; for tabs, it carries the tab width. Both effective widths and the spaces setting are retained independently and checked again before applying a response. A formatting response is discarded if the document version, active file, or formatting options changed while it was pending. Document and selected-range formatting remain server-provided edits and use the editor's undo history.

LuaLS completion snippets may request parameter hints with the known `editor.action.triggerParameterHints` command. FrostIDE opens its own signature help after inserting the snippet and selecting its first placeholder; arbitrary commands returned by servers are not executed.

LuaLS 3.19.1 is offered UTF-16 alone in both the standard LSP 3.17 `general.positionEncodings` capability and the older clangd `offsetEncoding` extension. In an isolated protocol replay against the engine distribution of LuaLS 3.19.1, offering the legacy UTF-8 encoding caused correctly ordered incremental edits to produce a null signature response and an incorrect formatting end line; the identical replay with UTF-16 produced signature information and the correct range. The replay used the copied engine-distribution Windows executable SHA-256 `18945cc8facb45f8eaad7d9987fc4bc849ce1f6d9673499cf8d12405f71a9f86`; the Linux 3.19.1 test archive SHA-256 is `e9235d2d72ef55bc41cf8c99cda2ed64777682024b4bb81f5dea425060c5cbb8`. This is a server-specific interoperability policy, not a reinterpretation of returned positions. C++ and C# keep their existing encoding offers. Initialization rejects an explicit encoding that was not offered.

## Scope

Real-server tests cover keyboard-triggered signatures and completion, generated engine API annotations, range and document formatting, EditorConfig and undo/redo, generic hover and module navigation. Lua debugging, comprehensive snippet transforms, and broader language and performance acceptance remain separate work. The test uses a copied server and scratch workspace; it does not modify the engine distribution or project.
