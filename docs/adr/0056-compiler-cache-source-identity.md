<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0056: Compiler-cache source identity

- Status: accepted
- Date: 2026-11-02
- Plan reference: sections 6.4 and 6.7, budget B28; ADR 0049.

## Context

The shared sccache 0.19 cache can return a `/Z7` object compiled from an identical file in a
second checkout. Its embedded debug records still name the original checkout; a debugger may
then bind breakpoints to deleted or unrelated source files. Identical source text and successful
compilation do not establish debug-source identity. Disabling caching globally would discard
B28's intended build acceleration.

## Decision

For the automatically selected sccache launcher, hash the canonical source and build roots into
a build-tree identity. A generated CMake launcher combines that identity with the current
`SCCACHE_C_CUSTOM_CACHE_BUSTER` value on each compile, then invokes sccache. The user value is
read at build time, not frozen during configuration. Explicit CMake compiler launchers remain
untouched. Changing either root produces a cold cache; repeated compiles in one tree remain
cacheable. Unset and empty user busters have the same meaning. This uses sccache's documented
C/C++ cache-buster mechanism: https://github.com/mozilla/sccache/blob/v0.19.0/README.md.

## Verification and consequences

The Windows test creates two source trees and a third build tree, compiles identical `/Z7`
translation units through pinned sccache, inspects object debug records with `llvm-readobj`,
and asserts separate misses and a same-tree hit. It also tests unset, empty, and changing
user busters and an explicit user launcher using a private sccache server and cache directory.
The generated launcher starts one short CMake process per compile; its impact on B28 should be
measured in the hardening phase. vcpkg builds and explicitly supplied launchers retain their
own cache policy. Existing entries produced before this change remain on disk but cannot match
the new keys.
