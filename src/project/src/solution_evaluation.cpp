// SPDX-License-Identifier: Apache-2.0
//
// SolutionEvaluation (frost/project/project_host.hpp): a solution's projects evaluated on a
// worker thread, through ProjectHost and, once it is unavailable, through MSBuild.exe (SLN-2).

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/project_host.hpp"

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

std::optional<io::ProcessId> SolutionEvaluation::hostPid() const {
    LockGuard lock(m_mutex);
    return m_hostPid;
}

void SolutionEvaluation::publish(std::function<void(SolutionEvaluationStatus&)> change) {
    LockGuard lock(m_mutex);
    change(m_status);
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

    std::size_t done = 0;
    for (const Planned& planned : building) {
        if (stop.stop_requested()) {
            break;
        }
        if (!trusted()) {
            giveUp("trust was revoked, so MSBuild stopped evaluating the projects");
            return;
        }
        const ProjectInfo& info = m_model->project(planned.id);
        const ProjectEvaluationRequest request{.projectPath = info.path,
                                               .configuration = planned.mapping.configuration,
                                               .platform = planned.mapping.platform,
                                               .solutionPath = m_model->path(),
                                               .itemTypes = {},
                                               .metadata = {},
                                               .properties = {"ConfigurationType", "PlatformToolset"}};
        std::optional<EvaluatedProjectItems> result;
        std::string failure;
        if (host) {
            auto evaluated = host->evaluate(request, stop);
            if (evaluated) {
                result =
                    toEvaluatedProjectItems(*evaluated, EvaluationSource::ProjectHost, host->hello().msbuildVersion);
            } else if (evaluated.error().code() == ErrorCode::Cancelled) {
                break;
            } else if (!host->running() || evaluated.error().code() == ErrorCode::Timeout) {
                // The helper itself is gone: this project and the rest go to MSBuild.exe.
                useFallback(std::format("{} while evaluating {}", evaluated.error().message(), info.name));
            } else {
                failure = evaluated.error().message(); // MSBuild could not load this project.
            }
        }
        if (!host && !result && failure.empty()) {
            auto evaluated = detail::evaluateWithMsBuildCli(m_launcher, msbuild, request, m_options.environment,
                                                            m_options.requestTimeout, stop);
            if (evaluated) {
                result = toEvaluatedProjectItems(*evaluated, EvaluationSource::MsBuildCli, {});
            } else if (evaluated.error().code() == ErrorCode::Cancelled) {
                break;
            } else {
                failure = evaluated.error().message();
            }
        }
        ++done;
        {
            LockGuard lock(m_mutex);
            if (result) {
                m_results.push_back(std::move(*result));
            } else {
                m_status.warnings.push_back(std::format(
                    "{}: MSBuild could not evaluate it ({}); its files are shown as written", info.name, failure));
            }
            m_status.evaluated = done;
            ++m_status.generation;
        }
        if (m_options.afterProject) {
            m_options.afterProject(done, host ? std::optional<io::ProcessId>(host->pid()) : std::nullopt);
        }
    }
    if (stop.stop_requested()) {
        publish([](SolutionEvaluationStatus& status) {
            status.state = SolutionEvaluationState::Stopped;
            ++status.generation;
        });
        return;
    }
    publish([](SolutionEvaluationStatus& status) {
        status.state = SolutionEvaluationState::Ready;
        ++status.generation;
    });
    // An idle helper that ends (killed, crashed) is reported too: the banner says so, and any
    // later evaluation of this generation would go through MSBuild.exe.
    if (host && host->waitUntilEnded(stop)) {
        useFallback(host->endedError().message());
    }
#endif
}

} // namespace frost::project
