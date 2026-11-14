// SPDX-License-Identifier: Apache-2.0
// Strict bounded Content-Length framing for the engine's authenticated DAP socket.
#pragma once
#include "frost/core/error.hpp"
#include "frost/io/loopback_stream.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace frost::debug::detail {
inline constexpr std::size_t kLuaDapPayloadLimit = 1024U * 1024U;
inline constexpr std::size_t kLuaDapHeaderLimit = 8192;

// Pure validation; refuses duplicate/malformed lengths, non-ASCII headers and oversized frames.
[[nodiscard]] Result<std::size_t> luaDapContentLength(std::string_view header);
// Reads exactly one frame, leaving following events unread. The stream's deadline/stop apply.
[[nodiscard]] Result<std::string> readLuaDapFrame(io::LoopbackStream& stream);
// One-writer incremental decoder; a fault closes the caller's transport rather than resynchronizing.
class LuaDapFrameDecoder {
public:
    [[nodiscard]] Result<void> feed(std::string_view bytes);
    [[nodiscard]] std::optional<Result<std::string>> next();

private:
    std::string m_buffer;
};
} // namespace frost::debug::detail
