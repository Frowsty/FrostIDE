// SPDX-License-Identifier: Apache-2.0
//
// SolutionEvaluation (frost/project/project_host.hpp): a solution's projects evaluated on a
// worker thread, through ProjectHost and, once it is unavailable, through MSBuild.exe (SLN-2);
// served from the ProjectModelCache when an entry is still valid and revalidated afterwards
// (SLN-4); with each project's GetClCommandLines when asked for (SLN-5).

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/project_model_cache.hpp"

#include "paths.hpp"

#include <algorithm>
#include <format>

namespace frost::project {
namespace detail {
// project_host.cpp.
Result<HostEvaluation>
evaluateWithMsBuildCli(io::IProcessLauncher& launcher, std::string_view msbuildPath,
                       const ProjectEvaluationRequest& request,
                       const std::vector<std::pair<std::string, std::optional<std::string>>>& environment,
                       IClock::Duration timeout, std::stop_token stop);
} // namespace detail

namespace {

// One project to evaluate and the project configuration it builds as.
struct Planned {
    ProjectId id;
    ProjectConfigurationMapping mapping;
};

// Same items (type, include, path, wildcard flag, in order) and imports.
bool sameItems(const EvaluatedProjectItems& a, const EvaluatedProjectItems& b) {
    if (a.items.size() != b.items.size() || a.imports != b.imports || a.configuration != b.configuration ||
        a.platform != b.platform) {
        return false;
    }
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        const ProjectItem& x = a.items[i];
        const ProjectItem& y = b.items[i];
        if (x.type != y.type || x.itemName != y.itemName || x.include != y.include || x.path != y.path ||
            x.fromWildcard != y.fromWildcard) {
            return false;
        }
    }
    return true;
}

} // namespace

SolutionEvaluation::SolutionEvaluation(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                       std::shared_ptr<const SolutionModel> model, SolutionEvaluationOptions options)
    : m_launcher(launcher), m_fs(fs), m_model(std::move(model)), m_options(std::move(options)) {
    m_worker = std::jthread([this](std::stop_token stop) { run(stop); });
}

SolutionEvaluation::~SolutionEvaluation() { stopAndJoin(); }

void SolutionEvaluation::requestStop() { m_worker.request_stop(); }

