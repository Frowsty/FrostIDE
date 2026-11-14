// SPDX-License-Identifier: Apache-2.0
// Structured Natvis projection through the documented Debugger Data Model, never parsed dx text.
// https://learn.microsoft.com/windows-hardware/drivers/debugger/data-model-cpp-objects
// https://learn.microsoft.com/windows-hardware/drivers/debugger/data-model-cpp-concepts
#include "frost/io/utf.hpp"

#include "internal.hpp"

#include <algorithm>
#include <format>
#include <limits>

namespace frost::dbgeng {

Error engineError(std::string_view operation, HRESULT status) {
    return Error(ErrorCode::IoError,
                 std::format("{} failed (HRESULT {:#010x})", operation, static_cast<ULONG>(status)));
}

Result<std::string> utf8(std::wstring_view value) { return io::fromWide(value); }

Result<void> ModelStore::initialize(IDebugClient5* client) {
    ComPtr<IHostDataModelAccess> access;
    HRESULT status = client->QueryInterface(__uuidof(IHostDataModelAccess), reinterpret_cast<void**>(access.put()));
    if (FAILED(status))
        return fail(ErrorCode::Unsupported, "System32 DbgEng has no structured data model; Natvis is unavailable");
    status = access->GetDataModel(m_manager.put(), m_host.put());
    if (FAILED(status) || !m_manager || !m_host)
        return std::unexpected(engineError("GetDataModel", status));
    status = m_host.query(m_evaluator);
    if (FAILED(status))
        return fail(ErrorCode::Unsupported, "System32 DbgEng has no data model expression evaluator");
    status = client->QueryInterface(__uuidof(IDebugSymbols3), reinterpret_cast<void**>(m_symbols.put()));
    if (FAILED(status))
        return std::unexpected(engineError("IDebugSymbols3", status));
    status = client->QueryInterface(__uuidof(IDebugSystemObjects4), reinterpret_cast<void**>(m_systems.put()));
    if (FAILED(status))
        return std::unexpected(engineError("IDebugSystemObjects4", status));
    return {};
}

void ModelStore::clear() noexcept { m_values.clear(); }
void ModelStore::shutdown() noexcept {
    clear();
    m_evaluator.reset();
    m_symbols.reset();
    m_systems.reset();
    m_host.reset();
    m_manager.reset();
}

Result<void> ModelStore::select(FrameContext frame) {
    HRESULT status = m_systems->SetCurrentThreadId(frame.thread);
    if (SUCCEEDED(status))
        status = m_symbols->SetScopeFrameByIndex(frame.frame);
    if (FAILED(status))
        return std::unexpected(engineError("select data model frame", status));
    return {};
}

Result<ComPtr<IModelObject>> ModelStore::expression(std::wstring_view text, bool extended,
                                                    ComPtr<IKeyStore>* metadata) {
    const std::wstring terminated(text);
    ComPtr<IModelObject> result;
    const HRESULT status =
        extended ? m_evaluator->EvaluateExtendedExpression(USE_CURRENT_HOST_CONTEXT, terminated.c_str(), nullptr,
                                                           result.put(), metadata ? metadata->put() : nullptr)
                 : m_evaluator->EvaluateExpression(USE_CURRENT_HOST_CONTEXT, terminated.c_str(), nullptr, result.put(),
                                                   metadata ? metadata->put() : nullptr);
    if (FAILED(status) || !result)
        return std::unexpected(engineError("data model expression", status));
    ModelObjectKind kind{};
    if (SUCCEEDED(result->GetKind(&kind)) && (kind == ObjectError || kind == ObjectNoValue)) {
        auto description = display(result.get(), nullptr);
        return fail(ErrorCode::NotFound, description ? *description : "debugger expression has no value");
    }
    return result;
}

Result<std::string> ModelStore::display(IModelObject* value, IKeyStore* metadata) {
    // The display string is used only as the DAP value cell. Child traversal always uses actual
    // keys, raw fields and iterable objects, as required by IStringDisplayableConcept's contract.
    ComPtr<IUnknown> displayConcept;
    if (SUCCEEDED(value->GetConcept(__uuidof(IStringDisplayableConcept), displayConcept.put(), nullptr)) &&
        displayConcept) {
        auto* displayable = static_cast<IStringDisplayableConcept*>(displayConcept.get());
        Bstr text;
        const HRESULT status = displayable->ToDisplayString(value, metadata, text.put());
        if (SUCCEEDED(status))
            return utf8(text.view());
        return std::unexpected(engineError("Natvis display string", status));
    }
    VARIANT intrinsic{};
    VariantInit(&intrinsic);
    HRESULT status = value->GetIntrinsicValue(&intrinsic);
    if (SUCCEEDED(status)) {
        VARIANT text{};
        VariantInit(&text);
        status = VariantChangeType(&text, &intrinsic, VARIANT_ALPHABOOL, VT_BSTR);
        Result<std::string> result =
            FAILED(status) ? Result<std::string>(std::unexpected(engineError("intrinsic display", status)))
                           : utf8(std::wstring_view(text.bstrVal, SysStringLen(text.bstrVal)));
        VariantClear(&text);
        VariantClear(&intrinsic);
        return result;
    }
    VariantClear(&intrinsic);
    return std::unexpected(engineError("object display unavailable", status));
}

Result<std::int64_t> ModelStore::retain(ComPtr<IModelObject> object, ComPtr<IKeyStore> metadata, FrameContext frame) {
    if (m_values.size() >= 8192 || m_next == std::numeric_limits<std::int64_t>::max())
        return fail(ErrorCode::OutOfRange, "debugger variable reference budget exhausted");
    const std::int64_t id = m_next++;
    m_values.emplace(id, Value{std::move(object), std::move(metadata), frame});
    return id;
}

Result<std::int64_t> ModelStore::scope(FrameContext frame, bool parameters) {
    if (auto selected = select(frame); !selected)
        return std::unexpected(std::move(selected.error()));
    // Parameters and LocalVariables are distinct actual native scopes:
    // https://learn.microsoft.com/windows-hardware/drivers/debugger/native-objects-in-javascript-extensions
    auto locals = expression(parameters ? L"@$curframe.Parameters" : L"@$curframe.LocalVariables", true);
    if (!locals)
        return std::unexpected(std::move(locals.error()));
    return retain(std::move(*locals), {}, frame);
}

Result<rpc::Json> ModelStore::variable(std::string name, ComPtr<IModelObject> object, ComPtr<IKeyStore> metadata,
                                       FrameContext frame) {
    auto text = display(object.get(), metadata.get());
    auto result = rpc::Json::object().set("name", std::move(name));
    result.set("value", text ? std::move(*text) : std::format("<unavailable: {}>", text.error().message()));
    ComPtr<IDebugHostType> type;
    if (SUCCEEDED(object->GetTypeInfo(type.put())) && type) {
        Bstr nameOfType;
        if (SUCCEEDED(type->GetName(nameOfType.put()))) {
            if (auto converted = utf8(nameOfType.view()); converted)
                result.set("type", std::move(*converted));
        }
    }
    ModelObjectKind kind{};
    const HRESULT status = object->GetKind(&kind);
    if (FAILED(status))
        return std::unexpected(engineError("model object kind", status));
    std::int64_t reference = 0;
    if (kind != ObjectIntrinsic && kind != ObjectNoValue && kind != ObjectError && kind != ObjectMethod) {
        auto retained = retain(std::move(object), std::move(metadata), frame);
        if (!retained)
            return std::unexpected(std::move(retained.error()));
        reference = *retained;
    }
    result.set("variablesReference", reference);
    return result;
}

Result<rpc::Json> ModelStore::evaluate(std::string_view text, FrameContext frame) {
    if (auto selected = select(frame); !selected)
        return std::unexpected(std::move(selected.error()));
    auto wide = io::toWide(text);
    if (!wide)
        return std::unexpected(std::move(wide.error()));
    ComPtr<IKeyStore> metadata;
    auto object = expression(*wide, false, &metadata);
    if (!object)
        return std::unexpected(std::move(object.error()));
    auto result = variable(std::string(text), std::move(*object), std::move(metadata), frame);
    if (!result)
        return std::unexpected(std::move(result.error()));
    result->set("result", result->stringAt("value"));
    return result;
}

Result<rpc::Json> ModelStore::children(std::int64_t reference, std::size_t start, std::size_t count,
                                       std::string_view filter) {
    const auto found = m_values.find(reference);
    if (found == m_values.end())
        return fail(ErrorCode::Cancelled, "variable reference belongs to an expired stop");
    if (start > 65536 || count > 4096 || (filter != "" && filter != "named" && filter != "indexed"))
        return fail(ErrorCode::InvalidArgument, "variable page exceeds the bounded adapter limits");
    if (auto selected = select(found->second.frame); !selected)
        return std::unexpected(std::move(selected.error()));
    IModelObject* const parent = found->second.object.get();
    const FrameContext frame = found->second.frame;
    const std::size_t limit = count == 0 ? 256 : count;
    rpc::Json variables = rpc::Json::array();
    std::size_t ordinal = 0;
    const auto add = [&](std::string name, ComPtr<IModelObject> object, ComPtr<IKeyStore> metadata) -> Result<void> {
        if (ordinal++ < start)
            return {};
        auto item = variable(std::move(name), std::move(object), std::move(metadata), frame);
        if (!item)
            return std::unexpected(std::move(item.error()));
        variables.push(std::move(*item));
        return {};
    };
    bool anyNamed = false;
    std::size_t visited = 0;
    if (filter != "indexed") {
        ComPtr<IKeyEnumerator> keys;
        if (SUCCEEDED(parent->EnumerateKeyValues(keys.put())) && keys) {
            while (variables.size() < limit && visited++ < 65536 + 4096) {
                Bstr name;
                ComPtr<IModelObject> child;
                ComPtr<IKeyStore> metadata;
                const HRESULT status = keys->GetNext(name.put(), child.put(), metadata.put());
                if (status == E_BOUNDS)
                    break;
                if (FAILED(status) || !child)
                    return std::unexpected(engineError("enumerate Natvis keys", status));
                ModelObjectKind kind{};
                if (SUCCEEDED(child->GetKind(&kind)) && (kind == ObjectMethod || kind == ObjectNoValue))
                    continue;
                anyNamed = true;
                auto converted = utf8(name.view());
                if (!converted)
                    return std::unexpected(std::move(converted.error()));
                if (auto added = add(std::move(*converted), std::move(child), std::move(metadata)); !added)
                    return std::unexpected(std::move(added.error()));
            }
        }
        // Unvisualized native objects expose their actual fields, not guessed offsets.
        if (!anyNamed && variables.size() < limit) {
            ComPtr<IRawEnumerator> fields;
            if (SUCCEEDED(parent->EnumerateRawValues(SymbolField, 0, fields.put())) && fields) {
                while (variables.size() < limit && visited++ < 65536 + 4096) {
                    Bstr name;
                    ComPtr<IModelObject> child;
                    SymbolKind kind{};
                    const HRESULT status = fields->GetNext(name.put(), &kind, child.put());
                    if (status == E_BOUNDS)
                        break;
                    if (FAILED(status) || !child)
                        return std::unexpected(engineError("enumerate native fields", status));
                    auto converted = utf8(name.view());
                    if (!converted)
                        return std::unexpected(std::move(converted.error()));
                    if (auto added = add(std::move(*converted), std::move(child), {}); !added)
                        return std::unexpected(std::move(added.error()));
                }
            }
        }
    }
    if (filter != "named" && variables.size() < limit) {
        ComPtr<IUnknown> iterableConcept;
        if (SUCCEEDED(parent->GetConcept(__uuidof(IIterableConcept), iterableConcept.put(), nullptr)) &&
            iterableConcept) {
            auto* iterable = static_cast<IIterableConcept*>(iterableConcept.get());
            ULONG64 dimensions = 0;
            HRESULT status = iterable->GetDefaultIndexDimensionality(parent, &dimensions);
            if (FAILED(status) || dimensions > 1)
                return fail(ErrorCode::Unsupported, "Natvis multidimensional indexing is unavailable in this slice");
            ComPtr<IModelIterator> iterator;
            status = iterable->GetIterator(parent, iterator.put());
            if (FAILED(status))
                return std::unexpected(engineError("Natvis iterator", status));
            for (std::size_t index = 0; variables.size() < limit && visited++ < 65536 + 4096; ++index) {
                ComPtr<IModelObject> child;
                ComPtr<IModelObject> indexObject;
                ComPtr<IKeyStore> metadata;
                status = iterator->GetNext(child.put(), dimensions, dimensions ? indexObject.put() : nullptr,
                                           metadata.put());
                if (status == E_BOUNDS)
                    break;
                if (FAILED(status) || !child)
                    return std::unexpected(engineError("Natvis iterator element", status));
                std::string name = std::format("[{}]", index);
                if (indexObject) {
                    if (auto indexText = display(indexObject.get(), nullptr); indexText)
                        name = std::format("[{}]", *indexText);
                }
                if (auto added = add(std::move(name), std::move(child), std::move(metadata)); !added)
                    return std::unexpected(std::move(added.error()));
            }
        }
    }
    if (visited >= 65536 + 4096 && variables.size() < limit)
        return fail(ErrorCode::OutOfRange, "debugger variable traversal budget exhausted");
    return rpc::Json::object().set("variables", std::move(variables));
}

} // namespace frost::dbgeng
