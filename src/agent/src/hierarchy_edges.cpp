// SPDX-License-Identifier: Apache-2.0
// Bounded C2 hierarchy expansion uses actual LSP edge methods, never guessed relationships.
#include "frost/agent/ide_queries.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
namespace frost::agent {
namespace {
using rpc::Json;
bool rangeShape(const Json& range) {
    const auto* start = range.find("start");
    const auto* end = range.find("end");
    if (!start || !end || !start->isObject() || !end->isObject())
        return false;
    const auto sl = start->unsignedAt("line"), sc = start->unsignedAt("character"), el = end->unsignedAt("line"),
               ec = end->unsignedAt("character");
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    return sl && sc && el && ec && *sl <= maximum && *sc <= maximum && *el <= maximum && *ec <= maximum &&
           std::pair{*sl, *sc} <= std::pair{*el, *ec};
}
bool itemShape(const Json& item) {
    const auto* uri = item.find("uri");
    const auto* range = item.find("range");
    const auto* selection = item.find("selectionRange");
    return item.isObject() && item.find("name") && item.find("name")->isString() && uri && uri->isString() &&
           !uri->asString()->empty() && range && rangeShape(*range) && selection && rangeShape(*selection);
}
Json publicData(const Json& value) {
    if (value.isArray()) {
        auto result = Json::array();
        for (const auto& item : value.items())
            result.push(publicData(item));
        return result;
    }
    if (!value.isObject())
        return value;
    auto result = Json::object();
    for (std::size_t i = 0; i < value.keys().size(); ++i)
        if (value.keys()[i] != "data")
            result.set(value.keys()[i], publicData(value.items()[i]));
    return result;
}
struct Query final : std::enable_shared_from_this<Query> {
    bool types = false, completed = false;
    IdeQueryRequest request;
    IdeResourceAdmission admitted;
    IdeQueryDone done;
    std::vector<Json> roots;
    std::size_t next = 0, retainedBytes = 2;
    Json root, first, output = Json::array();
    void finish(Result<Json> result) {
        if (completed)
            return;
        completed = true;
        auto completion = std::move(done);
        completion(std::move(result));
    }
    void prepared(Result<Json> value) {
        if (!value) {
            finish(std::unexpected(value.error()));
            return;
        }
        if (value->isNull()) {
            finish(Json::array());
            return;
        }
        if (!value->isArray()) {
            finish(fail(ErrorCode::Corrupt, "hierarchy preparation did not return an item array"));
            return;
        }
        if (value->size() > 8 || value->dump().size() > 1024 * 1024) {
            finish(fail(ErrorCode::OutOfRange, "hierarchy root result exceeds bounds"));
            return;
        }
        for (const auto& item : value->items()) {
            if (!itemShape(item)) {
                finish(fail(ErrorCode::Corrupt, "malformed hierarchy root item"));
                return;
            }
            if (admitted(item.stringAt("uri")))
                roots.push_back(item);
        }
        advance();
    }
    bool append(Json record) {
        const auto bytes = record.dump().size() + 1;
        if (retainedBytes + bytes > 1024 * 1024) {
            finish(fail(ErrorCode::OutOfRange, "hierarchy retained result exceeds bound"));
            return false;
        }
        retainedBytes += bytes;
        output.push(std::move(record));
        return true;
    }
    void advance() {
        if (completed)
            return;
        if (next == roots.size()) {
            finish(scopedLocations(output, admitted).value_or(Json::array()));
            return;
        }
        root = roots[next++];
        if (!admitted(root.stringAt("uri"))) {
            finish(fail(ErrorCode::PermissionDenied, "hierarchy root authorization changed"));
            return;
        }
        auto self = shared_from_this();
        request(types ? "typeHierarchy/supertypes" : "callHierarchy/incomingCalls", Json::object().set("item", root),
                [self](Result<Json> value) { self->edges(std::move(value), false); });
    }
    void edges(Result<Json> value, bool second) {
        if (completed)
            return;
        if (!value) {
            finish(std::unexpected(value.error()));
            return;
        }
        if (value->isNull())
            *value = Json::array();
        if (!value->isArray()) {
            finish(fail(ErrorCode::Corrupt, "hierarchy edge provider did not return an array"));
            return;
        }
        if (value->size() > 128 || value->dump().size() > 1024 * 1024) {
            finish(fail(ErrorCode::OutOfRange, "hierarchy edge result exceeds bounds"));
            return;
        }
        auto authorized = Json::array();
        for (const auto& edge : value->items()) {
            const auto* node = types ? &edge : edge.find(second ? "to" : "from");
            if (!node || !itemShape(*node)) {
                finish(fail(ErrorCode::Corrupt, "malformed hierarchy edge item"));
                return;
            }
            if (!types) {
                const auto* ranges = edge.find("fromRanges");
                if (!ranges || !ranges->isArray()) {
                    finish(fail(ErrorCode::Corrupt, "call edge is missing source ranges"));
                    return;
                }
                for (const auto& range : ranges->items())
                    if (!rangeShape(range)) {
                        finish(fail(ErrorCode::Corrupt, "malformed call edge source range"));
                        return;
                    }
            }
            if (admitted(node->stringAt("uri")))
                authorized.push(publicData(edge));
        }
        if (!second) {
            first = std::move(authorized);
            if (!admitted(root.stringAt("uri"))) {
                finish(fail(ErrorCode::PermissionDenied, "hierarchy root authorization changed"));
                return;
            }
            auto self = shared_from_this();
            request(types ? "typeHierarchy/subtypes" : "callHierarchy/outgoingCalls", Json::object().set("item", root),
                    [self](Result<Json> result) { self->edges(std::move(result), true); });
            return;
        }
        if (!admitted(root.stringAt("uri"))) {
            finish(fail(ErrorCode::PermissionDenied, "hierarchy root authorization changed"));
            return;
        }
        if (!append(Json::object().set("relation", "root").set("item", publicData(root))))
            return;
        const auto identity = Json::object()
                                  .set("uri", std::string(root.stringAt("uri")))
                                  .set("selectionRange", *root.find("selectionRange"));
        for (const auto& edge : first.items())
            if (admitted((types ? edge : *edge.find("from")).stringAt("uri")) &&
                !append(Json::object()
                            .set("relation", types ? "supertype" : "incomingCall")
                            .set("root", identity)
                            .set(types ? "item" : "edge", edge)))
                return;
        for (const auto& edge : authorized.items())
            if (admitted((types ? edge : *edge.find("to")).stringAt("uri")) &&
                !append(Json::object()
                            .set("relation", types ? "subtype" : "outgoingCall")
                            .set("root", identity)
                            .set(types ? "item" : "edge", edge)))
                return;
        advance();
    }
};
} // namespace
std::optional<Json> scopedLocations(const Json& value, const IdeResourceAdmission& admitted) {
    if (value.isArray()) {
        auto array = Json::array();
        for (const auto& item : value.items())
            if (auto scoped = scopedLocations(item, admitted))
                array.push(std::move(*scoped));
        return array;
    }
    if (!value.isObject())
        return value;
    for (const auto* key : {"uri", "targetUri"})
        if (const auto* uri = value.find(key); uri && uri->isString() && !admitted(*uri->asString()))
            return std::nullopt;

    auto object = Json::object();
    for (std::size_t i = 0; i < value.keys().size(); ++i) {
        const auto& key = value.keys()[i];
        if (key == "data")
            continue;
        if (auto scoped = scopedLocations(value.items()[i], admitted))
            object.set(key, std::move(*scoped));
        else if (key == "location")
            return std::nullopt;
    }
    return object;
}
void hierarchyEdges(bool types, Json params, IdeQueryRequest request, IdeResourceAdmission admitted,
                    IdeQueryDone done) {
    if (!params.isObject() || !request || !admitted || !done) {
        if (done)
            done(
                fail(ErrorCode::InvalidArgument, "hierarchy query requires owner-thread request/admission/completion"));
        return;
    }
    auto state = std::make_shared<Query>();
    state->types = types;
    state->request = std::move(request);
    state->admitted = std::move(admitted);
    state->done = std::move(done);
    state->request(types ? "textDocument/prepareTypeHierarchy" : "textDocument/prepareCallHierarchy", std::move(params),
                   [state](Result<Json> result) { state->prepared(std::move(result)); });
}
} // namespace frost::agent
