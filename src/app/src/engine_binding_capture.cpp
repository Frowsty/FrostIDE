// SPDX-License-Identifier: Apache-2.0
// FEC-13 actual schema export, read-only AST child runs and immutable filesystem/provider capture.
#include "frost/engine/engine_cli.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/lua_host.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include "engine_binding_work.hpp"

#include <algorithm>
#include <chrono>
#include <set>

namespace frost::app::binding_work {
namespace {
bool inside(std::string_view root, std::string_view path) {
    if (root.empty() || !io::isAbsolutePath(root) || !io::isAbsolutePath(path))
        return false;
    const auto base = lang::documentKey(root);
    const auto key = lang::documentKey(path);
    return key == base ||
           (key.starts_with(base) && key.size() > base.size() &&
            (base.ends_with('/') || base.ends_with('\\') || key[base.size()] == '/' || key[base.size()] == '\\'));
}
bool languageFile(std::string_view path) {
    auto name = lang::documentKey(path);
    for (auto& ch : name)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + ('a' - 'A'));
    for (std::string_view extension : {".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".hh", ".hxx", ".inl", ".lua"})
        if (name.ends_with(extension))
            return true;
    return false;
}
Result<void> rememberPhysical(Graph& graph, std::string spelling, const std::string& physical) {
    auto [at, inserted] = graph.physicalReceipts.emplace(std::move(spelling), physical);
    if (!inserted && lang::documentKey(at->second) != lang::documentKey(physical))
        return fail(ErrorCode::Busy, "An original binding path retargeted during capture");
    return {};
}

} // namespace

Result<void> captureOpenDocumentReceipts(io::IFileSystem& fs, Graph& graph,
                                         std::span<const engine::BindingDocumentCapture> openDocuments, std::stop_token stop) {
    std::map<std::string, std::string, std::less<>> openIdentities;
    for (const auto& document : openDocuments) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "The binding document alias capture was cancelled");
        auto physical = fs.physicalPath(document.path);
        if (!physical)
            return std::unexpected(physical.error());
        if (!openIdentities.emplace(lang::documentKey(*physical), document.path).second)
            return fail(ErrorCode::Busy, "Several open aliases name one physical binding document");
        if (auto receipt = rememberPhysical(graph, document.path, *physical); !receipt)
            return std::unexpected(receipt.error());
    }
    return {};
}

Result<void> captureDocumentSnapshot(io::IFileSystem& fs, Graph& graph,
                                     std::span<const engine::BindingDocumentCapture> openDocuments,
                                     std::string path, std::size_t& bytes) {
    auto resolved = resolveDocumentPath(fs, graph.physicalRoot, graph.engineRoot, graph.metadataPath, path);
    if (!resolved)
        return std::unexpected(resolved.error());
    const auto physical = resolved->physical;
    if (auto receipt = rememberPhysical(graph, path, physical); !receipt)
        return std::unexpected(receipt.error());
    const auto key = lang::documentKey(physical);
    const auto role = resolved->role;
    if (std::ranges::any_of(graph.documents,
                            [&](const auto& document) { return lang::documentKey(document.physicalPath) == key; }))
        return {};
    if (bytes >= 256U * 1024U * 1024U || graph.documents.size() >= 100000)
        return fail(ErrorCode::OutOfRange, "Binding document capture budget exhausted before reading another file");
    auto contents = readDocumentSnapshot(fs, physical, bytes);
    if (!contents)
        return std::unexpected(contents.error());
    if (contents->size() > 16U * 1024U * 1024U || bytes + contents->size() > 256U * 1024U * 1024U ||
        graph.documents.size() >= 100000)
        return fail(ErrorCode::OutOfRange, "Binding document capture exceeded its bounded 256 MiB/100000 file budget");
    bytes += contents->size();
    const auto expected = graph.inputHashes.find(physical);
    const bool astInput = expected != graph.inputHashes.end();
    const auto hash = io::sha256Hex(*contents);
    if (astInput && expected->second != hash)
        return fail(ErrorCode::Busy, "A parsed AST input changed before its document snapshot was captured");
    graph.inputHashes.emplace(physical, hash);
    const engine::BindingDocumentCapture* open = nullptr;
    for (const auto& document : openDocuments) {
        const auto receipt = graph.physicalReceipts.find(document.path);
        if (receipt != graph.physicalReceipts.end() && lang::documentKey(receipt->second) == key) {
            if (open)
                return fail(ErrorCode::Busy, "Several open document aliases name the same physical binding source");
            open = &document;
        }
    }
    if (open) {
        // LibTooling reads actual disk files. A dirty registration/header input cannot pretend
        // the AST was parsed from its unsaved overlay. Lua call sites can use their captured buffer.
        if (astInput && !luaSource(path) && open->snapshot.text() != *contents)
            return fail(ErrorCode::Busy, "Save native binding-analysis inputs before querying their disk AST");
        auto document = *open;
        document.physicalPath = physical;
        document.role = role;
        graph.documents.push_back(std::move(document));
        return {};
    }
    auto buffer = text::TextBuffer::fromUtf8(std::move(*contents));
    if (!buffer)
        return std::unexpected(buffer.error());
    graph.documents.push_back({path, buffer->snapshot(), {}, role, physical});
    return {};
}

