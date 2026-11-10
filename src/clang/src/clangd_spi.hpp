// SPDX-License-Identifier: Apache-2.0
//
// Minimal LLVM 23.1.4 clangd SPI declarations for frost-clang (CLANG-0, ADR 0083).
// Upstream LLVM installs clangdMain and clangDaemon static archives under lib/ but does not install
// clang-tools-extra/clangd/*.h under include/. These declarations match LLVM 23.1.4's
// clang-tools-extra/clangd/{FeatureModule.h,LSPBinder.h,tool/ClangdMain.h} byte-for-byte.
#pragma once

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Registry.h"

#include <chrono>
#include <memory>
#include <optional>

namespace clang {
class ASTContext;
class CompilerInstance;
class Diagnostic;
class Preprocessor;

namespace tooling {
struct CompileCommand;
} // namespace tooling

namespace clangd {

class CanonicalIncludes;
class ParsedAST;
class SymbolIndex;
class ThreadsafeFS;
struct Diag;

class Deadline {
public:
    static Deadline zero() { return Deadline(Type::Zero); }
    static Deadline infinity() { return Deadline(Type::Infinite); }

private:
    enum class Type { Zero, Infinite, Finite };
    explicit Deadline(Type type) : m_type(type) {}
    [[maybe_unused]] std::chrono::steady_clock::time_point m_time;
    [[maybe_unused]] Type m_type = Type::Zero;
};

template<typename T>
using Callback = llvm::unique_function<void(llvm::Expected<T>)>;

class LSPBinder {
public:
    using JSON = llvm::json::Value;

    struct RawOutgoing {
        virtual ~RawOutgoing() = default;
        virtual void callMethod(llvm::StringRef method, JSON params, Callback<JSON> reply) = 0;
        virtual void notify(llvm::StringRef method, JSON params) = 0;
    };

    struct RawHandlers {
        llvm::StringMap<llvm::unique_function<void(JSON)>> NotificationHandlers;
        llvm::StringMap<llvm::unique_function<void(JSON, Callback<JSON>)>> MethodHandlers;
        llvm::StringMap<llvm::unique_function<void(JSON, Callback<JSON>)>> CommandHandlers;
    };

    LSPBinder(RawHandlers& raw, RawOutgoing& out) : m_raw(raw), m_out(out) {}

    template<typename HandlerT>
    void bindRawMethod(llvm::StringLiteral method, HandlerT* self,
                       void (HandlerT::*handler)(const JSON&, Callback<JSON>)) {
        m_raw.MethodHandlers[method] = [self, handler](JSON rawParams, Callback<JSON> reply) {
            (self->*handler)(rawParams, std::move(reply));
        };
    }

    [[nodiscard]] RawOutgoing& outgoing() noexcept { return m_out; }

private:
    RawHandlers& m_raw;
    RawOutgoing& m_out;
};

class FeatureModule {
public:
    virtual ~FeatureModule() = default;

    struct Facilities {
        class AsyncTaskRunner* Work = nullptr;
        const SymbolIndex* Index = nullptr;
        const ThreadsafeFS* TFS = nullptr;
    };
    void initialize(const Facilities& f);

    virtual void initializeLSP(LSPBinder& /*bind*/, const llvm::json::Object& /*clientCaps*/,
                               llvm::json::Object& /*serverCaps*/) {}

    virtual void stop() {}
    virtual bool blockUntilIdle(Deadline /*d*/) { return true; }
    virtual void adjustCompileCommand(tooling::CompileCommand& /*cmd*/, llvm::StringRef /*file*/) const {}

    struct ASTListener {
        virtual ~ASTListener() = default;
        virtual void sawDiagnostic(const clang::Diagnostic& /*d*/, Diag& /*out*/) {}
        virtual void beforeExecute(CompilerInstance& /*ci*/) {}
    };
    virtual std::unique_ptr<ASTListener> astListeners() { return nullptr; }

protected:
    Facilities& facilities();

private:
    std::optional<Facilities> m_fac;
};

using FeatureModuleRegistry = llvm::Registry<FeatureModule>;

int clangdMain(int argc, char* argv[]);

} // namespace clangd
} // namespace clang

namespace llvm {
extern template class LLVM_TEMPLATE_ABI Registry<clang::clangd::FeatureModule>;
} // namespace llvm
