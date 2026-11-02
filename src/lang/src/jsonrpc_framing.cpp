// SPDX-License-Identifier: Apache-2.0
//
// The JSON-RPC pieces of jsonrpc.hpp that are not the connection: toError and the IAsyncProcess
// byte stream. The frame codec itself is frost::rpc's (frost/rpc/framing.hpp; ADR 0050).

#include "frost/io/async_process.hpp"
#include "frost/lang/jsonrpc.hpp"

#include <format>

namespace frost::lang {
namespace {

class ProcessByteStream final : public IByteStream {
public:
    explicit ProcessByteStream(std::shared_ptr<io::IAsyncProcess> process) : m_process(std::move(process)) {}

    sched::Task<Result<std::size_t>> read(std::span<char> buffer, std::stop_token token) override {
        return m_process->readStdout(buffer, std::move(token));
    }
    sched::Task<Result<void>> write(std::string bytes, std::stop_token token) override {
        return m_process->writeStdin(std::move(bytes), std::move(token));
    }
    void closeWrite() noexcept override { m_process->closeStdin(); }

private:
    std::shared_ptr<io::IAsyncProcess> m_process;
};

} // namespace

Error toError(std::string_view method, const ResponseError& error) {
    ErrorCode code = ErrorCode::IoError;
    switch (error.code) {
    case rpc_error::kRequestCancelled: code = ErrorCode::Cancelled; break;
    case rpc_error::kContentModified:
    case rpc_error::kServerCancelled: code = ErrorCode::Busy; break;
    case rpc_error::kMethodNotFound: code = ErrorCode::Unsupported; break;
    case rpc_error::kInvalidParams:
    case rpc_error::kInvalidRequest: code = ErrorCode::InvalidArgument; break;
    default: break;
    }
    return Error(code, std::format("{}: server error {}: {}", method, error.code, error.message));
}

std::unique_ptr<IByteStream> processByteStream(std::shared_ptr<io::IAsyncProcess> process) {
    return std::make_unique<ProcessByteStream>(std::move(process));
}

} // namespace frost::lang
