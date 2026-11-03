// SPDX-License-Identifier: Apache-2.0
// Authentication handshake for the experimental FrostEngine Lua debug endpoint.
#include "frost/debug/lua_debug_endpoint.hpp"

#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include <array>
#include <utility>

namespace frost::debug {

namespace {
Result<rpc::Json> readHandshake(io::LoopbackStream& stream) {
    rpc::FrameDecoder decoder(1024);
    // One byte at a time preserves any following event for the eventual protocol reader.
    for (std::size_t count = 0; count < 5120; ++count) {
        std::array<char, 1> byte{};
        auto read = stream.read(byte);
        if (!read)
            return std::unexpected(std::move(read.error()));
        if (*read == 0)
            return fail(ErrorCode::IoError, "Lua debug endpoint closed during handshake");
        decoder.feed(std::string_view(byte.data(), *read));
        if (auto frame = decoder.next()) {
            if (!*frame)
                return std::unexpected(std::move(frame->error()));
            return rpc::parseJson(**frame);
        }
    }
    return fail(ErrorCode::OutOfRange, "Lua debug handshake response exceeds limit");
}
Result<LuaDebugPlayConnection> connectEndpoint(std::uint16_t port, std::string_view token,
                                               io::LoopbackStream::Deadline deadline, bool armPause, bool reservePlay,
                                               std::stop_token stop, bool requireGeneration) {
    if (token.size() < 32 || token.size() > 1024)
        return fail(ErrorCode::InvalidArgument, "invalid Lua debug token length");
    auto stream = io::LoopbackStream::connect(port, deadline, stop);
    if (!stream)
        return std::unexpected(std::move(stream.error()));
    const std::string request = rpc::Json::object().set("token", token).dump();
    if (auto sent = (*stream)->write(rpc::encodeFrame(request)); !sent)
        return std::unexpected(std::move(sent.error()));
    auto reply = readHandshake(**stream);
    if (!reply)
        return std::unexpected(std::move(reply.error()));
    if (reply->boolAt("authenticated") != true)
        return fail(ErrorCode::PermissionDenied, "Lua debug endpoint rejected authentication");
    LuaDebugPlayConnection result;
    const auto generation = reply->integerAt("connectionGeneration");
    if (requireGeneration) {
        if (!generation || *generation <= 0)
            return fail(ErrorCode::Corrupt, "Lua debug endpoint omitted its connection generation");
        result.connectionGeneration = static_cast<std::uint64_t>(*generation);
    }
    if (reservePlay) {
        if (auto sent = (*stream)->write(rpc::encodeFrame(R"({"command":"reservePlay","requestId":1})")); !sent)
            return std::unexpected(std::move(sent.error()));
        reply = readHandshake(**stream);
        if (!reply)
            return std::unexpected(std::move(reply.error()));
        const auto operation = reply->integerAt("operationId");
        if (reply->integerAt("requestId") != 1 || reply->boolAt("success") != true || !operation || *operation <= 0)
            return fail(ErrorCode::IoError, "Lua debug endpoint did not reserve play");
        result.operationId = static_cast<std::uint64_t>(*operation);
    }
    if (armPause) {
        const std::int64_t id = reservePlay ? 2 : 1;
        auto arm = rpc::Json::object().set("command", reservePlay ? "armPlay" : "armPause").set("requestId", id);
        if (reservePlay)
            arm.set("operationId", static_cast<std::int64_t>(result.operationId));
        if (auto sent = (*stream)->write(rpc::encodeFrame(arm.dump())); !sent)
            return std::unexpected(std::move(sent.error()));
        reply = readHandshake(**stream);
        if (!reply)
            return std::unexpected(std::move(reply.error()));
        if (reply->integerAt("requestId") != id || reply->boolAt("success") != true)
            return fail(ErrorCode::IoError, "Lua debug endpoint did not arm the initial pause");
    }
    (*stream)->beginDuplex();
    result.stream = std::move(*stream);
    result.nextWireRequest = reservePlay ? 3 : 2;
    return result;
}
} // namespace

Result<LuaDebugPlayConnection> connectLuaDebugPlayEndpoint(std::uint16_t port, std::string_view token,
                                                           io::LoopbackStream::Deadline deadline,
                                                           std::stop_token stop) {
    return connectEndpoint(port, token, deadline, true, true, stop, true);
}

Result<std::unique_ptr<io::LoopbackStream>> connectLuaDebugEndpoint(std::uint16_t port, std::string_view token,
                                                                    io::LoopbackStream::Deadline deadline,
                                                                    bool armPause, std::stop_token stop,
                                                                    std::uint64_t* connectionGeneration) {
    auto result = connectEndpoint(port, token, deadline, armPause, false, stop, connectionGeneration != nullptr);
    if (!result)
        return std::unexpected(std::move(result.error()));
    if (connectionGeneration)
        *connectionGeneration = result->connectionGeneration;
    return std::move(result->stream);
}

} // namespace frost::debug
