// SPDX-License-Identifier: Apache-2.0
// Bounded physical-project reader for actual engine-reported schema declaration navigation.
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/text/snapshot.hpp"
namespace frost::app {
// Worker-safe as fs. Reads only a physical descendant, at most 2MiB, with lossless text
// decoding and retained before/after root and file identities. No process or file mutation.
[[nodiscard]] Result<text::Snapshot> readEngineSchemaDeclaration(io::IFileSystem&, std::string_view root,
                                                                 std::string_view path);
} // namespace frost::app
