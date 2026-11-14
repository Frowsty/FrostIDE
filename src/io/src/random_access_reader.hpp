// SPDX-License-Identifier: Apache-2.0
// Native retained-handle reader factory, private to the IFileSystem implementations.
#pragma once
#include "frost/io/file_system.hpp"
namespace frost::io::detail {
[[nodiscard]] Result<std::unique_ptr<IRandomAccessReader>> openNativeRandomAccess(std::string_view path);
}
