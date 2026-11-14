// SPDX-License-Identifier: Apache-2.0
// Test-only synchronous native effect observation; no configurable product mutation/fault payload.
#pragma once
#include "frost/io/file_transaction_replace.hpp"
namespace frost::io::detail {
enum class ReplacementBoundary {
    MetadataCaptured,
    ByteImagesPrepared,
    OriginalRetired,
    AfterInstalled,
    AfterRetired,
    BeforeRestored
};
using ReplacementObserver = void (*)(void*, ReplacementBoundary, const FileReplacementReceipt&);
// Calling thread only. Test owns context through all effects; observer must not throw or reenter.
// Reset before context destruction. Production leaves the observer null.
void setReplacementObserver(ReplacementObserver observer, void* context) noexcept;
} // namespace frost::io::detail
