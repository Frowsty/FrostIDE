// SPDX-License-Identifier: Apache-2.0
// FEC-13: read-only LibTooling registration-argument mapping across the ADR 0083 process boundary.
#pragma once

#include <string_view>

namespace frost::clang_engine {

/*
 * CLI: frost-clang --binding-map <request.json>. No rewrite mode, replacements or output files.
 * Request schemaVersion=1, workspaceRoot/compileCommandsDirectory/translationUnit absolute paths,
 * optional compileCommandIndex (required when the TU has multiple commands), registrations array:
 * {id,qualifiedName,name,kind,source:{file:repositoryRelativeForwardSlashes,line:1based}}.
 * Each schema source must identify the actual Frost Script*Builder registration call; native
 * association comes exclusively from that call's callable argument AST, never a name search.
 *
 * Stdout JSON:
 * {schemaVersion:1,translationUnitParsed:bool,sourceInputs:[{path,sha256}],results:[{id,qualifiedName,source,
 * status:mapped|ambiguous|unavailable,reason,registrationRange?:{path,offset,length},
 * nameLiteral?:{range:{path,offset,length},decodedValue,spelling},nameLiteralReason?:string,targets:[{
 * role:callable|getter|setter,usr,qualifiedName,kind:function|method|lambda|field,
 * declaration:{path,offset,length},definition?:{path,offset,length},argumentRange:{path,offset,length}
 * }]}]}. Ranges are half-open UTF-8 file bytes, paths absolute physical paths inside workspaceRoot.
 * Ordinary native declaration/definition ranges select the actual name token. A lambda's implicit
 * operator has no spelled name: its AST-owned extent is returned and is not a verified LSP seed.
 * nameLiteral proves one exact nonmacro string token, including its physical spelling and decoded
 * value. The caller must verify captured bytes before any explicit selected Lua-surface preview.
 * Computed/concatenated/macro keys and constructor/operator APIs yield no editable name literal.
 * sourceInputs contains lowercase SHA256 of actual parsed SourceManager buffers for every file
 * physically inside workspaceRoot (including system-marked CMake forced headers), not fresh disk reads. Serialized
 * PCH/module AST inputs cannot prove original parsed bytes via a lazily loaded current buffer and make analysis
 * unavailable. Consumers revalidate this manifest and the captured CDB bytes. compilationContext reports
 * directory/source, originalArguments, final analysisArguments, normalizations and textualPchHeaders. Only proven CL
 * /Yu+matching first /FI transports become textual source: /Yu and /Fp are removed, all semantic flags/forced includes
 * stay in order and the pinned driver disables implicit cache consumption. Unproven source context fails closed.
 * docSignatureLiteral?:{range,decodedValue,spelling,decodedNameOffset,decodedNameLength} captures only
 * the actual structured Frost::ScriptDoc.Signature field's unique literal. Its trimmed leading name
 * must equal the captured registration name; callable delimiter is '(' and property delimiter ':'.
 * Missing/computed/mutable documentation yields docSignatureLiteralReason, never a guessed edit.
 * Lambdas refer to their own call operator, never guessed functions called inside their bodies.
 * Immutable aliases and explicitly cast overloads retain the canonical declaration's USR. Multiple
 * possible registrations/targets and unsupported mutable/factory expressions have explicit status.
 *
 * Synchronous, owning, invocation-local state; no shared mutable state. Work is proportional to the
 * selected TU AST and requested registrations. Exit 0 means parsed (individual entries can remain
 * unavailable), 1 means analysis unavailable, 2 means invalid input. resourceDirectory is the pinned
 * compiler resource directory supplied by the existing helper entry point. Does not launch tools.
 */
[[nodiscard]] int runBindingMap(int argc, char* argv[], std::string_view resourceDirectory = {});

} // namespace frost::clang_engine
