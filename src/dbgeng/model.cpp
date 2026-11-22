// SPDX-License-Identifier: Apache-2.0
// Structured Natvis projection through the documented Debugger Data Model, never parsed dx text.
// https://learn.microsoft.com/windows-hardware/drivers/debugger/data-model-cpp-objects
// https://learn.microsoft.com/windows-hardware/drivers/debugger/data-model-cpp-concepts
#include "frost/io/utf.hpp"

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <optional>

namespace frost::dbgeng {

Error engineError(std::string_view operation, HRESULT status) {
    return Error(ErrorCode::IoError,
                 std::format("{} failed (HRESULT {:#010x})", operation, static_cast<ULONG>(status)));
}

Result<std::string> utf8(std::wstring_view value) { return io::fromWide(value); }

Result<void> ModelStore::initialize(IDebugClient5* client) {
    HRESULT status = client->QueryInterface(__uuidof(IDebugSymbols3), reinterpret_cast<void**>(m_symbols.put()));
    if (FAILED(status))
        return std::unexpected(engineError("IDebugSymbols3", status));
    status = client->QueryInterface(__uuidof(IDebugSystemObjects4), reinterpret_cast<void**>(m_systems.put()));
    if (FAILED(status))
        return std::unexpected(engineError("IDebugSystemObjects4", status));
    // The operating-system engine can expose native symbols without the optional data model.
    // Missing Natvis must not disable launch, stack, raw locals or native writes (ADR 0081).
    ComPtr<IHostDataModelAccess> access;
    if (SUCCEEDED(client->QueryInterface(__uuidof(IHostDataModelAccess), reinterpret_cast<void**>(access.put()))) &&
        SUCCEEDED(access->GetDataModel(m_manager.put(), m_host.put())) && m_host) {
        (void)m_host.query(m_evaluator);
    }
    return {};
}

void ModelStore::clear() noexcept {
    m_values.clear();
    m_basic.clear();
}
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
    if (!structured()) {
        if (m_basic.size() >= 8192 || m_next == std::numeric_limits<std::int64_t>::max())
            return fail(ErrorCode::OutOfRange, "Variable handle budget exhausted");
        const auto flags = parameters ? ULONG{DEBUG_SCOPE_GROUP_ARGUMENTS} : ULONG{DEBUG_SCOPE_GROUP_LOCALS};
        ComPtr<IDebugSymbolGroup2> group;
        const auto status = m_symbols->GetScopeSymbolGroup2(flags, nullptr, group.put());
        if (FAILED(status))
            return std::unexpected(engineError("native scope symbols", status));
        const auto id = m_next++;
        m_basic.emplace(id, BasicValue{frame, flags, {}, {}});
        return id;
    }
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
    Location nativeLocation;
    if (SUCCEEDED(object->GetLocation(&nativeLocation)) && nativeLocation.HostDefined == 0)
        result.set("memoryReference", std::format("{:#x}", nativeLocation.Offset));
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
    if (!structured()) {
        ComPtr<IDebugSymbolGroup2> group;
        auto status = m_symbols->GetScopeSymbolGroup2(DEBUG_SCOPE_GROUP_ALL, nullptr, group.put());
        if (FAILED(status))
            return std::unexpected(engineError("native evaluate scope", status));
        auto wide = io::toWide(text);
        if (!wide)
            return std::unexpected(std::move(wide.error()));
        ULONG index = DEBUG_ANY_ID;
        status = group->AddSymbolWide(wide->c_str(), &index);
        if (FAILED(status))
            return std::unexpected(engineError("native expression", status));
        auto result =
            basicVariable(group.get(), index, BasicValue{frame, DEBUG_SCOPE_GROUP_ALL, std::string(text), {}});
        if (result)
            result->set("result", result->stringAt("value"));
        return result;
    }
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

namespace {
bool hasCanonicalVisualizer(const ComPtr<IDataModelManager>& manager, IDebugHostType* type) {
    ComPtr<IDataModelManager2> manager2;
    ComPtr<IModelObject> model;
    return type && manager && SUCCEEDED(manager.query(manager2)) &&
           SUCCEEDED(manager2->GetModelForType(type, model.put(), nullptr, nullptr)) && model;
}

// MSVC's tuple Natvis has an address view for interactive editing. When the debugger
// supplies a pointer to the declared element type, take one level off for display.
void projectTupleElement(IDebugHostType* parent, std::string_view name, ComPtr<IModelObject>& child) {
    if (name.size() < 3 || name.front() != '[' || name.back() != ']')
        return;
    ULONG64 ordinal = 0;
    const auto end = name.data() + name.size() - 1;
    const auto parsed = std::from_chars(name.data() + 1, end, ordinal);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return;
    ComPtr<IDebugHostSymbol> argument;
    ComPtr<IDebugHostType> argumentType, childType, pointedTo;
    TypeKind kind{};
    bool same = false;
    if (FAILED(parent->GetGenericArgumentAt(ordinal, argument.put())) || !argument ||
        FAILED(argument.query(argumentType)) || FAILED(child->GetTypeInfo(childType.put())) || !childType ||
        FAILED(childType->GetTypeKind(&kind)) || kind != TypePointer ||
        FAILED(childType->GetBaseType(pointedTo.put())) || !pointedTo ||
        FAILED(pointedTo->CompareAgainst(argumentType.get(), 0, &same)) || !same)
        return;
    ComPtr<IModelObject> value;
    if (SUCCEEDED(child->Dereference(value.put())) && value)
        child = std::move(value);
}
} // namespace

Result<rpc::Json> ModelStore::children(std::int64_t reference, std::size_t start, std::size_t count,
                                       std::string_view filter, bool raw) {
    if (m_basic.contains(reference))
        return basicChildren(reference, start, count);
    const auto found = m_values.find(reference);
    if (found == m_values.end())
        return fail(ErrorCode::Cancelled, "variable reference belongs to an expired stop");
    if (start > 65536 || count > 4096 || (!filter.empty() && filter != "named" && filter != "indexed"))
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
    constexpr std::size_t maxVisits = 65536 + 4096;
    std::size_t visited = 0;
    const auto wantAnother = [&] { return variables.size() < limit && visited < maxVisits; };
    const auto enumerateRaw = [&](IModelObject* source) -> Result<void> {
        ComPtr<IRawEnumerator> fields;
        if (FAILED(source->EnumerateRawValues(SymbolField, 0, fields.put())) || !fields)
            return {};
        while (wantAnother()) {
            ++visited;
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
                return added;
        }
        return {};
    };
    const auto enumerateIndexed = [&](IModelObject* source, IIterableConcept* iterable) -> Result<void> {
        ULONG64 dimensions = 0;
        HRESULT status = iterable->GetDefaultIndexDimensionality(source, &dimensions);
        if (FAILED(status) || dimensions > 1)
            return fail(ErrorCode::Unsupported, "Natvis multidimensional indexing is unavailable in this slice");
        ComPtr<IModelIterator> iterator;
        status = iterable->GetIterator(source, iterator.put());
        if (FAILED(status) || !iterator)
            return std::unexpected(engineError("Natvis iterator", status));
        for (std::size_t index = 0; wantAnother(); ++index) {
            ++visited;
            ComPtr<IModelObject> child;
            ComPtr<IModelObject> indexObject;
            ComPtr<IKeyStore> metadata;
            status =
                iterator->GetNext(child.put(), dimensions, dimensions ? indexObject.put() : nullptr, metadata.put());
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
                return added;
        }
        return {};
    };
    const auto enumerateProjected = [&](const auto& self, IModelObject* source, unsigned depth) -> Result<void> {
        if (depth == 16)
            return fail(ErrorCode::OutOfRange, "Natvis expanded item nesting exceeds budget");
        ComPtr<IDebugHostType> type;
        (void)source->GetTypeInfo(type.put());
        Bstr typeName;
        const bool tuple =
            type && SUCCEEDED(type->GetName(typeName.put())) && typeName.view().starts_with(L"std::tuple<");
        bool anyNamed = false;
        ComPtr<IKeyEnumerator> keys;
        if (SUCCEEDED(source->EnumerateKeyValues(keys.put())) && keys) {
            while (wantAnother()) {
                ++visited;
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
                // DbgEng models a Natvis ExpandedItem as a wrapper. Project its children in place.
                if (name.view() == L"[+]" && metadata) {
                    if (auto expanded = self(self, child.get(), depth + 1); !expanded)
                        return expanded;
                    continue;
                }
                anyNamed = true;
                if (filter == "indexed")
                    continue;
                auto converted = utf8(name.view());
                if (!converted)
                    return std::unexpected(std::move(converted.error()));
                if (tuple)
                    projectTupleElement(type.get(), *converted, child);
                if (auto added = add(std::move(*converted), std::move(child), std::move(metadata)); !added)
                    return added;
            }
        }
        if (!wantAnother())
            return {};
        ComPtr<IUnknown> iterableConcept;
        if (SUCCEEDED(source->GetConcept(__uuidof(IIterableConcept), iterableConcept.put(), nullptr)) &&
            iterableConcept) {
            if (filter != "named")
                return enumerateIndexed(source, static_cast<IIterableConcept*>(iterableConcept.get()));
        } else if (filter != "indexed" && !anyNamed && !hasCanonicalVisualizer(m_manager, type.get())) {
            return enumerateRaw(source);
        }
        return {};
    };
    if (raw) {
        if (filter != "indexed") {
            if (auto fields = enumerateRaw(parent); !fields)
                return std::unexpected(std::move(fields.error()));
        }
    } else if (auto projected = enumerateProjected(enumerateProjected, parent, 0); !projected) {
        return std::unexpected(std::move(projected.error()));
    }
    if (visited >= maxVisits && variables.size() < limit)
        return fail(ErrorCode::OutOfRange, "debugger variable traversal budget exhausted");
    return rpc::Json::object().set("variables", std::move(variables));
}

namespace {
Result<ULONG> basicChild(IDebugSymbolGroup2* group, ULONG parent, std::wstring_view name) {
    ULONG count = 0;
    auto status = group->GetNumberSymbols(&count);
    if (FAILED(status) || count > 65536)
        return fail(ErrorCode::OutOfRange, "Native symbol group exceeds budget");
    std::optional<ULONG> found;
    for (ULONG index = 0; index < count; ++index) {
        DEBUG_SYMBOL_PARAMETERS parameter{};
        std::array<wchar_t, 4096> actual{};
        if (FAILED(group->GetSymbolParameters(index, 1, &parameter)) || parameter.ParentSymbol != parent)
            continue;
        if (SUCCEEDED(group->GetSymbolNameWide(index, actual.data(), static_cast<ULONG>(actual.size()), nullptr)) &&
            name == actual.data()) {
            if (found)
                return fail(ErrorCode::Unsupported, "Ambiguous native symbol ancestry");
            found = index;
        }
    }
    if (!found)
        return fail(ErrorCode::NotFound, "Native symbol no longer exists");
    return *found;
}
} // namespace
Result<std::pair<ComPtr<IDebugSymbolGroup2>, ULONG>> ModelStore::basicGroup(const BasicValue& saved, bool expand) {
    if (auto selected = select(saved.frame); !selected)
        return std::unexpected(std::move(selected.error()));
    ComPtr<IDebugSymbolGroup2> group;
    auto status = m_symbols->GetScopeSymbolGroup2(saved.flags, nullptr, group.put());
    if (FAILED(status))
        return std::unexpected(engineError("fresh native scope", status));
    if (!saved.expression.empty()) {
        auto wide = io::toWide(saved.expression);
        if (!wide)
            return std::unexpected(std::move(wide.error()));
        ULONG index = DEBUG_ANY_ID;
        status = group->AddSymbolWide(wide->c_str(), &index);
        if (FAILED(status))
            return std::unexpected(engineError("native expression", status));
    }
    // ExpandSymbol can renumber every index. Resolve the whole ancestry again after each
    // expansion; no mutable symbol group/index is retained in a DAP handle.
    auto resolve = [&](std::size_t length) -> Result<ULONG> {
        ULONG parent = DEBUG_ANY_ID;
        for (std::size_t depth = 0; depth < length; ++depth) {
            auto found = basicChild(group.get(), parent, saved.path[depth]);
            if (!found)
                return std::unexpected(std::move(found.error()));
            parent = *found;
        }
        return parent;
    };
    for (std::size_t depth = 1; depth <= saved.path.size(); ++depth) {
        auto parent = resolve(depth);
        if (!parent)
            return std::unexpected(std::move(parent.error()));
        if (depth < saved.path.size() || expand) {
            status = group->ExpandSymbol(*parent, TRUE);
            if (FAILED(status))
                return std::unexpected(engineError("expand native symbol", status));
        }
    }
    auto parent = resolve(saved.path.size());
    if (!parent)
        return std::unexpected(std::move(parent.error()));
    return std::pair{std::move(group), *parent};
}
Result<rpc::Json> ModelStore::basicVariable(IDebugSymbolGroup2* group, ULONG index, const BasicValue& parent) {
    std::array<wchar_t, 4096> name{}, value{}, type{};
    DEBUG_SYMBOL_PARAMETERS parameter{};
    auto status = group->GetSymbolParameters(index, 1, &parameter);
    if (SUCCEEDED(status))
        status = group->GetSymbolNameWide(index, name.data(), static_cast<ULONG>(name.size()), nullptr);
    if (SUCCEEDED(status))
        status = group->GetSymbolValueTextWide(index, value.data(), static_cast<ULONG>(value.size()), nullptr);
    if (FAILED(status))
        return std::unexpected(engineError("native symbol value", status));
    auto n = utf8(name.data()), v = utf8(value.data());
    if (!n || !v)
        return fail(ErrorCode::Corrupt, "Invalid native symbol text");
    auto result = rpc::Json::object().set("name", *n).set("value", *v).set("variablesReference", 0);
    if (SUCCEEDED(group->GetSymbolTypeNameWide(index, type.data(), static_cast<ULONG>(type.size()), nullptr)))
        if (auto t = utf8(type.data()); t)
            result.set("type", *t);
    ULONG64 address = 0;
    if (SUCCEEDED(group->GetSymbolOffset(index, &address)))
        result.set("memoryReference", std::format("{:#x}", address));
    if (parameter.SubElements > 0) {
        if (m_basic.size() >= 8192 || m_next == std::numeric_limits<std::int64_t>::max() || parent.path.size() >= 64)
            return fail(ErrorCode::OutOfRange, "Variable handle budget exhausted");
        auto child = parent;
        child.path.emplace_back(name.data());
        const auto id = m_next++;
        m_basic.emplace(id, std::move(child));
        result.set("variablesReference", id);
    }
    return result;
}
Result<rpc::Json> ModelStore::basicChildren(std::int64_t reference, std::size_t start, std::size_t count) {
    if (start > 65536 || count > 4096)
        return fail(ErrorCode::InvalidArgument, "Invalid native variable page");
    const auto saved = m_basic.at(reference);
    auto resolved = basicGroup(saved, true);
    if (!resolved)
        return std::unexpected(std::move(resolved.error()));
    auto& group = resolved->first;
    const auto parent = resolved->second;
    ULONG total = 0;
    auto status = group->GetNumberSymbols(&total);
    if (FAILED(status) || total > 65536)
        return fail(ErrorCode::OutOfRange, "Native symbol group exceeds budget");
    rpc::Json result = rpc::Json::array();
    std::size_t ordinal = 0;
    for (ULONG index = 0; index < total && result.size() < (count ? count : 256); ++index) {
        DEBUG_SYMBOL_PARAMETERS parameter{};
        status = group->GetSymbolParameters(index, 1, &parameter);
        if (FAILED(status))
            return std::unexpected(engineError("native symbol parameters", status));
        if (parameter.ParentSymbol != parent || ordinal++ < start)
            continue;
        auto item = basicVariable(group.get(), index, saved);
        if (!item)
            return std::unexpected(std::move(item.error()));
        result.push(std::move(*item));
    }
    return rpc::Json::object().set("variables", std::move(result));
}
Result<rpc::Json> ModelStore::write(std::int64_t reference, std::string_view name, std::string_view value) {
    if (name.empty() || name.size() > 4096 || value.empty() || value.size() > 16384 || name.find('\0') != name.npos ||
        value.find('\0') != value.npos)
        return fail(ErrorCode::InvalidArgument, "Invalid variable write");
    auto wide = io::toWide(name), replacementText = io::toWide(value);
    if (!wide || !replacementText)
        return fail(ErrorCode::InvalidArgument, "Invalid UTF-8 variable write");
    if (auto found = m_basic.find(reference); found != m_basic.end()) {
        const auto saved = found->second;
        auto resolved = basicGroup(saved, true);
        if (!resolved)
            return std::unexpected(std::move(resolved.error()));
        auto child = basicChild(resolved->first.get(), resolved->second, *wide);
        if (!child)
            return std::unexpected(std::move(child.error()));
        auto status = resolved->first->WriteSymbolWide(*child, replacementText->c_str());
        if (FAILED(status))
            return std::unexpected(engineError("write native symbol", status));
        return basicVariable(resolved->first.get(), *child, saved);
    }
    auto found = m_values.find(reference);
    if (found == m_values.end())
        return fail(ErrorCode::Cancelled, "Variable reference belongs to an expired stop");
    if (auto selected = select(found->second.frame); !selected)
        return std::unexpected(std::move(selected.error()));
    auto replacement = expression(*replacementText, false);
    if (!replacement)
        return std::unexpected(std::move(replacement.error()));
    ComPtr<IModelObject> target;
    ComPtr<IKeyStore> metadata;
    auto status = found->second.object->GetKeyReference(wide->c_str(), target.put(), metadata.put());
    if (FAILED(status))
        status = found->second.object->GetRawReference(SymbolField, wide->c_str(), 0, target.put());
    if (FAILED(status))
        return std::unexpected(engineError("find native assignment reference", status));
    ComPtr<IDebugHostEvaluator2> assignment;
    status = m_evaluator.query(assignment);
    if (FAILED(status))
        return fail(ErrorCode::Unsupported, "System32 evaluator has no native assignment API");
    ComPtr<IModelObject> actual;
    ComPtr<IKeyStore> actualMetadata;
    status = assignment->AssignTo(target.get(), replacement->get(), actual.put(), actualMetadata.put());
    if (FAILED(status) || !actual)
        return std::unexpected(engineError("assign native model value", status));
    return variable(std::string(name), std::move(actual), std::move(actualMetadata), found->second.frame);
}
Result<std::pair<ULONG64, ULONG64>> ModelStore::location(std::int64_t reference, std::string_view name) {
    auto wide = io::toWide(name);
    if (!wide)
        return std::unexpected(std::move(wide.error()));
    if (auto found = m_basic.find(reference); found != m_basic.end()) {
        auto resolved = basicGroup(found->second, true);
        if (!resolved)
            return std::unexpected(std::move(resolved.error()));
        auto child = basicChild(resolved->first.get(), resolved->second, *wide);
        if (!child)
            return std::unexpected(std::move(child.error()));
        ULONG64 address = 0;
        ULONG size = 0;
        auto status = resolved->first->GetSymbolOffset(*child, &address);
        if (SUCCEEDED(status))
            status = resolved->first->GetSymbolSize(*child, &size);
        if (FAILED(status))
            return std::unexpected(engineError("native symbol location", status));
        return std::pair<ULONG64, ULONG64>{address, size};
    }
    auto found = m_values.find(reference);
    if (found == m_values.end())
        return fail(ErrorCode::Cancelled, "Variable reference belongs to an expired stop");
    if (auto selected = select(found->second.frame); !selected)
        return std::unexpected(std::move(selected.error()));
    ComPtr<IModelObject> target;
    auto status = found->second.object->GetKeyReference(wide->c_str(), target.put(), nullptr);
    if (FAILED(status))
        status = found->second.object->GetRawReference(SymbolField, wide->c_str(), 0, target.put());
    if (FAILED(status))
        return std::unexpected(engineError("native data variable", status));
    Location location;
    ComPtr<IDebugHostType> type;
    status = target->GetTargetInfo(&location, type.put());
    ULONG64 size = 0;
    if (SUCCEEDED(status) && type)
        status = type->GetSize(&size);
    if (FAILED(status) || !type || location.HostDefined != 0)
        return fail(ErrorCode::Unsupported, "Variable has no virtual memory location");
    return std::pair<ULONG64, ULONG64>{location.Offset, size};
}
} // namespace frost::dbgeng
