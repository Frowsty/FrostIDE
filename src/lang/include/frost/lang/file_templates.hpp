// SPDX-License-Identifier: Apache-2.0
// File template sets rendered by the shared engine; no directories/files/project effects.
#pragma once
#include "frost/lang/templates.hpp"
namespace frost::lang {
enum class FileTemplateKind { Class, Source, Header, CMakeScript, Executable, LuaModule };
struct FileTemplateRequest { FileTemplateKind kind=FileTemplateKind::Class; std::string name, author, description; };
struct GeneratedTemplateFile { std::string name, contents; };
// C++ qualified identifiers only (ASCII namespace components), no paths. Existing parent and
// endpoint absence, encoding, trust and project membership belong to one caller transaction.
[[nodiscard]] Result<std::vector<GeneratedTemplateFile>> renderFileTemplate(const FileTemplateRequest&);
}