bool luaSource(std::string_view path) noexcept {
    if (path.size() < 4)
        return false;
    const auto tail = path.substr(path.size() - 4);
    return tail[0] == '.' && (tail[1] == 'l' || tail[1] == 'L') && (tail[2] == 'u' || tail[2] == 'U') &&
           (tail[3] == 'a' || tail[3] == 'A');
}
Result<DocumentPath> resolveDocumentPath(io::IFileSystem& fs, std::string_view workspace, std::string_view engine,
                                         std::string_view metadata, std::string_view candidate) {
    auto physical = fs.physicalPath(candidate);
    if (!physical)
        return std::unexpected(physical.error());
    auto generated = metadata.empty() ? Result<std::string>(fail(ErrorCode::NotFound)) : fs.physicalPath(metadata);
    if (!metadata.empty() && !generated)
        return std::unexpected(generated.error());
    auto physicalWorkspace = fs.physicalPath(workspace);
    if (!physicalWorkspace)
        return std::unexpected(physicalWorkspace.error());
    auto physicalEngine = engine.empty() ? Result<std::string>(std::string{}) : fs.physicalPath(engine);
    if (!physicalEngine)
        return std::unexpected(physicalEngine.error());
    const auto role = generated && lang::documentKey(*generated) == lang::documentKey(*physical)
                          ? engine::BindingDocumentRole::GeneratedEngineMetadata
                      : inside(*physicalWorkspace, *physical) || inside(*physicalEngine, *physical)
                          ? engine::BindingDocumentRole::UserSource
                          : engine::BindingDocumentRole::ExternalReadOnly;
    return DocumentPath{std::move(*physical), role};
}
bool expectsEngineMetadata(const rpc::Json& configuration, std::string_view metadataDirectory) {
    const auto* workspace = configuration.find("workspace");
    const auto* library = workspace ? workspace->find("library") : nullptr;
    if (!library || !library->isArray())
        return false;
    return std::ranges::any_of(library->items(), [&](const auto& item) {
        return item.isString() && lang::documentKey(*item.asString()) == lang::documentKey(metadataDirectory);
    });
}

Result<RenameDefinitionAdmission> admitRenameDefinitions(io::IFileSystem& fs, const rpc::Json& definitions,
                                                         std::string_view metadata,
                                                         std::string_view expectedPhysicalMetadata, bool metadataRequired) {
    auto generated = fs.physicalPath(metadata);
    if (!generated && (metadataRequired || !expectedPhysicalMetadata.empty() || generated.error().code() != ErrorCode::NotFound))
        return std::unexpected(generated.error());
    if (!expectedPhysicalMetadata.empty() &&
        lang::documentKey(*generated) != lang::documentKey(expectedPhysicalMetadata))
        return fail(ErrorCode::Busy, "The expected engine metadata physical identity changed");
    RenameDefinitionAdmission admission;
    if (generated)
        admission.physicalMetadata = *generated;
    auto items = definitions;
    if (items.isObject())
        items = rpc::Json::array().push(std::move(items));
    if (!items.isArray() || items.size() == 0)
        return fail(ErrorCode::NotFound, "LuaLS did not confirm a semantic definition for this rename");
    for (const auto& item : items.items()) {
        const auto location = lang::parseLocation(item);
        if (!location || location->range.end < location->range.start)
            return fail(ErrorCode::Corrupt, "LuaLS returned an invalid definition location");
        const auto actual = fs.physicalPath(location->path);
        if (!actual)
            return std::unexpected(actual.error());
        if (generated && lang::documentKey(*actual) == lang::documentKey(*generated))
            admission.bound = true;
    }
    if (!admission.physicalMetadata.empty()) {
        const auto current = fs.physicalPath(metadata);
        if (!current || lang::documentKey(*current) != lang::documentKey(admission.physicalMetadata))
            return fail(ErrorCode::Busy, "Engine metadata changed while admitting actual definitions");
    }
    return admission;
}

