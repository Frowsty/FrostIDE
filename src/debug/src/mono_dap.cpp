// SPDX-License-Identifier: Apache-2.0
// Bridge an admitted Mono runtime session to debugger adapter requests.
#include "frost/debug/mono_dap.hpp"

#include "frost/debug/mono_protocol_session.hpp"
#include "frost/debug/mono_responses.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace frost::debug::mono {
namespace {
using namespace std::chrono_literals;
struct Request {
    std::int64_t seq = 0;
    std::string command;
    rpc::Json args;
    IDebugAdapterClient::Completion done;
    std::atomic<bool> completed{false};
};
struct Bound {
    std::uint32_t event = 0, method = 0;
    SourcePoint point{};
};
struct Worker : std::enable_shared_from_this<Worker> {
    AdapterTarget target;
    DapHandlers handlers;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Request>> queue;
    std::map<std::int64_t, std::shared_ptr<Request>> requests;
    std::stop_source stop;
    bool closing = false;
    std::int64_t next = 1;
    std::unique_ptr<BinaryLoopback> stream;
    ProtocolSession session;
    std::deque<Packet> replies, events;
    std::map<std::string, std::vector<Bound>> breakpoints;
    std::map<std::int64_t, Frame> frames;
    std::map<std::uint32_t, std::uint32_t> methods;
    std::uint32_t assembly = 0, stoppedThread = 0;
    std::int64_t nextFrame = 1;
    bool stepPending = false;
    bool admitted = false;
    bool detached = false;
    bool protocolHealthy = true;

