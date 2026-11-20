// SPDX-License-Identifier: Apache-2.0
// CPP-8/13 one captured CDB command, immutable inputs and typed read-only tool receipts.
#pragma once
#include "semantic_snapshot.hpp"

namespace frost::clang_engine {
struct SemanticCommand {
    std::string root, configuration, identity, databaseSha256;
    std::uint64_t commandIndex = 0;
    clang::tooling::CompileCommand command;
    llvm::IntrusiveRefCntPtr<semantic::SnapshotFileSystem> files;
};
// No command is executed by the OS. The admitted driver is replaced with pinned LibTooling's
// frontend. Output/dependency actions are stripped; plugins, opaque PCH and implicit modules refuse.
std::optional<SemanticCommand> semanticCommand(const llvm::json::Object& request, llvm::StringRef resourceDirectory,
                                               std::string& error);
llvm::json::Array semanticDependencies(const SemanticCommand& command);
llvm::json::Object semanticUnitReceipt(const SemanticCommand& command, llvm::StringRef kind);
#ifdef FROST_CLANG_PLAN_TESTS
int checkSemanticCommand(std::string_view resourceDirectory);
#endif
} // namespace frost::clang_engine
