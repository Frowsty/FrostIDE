// SPDX-License-Identifier: Apache-2.0
// FEC-13 mandatory real mapper/clangd/LuaLS oracle, plus test-only application in a verified disposable engine clone.
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include "binding_corpus_support.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

namespace frost::engine {
namespace {
namespace corpus = frost::test::binding;
using corpus::Json;
using namespace std::chrono_literals;

Result<Json> cli(corpus::Evidence& evidence, std::string tag, const std::string& program, const std::string& project,
                 std::string command, Json params = Json::object()) {
    auto output =
        evidence.command(std::move(tag), program,
                         {command, "--params", params.dump(), "--project", project, "--log-level", "error"}, project);
    if (!output)
        return std::unexpected(output.error());
    auto parsed = rpc::parseJson(output->stdoutBytes);
    if (!parsed || parsed->boolAt("ok") != true || !parsed->find("result") || output->exitCode != 0)
        return fail(ErrorCode::IoError, "actual engine command refused; inspect retained stdout/stderr");
    return *parsed->find("result");
}

struct Analysis {
    std::shared_ptr<const BindingIndex> index;
    std::vector<BindingDocumentCapture> documents;
    std::vector<BindingQuerySeed> seeds;
    std::map<std::string, std::string> originalFiles;
    std::string usr, metadataPath, scriptPath;
    BindingProviderCapture nativeCapture, luaCapture;
    std::unique_ptr<corpus::Environment> environment;
};

Result<void> addDocument(Analysis& analysis, const std::string& path, BindingDocumentRole role) {
    auto bytes = io::realFileSystem().readFile(path);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto buffer = text::TextBuffer::fromUtf8(*bytes);
    if (!buffer)
        return std::unexpected(buffer.error());
    analysis.originalFiles[path] = *bytes;
    analysis.documents.push_back({path, buffer->snapshot(), std::nullopt, role});
    return {};
}

Result<void> luaDocuments(Analysis& analysis, const std::string& directory, std::size_t& count) {
    auto entries = io::realFileSystem().listDirectory(directory);
    if (!entries)
        return std::unexpected(entries.error());
    for (const auto& entry : *entries) {
        if (++count > 100000)
            return fail(ErrorCode::OutOfRange, "owned corpus project exceeds bounded file inventory");
        const auto path = io::joinPath(directory, entry.name).value_or("");
        if (entry.type == io::FileType::Directory) {
            if (entry.name == ".frost" || entry.name == ".git")
                continue;
            auto walked = luaDocuments(analysis, path, count);
            if (!walked)
                return walked;
        } else if (entry.type == io::FileType::Regular && entry.name.ends_with(".lua")) {
            auto captured = addDocument(analysis, path, BindingDocumentRole::UserSource);
            if (!captured)
                return captured;
        }
    }
    return {};
}

Result<Analysis> analyze(corpus::Evidence& evidence, const std::string& engineRoot, const std::string& program,
                         const std::string& project, const std::string& identity) {
    auto& fs = io::realFileSystem();
    auto rawSchema = cli(evidence, "baseline-schema", program, project, "schema.export");
    if (!rawSchema)
        return std::unexpected(rawSchema.error());
    auto schema = parseSchemaDocument(*rawSchema);
    if (!schema)
        return std::unexpected(schema.error());
    const auto selected =
        std::ranges::find_if(schema->luaEntries, [&](const auto& entry) { return entry.qualifiedName == identity; });
    if (selected == schema->luaEntries.end() || !selected->source)
        return fail(ErrorCode::NotFound, "actual exported schema does not contain required source-backed binding");
    const auto tu = fs.canonicalPath(io::joinPath(engineRoot, selected->source->file).value_or(""));
    const auto cdb =
        fs.canonicalPath(engineRoot + "/build/" + FROST_CORPUS_FROSTENGINE_PRESET + "/compile_commands.json");
    if (!tu || !cdb)
        return fail(ErrorCode::NotFound, "actual configured binding TU/CDB unavailable");
    auto cdbBytes = fs.readFile(*cdb);
    if (!cdbBytes)
        return std::unexpected(cdbBytes.error());
    auto retainedCdb = evidence.write("captured-compile-commands.json", *cdbBytes);
    if (!retainedCdb)
        return std::unexpected(retainedCdb.error());
    auto commands = rpc::parseJson(*cdbBytes);
    if (!commands || !commands->isArray())
        return fail(ErrorCode::Corrupt, "configured engine CDB is not an array");
    std::size_t contexts = 0;
    for (const auto& command : commands->items()) {
        const auto file = command.stringAt("file");
        const auto absolute = io::isAbsolutePath(file) ? std::string(file)
                                                       : io::joinPath(command.stringAt("directory"), file).value_or("");
        if (lang::documentKey(absolute) == lang::documentKey(*tu))
            ++contexts;
    }
    if (contexts != 1)
        return fail(ErrorCode::Busy, "required actual engine TU has no unique configured compilation context");
    auto request = bindingMapRequest(*schema, engineRoot, io::parentPath(*cdb), *tu);
    if (!request)
        return std::unexpected(request.error());
    const auto requestPath = io::joinPath(evidence.root, "binding-request.json").value_or("");
    auto saved = fs.writeFileAtomic(requestPath, request->dump());
    if (!saved)
        return std::unexpected(saved.error());
    auto mapped =
        evidence.command("actual-binding-map", FROST_CLANG_BINARY, {"--binding-map", requestPath}, engineRoot, 10min);
    if (!mapped)
        return std::unexpected(mapped.error());
    auto response = rpc::parseJson(mapped->stdoutBytes);
    if (!response || mapped->exitCode != 0 || response->boolAt("translationUnitParsed") != true)
        return fail(ErrorCode::Unsupported,
                    "actual pinned mapper cannot prove required AST/context provenance; see evidence");
    auto report = parseBindingMapReport(*request, *response);
    if (!report)
        return std::unexpected(report.error());
    const auto* context = response->find("compilationContext");
    const auto* originalArguments = context ? context->find("originalArguments") : nullptr;
    const auto* analysisArguments = context ? context->find("analysisArguments") : nullptr;
    const auto* textualHeaders = context ? context->find("textualPchHeaders") : nullptr;
    if (!context || !context->stringAt("error").empty() || !originalArguments || !originalArguments->isArray() ||
        originalArguments->items().empty() || !analysisArguments || !analysisArguments->isArray() ||
        analysisArguments->items().empty() || !textualHeaders || !textualHeaders->isArray())
        return fail(ErrorCode::Corrupt, "actual mapper omitted its original/derived compilation-context evidence");
    const bool originalUsedMsvcPch = std::ranges::any_of(originalArguments->items(), [](const Json& argument) {
        const auto* value = argument.asString();
        return value && (value->starts_with("/Yu") || value->starts_with("-Yu"));
    });
    if (originalUsedMsvcPch && textualHeaders->items().empty())
        return fail(ErrorCode::Corrupt, "actual MSVC PCH context has no proven textual header source");
    for (const auto& header : textualHeaders->items()) {
        const auto* path = header.asString();
        if (!path || !std::ranges::any_of(report->sourceInputs, [&](const auto& input) {
                return lang::documentKey(input.path) == lang::documentKey(*path);
            }))
            return fail(ErrorCode::Corrupt, "actual textual PCH header is absent from parsed-buffer SHA256 evidence");
    }
    auto index = BindingIndex::create(engineRoot, 1, *schema, report->mappings, report->issues);
    if (!index)
        return std::unexpected(index.error());
    const auto* binding = index->findLua(identity);
    if (!binding || binding->resolution != BindingResolution::Resolved || binding->native.size() != 1 ||
        binding->native.front().kind != "function")
        return fail(ErrorCode::Unsupported, "required real binding does not have one actual non-lambda native target");
    Analysis analysis;
    analysis.usr = binding->native.front().usr;
    analysis.index = std::make_shared<const BindingIndex>(std::move(*index));
    analysis.originalFiles[*cdb] = *cdbBytes;
    for (const auto& input : report->sourceInputs) {
        auto bytes = fs.readFile(input.path);
        if (!bytes || io::sha256Hex(*bytes) != input.sha256)
            return fail(ErrorCode::Busy, "actual AST input changed after the mapper captured its buffer");
        auto added = addDocument(analysis, input.path,
                                 lang::documentKey(input.path) == lang::documentKey(*tu)
                                     ? BindingDocumentRole::UserSource
                                     : BindingDocumentRole::ExternalReadOnly);
        if (!added)
            return std::unexpected(added.error());
    }
    auto generated = generateEngineLuaMetadata(*schema);
    analysis.metadataPath = io::joinPath(project, ".frost/lua-api/frost_schema.lua").value_or("");
    auto metaParent = fs.createDirectories(io::parentPath(analysis.metadataPath));
    if (!metaParent)
        return std::unexpected(metaParent.error());
    auto metaWritten = fs.writeFileAtomic(analysis.metadataPath, generated.text);
    if (!metaWritten)
        return std::unexpected(metaWritten.error());
    auto metaCaptured = addDocument(analysis, analysis.metadataPath, BindingDocumentRole::GeneratedEngineMetadata);
    if (!metaCaptured)
        return std::unexpected(metaCaptured.error());
    auto anchors = captureBindingLuaAnchors(generated, analysis.metadataPath, analysis.documents.back().snapshot);
    if (!anchors)
        return std::unexpected(anchors.error());
    analysis.scriptPath = io::joinPath(project, "Assets/Scripts/Hero.lua").value_or("");
    std::size_t files = 0;
    auto capturedLua = luaDocuments(analysis, project, files);
    if (!capturedLua)
        return std::unexpected(capturedLua.error());
    analysis.environment = std::make_unique<corpus::Environment>();
    auto& environment = *analysis.environment;
    if (!environment.ok())
        return fail(ErrorCode::Internal, "could not initialize real-server test scheduler");
    auto clangd = lang::findClangd(fs, lang::defaultClangdSearch());
    if (!clangd || clangd->origin == lang::ClangdOrigin::Path)
        return fail(ErrorCode::NotFound, "mandatory pinned clangd is unavailable");
    lang::ClangdServiceOptions nativeOptions;
    nativeOptions.clangdPath = clangd->path;
    nativeOptions.workspaceRoot = engineRoot;
    nativeOptions.launch.trusted = true;
    nativeOptions.launch.jobs = 0;
    nativeOptions.launch.backgroundIndex = true; // Full real CDB, actual cold/warm index, no copied shards.
    nativeOptions.launch.extraArguments = {"--enable-config=false"};
    nativeOptions.compileCommands = std::make_shared<corpus::Database>(*cdb);
    environment.native = std::make_unique<lang::ClangdService>(io::realProcessLauncher(), environment.context(),
                                                               std::move(nativeOptions));
    lang::setResourceGovernor(*environment.native, std::make_shared<sched::ResourceGovernor>());
    auto started = environment.native->start();
    if (!started || !corpus::wait([&] { return environment.native->state() == lang::ClangdState::Running; }))
        return fail(ErrorCode::IoError, "pinned clangd failed initialization");
    const auto sdk = test::environment("FROST_SDK");
#ifdef _WIN32
    constexpr auto luaExecutable = "lua-language-server.exe";
#else
    constexpr auto luaExecutable = "lua-language-server";
#endif
    const auto luaPath =
        io::joinPath(sdk, "luals-" + std::string(lang::pinnedLuaLanguageServerVersion()) + "/bin/" + luaExecutable);
    if (!luaPath)
        return fail(ErrorCode::NotFound, "pinned LuaLS SDK path unavailable");
    auto lua = lang::findLuaLanguageServer(fs, {.configuredPath = *luaPath});
    if (!lua)
        return std::unexpected(lua.error());
    lang::LuaServiceOptions luaOptions;
    luaOptions.serverPath = *lua;
    luaOptions.workspaceRoot = project;
    luaOptions.trusted = true;
    luaOptions.arguments = lang::luaLanguageServerArguments(evidence.root + "/luals-state").value();
    luaOptions.configuration = lang::luaDefaults(schema->luaVersion);
    luaOptions.configuration.set(
        "workspace", Json::object()
                         .set("checkThirdParty", false)
                         .set("library", Json::array({std::string(io::parentPath(analysis.metadataPath))})));
    environment.lua =
        std::make_unique<lang::LuaService>(io::realProcessLauncher(), environment.context(), std::move(luaOptions));
    auto luaStarted = environment.lua->start();
    if (!luaStarted || !corpus::wait([&] { return environment.lua->state() == lang::LuaState::Running; }))
        return fail(ErrorCode::IoError, "pinned LuaLS failed initialization");
    const auto nativeCapabilities = environment.native->capabilities();
    const auto luaCapabilities = environment.lua->capabilities();
    if (!nativeCapabilities || !luaCapabilities ||
        nativeCapabilities->serverVersion.find("23.1.4") == std::string::npos ||
        luaCapabilities->serverVersion.find(lang::pinnedLuaLanguageServerVersion()) == std::string::npos)
        return fail(ErrorCode::Unsupported, "actual language-server identities do not match the SDK pins");
    for (auto& document : analysis.documents) {
        if (lang::documentKey(document.path) == lang::documentKey(*tu)) {
            auto opened = environment.native->openDocument(document.path, document.snapshot);
            if (!opened)
                return std::unexpected(opened.error());
            document.lspVersion = environment.native->documentVersion(document.path);
        } else if (document.path.ends_with(".lua")) {
            auto opened = environment.lua->openDocument(document.path, document.snapshot);
            if (!opened)
                return std::unexpected(opened.error());
            document.lspVersion = environment.lua->documentVersion(document.path);
        }
    }
    if (!corpus::wait([&] { return lang::indexMetrics(*environment.native).completed; }, 15min))
        return fail(ErrorCode::Timeout, "full configured engine index did not finish");
    const auto metrics = lang::indexMetrics(*environment.native);
    if (metrics.governorRssCapBytes == 0 || metrics.peakRssBytes >= metrics.governorRssCapBytes)
        return fail(ErrorCode::OutOfRange, "full engine index exceeded the actual ResourceGovernor cap");
    const auto nativePid = environment.native->serverPid();
    const auto luaPid = environment.lua->serverPid();
    if (!nativePid || !luaPid)
        return fail(ErrorCode::IoError, "actual provider process disappeared before identity capture");
    analysis.nativeCapture = {BindingLanguage::Native, clangd->path, static_cast<std::uint64_t>(*nativePid),
                              environment.native->encoding()};
    analysis.luaCapture = {BindingLanguage::Lua, *lua, static_cast<std::uint64_t>(*luaPid),
                           environment.lua->encoding()};
    auto seeds =
        bindingUsageSeeds(*analysis.index, analysis.usr, analysis.nativeCapture, analysis.luaCapture, *anchors);
    if (!seeds)
        return std::unexpected(seeds.error());
    analysis.seeds = std::move(*seeds);
    for (auto& seed : analysis.seeds) {
        const auto document = std::ranges::find_if(analysis.documents, [&](const auto& captured) {
            return lang::documentKey(captured.path) == lang::documentKey(seed.anchor.path);
        });
        if (document == analysis.documents.end())
            return fail(ErrorCode::NotFound, "semantic seed has no captured real input");
        seed.anchorSnapshot = document->snapshot;
    }
    const auto script = std::ranges::find_if(analysis.documents, [&](const auto& document) {
        return lang::documentKey(document.path) == lang::documentKey(analysis.scriptPath);
    });
    const auto luaSeed = std::ranges::find_if(analysis.seeds, [&](const auto& seed) {
        return seed.provider.language == BindingLanguage::Lua && seed.luaQualifiedName == identity;
    });
    if (script == analysis.documents.end() || luaSeed == analysis.seeds.end())
        return fail(ErrorCode::NotFound, "actual user script or generated semantic Lua anchor unavailable");
    const auto scriptBytes = script->snapshot.text();
    const auto call = scriptBytes.find(identity);
    if (call == std::string::npos)
        return fail(ErrorCode::NotFound, "owned test input does not call the selected actual API");
    const auto member = identity.rfind('.');
    const auto callOffset = ByteOffset{call + (member == std::string::npos ? 0 : member + 1)};
    const auto definitionParams =
        Json::object()
            .set("textDocument", Json::object().set("uri", lang::pathToUri(script->path).value()))
            .set("position",
                 lang::toJson(lang::toLspPosition(script->snapshot, callOffset, environment.lua->encoding())));
    const auto anchorPosition =
        lang::toLspPosition(*luaSeed->anchorSnapshot, luaSeed->anchor.bytes.begin, environment.lua->encoding());
    if (!corpus::wait([&] {
            auto definition = environment.run(environment.lua->request("textDocument/definition", definitionParams));
            if (!definition)
                return false;
            const auto matches = [&](const Json& location) {
                const auto uri = location.stringAt("uri", location.stringAt("targetUri"));
                auto path = lang::uriToPath(uri);
                const auto* range = location.find("targetSelectionRange");
                if (!range)
                    range = location.find("range");
                const auto parsed = range ? lang::parseRange(*range) : std::nullopt;
                return path && lang::documentKey(*path) == lang::documentKey(analysis.metadataPath) && parsed &&
                       parsed->start <= anchorPosition && anchorPosition <= parsed->end;
            };
            if (definition->isObject())
                return matches(*definition);
            return definition->isArray() && std::ranges::any_of(definition->items(), matches);
        }))
        return fail(ErrorCode::Timeout,
                    "pinned LuaLS never resolved the actual user call to its exact generator-recorded declaration");
    if (fs.readFile(*cdb).value_or("") != *cdbBytes)
        return fail(ErrorCode::Busy, "configured CDB changed while establishing actual providers");
    return analysis;
}

BindingQueryAdmission admitted(Analysis& analysis) {
    return [&analysis](const BindingProviderCapture& provider) {
        const auto& environment = *analysis.environment;
        if (provider == analysis.nativeCapture) {
            const auto pid = environment.native->serverPid();
            return environment.native->state() == lang::ClangdState::Running && pid &&
                   static_cast<std::uint64_t>(*pid) == provider.generation && environment.native->restarts() == 0;
        }
        const auto pid = environment.lua->serverPid();
        return provider == analysis.luaCapture && environment.lua->state() == lang::LuaState::Running && pid &&
               static_cast<std::uint64_t>(*pid) == provider.generation && environment.lua->restarts() == 0;
    };
}

void unchanged(const Analysis& analysis) {
    for (const auto& [path, bytes] : analysis.originalFiles) {
        auto current = io::realFileSystem().readFile(path);
        ASSERT_TRUE(current) << path;
        EXPECT_EQ(io::sha256Hex(*current), io::sha256Hex(bytes)) << "read-only preview wrote " << path;
    }
}

std::string key(std::string_view provider, std::string_view uri, const Json& range) {
    const auto path = lang::uriToPath(uri);
    const auto parsed = lang::parseRange(range);
    if (!path || !parsed)
        return "invalid raw provider location";
    return std::format("{} {} {}:{}-{}:{}", provider, lang::documentKey(*path), parsed->start.line,
                       parsed->start.character, parsed->end.line, parsed->end.character);
}

Result<std::vector<BindingUsage>> usages(Analysis& analysis, corpus::Evidence& evidence) {
    auto& environment = *analysis.environment;
    corpus::RecordingProvider native(*environment.native), lua(*environment.lua);
    std::vector<BindingProviderRequest> requests;
    for (const auto& seed : analysis.seeds)
        requests.push_back({seed, seed.provider.language == BindingLanguage::Native ? &native : &lua});
    auto actual = environment.run(queryBindingUsages(requests, analysis.documents, admitted(analysis)));
    if (!actual)
        return std::unexpected(actual.error());
    std::set<std::string> expected;
    Json raw = Json::array();
    for (const auto* recording : {&native, &lua}) {
        auto& server = recording == &native ? static_cast<lang::ILspRequester&>(*environment.native)
                                            : static_cast<lang::ILspRequester&>(*environment.lua);
        const auto& provider = recording == &native ? analysis.nativeCapture : analysis.luaCapture;
        for (const auto& request : recording->records) {
            raw.push(Json::object()
                         .set("provider", provider.id)
                         .set("method", request.method)
                         .set("params", request.params)
                         .set("response", request.result));
            if (request.method != "textDocument/references")
                continue;
            auto oracle = environment.run(server.request(request.method, request.params));
            if (!oracle || !oracle->isArray())
                return fail(ErrorCode::Corrupt, "actual raw provider reference oracle unavailable");
            raw.push(Json::object()
                         .set("provider", provider.id)
                         .set("method", "oracle:textDocument/references")
                         .set("params", request.params)
                         .set("response", *oracle));
            for (const auto& location : oracle->items()) {
                const auto* range = location.find("range");
                if (!range || location.stringAt("uri").empty())
                    return fail(ErrorCode::Corrupt, "actual reference oracle has malformed location");
                expected.insert(key(provider.id, location.stringAt("uri"), *range));
            }
        }
    }
    auto saved = evidence.write("actual-reference-responses.json", raw.dump());
    if (!saved)
        return std::unexpected(saved.error());
    std::set<std::string> observed;
    for (const auto& usage : *actual) {
        const lang::LspRange range{
            lang::toLspPosition(usage.capturedSnapshot, usage.location.bytes.begin, usage.provider.encoding),
            lang::toLspPosition(usage.capturedSnapshot, usage.location.bytes.end, usage.provider.encoding)};
        observed.insert(key(usage.provider.id, lang::pathToUri(usage.location.path).value(), lang::toJson(range)));
    }
    if (observed != expected)
        return fail(ErrorCode::Corrupt,
                    "combined usages differ from independently requested raw actual provider locations");
    return actual;
}

Result<BindingSelectedRenamePreview> preview(Analysis& analysis, corpus::Evidence& evidence, std::string luaIdentity,
                                             std::string luaName, std::string nativeName) {
    corpus::RecordingProvider native(*analysis.environment->native), lua(*analysis.environment->lua);
    std::optional<SelectedLuaBindingRename> selected;
    std::vector<BindingRenameRequest> nativeRenames;
    for (const auto& seed : analysis.seeds) {
        if (seed.provider.language == BindingLanguage::Native)
            nativeRenames.push_back({{seed, &native}, nativeName});
        else if (seed.luaQualifiedName == luaIdentity)
            selected = SelectedLuaBindingRename{{seed, &lua}, luaName};
    }
    if (!selected)
        return fail(ErrorCode::NotFound, "actual selected Lua declaration seed unavailable");
    auto result = analysis.environment->run(querySelectedBindingRenamePreview(analysis.index, *selected, nativeRenames,
                                                                              analysis.documents, admitted(analysis)));
    Json raw = Json::array();
    for (const auto* provider : {&native, &lua})
        for (const auto& request : provider->records)
            raw.push(Json::object()
                         .set("method", request.method)
                         .set("params", request.params)
                         .set("response", request.result));
    auto saved = evidence.write("actual-rename-responses.json", raw.dump());
    if (!saved)
        return std::unexpected(saved.error());
    if (!result)
        return std::unexpected(result.error()); // Missing LuaLS @meta rename support is a real acceptance failure.
    Json manifest = Json::array();
    for (const auto& file : result->sourceFiles) {
        Json edits = Json::array();
        for (const auto& edit : file.edits)
            edits.push(Json::object()
                           .set("offset", edit.location.bytes.begin.value())
                           .set("length", edit.location.bytes.size())
                           .set("before", edit.before)
                           .set("after", edit.after));
        manifest.push(Json::object()
                          .set("path", file.path)
                          .set("snapshotVersion", file.snapshot.version())
                          .set("sha256", io::sha256Hex(file.snapshot.text()))
                          .set("edits", edits)
                          .set("preview", file.preview));
    }
    auto retained = evidence.write("reviewed-preview.json", manifest.dump());
    auto generated = evidence.write("reviewed-generated-metadata.lua", result->generatedResource.regenerated.text);
    auto coverage =
        evidence.write("reviewed-preview-coverage.json",
                       Json::object()
                           .set("scope", "provider-indexed")
                           .set("embeddedLanguageReferencesVerified", result->embeddedLanguageReferencesVerified)
                           .set("applicationSafe", result->applicationSafe)
                           .dump());
    if (!retained || !generated || !coverage)
        return fail(ErrorCode::IoError, "could not retain exact reviewed preview evidence");
    return result;
}

bool beneath(std::string_view path, std::string_view root) {
    const auto prefix = lang::documentKey(root);
    const auto key = lang::documentKey(path);
    return key.size() > prefix.size() && key.starts_with(prefix) &&
           (key[prefix.size()] == '/' || key[prefix.size()] == '\\');
}

Result<void> testOnlyApply(const BindingSelectedRenamePreview& preview, const std::string& clone,
                           const std::string& project) {
    auto& fs = io::realFileSystem();
    for (const auto& file : preview.sourceFiles) {
        if (!beneath(file.path, clone) && !beneath(file.path, project))
            return fail(ErrorCode::PermissionDenied, "test-only oracle refuses an edit outside its disposable roots");
        auto disk = fs.readFile(file.path);
        if (!disk || *disk != file.snapshot.text())
            return fail(ErrorCode::Busy, "test-only oracle refuses changed captured source bytes");
        for (const auto& edit : file.edits)
            if (!edit.capturedSnapshot.sharesStorageWith(file.snapshot) ||
                edit.snapshotVersion != file.snapshot.version() ||
                lang::documentKey(edit.location.path) != lang::documentKey(file.path) ||
                edit.location.bytes.begin > edit.location.bytes.end || edit.location.bytes.end > file.snapshot.size() ||
                file.snapshot.text(edit.location.bytes) != edit.before)
                return fail(ErrorCode::Busy, "test-only oracle refuses mismatched exact preview provenance");
    }
    // All documents are checked before the first write. This is a fixture oracle, never product apply.
    for (const auto& file : preview.sourceFiles) {
        auto written = fs.writeFileAtomic(file.path, file.preview);
        if (!written)
            return written;
    }
    return {};
}

TEST(BindingQueriesCorpus, PinnedEngineUsagesMatchRawProvidersAndEmbeddedLuaRemainsExplicitlyUnverified) {
    test::RealEngineBuild pinned;
    auto ready = pinned.prepare();
    ASSERT_TRUE(ready) << ready.error().message();
    corpus::Evidence evidence;
    const auto project = io::joinPath(evidence.root, "LuaProject").value();
    ASSERT_TRUE(pinned.copyProject(project));
    ASSERT_TRUE(test::installAcceptanceScript(project, "local Probe = {}\nfunction Probe:OnCreate()\n  local frame = "
                                                       "Application.GetFrameCount()\nend\nreturn Probe\n"));
    auto analysis = analyze(evidence, pinned.corpus, pinned.program, project, "Application.GetFrameCount");
    ASSERT_TRUE(analysis) << analysis.error().message();
    auto actual = usages(*analysis, evidence);
    ASSERT_TRUE(actual) << actual.error().message();
    EXPECT_TRUE(std::ranges::any_of(
        *actual, [](const auto& usage) { return usage.provider.language == BindingLanguage::Native; }));
    EXPECT_TRUE(std::ranges::any_of(*actual,
                                    [](const auto& usage) { return usage.provider.language == BindingLanguage::Lua; }));
    auto unverifiedPreview =
        preview(*analysis, evidence, "Application.GetFrameCount", "Fec13UnverifiedFrameCount", "Fec13UnverifiedNative");
    ASSERT_TRUE(unverifiedPreview) << unverifiedPreview.error().message();
    EXPECT_FALSE(unverifiedPreview->embeddedLanguageReferencesVerified);
    EXPECT_FALSE(unverifiedPreview->applicationSafe);
    const auto embeddedPath =
        io::joinPath(pinned.corpus, "Frost-Tests/src/Scripting/ScriptServiceBindingsTests.cpp").value();
    const auto embedded = io::realFileSystem().readFile(embeddedPath);
    ASSERT_TRUE(embedded);
    ASSERT_NE(embedded->find("assert(Application.GetFrameCount() == 0)"), std::string::npos);
    EXPECT_TRUE(std::ranges::none_of(*actual, [&](const auto& usage) {
        return lang::documentKey(usage.location.path) == lang::documentKey(embeddedPath);
    }));
    EXPECT_TRUE(std::ranges::none_of(unverifiedPreview->sourceFiles, [&](const auto& file) {
        return lang::documentKey(file.path) == lang::documentKey(embeddedPath);
    }));
    ASSERT_TRUE(evidence.write(
        "unverified-embedded-lua.json",
        Json::object()
            .set("path", embeddedPath)
            .set("sha256", io::sha256Hex(*embedded))
            .set("reason",
                 "Lua embedded in C++ strings is outside these providers; no application-safe or rebuild claim")
            .dump()));
    unchanged(*analysis);
    EXPECT_EQ(io::realFileSystem().readFile(embeddedPath).value_or(""), *embedded);
}

TEST(BindingQueriesCorpus, ActualReviewedFixturePreviewRebuildsAndReexportsInDisposableVerifiedClone) {
    test::RealEngineBuild pinned;
    auto ready = pinned.prepare();
    ASSERT_TRUE(ready) << ready.error().message();
    corpus::Evidence evidence;
    auto clone = corpus::disposableClone(evidence, pinned.corpus);
    ASSERT_TRUE(clone) << clone.error().message();
    const auto sourcePath = io::joinPath(*clone, "Frost/src/Frost/Scripting/Bindings/ApplicationBindings.cpp").value();
    auto source = io::realFileSystem().readFile(sourcePath);
    ASSERT_TRUE(source);
    const std::string marker = "void RegisterApplicationApi(ScriptApiBuilder& api)";
    const auto at = source->find(marker);
    ASSERT_NE(at, std::string::npos);
    ASSERT_EQ(source->find(marker, at + marker.size()), std::string::npos);
    const auto brace = source->find('{', at + marker.size());
    ASSERT_NE(brace, std::string::npos);
    source->insert(brace + 1, "\n\t\tapi.Namespace(\"Fec13Corpus\", \"Documented real-engine corpus fixture.\")\n"
                              "\t\t\t.RawFunction(\"Probe\", &Fec13OriginalNative, { .Signature = \"Probe(): "
                              "integer\", .Description = \"Returns the fixture value.\" });\n");
    source->insert(
        at, "static int Fec13OriginalNative(lua_State* L)\n\t{\n\t\tlua_pushinteger(L, 73);\n\t\treturn 1;\n\t}\n\n\t");
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(sourcePath, *source));
    ASSERT_TRUE(evidence.write("documented-fixture-baseline.cpp", *source));
    auto built = corpus::build(evidence, *clone, false);
    ASSERT_TRUE(built) << built.error().message();
    auto program = findEngineEditorProgram(io::realFileSystem(), *clone, FROST_CORPUS_FROSTENGINE_PRESET);
    ASSERT_TRUE(program);
    const auto project = io::joinPath(evidence.root, "LuaProject").value();
    ASSERT_TRUE(pinned.copyProject(project));
    ASSERT_TRUE(test::installAcceptanceScript(
        project, "local Probe = {}\nfunction Probe:OnCreate()\n  Test.Case('real selected binding', function()\n"
                 "    Test.Equal(Fec13Corpus.Probe(), 73)\n  end)\nend\nreturn Probe\n"));
    auto analysis = analyze(evidence, *clone, *program, project, "Fec13Corpus.Probe");
    ASSERT_TRUE(analysis) << analysis.error().message();
    auto actualUsages = usages(*analysis, evidence);
    ASSERT_TRUE(actualUsages) << actualUsages.error().message();
    auto reviewed = preview(*analysis, evidence, "Fec13Corpus.Probe", "ReviewedProbe", "Fec13ReviewedNative");
    ASSERT_TRUE(reviewed) << reviewed.error().message();
    EXPECT_FALSE(reviewed->embeddedLanguageReferencesVerified);
    EXPECT_FALSE(reviewed->applicationSafe); // The isolated fixture build validates only this test-only application.
    EXPECT_EQ(reviewed->renamedLuaIdentity, "Fec13Corpus.ReviewedProbe");
    const auto* bound = analysis->index->findLua("Fec13Corpus.Probe");
    ASSERT_NE(bound, nullptr);
    ASSERT_EQ(bound->native.size(), 1U);
    const auto& mapping = bound->native.front();
    const auto nativeFile = std::ranges::find_if(reviewed->sourceFiles, [&](const auto& file) {
        return lang::documentKey(file.path) == lang::documentKey(mapping.declaration.path);
    });
    ASSERT_NE(nativeFile, reviewed->sourceFiles.end());
    EXPECT_TRUE(std::ranges::any_of(nativeFile->edits, [&](const auto& edit) {
        return edit.location.bytes == mapping.declaration.bytes && edit.after == "Fec13ReviewedNative" &&
               edit.origin == BindingPreviewOrigin::LanguageServer;
    }));
    ASSERT_TRUE(mapping.nameLiteral && mapping.docSignatureLiteral);
    EXPECT_TRUE(std::ranges::any_of(nativeFile->edits, [&](const auto& edit) {
        return edit.location.bytes == mapping.nameLiteral->range.bytes && edit.after == "\"ReviewedProbe\"" &&
               edit.origin == BindingPreviewOrigin::RegistrationLiteral;
    }));
    EXPECT_TRUE(std::ranges::any_of(nativeFile->edits, [&](const auto& edit) {
        return edit.location.bytes == mapping.docSignatureLiteral->literal.range.bytes &&
               edit.after == "\"ReviewedProbe(): integer\"" && edit.origin == BindingPreviewOrigin::RegistrationLiteral;
    }));
    unchanged(*analysis);
    analysis->environment.reset(); // End every LSP process before building; only one heavy leg at a time.
    auto applied = testOnlyApply(*reviewed, *clone, project);
    ASSERT_TRUE(applied) << applied.error().message();
    auto rebuilt = corpus::build(evidence, *clone, true);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message();
    auto raw = cli(evidence, "rebuilt-schema", *program, project, "schema.export");
    ASSERT_TRUE(raw) << raw.error().message();
    auto schema = parseSchemaDocument(*raw);
    ASSERT_TRUE(schema) << schema.error().message();
    const auto actualMetadata = generateEngineLuaMetadata(*schema);
    ASSERT_TRUE(evidence.write("actual-rebuilt-generated-metadata.lua", actualMetadata.text));
    EXPECT_EQ(actualMetadata.text, reviewed->generatedResource.regenerated.text);
    auto refreshed = cli(evidence, "rebuilt-assets-refresh", *program, project, "assets.refresh");
    ASSERT_TRUE(refreshed) << refreshed.error().message();
    auto run = cli(evidence, "rebuilt-real-lua-test", *program, project, "test.run",
                   Json::object()
                       .set("scenes", Json::array({"Assets/Tests/FeatureTest.fscene"}))
                       .set("frames", 2)
                       .set("mode", "headless"));
    ASSERT_TRUE(run) << run.error().message();
    EXPECT_EQ(run->boolAt("passed"), true) << run->dump();
}

} // namespace
} // namespace frost::engine
