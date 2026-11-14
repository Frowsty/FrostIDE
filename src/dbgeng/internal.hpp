// SPDX-License-Identifier: Apache-2.0
// Windows-only owner-thread interfaces for the System32 DbgEng DAP adapter (ADR 0081).
#pragma once

#include "frost/core/error.hpp"
#include "frost/rpc/json.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <dbgeng.h>
#include <dbgmodel.h>
#include <oleauto.h>

namespace frost::dbgeng {

template<class T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* owned) noexcept : m_value(owned) {}
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            m_value = std::exchange(other.m_value, nullptr);
        }
        return *this;
    }
    void reset() noexcept {
        if (T* old = std::exchange(m_value, nullptr))
            old->Release();
    }
    [[nodiscard]] T* get() const noexcept { return m_value; }
    [[nodiscard]] T* operator->() const noexcept { return m_value; }
    explicit operator bool() const noexcept { return m_value != nullptr; }
    [[nodiscard]] T** put() noexcept {
        reset();
        return &m_value;
    }
    template<class U>
    [[nodiscard]] HRESULT query(ComPtr<U>& target) const noexcept {
        return m_value->QueryInterface(__uuidof(U), reinterpret_cast<void**>(target.put()));
    }

private:
    T* m_value = nullptr;
};

class Bstr {
public:
    ~Bstr() { SysFreeString(m_value); }
    Bstr() = default;
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    [[nodiscard]] BSTR* put() noexcept {
        SysFreeString(std::exchange(m_value, nullptr));
        return &m_value;
    }
    [[nodiscard]] std::wstring_view view() const noexcept {
        return m_value == nullptr ? std::wstring_view{} : std::wstring_view(m_value, SysStringLen(m_value));
    }

private:
    BSTR m_value = nullptr;
};

[[nodiscard]] Error engineError(std::string_view operation, HRESULT status);
[[nodiscard]] Result<std::string> utf8(std::wstring_view value);

struct FrameContext {
    ULONG thread = 0; // Actual DbgEng thread identifier, not an OS id.
    ULONG frame = 0;  // Actual frame index within that thread's current stop.
};

// All calls, model references and destruction stay on the DbgEng owner thread. References are
// cleared before every resume/end; monotonically increasing handles prevent stale-stop aliases.
class ModelStore {
public:
    [[nodiscard]] Result<void> initialize(IDebugClient5* client);
    void clear() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] Result<std::int64_t> scope(FrameContext frame, bool parameters);
    [[nodiscard]] Result<rpc::Json> evaluate(std::string_view expression, FrameContext frame);
    [[nodiscard]] Result<rpc::Json> children(std::int64_t reference, std::size_t start, std::size_t count,
                                             std::string_view filter);

private:
    struct Value {
        ComPtr<IModelObject> object;
        ComPtr<IKeyStore> metadata;
        FrameContext frame;
    };
    [[nodiscard]] Result<ComPtr<IModelObject>> expression(std::wstring_view text, bool extended,
                                                          ComPtr<IKeyStore>* metadata = nullptr);
    [[nodiscard]] Result<rpc::Json> variable(std::string name, ComPtr<IModelObject> object, ComPtr<IKeyStore> metadata,
                                             FrameContext frame);
    [[nodiscard]] Result<std::string> display(IModelObject* value, IKeyStore* metadata);
    [[nodiscard]] Result<std::int64_t> retain(ComPtr<IModelObject> object, ComPtr<IKeyStore> metadata,
                                              FrameContext frame);
    [[nodiscard]] Result<void> select(FrameContext frame);

    ComPtr<IDataModelManager> m_manager;
    ComPtr<IDebugHost> m_host;
    ComPtr<IDebugHostEvaluator> m_evaluator;
    ComPtr<IDebugSymbols3> m_symbols;
    ComPtr<IDebugSystemObjects4> m_systems;
    std::map<std::int64_t, Value> m_values;
    std::int64_t m_next = 1;
};

// Stdio DAP entry point. Only the worker owns DbgEng; input and output queues have fixed bounds.
[[nodiscard]] int runAdapter();

} // namespace frost::dbgeng
