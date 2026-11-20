// SPDX-License-Identifier: Apache-2.0
// UX-20 public data contribution API v1: isolated LSP/DAP tools and advertised server commands.
#pragma once
#include "frost/lang/language_registry.hpp"
namespace frost::lang {
inline constexpr unsigned kExtensionApiVersion = 1;
struct ExtensionCommand {
    std::string id, title, serverId, command;
    Json arguments = Json::array();
};
struct ExtensionDebugAdapter {
    std::string id, title, adapterId;
    LanguageServerDefinition tool;
    Json launch;
};
struct ExtensionContribution {
    std::string id, name, version;
    bool enabled{};
    Json languageServers = Json::array();
    std::vector<ExtensionCommand> commands;
    std::vector<ExtensionDebugAdapter> debugAdapters;
};
class ExtensionCatalog {
public:
    // Pure bounded API-version negotiation and capability validation. This loads data, never
    // native code/scripts into the IDE. Runtime tools still need exact pins, license and trust.
    [[nodiscard]] static Result<ExtensionCatalog> parse(std::string_view manifest);
    [[nodiscard]] const std::vector<ExtensionContribution>& contributions() const noexcept { return m_extensions; }
    // Compose enabled language contributions atomically with the user's existing local registry.
    // Selector/id collisions refuse the aggregate; neither server silently overrides the other.
    [[nodiscard]] Result<std::string> composeLanguageRegistry(std::string_view base) const;

private:
    std::vector<ExtensionContribution> m_extensions;
};
// Only exact ${file} and ${workspaceFolder} placeholders are supported, recursively in JSON
// values. Keys, executables and environment variables are not interpolated. Pure, max depth32.
[[nodiscard]] Result<Json> expandExtensionValues(const Json& value, std::string_view file, std::string_view workspace,
                                                 unsigned depth = 0);
// Capability evidence must come from the actual initialize response. A manifest alone cannot
// authorize a made-up workspace/executeCommand method or claim semantic capability.
[[nodiscard]] bool extensionCommandAdvertised(const ServerCapabilities& capabilities, std::string_view command);
} // namespace frost::lang
