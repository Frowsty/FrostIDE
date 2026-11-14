// SPDX-License-Identifier: Apache-2.0
// Forward actual DAP frames with bounded framing, fresh admission and credential redaction.
#include "lua_dap_transport.hpp"

#include "frost/rpc/framing.hpp"

#include "lua_dap_wire.hpp"

#include <algorithm>
#include <atomic>
#include <optional>

#include <sodium.h>

namespace frost::debug::detail {
namespace {
std::string redactText(std::string text, std::string_view secret) {
    if (secret.empty())
        return text;
    std::size_t offset = 0;
    while ((offset = text.find(secret, offset)) != std::string::npos) {
        text.replace(offset, secret.size(), "[redacted]");
        offset += 10;
    }
    return text;
}
rpc::Json redact(const rpc::Json& value, std::string_view secret) {
    if (const auto* text = value.asString())
        return redactText(*text, secret);
    if (value.isArray()) {
        auto result = rpc::Json::array();
        for (const auto& child : value.items())
            result.push(redact(child, secret));
        return result;
    }
    if (value.isObject()) {
        auto result = rpc::Json::object();
        for (std::size_t i = 0; i < value.size(); ++i)
            result.set(redactText(value.keys()[i], secret), redact(value.items()[i], secret));
        return result;
    }
    return value;
}
class LuaDapTransport final : public IDapTransport {
public:
    LuaDapTransport(std::unique_ptr<io::LoopbackStream> stream, LuaDebugIdentity identity, std::string credential,
                    LuaDebugAdmission admission, std::stop_token stop, std::int64_t lastServerSequence,
                    LuaDebugTransportCallbacks callbacks)
        : m_stream(std::move(stream)), m_identity(std::move(identity)), m_credential(std::move(credential)),
          m_admission(std::move(admission)), m_stop(stop), m_callbacks(std::move(callbacks)),
          m_lastServerSequence(lastServerSequence) {
        m_cancellation.emplace(stop, std::function<void()>([this] { close(); }));
    }
    ~LuaDapTransport() override {
        close();
        m_cancellation.reset(); // Join a concurrent callback before the stream and key disappear.
        sodium_memzero(m_credential.data(), m_credential.size());
    }
    Result<std::size_t> read(std::span<char> buffer) override {
        if (m_closed || m_stop.stop_requested())
            return std::size_t{0};
        auto admitted = m_admission(m_identity, "<receive>");
        if (!admitted) {
            close();
            return fail(admitted.error().code(), "Lua DAP owner admission was revoked");
        }
        if (m_readOffset == m_incoming.size()) {
            m_incoming.clear();
            m_readOffset = 0;
            auto frame = readLuaDapFrame(*m_stream);
            if (!frame) {
                close();
                return std::unexpected(frame.error());
            }
            admitted = m_admission(m_identity, "<receive>");
            if (!admitted || m_stop.stop_requested()) {
                close();
                return fail(admitted ? ErrorCode::Cancelled : admitted.error().code(),
                            "Lua DAP owner admission was revoked");
            }
            auto value = rpc::parseJson(*frame);
            if (!value || !value->isObject()) {
                close();
                return fail(ErrorCode::Corrupt, "invalid Lua DAP message");
            }
            const auto seq = value->integerAt("seq");
            const auto type = value->stringAt("type");
            if (!seq || *seq <= m_lastServerSequence || (type != "response" && type != "event" && type != "request")) {
                close();
                return fail(ErrorCode::Corrupt, "invalid Lua DAP message identity");
            }
            m_lastServerSequence = *seq;
            const auto attach = m_attachSequence.load();
            if (attach > 0 && type == "response" && value->integerAt("request_seq") == attach &&
                value->stringAt("command") == "attach") {
                attached(value->boolAt("success") == true
                             ? Result<void>{}
                             : Result<void>{fail(ErrorCode::PermissionDenied, "Lua peer refused attach")});
            }
            const auto disconnect = m_disconnectSequence.load();
            if (disconnect > 0 && type == "response" && value->integerAt("request_seq") == disconnect &&
                value->stringAt("command") == "disconnect") {
                detached(value->boolAt("success") == true
                             ? Result<void>{}
                             : Result<void>{fail(ErrorCode::PermissionDenied, "Lua peer refused detach")});
            }
            m_incoming = rpc::encodeFrame(redact(*value, m_credential).dump());
        }
        if (m_closed || m_stop.stop_requested())
            return std::size_t{0};
        const auto count = std::min(buffer.size(), m_incoming.size() - m_readOffset);
        std::copy_n(m_incoming.data() + m_readOffset, count, buffer.data());
        m_readOffset += count;
        return count;
    }
    Result<void> write(std::string_view bytes) override {
        if (m_closed || m_stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua DAP owner was revoked");
        if (auto fed = m_outgoing.feed(bytes); !fed) {
            close();
            return fed;
        }
        while (auto frame = m_outgoing.next()) {
            if (!*frame) {
                close();
                return std::unexpected(frame->error());
            }
            auto message = rpc::parseJson(**frame);
            if (!message || !message->isObject()) {
                close();
                return fail(ErrorCode::Corrupt, "invalid outgoing Lua DAP message");
            }
            const auto sequence = message->integerAt("seq");
            const auto type = message->stringAt("type");
            const auto command = message->stringAt("command");
            if (!sequence || *sequence <= m_lastClientSequence || command.empty() ||
                (type != "request" && type != "response") || command.starts_with("frost/auth")) {
                close();
                return fail(ErrorCode::InvalidArgument, "invalid outgoing Lua DAP identity or authentication replay");
            }
            auto admitted = m_admission(m_identity, command);
            if (!admitted || m_stop.stop_requested()) {
                close();
                return fail(admitted ? ErrorCode::Cancelled : admitted.error().code(),
                            "Lua DAP owner admission was revoked");
            }
            // Admission and the revocation token are checked at transmission, not cached at setup.
            if (command == "attach" && type == "request")
                m_attachSequence.store(*sequence);
            if (command == "disconnect" && type == "request")
                m_disconnectSequence.store(*sequence);
            auto written = m_stream->write(rpc::encodeFrame(**frame));
            if (!written) {
                close();
                return written;
            }
            m_lastClientSequence = *sequence;
        }
        return {};
    }
    void close() noexcept override {
        m_closed = true;
        m_stream->close();
        if (m_attachSequence.load() > 0)
            attached(fail(ErrorCode::IoError, "Lua transport closed before attach acknowledgement"));
        if (m_disconnectSequence.load() > 0)
            detached(fail(ErrorCode::IoError, "Lua transport closed before detach acknowledgement"));
    }

private:
    void attached(Result<void> result) noexcept {
        if (!m_attachReported.exchange(true) && m_callbacks.onAttached)
            m_callbacks.onAttached(std::move(result));
    }
    void detached(Result<void> result) noexcept {
        if (!m_detachReported.exchange(true) && m_callbacks.onDetached)
            m_callbacks.onDetached(std::move(result));
    }
    std::unique_ptr<io::LoopbackStream> m_stream;
    const LuaDebugIdentity m_identity;
    std::string m_credential;
    const LuaDebugAdmission m_admission;
    const std::stop_token m_stop;
    const LuaDebugTransportCallbacks m_callbacks;
    std::atomic<bool> m_closed{false}, m_attachReported{false}, m_detachReported{false};
    std::atomic<std::int64_t> m_attachSequence{0}, m_disconnectSequence{0};
    std::string m_incoming;
    std::size_t m_readOffset = 0;
    std::int64_t m_lastServerSequence = 0, m_lastClientSequence = 2;
    LuaDapFrameDecoder m_outgoing;
    std::optional<std::stop_callback<std::function<void()>>> m_cancellation;
};
} // namespace
std::unique_ptr<IDapTransport> makeLuaDapTransport(std::unique_ptr<io::LoopbackStream> stream,
                                                   LuaDebugIdentity identity, std::string credential,
                                                   LuaDebugAdmission admission, std::stop_token stop,
                                                   std::int64_t lastServerSequence,
                                                   LuaDebugTransportCallbacks callbacks) {
    return std::make_unique<LuaDapTransport>(std::move(stream), std::move(identity), std::move(credential),
                                             std::move(admission), stop, lastServerSequence, std::move(callbacks));
}
} // namespace frost::debug::detail