void SolutionEvaluation::stopAndJoin() {
    m_worker.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

SolutionEvaluationStatus SolutionEvaluation::status() const {
    LockGuard lock(m_mutex);
    return m_status;
}

std::vector<EvaluatedProjectItems> SolutionEvaluation::results() const {
    LockGuard lock(m_mutex);
    return m_results;
}

std::vector<ProjectClCommandLines> SolutionEvaluation::clCommandLines() const {
    LockGuard lock(m_mutex);
    return m_clCommandLines;
}

std::optional<io::ProcessId> SolutionEvaluation::hostPid() const {
    LockGuard lock(m_mutex);
    return m_hostPid;
}

void SolutionEvaluation::publish(std::function<void(SolutionEvaluationStatus&)> change) {
    LockGuard lock(m_mutex);
    change(m_status);
}

bool SolutionEvaluation::storeResult(EvaluatedProjectItems items, std::optional<ProjectClCommandLines> lines) {
    const std::string key = detail::pathKey(items.projectPath);
    bool changed = false;
    auto existing = std::ranges::find_if(
        m_results, [&key](const EvaluatedProjectItems& r) { return detail::pathKey(r.projectPath) == key; });
    if (existing == m_results.end()) {
        m_results.push_back(std::move(items));
        changed = true;
    } else if (!sameItems(*existing, items)) {
        *existing = std::move(items);
        changed = true;
    }
    auto line = std::ranges::find_if(
        m_clCommandLines, [&key](const ProjectClCommandLines& r) { return detail::pathKey(r.projectPath) == key; });
    if (lines) {
        if (line == m_clCommandLines.end()) {
            m_clCommandLines.push_back(std::move(*lines));
            changed = true;
        } else if (!(*line == *lines)) {
            *line = std::move(*lines);
            changed = true;
        }
    } else if (line != m_clCommandLines.end()) {
        m_clCommandLines.erase(line);
        changed = true;
    }
    return changed;
}

void SolutionEvaluation::run(std::stop_token stop) {
    const auto trusted = [this] { return m_options.trusted && m_options.trusted(); };
    const auto giveUp = [this](std::string why) {
        publish([&why](SolutionEvaluationStatus& status) {
            status.msbuildProblem = std::move(why);
            status.source.reset();
            status.state = SolutionEvaluationState::Ready;
        });
    };
    if (!m_model) {
        giveUp("no solution is open");
        return;
    }
    if (!trusted()) {
        giveUp("the folder is not trusted, so MSBuild does not evaluate its projects");
        return;
    }
    // The projects, in solution-file order, those that build in the configuration first.
    std::optional<std::size_t> configuration;
    if (!m_options.configuration.empty()) {
        configuration = m_model->findConfiguration(m_options.configuration);
    } else if (!m_model->configurations().empty()) {
        configuration = 0;
    }
    std::vector<Planned> building;
    std::vector<Planned> others;
    std::vector<std::string> skipped;
    for (std::size_t index = 0; index < m_model->projects().size(); ++index) {
        const ProjectInfo& info = m_model->projects()[index];
        if (info.kind != ProjectKind::Cpp || info.nodeKind != NodeKind::Project || !info.vcxproj) {
            continue;
        }
        const ProjectId id{static_cast<std::uint32_t>(index)};
        if (!configuration || *configuration >= info.configurationMap.size() ||
            !info.configurationMap[*configuration]) {
            skipped.push_back(
                std::format("{}: not part of the solution configuration {}; its files are shown as written", info.name,
                            m_options.configuration));
            continue;
        }
        const ProjectConfigurationMapping& mapping = *info.configurationMap[*configuration];
        (mapping.build ? building : others).push_back({id, mapping});
    }
    building.insert(building.end(), others.begin(), others.end());
    if (building.empty()) {
        // Nothing MSBuild would evaluate (no Visual C++ project in the configuration): no helper,
        // no MSBuild, and nothing to report.
        publish([&](SolutionEvaluationStatus& status) {
            status.warnings = skipped;
            status.state = SolutionEvaluationState::Ready;
            ++status.generation;
        });
        return;
    }

#ifndef _WIN32
    giveUp("MSBuild evaluation needs Windows with Visual Studio or its Build Tools");
    return;
#else
    std::string msbuild = m_options.msbuildPath;
    if (msbuild.empty()) {
        if (!m_options.locateMsBuild) {
            giveUp("no MSBuild was found");
            return;
        }
        auto located = m_options.locateMsBuild();
        if (!located) {
            giveUp(std::format("MSBuild unavailable: {}", located.error().message()));
            return;
        }
        msbuild = std::move(*located);
    }
    if (!io::isAbsolutePath(msbuild) || !m_fs.exists(msbuild).value_or(false)) {
        giveUp(std::format("MSBuild unavailable: {} does not exist", msbuild));
        return;
    }
    publish([&](SolutionEvaluationStatus& status) {
        status.state = SolutionEvaluationState::Evaluating;
        status.total = building.size();
        status.warnings = skipped;
    });

    const auto requestFor = [this](const Planned& planned) {
        return ProjectEvaluationRequest{.projectPath = m_model->project(planned.id).path,
                                        .configuration = planned.mapping.configuration,
                                        .platform = planned.mapping.platform,
                                        .solutionPath = m_model->path(),
                                        .itemTypes = {},
                                        .metadata = {},
                                        .properties = {"ConfigurationType", "PlatformToolset"}};
    };
    const auto keyFor = [this](const ProjectEvaluationRequest& request) {
        return ProjectModelCacheKey{.solutionPath = m_model->path(),
                                    .projectPath = request.projectPath,
                                    .configuration = request.configuration,
                                    .platform = request.platform};
    };
    // SLN-4: the inputs every entry is checked against; without them the cache is not used.
    std::optional<EvaluationInputs> baseInputs;
    if (m_options.cache) {
        if (auto identity = msbuildIdentity(m_fs, msbuild)) {
            baseInputs = EvaluationInputs{.globalProperties = {},
                                          .msbuildIdentity = std::move(*identity),
                                          .environment = environmentFingerprint(m_options.environment)};
        }
    }
    const auto inputsFor = [&baseInputs](const ProjectEvaluationRequest& request) {
        EvaluationInputs inputs = *baseInputs;
        inputs.globalProperties = evaluationGlobalProperties(request);
        return inputs;
    };

    std::size_t done = 0;
    // Phase 1: the valid cache entries, shown before any helper starts.
    std::vector<Planned> hits;
    std::vector<Planned> misses;
    for (const Planned& planned : building) {
        if (stop.stop_requested()) {
            break;
        }
        if (baseInputs) {
            const ProjectEvaluationRequest request = requestFor(planned);
            auto cached = m_options.cache->lookup(keyFor(request), inputsFor(request));
            if (cached && (!m_options.clCommandLines || cached->clCommandLines)) {
                ++done;
                {
                    LockGuard lock(m_mutex);
                    static_cast<void>(storeResult(std::move(cached->items), std::move(cached->clCommandLines)));
                    if (!m_status.source) {
                        m_status.source = EvaluationSource::ProjectHost; // The entries are ProjectHost's.
                    }
                    ++m_status.cached;
                    m_status.evaluated = done;
                    ++m_status.generation;
                }
                if (m_options.afterProject) {
                    m_options.afterProject(done, std::nullopt);
                }
                hits.push_back(planned);
                continue;
            }
        }
        misses.push_back(planned);
    }
    const auto stopped = [this] {
        publish([](SolutionEvaluationStatus& status) {
            status.state = SolutionEvaluationState::Stopped;
            status.revalidating = false;
            ++status.generation;
        });
    };
    if (stop.stop_requested()) {
        stopped();
        return;
    }
    if (misses.empty() && (hits.empty() || !m_options.revalidateCached)) {
        publish([](SolutionEvaluationStatus& status) {
            status.state = SolutionEvaluationState::Ready;
            ++status.generation;
        });
        return;
    }
    // Every project came from the cache: Ready before the helper starts; revalidation follows.
    const bool readyFromCache = misses.empty();
    if (readyFromCache) {
        publish([](SolutionEvaluationStatus& status) {
            status.state = SolutionEvaluationState::Ready;
            status.revalidating = true;
            ++status.generation;
        });
    }

    // ProjectHost first; a missing or failing helper sends every project to MSBuild.exe.
    std::unique_ptr<ProjectHostClient> host;
    std::string hostProblem;
    if (m_options.hostPath.empty() || !m_fs.exists(m_options.hostPath).value_or(false)) {
        hostProblem = std::format("ProjectHost is not installed ({})",
                                  m_options.hostPath.empty() ? std::string("no path") : m_options.hostPath);
    } else {
        auto started = ProjectHostClient::start(m_launcher,
                                                {.hostPath = m_options.hostPath,
                                                 .msbuildPath = msbuild,
                                                 .environment = m_options.environment,
                                                 .startTimeout = m_options.startTimeout,
                                                 .requestTimeout = m_options.requestTimeout},
                                                stop);
        if (started) {
            host = std::move(*started);
        } else if (started.error().code() != ErrorCode::Cancelled) {
            hostProblem = std::format("ProjectHost could not start: {}", started.error().message());
        }
    }
    const auto useFallback = [&](std::string why) {
        host.reset();
        LockGuard lock(m_mutex);
        m_hostPid.reset();
        if (m_options.clCommandLines && m_status.clCommandLinesProblem.empty()) {
            m_status.clCommandLinesProblem =
                std::format("GetClCommandLines needs ProjectHost, which is unavailable ({})", why);
        }
        m_status.projectHostProblem = std::move(why);
        m_status.source = EvaluationSource::MsBuildCli;
        m_status.msbuildVersion.clear();
        ++m_status.generation;
    };
    if (host) {
        LockGuard lock(m_mutex);
        m_hostPid = host->pid();
        m_status.source = EvaluationSource::ProjectHost;
        m_status.msbuildVersion = host->hello().msbuildVersion;
        ++m_status.generation;
    } else if (!stop.stop_requested()) {
        useFallback(hostProblem);
    }

    // Evaluates one project: ProjectHost (with GetClCommandLines when asked), else MSBuild.exe
    // unless `hostOnly`; stores ProjectHost results in the cache. False when cancelled.
    const auto evaluateOne = [&](const Planned& planned, bool hostOnly, bool& changed) -> bool {
        const ProjectInfo& info = m_model->project(planned.id);
        const ProjectEvaluationRequest request = requestFor(planned);
        std::optional<EvaluatedProjectItems> result;
        std::optional<ProjectClCommandLines> lines;
        std::vector<std::string> globs;
        std::string failure;
        std::string linesFailure;
        if (host) {
            auto evaluated = host->evaluate(request, stop);
            if (evaluated) {
                result =
                    toEvaluatedProjectItems(*evaluated, EvaluationSource::ProjectHost, host->hello().msbuildVersion);
                globs = evaluated->globs;
                if (m_options.clCommandLines) {
                    auto computed = host->clCommandLines(request, stop);
                    if (computed) {
                        lines = std::move(*computed);
                    } else if (computed.error().code() == ErrorCode::Cancelled) {
                        return false;
                    } else if (!host->running() || computed.error().code() == ErrorCode::Timeout) {
                        useFallback(std::format("{} while running GetClCommandLines for {}", computed.error().message(),
                                                info.name));
                    } else {
                        linesFailure = computed.error().message();
                    }
                }
            } else if (evaluated.error().code() == ErrorCode::Cancelled) {
                return false;
            } else if (!host->running() || evaluated.error().code() == ErrorCode::Timeout) {
                // The helper itself is gone: this project and the rest go to MSBuild.exe.
                useFallback(std::format("{} while evaluating {}", evaluated.error().message(), info.name));
            } else {
                failure = evaluated.error().message(); // MSBuild could not load this project.
            }
        }
        if (hostOnly && (!result || (m_options.clCommandLines && !lines))) {
            return true; // A revalidation never replaces a cached entry with less.
        }
        if (!host && !result && failure.empty()) {
            auto evaluated = detail::evaluateWithMsBuildCli(m_launcher, msbuild, request, m_options.environment,
                                                            m_options.requestTimeout, stop);
            if (evaluated) {
                result = toEvaluatedProjectItems(*evaluated, EvaluationSource::MsBuildCli, {});
            } else if (evaluated.error().code() == ErrorCode::Cancelled) {
                return false;
            } else {
                failure = evaluated.error().message();
            }
        }
        if (result && result->source == EvaluationSource::ProjectHost && baseInputs &&
            (lines || !m_options.clCommandLines)) {
            // A store that fails (a read-only cache, an import that vanished) only costs the next open.
            static_cast<void>(m_options.cache->store(keyFor(request), inputsFor(request),
                                                     {.items = *result, .clCommandLines = lines}, globs));
        }
        LockGuard lock(m_mutex);
        if (result) {
            changed = storeResult(std::move(*result), std::move(lines));
        } else {
            m_status.warnings.push_back(std::format(
                "{}: MSBuild could not evaluate it ({}); its files are shown as written", info.name, failure));
            changed = true;
        }
        if (!linesFailure.empty()) {
            m_status.warnings.push_back(std::format("{}: GetClCommandLines failed ({})", info.name, linesFailure));
            if (m_status.clCommandLinesProblem.empty()) {
                m_status.clCommandLinesProblem = std::format("GetClCommandLines failed for {}", info.name);
            }
        }
        return true;
    };

    // Phase 2: the projects without a valid entry.
    for (const Planned& planned : misses) {
        if (stop.stop_requested()) {
            break;
        }
        if (!trusted()) {
            giveUp("trust was revoked, so MSBuild stopped evaluating the projects");
            return;
        }
        bool changed = false;
        if (!evaluateOne(planned, false, changed)) {
            break;
        }
        ++done;
        {
            LockGuard lock(m_mutex);
            m_status.evaluated = done;
            ++m_status.generation;
        }
        if (m_options.afterProject) {
            m_options.afterProject(done, host ? std::optional<io::ProcessId>(host->pid()) : std::nullopt);
        }
    }
    if (stop.stop_requested()) {
        stopped();
        return;
    }
    const bool revalidate = m_options.revalidateCached && !hits.empty() && host;
    publish([revalidate, readyFromCache](SolutionEvaluationStatus& status) {
        status.revalidating = revalidate;
        if (!readyFromCache) {
            status.state = SolutionEvaluationState::Ready;
            ++status.generation;
        }
    });
    // Phase 3: revalidation of the entries served from the cache (see project_model_cache.hpp).
    if (revalidate) {
        for (const Planned& planned : hits) {
            if (stop.stop_requested() || !host || !trusted()) {
                break;
            }
            bool changed = false;
            if (!evaluateOne(planned, true, changed)) {
                break;
            }
            LockGuard lock(m_mutex);
            ++m_status.revalidated;
            if (changed) {
                ++m_status.generation;
            }
        }
        // Not a change of results(): the generation moved above only for a project that changed.
        publish([](SolutionEvaluationStatus& status) { status.revalidating = false; });
        if (stop.stop_requested()) {
            stopped();
            return;
        }
    }
    // An idle helper that ends (killed, crashed) is reported too: the banner says so, and any
    // later evaluation of this generation would go through MSBuild.exe.
    if (host && host->waitUntilEnded(stop)) {
        useFallback(host->endedError().message());
    }
#endif
}

} // namespace frost::project
