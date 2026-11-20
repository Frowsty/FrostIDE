// SPDX-License-Identifier: Apache-2.0
// Strict local registry values and actual launch-time trust/pin admission; no package installer.
#include "frost/lang/language_registry.hpp"

#include "frost/io/path.hpp"
#include "frost/io/process_sandbox.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"
#include "frost/lang/workspace_edit.hpp"

#include <algorithm>
#include <set>
namespace frost::lang {
namespace {
bool token(std::string_view s) {
    return !s.empty() && s.size() <= 80 && std::ranges::all_of(s, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool bounded(std::string_view s, std::size_t max) { return !s.empty() && s.size() <= max && s.find('\0') == s.npos; }
bool hash(std::string_view s) {
    return s.size() == 64 &&
           std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); });
}
bool absolute(std::string_view s) { return bounded(s, 32768) && io::isAbsolutePath(s); }
Result<std::vector<std::string>> strings(const Json& o, std::string_view key, std::size_t limit) {
    auto* a = o.find(key);
    if (!a || !a->isArray() || a->size() > limit)
        return fail(ErrorCode::InvalidArgument, "Invalid language registry array: " + std::string(key));
    std::vector<std::string> out;
    for (const auto& value : a->items()) {
        if (!value.isString() || value.asString()->size() > 32768 || value.asString()->find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "Invalid registry string");
        out.push_back(*value.asString());
    }
    return out;
}
Result<void> verify(io::IFileSystem& fs, std::string_view path, std::string_view expected) {
    auto status = fs.status(path);
    if (!status)
        return std::unexpected(status.error());
    if (status->type != io::FileType::Regular || status->size > 128 * 1024 * 1024)
        return fail(ErrorCode::Unsupported,
                    "Language server pin requires a bounded regular file: " + std::string(path));
    auto bytes = fs.readFile(path);
    if (!bytes)
        return std::unexpected(bytes.error());
    if (bytes->size() > 128 * 1024 * 1024 || io::sha256Hex(*bytes) != expected)
        return fail(ErrorCode::PermissionDenied, "Language server artifact pin changed: " + std::string(path));
    return {};
}
} // namespace
bool registryLicenseAllowed(std::string_view s) noexcept {
    for (auto allowed : {"MIT", "BSD-2-Clause", "BSD-3-Clause", "BSD-3-Clause WITH PCRE2-exception", "Apache-2.0",
                         "Apache-2.0 WITH LLVM-exception", "BSL-1.0", "Zlib", "ISC", "curl", "blessing", "OFL-1.1"})
        if (s == allowed)
            return true;
    return false;
}
Result<LanguageServerRegistry> LanguageServerRegistry::parse(std::string_view manifest) {
    if (manifest.size() > 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Language registry exceeds 1 MiB");
    auto root = parseJson(manifest);
    if (!root)
        return std::unexpected(root.error());
    auto* version = root->find("version");
    auto* servers = root->find("servers");
    if (!version || version->kind() != Json::Kind::Integer || version->asInteger() != 1 || !servers ||
        !servers->isArray() || servers->size() > 32)
        return fail(ErrorCode::InvalidArgument, "Expected version 1 language registry with at most 32 servers");
    LanguageServerRegistry out;
    std::set<std::string> ids, selectors;
    for (const auto& row : servers->items()) {
        LanguageServerDefinition d;
        d.id = row.stringAt("id");
        d.name = row.stringAt("name");
        d.languageId = row.stringAt("languageId");
        d.version = row.stringAt("version");
        d.executable = row.stringAt("executable");
        d.license = row.stringAt("license");
        d.licensePath = row.stringAt("licensePath");
        d.licenseSha256 = row.stringAt("licenseSha256");
        auto enabled = row.boolAt("enabled");
        if (!token(d.id) || !token(d.languageId) || !bounded(d.name, 160) || !bounded(d.version, 128) ||
            !absolute(d.executable) || !registryLicenseAllowed(d.license) || !absolute(d.licensePath) ||
            !hash(d.licenseSha256) || !enabled || !ids.insert(d.id).second)
            return fail(ErrorCode::InvalidArgument,
                        "Language server identity, version, license evidence or enabled flag is invalid");
        d.enabled = *enabled;
        auto extensions = strings(row, "extensions", 32), args = strings(row, "arguments", 128);
        if (!extensions)
            return std::unexpected(extensions.error());
        if (!args)
            return std::unexpected(args.error());
        d.extensions = std::move(*extensions);
        d.arguments = std::move(*args);
        if (d.extensions.empty())
            return fail(ErrorCode::InvalidArgument, "A registered language needs explicit extensions");
        for (const auto& ext : d.extensions) {
            if (ext.size() < 2 || ext.front() != '.' || !token(std::string_view(ext).substr(1)) ||
                !selectors.insert(ext).second)
                return fail(ErrorCode::InvalidArgument, "Invalid or duplicate language extension");
            for (auto builtin :
                 {".c",    ".h",     ".hh",    ".cc",    ".cpp",     ".cxx",     ".hpp",   ".hxx", ".cppm",
                  ".ixx",  ".inl",   ".ipp",   ".tpp",   ".m",       ".mm",      ".cu",    ".lua", ".cs",
                  ".json", ".jsonc", ".cmake", ".props", ".targets", ".vcxproj", ".csproj"})
                if (ext == builtin)
                    return fail(ErrorCode::PermissionDenied,
                                "Built-in language services cannot be replaced through this registry");
        }
        auto* artifacts = row.find("artifacts");
        if (!artifacts || !artifacts->isArray() || artifacts->size() > 64)
            return fail(ErrorCode::InvalidArgument, "Expected pinned language server artifacts");
        bool pinsExecutable = false;
        std::set<std::string> paths;
        for (const auto& a : artifacts->items()) {
            LanguageServerArtifact pin{std::string(a.stringAt("path")), std::string(a.stringAt("sha256"))};
            if (!absolute(pin.path) || !hash(pin.sha256) || !paths.insert(documentKey(pin.path)).second)
                return fail(ErrorCode::InvalidArgument, "Invalid or duplicate language artifact pin");
            pinsExecutable |= documentKey(pin.path) == documentKey(d.executable);
            d.artifacts.push_back(std::move(pin));
        }
        if (!pinsExecutable)
            return fail(ErrorCode::PermissionDenied, "Language server executable requires an explicit SHA256 pin");
        if (const auto* sandbox = row.find("sandbox")) {
            const auto* files = sandbox->find("files");
            if (!sandbox->isObject() || sandbox->stringAt("profile") != "read-only-stdio" || !files ||
                !files->isArray() || files->size() == 0 || files->size() > 64)
                return fail(ErrorCode::Unsupported,
                            "Expected explicit read-only-stdio sandbox and bounded pinned file inventory");
            for (const auto& key : sandbox->keys())
                if (key != "profile" && key != "files")
                    return fail(ErrorCode::PermissionDenied,
                                "Unknown sandbox grant; no implicit write/network/identity authority");
            std::set<std::string> sourcePaths, relativePaths;
            bool sandboxExecutable = false;
            for (const auto& file : files->items()) {
                LanguageSandboxFile selected{std::string(file.stringAt("path")),
                                             std::string(file.stringAt("relativePath")), false};
                const auto executable = file.boolAt("executable");
                auto relative = selected.relativePath;
                std::replace(relative.begin(), relative.end(), '\\', '/');
                bool safe = bounded(relative, 512) && !io::isAbsolutePath(relative) && relative.front() != '/';
                std::size_t at = 0;
                while (safe && at < relative.size()) {
                    const auto end = relative.find('/', at);
                    const auto part =
                        std::string_view(relative).substr(at, end == relative.npos ? relative.size() - at : end - at);
                    safe =
                        !part.empty() && part != "." && part != ".." && std::ranges::all_of(part, [](unsigned char c) {
                            return c > 32 && c < 127 && c != ':' && c != '*' && c != '?' && c != '"' && c != '<' &&
                                   c != '>' && c != '|';
                        });
                    if (end == relative.npos)
                        break;
                    at = end + 1;
                    if (at == relative.size())
                        safe = false;
                }
                if (!absolute(selected.path) || !executable || !safe ||
                    !sourcePaths.insert(documentKey(selected.path)).second ||
                    !relativePaths.insert(documentKey(relative)).second ||
                    std::ranges::none_of(d.artifacts, [&](const auto& pin) {
                        return documentKey(pin.path) == documentKey(selected.path);
                    }))
                    return fail(ErrorCode::PermissionDenied,
                                "Sandbox files require unique safe relative layout and exact reviewed artifact pins");
                selected.relativePath = relative;
                selected.executable = *executable;
                sandboxExecutable |= documentKey(selected.path) == documentKey(d.executable) && selected.executable;
                d.sandboxFiles.push_back(std::move(selected));
            }
            if (!sandboxExecutable)
                return fail(ErrorCode::PermissionDenied, "Sandbox executable requires explicit execute grant");
        }
        if (auto* configuration = row.find("configuration")) {
            if (!configuration->isObject())
                return fail(ErrorCode::InvalidArgument, "Registry configuration must map LSP section names to values");
            d.configuration = *configuration;
        }
        if (auto* init = row.find("initializationOptions"))
            d.initializationOptions = *init;
        out.m_definitions.push_back(std::move(d));
    }
    return out;
}
const LanguageServerDefinition* LanguageServerRegistry::forPath(std::string_view path) const {
    if (io::fileName(path) == "CMakeLists.txt")
        return nullptr;
    const auto dot = path.find_last_of('.'), slash = path.find_last_of("/\\");
    if (dot == path.npos || (slash != path.npos && dot < slash))
        return nullptr;
    std::string extension(path.substr(dot));
    for (char& c : extension)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    for (const auto& d : m_definitions)
        if (d.enabled && std::ranges::find(d.extensions, extension) != d.extensions.end())
            return &d;
    return nullptr;
}
Result<void> verifyLanguageServer(io::IFileSystem& fs, const LanguageServerDefinition& d) {
    if (!registryLicenseAllowed(d.license))
        return fail(ErrorCode::PermissionDenied, "Language license is outside the approved allowlist");
    if (!absolute(d.executable) || d.artifacts.empty() || d.artifacts.size() > 64 ||
        std::ranges::none_of(d.artifacts, [&](const auto& pin) {
            return documentKey(pin.path) == documentKey(d.executable) && hash(pin.sha256);
        }))
        return fail(ErrorCode::PermissionDenied, "Registered executable requires a reviewed SHA256 pin");
    if (!absolute(d.licensePath) || !hash(d.licenseSha256))
        return fail(ErrorCode::PermissionDenied, "Registered license evidence is not pinned");
    std::uint64_t total = 0;
    for (const auto& pin : d.artifacts) {
        auto status = fs.status(pin.path);
        if (!status)
            return std::unexpected(status.error());
        total += status->size;
        if (total > 256 * 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "Language server pin set exceeds 256 MiB");
        if (auto result = verify(fs, pin.path, pin.sha256); !result)
            return result;
    }
    auto license = fs.status(d.licensePath);
    if (!license)
        return std::unexpected(license.error());
    if (!license->size || license->size > 1024 * 1024)
        return fail(ErrorCode::InvalidArgument, "License evidence must be a nonempty file below 1 MiB");
    return verify(fs, d.licensePath, d.licenseSha256);
}
LuaServiceOptions registeredLanguageOptions(const LanguageServerDefinition& d, std::string root) {
    LuaServiceOptions options;
    options.serverPath = d.executable;
    options.workspaceRoot = std::move(root);
    options.arguments = d.arguments;
    options.languageId = d.languageId;
    options.displayName = d.name;
    options.configuration = d.configuration;
    options.configurationIsSectionMap = true;
    options.initializationOptions = d.initializationOptions;
    options.trusted = true;
    return options;
}
RegistryProcessLauncher::RegistryProcessLauncher(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                 project::TrustStore& trust, LanguageServerDefinition definition,
                                                 std::string root, std::uint64_t revision, std::string physicalRoot,
                                                 std::shared_ptr<std::atomic<bool>> contributionActive,
                                                 std::string sandboxPrivateParent)
    : m_launcher(launcher), m_fs(fs), m_trust(trust), m_definition(std::move(definition)), m_root(std::move(root)),
      m_physicalRoot(std::move(physicalRoot)), m_revision(revision),
      m_contributionActive(std::move(contributionActive)), m_sandboxPrivateParent(std::move(sandboxPrivateParent)) {}
Result<void> RegistryProcessLauncher::admitWorkspace() const {
    if (!m_active.load() || (m_contributionActive && !m_contributionActive->load()))
        return fail(ErrorCode::Cancelled, "Registered tool contribution was revoked");
    const auto trust = m_trust.snapshot(m_root);
    auto physical = m_fs.physicalPath(m_root);
    if (!trust.trusted || trust.revision != m_revision || !physical ||
        documentKey(*physical) != documentKey(m_physicalRoot))
        return fail(ErrorCode::Untrusted, "Registered language workspace identity or trust changed");
    return {};
}
Result<void> RegistryProcessLauncher::admitDocument(std::string_view path) const {
    if (auto current = admitWorkspace(); !current)
        return current;
    auto physical = m_fs.physicalPath(path);
    if (!physical)
        return std::unexpected(physical.error());
    auto prefix = documentKey(m_physicalRoot);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    if (!documentKey(*physical).starts_with(prefix))
        return fail(ErrorCode::PermissionDenied, "Document is outside the registered language workspace");
    return {};
}
Result<void> RegistryProcessLauncher::admitEdit(const Json& edit) const {
    if (auto current = admitWorkspace(); !current)
        return current;
    auto parsed = parseWorkspaceEdit(edit);
    if (!parsed)
        return std::unexpected(parsed.error());
    for (const auto& file : parsed->files)
        if (auto allowed = admitDocument(file.path); !allowed)
            return allowed;
    return {};
}
Result<void> RegistryProcessLauncher::admit(const io::ProcessSpec& spec) {
    if (auto allowed = admitWorkspace(); !allowed)
        return allowed;
    auto trust = m_trust.snapshot(m_root);
    auto physical = m_fs.physicalPath(m_root);
    if (!trust.trusted || trust.revision != m_revision || !physical ||
        documentKey(*physical) != documentKey(m_physicalRoot))
        return fail(ErrorCode::Untrusted, "Registered language workspace identity or trust changed before launch");
    if (spec.executable != m_definition.executable || spec.arguments != m_definition.arguments ||
        spec.workingDirectory != m_root || spec.rawArguments)
        return fail(ErrorCode::PermissionDenied,
                    "Registered language launch differs from the reviewed pinned definition");
    if (auto verified = verifyLanguageServer(m_fs, m_definition); !verified)
        return verified;
    trust = m_trust.snapshot(m_root);
    physical = m_fs.physicalPath(m_root);
    if (!trust.trusted || trust.revision != m_revision || !physical ||
        documentKey(*physical) != documentKey(m_physicalRoot))
        return fail(ErrorCode::Untrusted, "Workspace trust changed while verifying language server pins");
    return admitWorkspace();
}
Result<io::ProcessSpec> RegistryProcessLauncher::prepareSandbox(const io::ProcessSpec& original) {
    if (m_definition.sandboxFiles.empty())
        return original;
    auto spec = original;
    spec.cleanEnvironment = true;
    spec.environment.clear();
    spec.killOnClose = true;
    spec.sandbox = io::ProcessSandboxProfile{}; // No write or network grant in this public profile.
    for (const auto& file : m_definition.sandboxFiles)
        spec.sandbox->paths.push_back(
            {file.path,
             file.executable ? io::SandboxAccess::Read | io::SandboxAccess::Execute : io::SandboxAccess::Read, false});
#ifdef _WIN32
    if (m_sandboxPrivateParent.empty() || !io::isAbsolutePath(m_sandboxPrivateParent))
        return fail(ErrorCode::Unsupported, "Sandboxed extension requires an application-owned private staging parent");
    if (auto created = m_fs.createDirectories(m_sandboxPrivateParent); !created)
        return std::unexpected(created.error());
    std::vector<io::SandboxStagingFile> captured;
    std::size_t capturedBytes = 0;
    for (const auto& file : m_definition.sandboxFiles) {
        const auto pin = std::ranges::find_if(m_definition.artifacts, [&](const auto& artifact) {
            return documentKey(artifact.path) == documentKey(file.path);
        });
        if (pin == m_definition.artifacts.end())
            return fail(ErrorCode::PermissionDenied, "Sandbox runtime file is not pinned");
        auto bytes = m_fs.readFileSnapshot(file.path, 128 * 1024 * 1024);
        if (!bytes)
            return std::unexpected(bytes.error());
        if (io::sha256Hex(*bytes) != pin->sha256)
            return fail(ErrorCode::PermissionDenied, "Sandbox runtime changed while capturing staged bytes");
        capturedBytes += bytes->size();
        if (capturedBytes > 256 * 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "Captured sandbox runtime exceeds 256 MiB");
        captured.push_back(
            {file.path, file.relativePath, std::move(*bytes),
             file.executable ? io::SandboxAccess::Read | io::SandboxAccess::Execute : io::SandboxAccess::Read});
    }
    auto staged = io::stageWindowsSandboxProcess(spec, captured, m_sandboxPrivateParent);
    if (!staged)
        return std::unexpected(staged.error());
    if (auto current = admitWorkspace(); !current)
        return std::unexpected(current.error());
    return staged;
#else
    if (auto valid = io::validateProcessSandbox(spec, false); !valid)
        return std::unexpected(valid.error());
    return spec;
#endif
}
Result<std::unique_ptr<io::IProcess>> RegistryProcessLauncher::launch(const io::ProcessSpec& spec) {
    if (auto allowed = admit(spec); !allowed)
        return std::unexpected(allowed.error());
    auto prepared = prepareSandbox(spec);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (auto current = admitWorkspace(); !current)
        return std::unexpected(current.error());
    return m_launcher.launch(*prepared);
}
Result<std::unique_ptr<io::IAsyncProcess>> RegistryProcessLauncher::launchAsync(const io::ProcessSpec& spec,
                                                                                const io::AsyncContext& context) {
    if (auto allowed = admit(spec); !allowed)
        return std::unexpected(allowed.error());
    auto prepared = prepareSandbox(spec);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (auto current = admitWorkspace(); !current)
        return std::unexpected(current.error());
    return m_launcher.launchAsync(*prepared, context);
}
sched::Task<Result<Json>> RegistryLspRequester::request(std::string method, Json params, std::stop_token token) {
    return invoke(m_service, m_admission, std::move(method), std::move(params), token);
}
sched::Task<Result<Json>> RegistryLspRequester::invoke(std::shared_ptr<LuaService> service,
                                                       std::shared_ptr<RegistryProcessLauncher> admission,
                                                       std::string method, Json params, std::stop_token token) {
    if (auto current = admission->admitWorkspace(); !current)
        co_return std::unexpected(current.error());
    const auto* document = params.find("textDocument");
    if (!document)
        document = params.find("item");
    std::optional<std::string> path;
    if (document) {
        auto decoded = uriToPath(document->stringAt("uri"));
        if (!decoded)
            co_return std::unexpected(decoded.error());
        path = std::move(*decoded);
        if (auto allowed = admission->admitDocument(*path); !allowed)
            co_return std::unexpected(allowed.error());
    }
    const auto requestedMethod = method;
    auto result = co_await service->request(std::move(method), std::move(params), token);
    if (auto current = admission->admitWorkspace(); !current)
        co_return std::unexpected(current.error());
    if (path)
        if (auto current = admission->admitDocument(*path); !current)
            co_return std::unexpected(current.error());
    if (result && !result->isNull()) {
        if (requestedMethod == "textDocument/rename") {
            if (auto allowed = admission->admitEdit(*result); !allowed)
                co_return std::unexpected(allowed.error());
        } else if (requestedMethod == "textDocument/codeAction" && result->isArray()) {
            for (const auto& action : result->items())
                if (const auto* edit = action.find("edit"))
                    if (auto allowed = admission->admitEdit(*edit); !allowed)
                        co_return std::unexpected(allowed.error());
        } else if (requestedMethod == "codeAction/resolve") {
            if (const auto* edit = result->find("edit"))
                if (auto allowed = admission->admitEdit(*edit); !allowed)
                    co_return std::unexpected(allowed.error());
        }
    }
    co_return result;
}
std::optional<ServerCapabilities> RegistryLspRequester::serverCapabilities() const {
    return m_admission->admitWorkspace() ? m_service->capabilities() : std::nullopt;
}
void RegistryLspRequester::setApplyEditHandler(ApplyEditHandler handler) {
    auto admission = m_admission;
    auto dispatch = m_dispatch;
    m_service->setApplyEditHandler(
        [admission, dispatch, handler = std::move(handler)](const Json& params, std::function<void(bool)> done) {
            auto completed = std::make_shared<std::function<void(bool)>>(std::move(done));
            auto apply = [admission, handler, params, completed] {
                const auto* edit = params.find("edit");
                if (!handler || !edit || !admission->admitWorkspace()) {
                    (*completed)(false);
                    return;
                }
                if (!admission->admitEdit(*edit)) {
                    (*completed)(false);
                    return;
                }
                handler(params, [completed](bool applied) { (*completed)(applied); });
            };
            // The app dispatches this whole admission+apply boundary to its document owner. The
            // ordinary LanguageController then applies synchronously, without another queued gap.
            if (dispatch) {
                if (!dispatch(std::move(apply)))
                    (*completed)(false);
            } else
                apply();
        });
}
} // namespace frost::lang
