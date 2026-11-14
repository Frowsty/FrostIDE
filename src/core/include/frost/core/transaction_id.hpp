// SPDX-License-Identifier: Apache-2.0
// Opaque transaction-plan identities; values carry no filesystem or recovery authority.
#pragma once

#include <array>
#include <cstddef>

namespace frost {

/*
 * Owning, comparable identity bytes with no arithmetic, implicit conversion or ID generation.
 * All-zero is the unassigned sentinel. Pure, thread-safe value operations cost O(16).
 * Tags prevent mixing transactions, operations and artifact identities; no wire format is implied.
 */
template<class Tag>
class TransactionIdentity {
public:
    constexpr TransactionIdentity() noexcept = default;
    constexpr explicit TransactionIdentity(std::array<std::byte, 16> bytes) noexcept : m_bytes(bytes) {}
    [[nodiscard]] constexpr const std::array<std::byte, 16>& bytes() const noexcept { return m_bytes; }
    [[nodiscard]] constexpr bool assigned() const noexcept {
        for (const auto byte : m_bytes)
            if (byte != std::byte{})
                return true;
        return false;
    }
    friend constexpr bool operator==(TransactionIdentity, TransactionIdentity) noexcept = default;

private:
    std::array<std::byte, 16> m_bytes{};
};

using TransactionId = TransactionIdentity<struct TransactionIdTag>;
using TransactionOperationId = TransactionIdentity<struct TransactionOperationIdTag>;
using TransactionArtifactId = TransactionIdentity<struct TransactionArtifactIdTag>;

} // namespace frost