Result<std::string> readDocumentSnapshot(io::IFileSystem& fs, std::string_view physical, std::size_t bytes) {
    if (bytes >= 256U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Binding capture budget exhausted before opening another file");
    return fs.readFileSnapshot(physical, std::min<std::size_t>(16U * 1024U * 1024U, 256U * 1024U * 1024U - bytes));
}

bool Capture::current() const {
    if (!active || !active->load() || stop.stop_requested() || !trust || !native || !lua)
        return false;
    const auto permission = trust->snapshot(root);
    if (!permission.trusted || permission.revision != trustRevision || native->state() != lang::ClangdState::Running ||
        lua->state() != lang::LuaState::Running || native->serverPid().value_or(0) != nativePid ||
        lua->serverPid().value_or(0) != luaPid || native->restarts() != nativeRestarts ||
        lua->restarts() != luaRestarts || lua->configurationChangesSent() != luaConfigurationChanges ||
        native->encoding() != nativeProvider.encoding || lua->encoding() != luaProvider.encoding)
        return false;
    for (const auto& document : openDocuments) {
        const auto version =
            luaSource(document.path) ? lua->documentVersion(document.path) : native->documentVersion(document.path);
        if (document.lspVersion && version != document.lspVersion)
            return false;
    }
    for (const auto& lease : analysisDocuments)
        if (!native->analysisDocumentCurrent(lease))
            return false;
    return true;
}

const engine::BindingDocumentCapture* Graph::generatedDocument() const {
    const auto expected = physicalReceipts.find(metadataPath);
    if (expected == physicalReceipts.end())
        return nullptr;
    const engine::BindingDocumentCapture* found = nullptr;
    for (const auto& document : documents)
        if (document.role == engine::BindingDocumentRole::GeneratedEngineMetadata &&
            !document.physicalPath.empty() &&
            lang::documentKey(document.physicalPath) == lang::documentKey(expected->second)) {
            if (found)
                return nullptr;
            found = &document;
        }
    return found;
}

bool Graph::verifyReceipts(io::IFileSystem& fs) const {
    for (const auto& [spelling, expected] : physicalReceipts) {
        const auto actual = fs.physicalPath(spelling);
        if (!actual || lang::documentKey(*actual) != lang::documentKey(expected))
            return false;
    }
    return true;
}

bool Graph::verify(const Capture& capture) const {
    if (!capture.current())
        return false;
    const auto permission = capture.trust->snapshot(lexicalEngineRoot);
    if (!permission.trusted || permission.revision != engineTrustRevision)
        return false;
    if (!documents.empty())
        for (const auto& open : capture.openDocuments) {
            const auto prior = std::ranges::find_if(documents, [&](const auto& document) {
                return lang::documentKey(document.path) == lang::documentKey(open.path);
            });
            if (prior != documents.end() &&
                (!prior->snapshot.sharesStorageWith(open.snapshot) ||
                 prior->snapshot.version() != open.snapshot.version() || prior->lspVersion != open.lspVersion))
                return false;
        }
    const auto root = capture.fs->physicalPath(capture.root);
    const auto engine = capture.fs->physicalPath(lexicalEngineRoot);
    const auto database = capture.fs->physicalPath(capture.database.file);
    const auto commands = database ? capture.fs->readFileSnapshot(*database, 32U * 1024U * 1024U)
                                   : Result<std::string>(fail(ErrorCode::NotFound));
    const auto executable = capture.fs->physicalPath(program);
    const auto binary = executable ? capture.fs->status(*executable)
                                   : Result<io::FileStatus>(fail(ErrorCode::NotFound));
    if (!root || lang::documentKey(*root) != lang::documentKey(physicalRoot) || !engine ||
        lang::documentKey(*engine) != lang::documentKey(engineRoot) || !database ||
        lang::documentKey(*database) != lang::documentKey(databasePath) || !commands || *commands != databaseBytes ||
        !executable || lang::documentKey(*executable) != lang::documentKey(physicalProgram) ||
        !binary || binary->mtime != programMtime)
        return false;
    if (!verifyReceipts(*capture.fs))
        return false;
    for (const auto& [path, hash] : inputHashes) {
        if (!capture.current())
            return false;
        const auto canonical = capture.fs->physicalPath(path);
        const auto text = capture.fs->readFileSnapshot(path, 16U * 1024U * 1024U);
        if (!canonical || lang::documentKey(*canonical) != lang::documentKey(path) || !text ||
            io::sha256Hex(*text) != hash)
            return false;
    }
    return capture.current();
}

Result<std::shared_ptr<Graph>> buildGraph(const Capture& capture) {
    if (!capture.current() || !capture.host)
        return fail(ErrorCode::Cancelled, "The trusted binding capture is no longer current");
    auto cached = capture.host->cachedSchemaCapture(*capture.fs, capture.root);
    if (!cached || cached->program.empty())
        return fail(ErrorCode::NotFound,
                    "Actual FrostEngine schema warmup is unavailable; build the editor and start LuaLS");
    auto graph = std::make_shared<Graph>();
    const auto physicalRoot = capture.fs->physicalPath(capture.root);
    const auto engineRoot = capture.fs->physicalPath(cached->engineRoot);
    if (!physicalRoot || !engineRoot)
        return fail(ErrorCode::NotFound, "The binding workspace's physical root is unavailable");
    graph->physicalRoot = *physicalRoot;
    graph->engineRoot = *engineRoot;
    graph->lexicalEngineRoot = cached->engineRoot;
    if (auto receipt = rememberPhysical(*graph, capture.root, *physicalRoot); !receipt)
        return std::unexpected(receipt.error());
    if (auto receipt = rememberPhysical(*graph, cached->engineRoot, *engineRoot); !receipt)
        return std::unexpected(receipt.error());
    const auto trust = capture.trust->snapshot(graph->lexicalEngineRoot);
    if (!trust.trusted)
        return fail(ErrorCode::Untrusted, "Trust the engine checkout before semantic registration analysis");
    graph->engineTrustRevision = trust.revision;
    graph->program = cached->program;
    graph->programMtime = cached->programMtime;
    const auto executable = capture.fs->physicalPath(graph->program);
    if (!executable)
        return std::unexpected(executable.error());
    graph->physicalProgram = *executable;
    if (auto receipt = rememberPhysical(*graph, graph->program, *executable); !receipt)
        return std::unexpected(receipt.error());
    const auto databasePath = capture.fs->physicalPath(capture.database.file);
    if (!databasePath)
        return std::unexpected(databasePath.error());
    graph->databasePath = *databasePath;
    if (auto receipt = rememberPhysical(*graph, capture.database.file, *databasePath); !receipt)
        return std::unexpected(receipt.error());
    auto database = capture.fs->readFileSnapshot(*databasePath, 32U * 1024U * 1024U);
    if (!database)
        return std::unexpected(database.error());
    graph->databaseBytes = *database;
    auto commands = rpc::parseJson(*database);
    if (!commands || !commands->isArray())
        return fail(ErrorCode::Corrupt, "The actual clangd compilation database is not a JSON array");
    std::map<std::string, std::string, std::less<>> translationUnits;
    for (const auto& command : commands->items()) {
        auto file = io::isAbsolutePath(command.stringAt("file"))
                        ? Result<std::string>(std::string(command.stringAt("file")))
                        : io::joinPath(command.stringAt("directory"), command.stringAt("file"));
        if (!file)
            continue;
        auto physical = capture.fs->physicalPath(*file);
        if (!physical)
            return std::unexpected(physical.error());
        if (auto receipt = rememberPhysical(*graph, *file, *physical); !receipt)
            return std::unexpected(receipt.error());
        translationUnits[lang::documentKey(*physical)] = *physical;
    }
    if (!graph->verify(capture))
        return fail(ErrorCode::Cancelled, "Physical admission changed before schema export");
    engine::EngineCliRequest schemaRequest;
    schemaRequest.command = "schema.export";
    schemaRequest.workingDirectory = graph->lexicalEngineRoot;
    schemaRequest.timeout = std::chrono::seconds(30);
    schemaRequest.stopToken = capture.stop;
    auto rawSchema = engine::runEngineCliResult(*capture.launcher, graph->program, schemaRequest);
    if (!rawSchema)
        return std::unexpected(rawSchema.error());
    auto schema = engine::parseSchemaDocument(*rawSchema);
    if (!schema)
        return std::unexpected(schema.error());
    const auto generated = engine::generateEngineLuaMetadata(*schema);
    auto directory = lang::luaHostMetaDirectory(capture.luaStateDirectory, "FrostEngine", capture.root);
    if (!directory)
        return std::unexpected(directory.error());
    auto metadata = io::joinPath(*directory, engine::kEngineLuaStubFileName);
    if (!metadata)
        return std::unexpected(metadata.error());
    graph->metadataPath = *metadata;
    const auto metadataPath = capture.fs->physicalPath(*metadata);
    auto metadataText = metadataPath ? capture.fs->readFileSnapshot(*metadataPath, 16U * 1024U * 1024U)
                                     : Result<std::string>(fail(ErrorCode::NotFound));
    if (metadataPath) {
        if (auto receipt = rememberPhysical(*graph, *metadata, *metadataPath); !receipt)
            return std::unexpected(receipt.error());
    }
    if (!metadataText || *metadataText != generated.text)
        return fail(
            ErrorCode::Busy,
            "LuaLS's generated engine metadata differs from actual schema.export; reload Lua host configuration");
    std::map<std::string, engine::SchemaDocument, std::less<>> groups;
    std::vector<engine::BindingMappingIssue> issues;
    for (const auto& entry : schema->luaEntries) {
        if (!entry.source)
            continue;
        auto file = io::joinPath(graph->engineRoot, entry.source->file);
        auto physical = file ? capture.fs->physicalPath(*file) : Result<std::string>(fail(ErrorCode::InvalidArgument));
        if (!physical)
            return std::unexpected(physical.error());
        if (auto receipt = rememberPhysical(*graph, *file, *physical); !receipt)
            return std::unexpected(receipt.error());
        if (!translationUnits.contains(lang::documentKey(*physical))) {
            issues.push_back({entry.qualifiedName, *entry.source, engine::BindingResolution::Unresolved,
                              "The exact registration file has no compilation-database translation unit"});
            continue;
        }
        groups[*physical].luaEntries.push_back(entry);
    }
    auto created = capture.fs->createDirectories(capture.stateDirectory);
    if (!created)
        return std::unexpected(created.error());
    // Embedded calls can live outside registration TUs. Analyze every admitted CDB TU,
    // including those with no registration entries, before granting the bounded profile.
    for (const auto& [key, path] : translationUnits) {
        static_cast<void>(key);
        if (inside(graph->engineRoot, path))
            groups.try_emplace(path);
    }
    std::vector<engine::NativeBindingMapping> mappings;
    std::size_t sequence = 0;
    for (const auto& [translationUnit, entries] : groups) {
        if (!graph->verify(capture))
            return fail(ErrorCode::Cancelled, "Binding analysis was invalidated before the next tool run");
        const auto currentDatabase = capture.fs->readFileSnapshot(graph->databasePath, 32U * 1024U * 1024U);
        if (!currentDatabase || *currentDatabase != graph->databaseBytes)
            return fail(ErrorCode::Busy, "The compilation database changed during binding analysis");
        auto request =
            engine::bindingMapRequest(entries, graph->engineRoot, capture.database.directory, translationUnit);
        if (!request)
            return std::unexpected(request.error());
        auto requestPath = io::joinPath(capture.stateDirectory,
                                        "binding-" + capture.requestTag + "-" + std::to_string(++sequence) + ".json");
        if (!requestPath)
            return std::unexpected(requestPath.error());
        auto written = capture.fs->writeFileAtomic(*requestPath, request->dump());
        if (!written)
            return std::unexpected(written.error());
        io::ProcessSpec spec;
        spec.executable = capture.tool;
        spec.arguments = {"--binding-map", *requestPath};
        spec.workingDirectory = graph->engineRoot;
        auto output = capture.current()
                          ? io::runToCompletion(*capture.launcher, spec, {}, std::chrono::minutes(2), capture.stop)
                          : Result<io::ProcessResult>(fail(ErrorCode::Cancelled));
        (void)capture.fs->remove(*requestPath);
        if (!output)
            return std::unexpected(output.error());
        if (output->exitCode != 0 || output->stdoutBytes.size() > 32U * 1024U * 1024U)
            return fail(ErrorCode::IoError, "The actual binding analysis tool failed or exceeded its response limit");
        auto raw = rpc::parseJson(output->stdoutBytes);
        if (!raw)
            return std::unexpected(raw.error());
        auto report = engine::parseBindingMapReport(*request, *raw);
        if (!report)
            return std::unexpected(report.error());
        if (!report->mappings.empty() && report->sourceInputs.empty())
            return fail(ErrorCode::Unsupported, "The binding tool did not report actual parsed source-input digests");
        graph->embeddedLuaProfileVerified &= report->embeddedLuaProfileVerified;
        graph->embeddedLuaRefusals.insert(graph->embeddedLuaRefusals.end(),
            report->embeddedLuaRefusals.begin(), report->embeddedLuaRefusals.end());
        for (const auto& chunk : report->embeddedLua)
            if (std::ranges::find(graph->embeddedLua, chunk) == graph->embeddedLua.end())
                graph->embeddedLua.push_back(chunk);
        for (const auto& input : report->sourceInputs) {
            if (!inside(graph->engineRoot, input.path))
                return fail(ErrorCode::PermissionDenied, "The AST source manifest escaped its admitted checkout");
            const auto physical = capture.fs->physicalPath(input.path);
            if (!physical || lang::documentKey(*physical) != lang::documentKey(input.path))
                return fail(ErrorCode::Busy, "An AST source input changed physical identity before capture");
            auto [where, inserted] = graph->inputHashes.emplace(*physical, input.sha256);
            if (graph->inputHashes.size() > 100000)
                return fail(ErrorCode::OutOfRange, "The parsed AST source manifest exceeds its bounded file count");
            if (!inserted && where->second != input.sha256)
                return fail(ErrorCode::Busy, "Native source inputs changed between translation-unit analyses");
        }
        mappings.insert(mappings.end(), report->mappings.begin(), report->mappings.end());
        issues.insert(issues.end(), report->issues.begin(), report->issues.end());
    }
    auto index = engine::BindingIndex::create(graph->engineRoot, capture.revision, *schema, mappings, issues);
    if (!index)
        return std::unexpected(index.error());
    graph->index = std::make_shared<const engine::BindingIndex>(std::move(*index));
    if (!graph->verify(capture))
        return fail(ErrorCode::Busy, "The AST, schema binary, database or project identity changed during capture");
    std::size_t bytes = 0;
    std::vector<std::string> files = capture.candidates;
    for (const auto& [file, hash] : graph->inputHashes) {
        static_cast<void>(hash);
        files.push_back(file);
    }
    for (const auto& mapping : mappings) {
        files.push_back(mapping.declaration.path);
        if (mapping.definition)
            files.push_back(mapping.definition->path);
    }
    for (const auto& document : capture.openDocuments)
        files.push_back(document.path);
    files.push_back(graph->metadataPath);
    if (auto admitted = captureOpenDocumentReceipts(*capture.fs, *graph, capture.openDocuments, capture.stop); !admitted)
        return std::unexpected(admitted.error());
    for (const auto& file : files) {
        if (!languageFile(file) && !graph->inputHashes.contains(file))
            continue;
        if (!capture.current())
            return fail(ErrorCode::Cancelled, "The binding capture changed while reading documents");
        if (auto added = captureDocumentSnapshot(*capture.fs, *graph, capture.openDocuments, file, bytes); !added)
            return std::unexpected(added.error());
    }
    if (!graph->verify(capture))
        return fail(ErrorCode::Busy, "Binding documents changed while their immutable snapshots were captured");
    return graph;
}
} // namespace frost::app::binding_work
