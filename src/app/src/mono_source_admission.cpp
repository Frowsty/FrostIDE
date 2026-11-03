// SPDX-License-Identifier: Apache-2.0
// Admit Mono module and source evidence using bounded helper processes.
#include "mono_source_admission.hpp"

#include "frost/io/path.hpp"
#include "frost/rpc/json.hpp"

#include "mono_held_capture.hpp"
#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <chrono>
namespace frost::app {
namespace {
std::string digest(std::string_view bytes) {
    return QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
} // namespace
Result<MonoAdmittedModule> admitMonoModule(io::IProcessLauncher& launcher, std::string_view heldHelper,
                                           std::string_view dotnet, std::string_view guard, std::string_view root,
                                           std::string_view assemblyPath, std::string_view pdbPath,
                                           std::stop_token stop, const std::function<bool()>& current,
                                           std::chrono::milliseconds timeout) {
    using namespace std::chrono_literals;
    if (timeout <= 0ms || timeout > 60s)
        return fail(ErrorCode::InvalidArgument, "invalid Mono admission deadline");
    const auto end = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&] {
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        return std::max(0ms, std::min(30000ms, time));
    };
    if (!current || stop.stop_requested() || !current())
        return fail(ErrorCode::Cancelled, "Mono source admission is no longer current");
    auto live = [&] { return current() && std::chrono::steady_clock::now() < end; };
    auto rejected = [&](Error error) -> Result<MonoAdmittedModule> {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Mono source admission was cancelled");
        if (remaining() <= 0ms)
            return fail(ErrorCode::Timeout, "Mono source admission timed out");
        return std::unexpected(std::move(error));
    };
    auto next = remaining();
    if (next <= 0ms)
        return fail(ErrorCode::Timeout, "Mono source admission timed out");
    auto image = captureMonoHeldInput(launcher, heldHelper, root, assemblyPath, 1024 * 1024 - 15, next, stop, live);
    if (!image)
        return rejected(std::move(image.error()));
    next = remaining();
    if (next <= 0ms)
        return fail(ErrorCode::Timeout, "Mono source admission timed out");
    auto pdb = captureMonoHeldInput(launcher, heldHelper, root, pdbPath, 1024 * 1024 - 15, next, stop, live);
    if (!pdb)
        return rejected(std::move(pdb.error()));
    auto initial = monoDocumentsInput(*image, *pdb);
    if (!initial)
        return rejected(std::move(initial.error()));
    next = remaining();
    if (next <= 0ms)
        return fail(ErrorCode::Timeout, "Mono source admission timed out");
    auto inspected = runMonoGuardInput(launcher, dotnet, guard, root, false, std::move(*initial), next, stop, live);
    if (!inspected)
        return rejected(std::move(inspected.error()));
    auto manifest = rpc::parseJson(*inspected);
    const auto* documents = manifest ? manifest->find("documents") : nullptr;
    if (!manifest || !manifest->isObject() || manifest->stringAt("moduleId").empty() ||
        manifest->stringAt("assemblySha256") != digest(*image) || manifest->stringAt("pdbSha256") != digest(*pdb) ||
        !documents || !documents->isArray() || documents->size() == 0 || documents->size() > 16384)
        return fail(ErrorCode::Corrupt, "Mono document inspection does not match captured module");
    ManagedModuleSnapshot snapshot;
    snapshot.source.moduleId = manifest->stringAt("moduleId");
    snapshot.assemblyBytes = std::move(*image);
    snapshot.pdbBytes = std::move(*pdb);
    std::vector<std::string> order;
    std::size_t total = 0;
    constexpr std::size_t maxTotal = 16 * 1024 * 1024;
    for (const auto& value : documents->items()) {
        if (!value.asString() || !io::isAbsolutePath(*value.asString()) || value.asString()->size() > 4096)
            return fail(ErrorCode::Corrupt, "Mono inspection returned an invalid source path");
        const auto limit = std::min<std::size_t>(4 * 1024 * 1024, maxTotal - total);
        next = remaining();
        if (next <= 0ms)
            return fail(ErrorCode::Timeout, "Mono source admission timed out");
        auto source = captureMonoHeldInput(launcher, heldHelper, root, *value.asString(), limit, next, stop, live);
        if (!source)
            return rejected(std::move(source.error()));
        if (source->size() > maxTotal - total)
            return fail(ErrorCode::OutOfRange, "Mono source snapshots exceed the limit");
        total += source->size();
        order.push_back(*value.asString());
        if (!snapshot.source.sources.emplace(order.back(), std::move(*source)).second)
            return fail(ErrorCode::Corrupt, "Mono inspection returned duplicate source paths");
    }
    auto framed = monoMapInput(snapshot, order);
    if (!framed)
        return rejected(std::move(framed.error()));
    next = remaining();
    if (next <= 0ms)
        return fail(ErrorCode::Timeout, "Mono source admission timed out");
    auto output = runMonoGuardInput(launcher, dotnet, guard, root, true, std::move(*framed), next, stop, live);
    if (!output)
        return rejected(std::move(output.error()));
    PreparedManagedDebug prepared;
    prepared.moduleId = snapshot.source.moduleId;
    prepared.assemblySha256 = digest(snapshot.assemblyBytes);
    prepared.pdbSha256 = digest(snapshot.pdbBytes);
    for (const auto& path : order)
        prepared.sources.push_back({path, digest(snapshot.source.sources.at(path))});
    auto map = parseMonoSourceMap(*output, prepared, snapshot);
    if (!map)
        return rejected(std::move(map.error()));
    if (stop.stop_requested() || !current())
        return fail(ErrorCode::Cancelled, "Mono source admission is no longer current");
    if (remaining() <= 0ms)
        return fail(ErrorCode::Timeout, "Mono source admission timed out");
    return MonoAdmittedModule{std::make_shared<const ManagedModuleSnapshot>(std::move(snapshot)),
                              std::make_shared<const MonoSourceMap>(std::move(*map))};
}
} // namespace frost::app