    void finish(const std::shared_ptr<Request>& request, Result<rpc::Json> result) {
        if (request->completed.exchange(true))
            return;
        {
            std::lock_guard lock(mutex);
            requests.erase(request->seq);
        }
        if (request->done)
            request->done(std::move(result));
    }
    Result<void> read() {
        std::array<std::uint8_t, 8192> bytes{};
        auto count = stream->read(bytes);
        if (!count)
            return std::unexpected(std::move(count.error()));
        auto packets = session.feed(std::span(bytes).first(*count));
        if (!packets)
            return std::unexpected(std::move(packets.error()));
        for (auto& packet : *packets) {
            if (replies.size() + events.size() >= 128)
                return fail(ErrorCode::OutOfRange, "Mono packet queue full");
            if (packet.reply)
                replies.push_back(std::move(packet));
            else if (packet.payload.size() >= 6 && packet.payload[5] != 0)
                events.push_back(std::move(packet));
        }
        return {};
    }
    Result<Packet> call(Result<std::vector<std::uint8_t>> bytes) {
        if (!bytes) {
            protocolHealthy = false;
            return std::unexpected(std::move(bytes.error()));
        }
        if (auto written = stream->write(*bytes); !written) {
            protocolHealthy = false;
            return std::unexpected(std::move(written.error()));
        }
        while (replies.empty()) {
            auto result = read();
            if (!result) {
                protocolHealthy = false;
                return std::unexpected(std::move(result.error()));
            }
        }
        auto packet = std::move(replies.front());
        replies.pop_front();
        if (packet.error) {
            protocolHealthy = false;
            return fail(ErrorCode::IoError, "Mono runtime rejected a request");
        }
        return packet;
    }
    Result<void> renew() { return stream->renewDeadline(std::chrono::steady_clock::now() + 10s); }
    Result<void> attach() {
        if (stream)
            return fail(ErrorCode::Busy, "Mono session already attached");
        auto opened = target.connect(std::chrono::steady_clock::now() + 10s, stop.get_token());
        if (!opened)
            return std::unexpected(std::move(opened.error()));
        stream = std::move(*opened);
        const auto handshake =
            std::span(reinterpret_cast<const std::uint8_t*>(Wire::handshake), sizeof(Wire::handshake) - 1);
        if (auto written = stream->write(handshake); !written)
            return std::unexpected(std::move(written.error()));
        while (session.state().phase() == Phase::Handshake) {
            auto r = read();
            if (!r)
                return r;
        }
        if (!call(session.version()) || !call(session.negotiate()))
            return fail(ErrorCode::IoError, "Mono negotiation failed");
        if (session.state().phase() == Phase::Running && !call(session.suspend()))
            return fail(ErrorCode::IoError, "Mono suspend failed");
        auto domainPacket = call(session.rootDomain());
        if (!domainPacket)
            return std::unexpected(std::move(domainPacket.error()));
        auto domain = decodeId(domainPacket->payload);
        if (!domain)
            return std::unexpected(std::move(domain.error()));
        auto assembliesPacket = call(session.domainAssemblies(*domain));
        if (!assembliesPacket)
            return std::unexpected(std::move(assembliesPacket.error()));
        auto assemblies = decodeIds(assembliesPacket->payload);
        if (!assemblies)
            return std::unexpected(std::move(assemblies.error()));
        for (auto id : *assemblies) {
            auto location = call(session.metadata(MetadataQuery::AssemblyLocation, id));
            if (!location)
                return std::unexpected(std::move(location.error()));
            auto name = decodeString(location->payload);
            if (!name)
                return std::unexpected(std::move(name.error()));
            if (*name == target.module->assemblyPath) {
                if (assembly)
                    return fail(ErrorCode::Corrupt, "Mono module location is ambiguous");
                assembly = id;
            }
        }
        if (!assembly)
            return fail(ErrorCode::NotFound, "Mono module is not loaded at the verified path");
        auto image = call(session.metadata(MetadataQuery::AssemblyImage, assembly));
        auto pdb = call(session.metadata(MetadataQuery::AssemblyPdb, assembly));
        if (!image || !pdb)
            return fail(ErrorCode::IoError, "Mono module bytes unavailable");
        const auto bytes = [](const std::string& s) {
            return std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
        };
        if (!compareLoadedBlob(image->payload, bytes(target.module->image)) ||
            !compareLoadedBlob(pdb->payload, bytes(target.module->pdb)))
            return fail(ErrorCode::Corrupt, "Mono loaded module differs from verified snapshot");
        std::set<std::uint32_t> resolvedTokens;
        for (const auto& point : target.module->points) {
            if (!resolvedTokens.insert(point.methodToken).second)
                continue;
            auto reply = call(session.methodFromToken(assembly, point.methodToken));
            if (!reply)
                return std::unexpected(std::move(reply.error()));
            auto method = decodeId(reply->payload);
            if (!method)
                return std::unexpected(std::move(method.error()));
            if (!methods.emplace(*method, point.methodToken).second)
                return fail(ErrorCode::Corrupt, "Mono method identity is ambiguous");
        }
        return {};
    }
    Result<void> clearPath(const std::string& path) {
        auto found = breakpoints.find(path);
        if (found == breakpoints.end())
            return {};
        while (!found->second.empty()) {
            if (!call(session.clearBreakpoint(found->second.back().event)))
                return fail(ErrorCode::IoError, "could not clear Mono breakpoint");
            found->second.pop_back();
        }
        breakpoints.erase(found);
        return {};
    }
    Result<rpc::Json> setBreakpoints(const rpc::Json& args) {
        if (!stream || session.state().phase() != Phase::Suspended)
            return fail(ErrorCode::Busy, "Mono breakpoints require a suspended session");
        const auto* source = args.find("source");
        const std::string path(source ? source->stringAt("path") : "");
        const auto* requested = args.find("breakpoints");
        if (!requested || !requested->isArray() || requested->size() > 1024)
            return fail(ErrorCode::InvalidArgument, "invalid Mono breakpoints");
        if (auto cleared = clearPath(path); !cleared)
            return std::unexpected(std::move(cleared.error()));
        std::vector<rpc::Json> results;
        for (const auto& item : requested->items()) {
            auto line = item.integerAt("line");
            const SourcePoint* chosen = nullptr;
            for (const auto& point : target.module->points) {
                if (point.sourceIndex < target.module->sources.size() &&
                    target.module->sources[point.sourceIndex] == path && line && point.line == *line) {
                    if (chosen) {
                        chosen = nullptr;
                        break;
                    }
                    chosen = &point;
                }
            }
            if (!chosen) {
                results.push_back(
                    rpc::Json::object().set("verified", false).set("message", "No unique verified source location"));
                continue;
            }
            auto methodReply = call(session.methodFromToken(assembly, chosen->methodToken));
            if (!methodReply)
                return std::unexpected(std::move(methodReply.error()));
            auto method = decodeId(methodReply->payload);
            if (!method)
                return std::unexpected(std::move(method.error()));
            auto request = call(session.breakpoint(*method, static_cast<std::uint64_t>(chosen->ilOffset)));
            if (!request)
                return std::unexpected(std::move(request.error()));
            auto id = decodeId(request->payload);
            if (!id)
                return std::unexpected(std::move(id.error()));
            breakpoints[path].push_back({*id, *method, *chosen});
            results.push_back(rpc::Json::object()
                                  .set("id", static_cast<std::int64_t>(*id))
                                  .set("verified", true)
                                  .set("line", chosen->line));
        }
        return rpc::Json::object().set("breakpoints", rpc::Json::array(std::move(results)));
    }
    Result<void> stopped(Packet packet) {
        auto event = decodeStop(packet.payload);
        if (!event)
            return std::unexpected(std::move(event.error()));
        if (event->kind == 11 && stepPending) {
            if (!call(session.clearStep()))
                return fail(ErrorCode::IoError, "could not clear Mono step");
            stepPending = false;
        }
        auto reply = call(session.frames(event->thread));
        if (!reply)
            return std::unexpected(std::move(reply.error()));
        auto found = decodeFrames(reply->payload);
        if (!found || found->empty())
            return fail(ErrorCode::Corrupt, "Mono stop has no frame");
        bool mapped = false;
        const auto method = methods.find(event->method);
        if (method != methods.end() && found->front().method == event->method &&
            (event->kind != 11 || event->rawOffset == 0 ||
             (found->front().ilOffset >= 0 &&
              event->rawOffset == static_cast<std::uint64_t>(found->front().ilOffset)))) {
            for (const auto& point : target.module->points)
                if (point.methodToken == method->second && point.ilOffset == found->front().ilOffset &&
                    point.sourceIndex < target.module->sources.size())
                    mapped = true;
        }
        if (event->kind == 10) {
            bool owned = false;
            for (const auto& [path, list] : breakpoints)
                for (const auto& bound : list)
                    if (matchesBreakpoint(*event, *found, bound.event, bound.method, bound.point.ilOffset))
                        owned = true;
            mapped = mapped && owned;
        }
        if (!mapped)
            return fail(ErrorCode::Corrupt, "Mono stop is outside verified source locations");
        stoppedThread = event->thread;
        frames.clear();
        rpc::Json body = rpc::Json::object()
                             .set("reason", event->kind == 10 ? "breakpoint" : "step")
                             .set("threadId", static_cast<std::int64_t>(event->thread))
                             .set("allThreadsStopped", true);
        if (event->kind == 10)
            body.set("hitBreakpointIds", rpc::Json::array({static_cast<std::int64_t>(event->request)}));
        if (handlers.onEvent)
            handlers.onEvent("stopped", body);
        return {};
    }
    Result<rpc::Json> stack(const rpc::Json& args) {
        auto thread = args.integerAt("threadId");
        if (!thread || *thread != stoppedThread || !stoppedThread)
            return fail(ErrorCode::InvalidArgument, "stale Mono thread");
        auto reply = call(session.frames(stoppedThread));
        if (!reply)
            return std::unexpected(std::move(reply.error()));
        auto list = decodeFrames(reply->payload);
        if (!list)
            return std::unexpected(std::move(list.error()));
        std::vector<rpc::Json> result;
        frames.clear();
        for (const auto& frame : *list) {
            const auto method = methods.find(frame.method);
            if (method == methods.end())
                continue;
            for (const auto& point : target.module->points) {
                if (point.methodToken != method->second || point.ilOffset != frame.ilOffset ||
                    point.sourceIndex >= target.module->sources.size())
                    continue;
                if (nextFrame == INT64_MAX)
                    return fail(ErrorCode::OutOfRange, "Mono frame ID exhausted");
                const auto id = nextFrame++;
                frames.emplace(id, frame);
                result.push_back(
                    rpc::Json::object()
                        .set("id", id)
                        .set("name", "Managed method")
                        .set("source", rpc::Json::object().set("path", target.module->sources[point.sourceIndex]))
                        .set("line", point.line)
                        .set("column", 1)
                        .set("moduleId", target.module->moduleId));
                break;
            }
        }
        return rpc::Json::object()
            .set("totalFrames", static_cast<std::int64_t>(result.size()))
            .set("stackFrames", rpc::Json::array(std::move(result)));
    }
    Result<rpc::Json> variables(const rpc::Json& args) {
        auto frameId = args.integerAt("variablesReference");
        auto frame = frameId ? frames.find(*frameId) : frames.end();
        if (frame == frames.end())
            return fail(ErrorCode::InvalidArgument, "stale Mono frame");
        auto info = call(session.metadata(MetadataQuery::MethodLocals, frame->second.method));
        if (!info)
            return std::unexpected(std::move(info.error()));
        auto locals = decodeLocals258(info->payload);
        if (!locals)
            return std::unexpected(std::move(locals.error()));
        std::vector<rpc::Json> result;
        for (std::size_t i = 0; i < locals->size(); ++i) {
            const auto& local = (*locals)[i];
            if (frame->second.ilOffset < local.start || frame->second.ilOffset >= local.end)
                continue;
            auto value = call(session.local(stoppedThread, frame->second.id, static_cast<std::uint32_t>(i)));
            if (!value)
                return std::unexpected(std::move(value.error()));
            auto number = decodeInt32(value->payload);
            result.push_back(rpc::Json::object()
                                 .set("name", local.name)
                                 .set("value", number ? std::to_string(*number) : "<unsupported>")
                                 .set("variablesReference", 0));
        }
        return rpc::Json::object().set("variables", rpc::Json::array(std::move(result)));
    }
    Result<void> detach() {
        if (!stream)
            return {};
        if (session.state().phase() == Phase::Running && !call(session.suspend()))
            return fail(ErrorCode::IoError, "Mono suspend before detach failed");
        if (stepPending) {
            if (!call(session.clearStep()))
                return fail(ErrorCode::IoError, "Mono step clear failed");
            stepPending = false;
        }
        while (!breakpoints.empty()) {
            auto r = clearPath(breakpoints.begin()->first);
            if (!r)
                return r;
        }
        if (!call(session.dispose()))
            return fail(ErrorCode::IoError, "Mono detach failed");
        stream->close();
        detached = true;
        return {};
    }
    Result<rpc::Json> handle(const std::string& command, const rpc::Json& args) {
        if (command == "initialize")
            return rpc::Json::object().set("supportsConfigurationDoneRequest", true);
        if (command == "attach") {
            auto result = attach();
            if (!result)
                return std::unexpected(std::move(result.error()));
            admitted = true;
            return rpc::Json::object();
        }
        if (command != "disconnect" && !admitted)
            return fail(ErrorCode::Busy, "Mono session has not been admitted");
        if (command == "setBreakpoints")
            return setBreakpoints(args);
        if (command == "configurationDone" || command == "continue") {
            stoppedThread = 0;
            frames.clear();
            if (!stream || !call(session.resume()))
                return fail(ErrorCode::IoError, "Mono resume failed");
            return rpc::Json::object();
        }
        if (command == "threads") {
            if (!stoppedThread)
                return rpc::Json::object().set("threads", rpc::Json::array());
            return rpc::Json::object().set("threads",
                                           rpc::Json::array({rpc::Json::object()
                                                                 .set("id", static_cast<std::int64_t>(stoppedThread))
                                                                 .set("name", "Managed thread")}));
        }
        if (command == "stackTrace")
            return stack(args);
        if (command == "scopes") {
            auto id = args.integerAt("frameId");
            if (!id || !frames.contains(*id))
                return fail(ErrorCode::InvalidArgument, "stale Mono frame");
            return rpc::Json::object().set("scopes", rpc::Json::array({rpc::Json::object()
                                                                           .set("name", "Locals")
                                                                           .set("variablesReference", *id)
                                                                           .set("expensive", false)}));
        }
        if (command == "variables")
            return variables(args);
        if (command == "next") {
            if (!stoppedThread || !call(session.step(stoppedThread)))
                return fail(ErrorCode::IoError, "Mono step failed");
            stepPending = true;
            if (!call(session.resume()))
                return fail(ErrorCode::IoError, "Mono resume after step failed");
            stoppedThread = 0;
            frames.clear();
            return rpc::Json::object();
        }
        if (command == "disconnect") {
            auto r = detach();
            if (!r)
                return std::unexpected(std::move(r.error()));
            return rpc::Json::object();
        }
        return fail(ErrorCode::Unsupported, "Mono request is not supported by this slice");
    }
    void run() {
        Error ended(ErrorCode::Cancelled, "Mono adapter closed");
        auto nextIdleRenewal = std::chrono::steady_clock::now();
        for (;;) {
            std::shared_ptr<Request> request;
            {
                std::unique_lock lock(mutex);
                if (queue.empty() && (!stream || session.state().phase() != Phase::Running) && !closing)
                    cv.wait_for(lock, 50ms);
                if (closing)
                    break;
                if (!queue.empty()) {
                    request = std::move(queue.front());
                    queue.pop_front();
                }
            }
            if (request && !request->completed.load()) {
                if (stream && !renew()) {
                    ended = Error(ErrorCode::IoError, "Mono endpoint expired");
                    break;
                }
                auto result = handle(request->command, request->args);
                const bool attachOk = request->command == "attach" && static_cast<bool>(result);
                const bool disconnect = request->command == "disconnect";
                const bool failed = !result;
                const bool rejected =
                    failed && admitted && protocolHealthy && request->command != "attach" &&
                    request->command != "disconnect" &&
                    (result.error().code() == ErrorCode::InvalidArgument || result.error().code() == ErrorCode::Busy ||
                     result.error().code() == ErrorCode::Unsupported);
                if (failed && !rejected)
                    ended = result.error();
                finish(request, std::move(result));
                if (attachOk && handlers.onEvent)
                    handlers.onEvent("initialized", rpc::Json::object());
                if (disconnect || (failed && !rejected))
                    break;
            }
            while (!events.empty()) {
                auto packet = std::move(events.front());
                events.pop_front();
                if (auto result = stopped(std::move(packet)); !result) {
                    ended = std::move(result.error());
                    stop.request_stop();
                    break;
                }
            }
            if (stop.stop_requested())
                break;
            if (stream && session.state().phase() == Phase::Suspended &&
                std::chrono::steady_clock::now() >= nextIdleRenewal) {
                if (!renew()) {
                    ended = Error(ErrorCode::IoError, "Mono endpoint expired while suspended");
                    break;
                }
                nextIdleRenewal = std::chrono::steady_clock::now() + 1s;
            }
            if (stream && session.state().phase() == Phase::Running) {
                if (!renew()) {
                    ended = Error(ErrorCode::IoError, "Mono endpoint expired");
                    break;
                }
                auto ready = stream->pollReadable(std::chrono::steady_clock::now() + 50ms);
                if (!ready) {
                    ended = std::move(ready.error());
                    break;
                }
                if (*ready) {
                    auto r = read();
                    if (!r) {
                        ended = std::move(r.error());
                        break;
                    }
                }
            }
        }
        // Dispose only while the request stream is synchronized. A broken transport or
        // outstanding protocol request cannot safely be reused for cleanup.
        if (stream && protocolHealthy && session.state().phase() != Phase::Ended &&
            session.state().phase() != Phase::Closing) {
            if (!renew() || !detach())
                ended = Error(ErrorCode::IoError, "Mono cleanup could not be confirmed; runtime state is unknown");
        } else if (stream && !detached)
            ended = Error(ErrorCode::IoError, "Mono cleanup could not be confirmed; runtime state is unknown");
        if (stream)
            stream->close();
        session.close();
        std::vector<std::shared_ptr<Request>> remaining;
        {
            std::lock_guard lock(mutex);
            closing = true;
            for (auto& [seq, request] : requests) {
                (void)seq;
                remaining.push_back(request);
            }
        }
        for (auto& request : remaining)
            finish(request, std::unexpected(ended));
        if (handlers.onClosed)
            handlers.onClosed(ended);
    }
};
class Client final : public IDebugAdapterClient {
public:
    Client(AdapterTarget target, DapHandlers handlers) : state(std::make_shared<Worker>()) {
        state->target = std::move(target);
        state->handlers = std::move(handlers);
        thread = std::thread([s = state] { s->run(); });
    }
    ~Client() override {
        close();
        if (thread.joinable())
            thread.join();
    }
    std::int64_t request(std::string command, rpc::Json args, Completion done) override {
        std::unique_lock lock(state->mutex);
        if (state->closing || state->requests.size() >= 128 || state->next >= INT64_MAX) {
            lock.unlock();
            if (done)
                done(fail(ErrorCode::IoError, "Mono adapter is closed or busy"));
            return 0;
        }
        auto request = std::make_shared<Request>();
        request->seq = state->next++;
        request->command = std::move(command);
        request->args = std::move(args);
        request->done = std::move(done);
        state->requests.emplace(request->seq, request);
        state->queue.push_back(request);
        lock.unlock();
        state->cv.notify_one();
        return request->seq;
    }
    void cancel(std::int64_t seq) override {
        std::shared_ptr<Request> request;
        {
            std::lock_guard lock(state->mutex);
            const auto it = state->requests.find(seq);
            if (it != state->requests.end())
                request = it->second;
        }
        if (request) {
            state->stop.request_stop();
            state->finish(request, fail(ErrorCode::Cancelled, "Mono request cancelled"));
            state->cv.notify_one();
        }
    }
    void setSupportsCancel(bool) noexcept override {}
    void close() noexcept override {
        {
            std::lock_guard lock(state->mutex);
            state->closing = true;
        }
        state->stop.request_stop();
        state->cv.notify_one();
    }
    bool closed() const noexcept override {
        std::lock_guard lock(state->mutex);
        return state->closing;
    }
    std::size_t pendingRequests() const override {
        std::lock_guard lock(state->mutex);
        return state->requests.size();
    }

private:
    std::shared_ptr<Worker> state;
    std::thread thread;
};
} // namespace
AdapterFactory monoAdapterFactory(AdapterTarget target) {
    return [target = std::move(target)](DapHandlers handlers) mutable -> Result<std::unique_ptr<IDebugAdapterClient>> {
        if (!target.module || !target.connect || target.module->assemblyPath.empty() ||
            target.module->moduleId.empty() || target.module->image.empty() || target.module->pdb.empty() ||
            target.module->image.size() > Wire::maxPacket - 15 || target.module->pdb.size() > Wire::maxPacket - 15 ||
            target.module->points.size() > 100000)
            return fail(ErrorCode::InvalidArgument, "invalid verified Mono target");
        return std::unique_ptr<IDebugAdapterClient>(new Client(target, std::move(handlers)));
    };
}
} // namespace frost::debug::mono
