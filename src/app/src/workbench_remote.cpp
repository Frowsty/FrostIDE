// SPDX-License-Identifier: Apache-2.0
// CMK-16/RUN-15 application-owned endpoint/profile workflows and revocable remote execution.
#include "frost/app/workbench.hpp"
#include "frost/build/remote_process.hpp"
#include "frost/build/build_service.hpp"
#include "frost/io/async_process.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/build/wsl_cmake.hpp"
#include "frost/build/output_parsers.hpp"
#include "frost/build/line_splitter.hpp"
#include "frost/build/run_session.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/resource_governor.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "remote_dialogs.hpp"
#include "cmake_integration.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextDocument>
#include "frost/settings/keys.hpp"
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <map>
#include <tuple>
namespace frost::app {
namespace {
using namespace std::chrono_literals;
QString q(std::string_view value){return QString::fromUtf8(value.data(),static_cast<qsizetype>(value.size()));}
bool within(std::string_view root,std::string_view path) {
    const auto key=lang::documentKey(path),base=lang::documentKey(root);
    return key==base||(key.starts_with(base)&&key.size()>base.size()&&(key[base.size()]=='/'||key[base.size()]=='\\'));
}
std::string registryPath(std::string_view explicitPath,std::string_view settings,std::string_view leaf) {
    if(!explicitPath.empty())return std::string(explicitPath);
    auto parent=io::parentPath(settings);
    std::string fallback;
    if(parent.empty()){auto trust=project::TrustStore::defaultStorePath();if(!trust)return {};fallback=std::string(io::parentPath(*trust));parent=fallback;}
    return io::joinPath(parent,leaf).value_or("");
}
Result<std::optional<std::string>> readOptional(io::IFileSystem& fs,const std::string& path) {
    auto bytes=fs.readFileSnapshot(path,1024U*1024U);if(bytes)return std::optional<std::string>{std::move(*bytes)};
    if(bytes.error().code()==ErrorCode::NotFound)return std::optional<std::string>{};return std::unexpected(bytes.error());
}
Result<void> saveRegistry(io::IFileSystem& fs,const std::string& path,const std::optional<std::string>& before,std::string bytes) {
    auto current=readOptional(fs,path);if(!current)return std::unexpected(current.error());
    if(*current!=before)return fail(ErrorCode::Busy,"Application remote settings changed while the dialog was open");
    if(auto created=fs.createDirectories(io::parentPath(path));!created)return created;
    return fs.writeFileAtomic(path,bytes);
}
struct RemoteCapture {
    build::RemoteTarget target;
    std::string root,physicalRoot,registry,registryPhysical,registryBytes;
    std::string profiles,profilesPhysical,profilesBytes;
    std::uint64_t revision=0;
    std::stop_token projectStop,jobStop;
    std::map<std::string,std::string> mappingIdentities;
};
Result<void> admit(io::IFileSystem& fs,project::TrustStore& trust,const RemoteCapture& capture) {
    if(capture.projectStop.stop_requested()||capture.jobStop.stop_requested())return fail(ErrorCode::Cancelled,"Remote command context was cancelled");
    const auto permission=trust.snapshot(capture.root);
    if(!permission.trusted||permission.revision!=capture.revision)return fail(ErrorCode::Untrusted,"Project trust changed before/during remote execution");
    auto physical=fs.physicalPath(capture.root);
    if(!physical||lang::documentKey(*physical)!=lang::documentKey(capture.physicalRoot))return fail(ErrorCode::Busy,"Remote command project identity changed");
    for(const auto& [path,identity,bytes]:std::array{
        std::tuple{capture.registry,capture.registryPhysical,capture.registryBytes},
        std::tuple{capture.profiles,capture.profilesPhysical,capture.profilesBytes}}) {
        if(path.empty())continue;
        auto actual=fs.physicalPath(path);auto current=fs.readFileSnapshot(path,1024U*1024U);
        if(!actual||lang::documentKey(*actual)!=lang::documentKey(identity)||!current||*current!=bytes)
            return fail(ErrorCode::Untrusted,"Named application remote endpoint/profile changed; command admission revoked");
    }
    for(const auto& [local,identity]:capture.mappingIdentities) {
        auto actual=fs.physicalPath(local);if(!actual||lang::documentKey(*actual)!=lang::documentKey(identity))
            return fail(ErrorCode::Busy,"Local remote-mapping root identity changed");
    }
    return {};
}
Result<std::shared_ptr<RemoteCapture>> captureEndpoint(io::IFileSystem& fs,project::TrustStore& trust,
    std::string root,std::string registry,std::string targetId,std::stop_token projectStop) {
    auto bytes=fs.readFileSnapshot(registry,1024U*1024U);if(!bytes)return std::unexpected(bytes.error());
    auto targets=build::parseRemoteTargets(*bytes);if(!targets)return std::unexpected(targets.error());
    const auto target=std::find_if(targets->begin(),targets->end(),[&](const auto& row){return row.id==targetId;});
    if(target==targets->end())return fail(ErrorCode::NotFound,"Named remote target is not saved in the application registry");
    if(!target->executionApproved)return fail(ErrorCode::Untrusted,"Named remote target has no explicit application execution approval");
    auto physical=fs.physicalPath(root),registryPhysical=fs.physicalPath(registry);
    if(!physical||!registryPhysical)return fail(ErrorCode::Busy,"Remote root/registry physical identity unavailable");
    if(within(*physical,*registryPhysical))return fail(ErrorCode::Untrusted,"Remote endpoint registry must be application-owned outside the project");
    auto capture=std::make_shared<RemoteCapture>();capture->target=*target;capture->root=std::move(root);capture->physicalRoot=*physical;
    capture->registry=std::move(registry);capture->registryPhysical=*registryPhysical;capture->registryBytes=*bytes;
    capture->revision=trust.snapshot(capture->root).revision;capture->projectStop=projectStop;
    for(const auto& mapping:target->mappings)if(auto identity=fs.physicalPath(mapping.localRoot))capture->mappingIdentities.emplace(mapping.localRoot,*identity);
    if(auto valid=admit(fs,trust,*capture);!valid)return std::unexpected(valid.error());return capture;
}
class DeferredEndpointLauncher final:public io::IProcessLauncher {
public:
    DeferredEndpointLauncher(io::IFileSystem& fs,project::TrustStore& trust,io::IProcessLauncher& local,
        std::string root,std::string registry,std::string expectedBytes,std::string target,std::uint64_t revision,std::stop_token stop)
        :m_fs(fs),m_trust(trust),m_local(local),m_root(std::move(root)),m_registry(std::move(registry)),m_expectedBytes(std::move(expectedBytes)),m_target(std::move(target)),m_revision(revision),m_stop(stop) {}
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto capture=captureEndpoint(m_fs,m_trust,m_root,m_registry,m_target,m_stop);
        if(!capture)return std::unexpected(capture.error());
        if((*capture)->registryBytes!=m_expectedBytes||(*capture)->revision!=m_revision)return fail(ErrorCode::Untrusted,"Remote Run trust admission changed while queued");
        auto permission=[fs=&m_fs,trust=&m_trust,capture=*capture](){return admit(*fs,*trust,*capture);};
        build::RemoteProcessLauncher launcher(m_local,(*capture)->target,{.admit=permission});return launcher.launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&,const io::AsyncContext&) override {
        return fail(ErrorCode::Unsupported,"Remote Run uses owned RunSession workers");
    }
private:
    io::IFileSystem& m_fs;project::TrustStore& m_trust;io::IProcessLauncher& m_local;
    std::string m_root,m_registry,m_expectedBytes,m_target;std::uint64_t m_revision;std::stop_token m_stop;
};
struct Reservation {
    build::BuildService* service;std::uint64_t token;
    ~Reservation(){service->releaseExternalLaunch(token);}
};
using RemoteOutput=std::function<void(build::RunStream,std::string)>;
using RemoteDone=std::function<void(Result<build::RunExit>)>;
[[maybe_unused]] Result<build::RunExit> executeWslBuild(io::IFileSystem* fs,io::IProcessLauncher* local,project::TrustStore* trust,
    const std::shared_ptr<RemoteCapture>& capture,const build::WslCMakeProfile& profile,build::WslCMakeOperation operation,
    const std::shared_ptr<sched::ResourceGovernor>& governor,const RemoteOutput& output) {
    auto actual=captureEndpoint(*fs,*trust,capture->root,capture->registry,capture->target.id,capture->projectStop);
    if(!actual)return std::unexpected(actual.error());
    if((*actual)->target!=capture->target||(*actual)->registryBytes!=capture->registryBytes||(*actual)->revision!=capture->revision)
        return fail(ErrorCode::Untrusted,"Selected remote endpoint/trust changed while the WSL command was queued");
    capture->physicalRoot=(*actual)->physicalRoot;capture->registryPhysical=(*actual)->registryPhysical;capture->mappingIdentities=(*actual)->mappingIdentities;
    auto profilePhysical=fs->physicalPath(capture->profiles);auto profileBytes=fs->readFileSnapshot(capture->profiles,1024U*1024U);
    if(!profilePhysical||within(capture->physicalRoot,*profilePhysical)||!profileBytes||*profileBytes!=capture->profilesBytes)
        return fail(ErrorCode::Untrusted,"Selected WSL profile changed or is not application-owned");
    capture->profilesPhysical=*profilePhysical;
    auto lease=governor->tryAcquireHeavyLeg(sched::HeavyLegKind::Build);if(!lease)return std::unexpected(lease.error());
    const auto permission=[=](){return admit(*fs,*trust,*capture);};
    build::RemoteProcessLauncher launcher(*local,capture->target,{.admit=permission});
    auto command=build::wslCMakeCommand(profile,capture->target,operation,governor->buildJobs());if(!command)return std::unexpected(command.error());
    auto cmake=*command;cmake.executable=profile.cmakeProgram;cmake.arguments={"--version"};
    auto cmakeVersion=io::runToCompletion(launcher,cmake,"",30s,capture->jobStop);
    if(!cmakeVersion)return std::unexpected(cmakeVersion.error().withContext("Remote CMake tool probe"));
    if(cmakeVersion->exitCode!=0)return fail(ErrorCode::IoError,"Remote CMake version probe failed: "+cmakeVersion->stderrBytes);
    auto ninja=cmake;ninja.executable=profile.ninjaProgram;
    auto ninjaVersion=io::runToCompletion(launcher,ninja,"",30s,capture->jobStop);
    if(!ninjaVersion)return std::unexpected(ninjaVersion.error().withContext("Remote Ninja tool probe"));
    if(ninjaVersion->exitCode!=0)return fail(ErrorCode::IoError,"Remote Ninja version probe failed: "+ninjaVersion->stderrBytes);
    if(auto versions=build::validateWslCMakeToolVersions(cmakeVersion->stdoutBytes,ninjaVersion->stdoutBytes);!versions)return std::unexpected(versions.error());
    output(build::RunStream::Stdout,cmakeVersion->stdoutBytes+ninjaVersion->stdoutBytes);
    if(auto current=permission();!current)return std::unexpected(current.error());
    build::RunSessionCallbacks callbacks;callbacks.onOutput=[output](auto stream,std::string_view bytes){output(stream,std::string(bytes));};
    auto session=build::RunSession::start(launcher,*command,std::move(callbacks),systemClock(),{.terminateDescendantsOnExit=true});
    if(!session)return std::unexpected(session.error());
    std::stop_callback stopped(capture->jobStop,[&]{(*session)->stop();});
    std::stop_callback projectStopped(capture->projectStop,[&]{(*session)->stop();});
    std::optional<build::RunExit> result;
    while(!(result=(*session)->waitFor(250ms)))if(auto current=permission();!current)(*session)->stop();
    if(result->stopped&&!result->waitFailed&&!result->cleanupFailed)return *result;
    if(auto current=permission();!current)return std::unexpected(current.error());return *result;
}
[[maybe_unused]] sched::Task<void> runWslBuild(io::IFileSystem* fs,io::IProcessLauncher* local,project::TrustStore* trust,
    std::shared_ptr<RemoteCapture> capture,build::WslCMakeProfile profile,build::WslCMakeOperation operation,
    std::shared_ptr<sched::ResourceGovernor> governor,std::shared_ptr<Reservation> reservation,RemoteOutput output,RemoteDone done) {
    auto result=executeWslBuild(fs,local,trust,capture,profile,operation,governor,output);
    reservation.reset(); // Native/CMake handoff may retry only after both shared leases are released.
    done(std::move(result));co_return;
}
struct BuildView {
    std::array<build::LineSplitter,2> lines;
    std::array<std::unique_ptr<build::IOutputParser>,2> parsers;
    QPointer<QPlainTextEdit> output;
    QPointer<QTableWidget> diagnostics;
    std::vector<build::Diagnostic> rows;
    build::RemoteTarget target;
    void diagnostic(build::Diagnostic value) {
        if(!diagnostics || rows.size()>=20000)return;
        const auto row=diagnostics->rowCount();diagnostics->insertRow(row);
        auto mapped=io::isAbsolutePath(value.file,io::PathStyle::Posix)?build::mapRemoteToLocal(target,value.file):Result<std::string>(fail(ErrorCode::NotFound,"Relative remote origin needs evaluated directory context"));
        diagnostics->setItem(row,0,new QTableWidgetItem(q(value.file)));
        diagnostics->setItem(row,1,new QTableWidgetItem(mapped?q(*mapped):QStringLiteral("Unmapped; navigation unavailable")));
        diagnostics->setItem(row,2,new QTableWidgetItem(QString::number(value.line)));
        diagnostics->setItem(row,3,new QTableWidgetItem(q(value.message)));rows.push_back(std::move(value));
    }
    void feed(build::RunStream stream,const std::string& text) {
        if(output){output->moveCursor(QTextCursor::End);output->insertPlainText(q(text));
            const auto excess=output->document()->characterCount()-2*1024*1024;
            if(excess>0){QTextCursor cursor(output->document());cursor.setPosition(0);cursor.setPosition(excess,QTextCursor::KeepAnchor);cursor.removeSelectedText();}}
        const auto index=stream==build::RunStream::Stdout?0U:1U;
        lines[index].feed(text,[&](auto line){parsers[index]->feed(line,[&](auto value){diagnostic(std::move(value));});});
    }
    void finish(){for(std::size_t i=0;i<2;++i){lines[i].finish([&](auto line){parsers[i]->feed(line,[&](auto value){diagnostic(std::move(value));});});parsers[i]->finish([&](auto value){diagnostic(std::move(value));});}}
};
}
void Workbench::registerRemoteActions() {
    for(const auto& [id,label]:std::array{std::pair{"remote.targets","Remote Targets..."},std::pair{"cmake.wsl","WSL CMake Toolchains..."}}) {
        input::ActionSpec action;action.id=id;action.name=label;action.group=id==std::string_view("cmake.wsl")?"Build":"Run";
        action.handler=[this,id](const auto&,const auto&){if(id==std::string_view("remote.targets"))editRemoteTargets();else showWslCMake();};
        action.enabled=[this,id](const auto&){return !m_shutDown && (id==std::string_view("remote.targets") || (!m_projectRoot.empty()&&!m_remoteBuildActive));};
        (void)m_registry.add(std::move(action));
    }
    connect(this,&Workbench::projectChanged,this,[this]{m_remoteStop.request_stop();m_remoteStop=std::stop_source{};});
}
void Workbench::editRemoteTargets() {
    if(!m_window)return;
    const auto path=registryPath(m_options.remoteTargetsPath,m_options.applicationSettingsFile,"remote-targets.json");
    auto before=readOptional(m_services.fs,path);if(!before){notify("Remote targets",q(before.error().message()),ui::NotificationKind::Error);return;}
    auto targets=*before?build::parseRemoteTargets(**before):Result<std::vector<build::RemoteTarget>>(std::vector<build::RemoteTarget>{});
    if(!targets){notify("Remote targets",q(targets.error().message()),ui::NotificationKind::Error);return;}
    auto edited=editRemoteTargetForms(m_window,std::move(*targets));if(!edited)return;
    if(auto saved=saveRegistry(m_services.fs,path,*before,build::remoteTargetsJson(*edited));!saved)notify("Remote targets",q(saved.error().message()),ui::NotificationKind::Error);
}
Result<run::RunHandlers> Workbench::prepareRemoteRun(const run::RunConfiguration& configuration) {
    if(configuration.type!=run::RunType::Remote||m_projectRoot.empty()||m_shutDown||m_remoteBuildActive)
        return fail(ErrorCode::Untrusted,"Open a trusted project and finish exclusive work before remote Run");
    const auto registry=registryPath(m_options.remoteTargetsPath,m_options.applicationSettingsFile,"remote-targets.json");
    const auto root=m_projectRoot;const auto permission=m_services.trust.snapshot(root);const auto stop=m_remoteStop.get_token();
    if(!permission.trusted)return fail(ErrorCode::Untrusted,"The project is not trusted for remote Run");
    auto admission=[trust=&m_services.trust,root,revision=permission.revision,stop]()->Result<void>{
        const auto current=trust->snapshot(root);if(stop.stop_requested())return fail(ErrorCode::Cancelled,"Remote Run context cancelled");
        if(!current.trusted||current.revision!=revision)return fail(ErrorCode::Untrusted,"Remote Run trust admission changed");return {};};
    auto selectedBytes=m_services.fs.readFileSnapshot(registry,1024U*1024U);if(!selectedBytes)return std::unexpected(selectedBytes.error());
    run::RunHandlers handlers;handlers.remoteTargetId=configuration.target;handlers.launchAdmission=admission;
    handlers.remoteLauncher=std::make_shared<DeferredEndpointLauncher>(m_services.fs,m_services.trust,m_services.launcher,root,registry,std::move(*selectedBytes),configuration.target,permission.revision,stop);
    return handlers;
}
void Workbench::showWslCMake() {
#ifndef _WIN32
    notify("WSL CMake","WSL build toolchains require the Windows IDE.",ui::NotificationKind::Warning);return;
#else
    if(!m_window||!m_pool||!m_buildService||m_projectRoot.empty()||m_remoteBuildActive)return;
    if((m_runController&&m_runController->running())||(m_debugSessions&&m_debugSessions->anyDebugging())||
        m_buildService->externalLaunchAdmissionBusy()) {notify("WSL CMake","Finish the current build, run, debugger or project edit first.",ui::NotificationKind::Warning);return;}
    const auto root=m_projectRoot;const auto rootStop=m_remoteStop.get_token();
    auto sourceStatus=m_services.fs.status(root);if(!sourceStatus){notify("WSL CMake",q(sourceStatus.error().message()),ui::NotificationKind::Error);return;}
    const auto source=sourceStatus->type==io::FileType::Directory?root:std::string(io::parentPath(root));
    const auto registry=registryPath(m_options.remoteTargetsPath,m_options.applicationSettingsFile,"remote-targets.json");
    const auto profilesPath=registryPath(m_options.wslCMakeProfilesPath,m_options.applicationSettingsFile,"wsl-cmake-profiles.json");
    auto targetBytes=readOptional(m_services.fs,registry),profileBytes=readOptional(m_services.fs,profilesPath);
    if(!targetBytes||!profileBytes){notify("WSL CMake",q(!targetBytes?targetBytes.error().message():profileBytes.error().message()),ui::NotificationKind::Error);return;}
    if(!*targetBytes){editRemoteTargets();return;}
    auto targets=build::parseRemoteTargets(**targetBytes);if(!targets){notify("WSL CMake",q(targets.error().message()),ui::NotificationKind::Error);return;}
    auto profiles=*profileBytes?build::parseWslCMakeProfiles(**profileBytes):Result<std::vector<build::WslCMakeProfile>>(std::vector<build::WslCMakeProfile>{});
    if(!profiles){notify("WSL CMake",q(profiles.error().message()),ui::NotificationKind::Error);return;}
    QStringList choices;std::vector<std::size_t> indexes;
    for(std::size_t i=0;i<profiles->size();++i)if(lang::documentKey((*profiles)[i].localSourceDirectory)==lang::documentKey(source)){choices.push_back(q((*profiles)[i].name));indexes.push_back(i);}
    choices.push_back("New WSL profile...");bool selected=false;
    const auto choice=QInputDialog::getItem(m_window,"WSL CMake","Application build profile:",choices,0,false,&selected);if(!selected)return;
    build::WslCMakeProfile profile;std::optional<std::size_t> existing;
    const auto index=choices.indexOf(choice);if(index>=0&&static_cast<std::size_t>(index)<indexes.size()){existing=indexes[static_cast<std::size_t>(index)];profile=(*profiles)[*existing];}
    else{profile.id="wsl."+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();profile.name="WSL Debug";profile.localSourceDirectory=source;profile.cacheDefinitions={"CMAKE_BUILD_TYPE:STRING=Debug"};}
    auto edited=editWslProfileForm(m_window,std::move(profile),*targets);if(!edited)return;
    if(m_projectRoot!=root||rootStop.stop_requested()||m_shutDown)return;
    if(existing)(*profiles)[*existing]=edited->profile;else profiles->push_back(edited->profile);
    const auto savedProfiles=build::wslCMakeProfilesJson(*profiles);
    if(auto saved=saveRegistry(m_services.fs,profilesPath,*profileBytes,savedProfiles);!saved){notify("WSL CMake",q(saved.error().message()),ui::NotificationKind::Error);return;}
    if(!edited->operation)return;
    run::RunConfiguration intent;intent.id="remote.wsl-build";intent.name=edited->profile.name;intent.type=run::RunType::Remote;intent.target=edited->profile.targetId;
    intent.program=edited->profile.cmakeProgram;intent.workingDirectory="/";
    if(auto trusted=mayRunConfiguration(intent);!trusted){notify("WSL CMake",q(trusted.error().message()),ui::NotificationKind::Warning);return;}
    auto target=std::find_if(targets->begin(),targets->end(),[&](const auto& row){return row.id==edited->profile.targetId;});
    if(target==targets->end()||!target->executionApproved){notify("WSL CMake","Approve the named application endpoint before executing its profile.",ui::NotificationKind::Warning);return;}
    const auto permission=m_services.trust.snapshot(root);
    auto reservation=m_buildService->reserveExternalLaunch();if(!reservation){notify("WSL CMake",q(reservation.error().message()),ui::NotificationKind::Warning);return;}
    auto reserved=std::shared_ptr<Reservation>(new Reservation{m_buildService.get(),*reservation});
    if(m_services.settings.snapshot()->get(settings::key::buildSaveAllBeforeBuild)) {
        const auto saved=saveAllDocuments();if(!saved.failed.empty()){notify("WSL CMake",q(saved.failed.front().error.message()),ui::NotificationKind::Error);return;}
    }
    if(!m_resourceGovernor)m_resourceGovernor=std::make_shared<sched::ResourceGovernor>();
    auto capture=std::make_shared<RemoteCapture>();capture->target=*target;capture->root=root;capture->registry=registry;capture->registryBytes=**targetBytes;
    capture->profiles=profilesPath;capture->profilesBytes=savedProfiles;capture->revision=permission.revision;capture->projectStop=rootStop;
    auto stop=std::make_shared<std::stop_source>();capture->jobStop=stop->get_token();
    auto* dialog=new QDialog(m_window);dialog->setObjectName("wslCMakeOutputDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(q(edited->profile.name)+" — "+q(target->name));dialog->resize(1000,700);
    auto* layout=new QVBoxLayout(dialog);auto* status=new QLabel("Waiting for governed remote command admission…",dialog);status->setObjectName("wslCMakeStatus");layout->addWidget(status);
    auto* guidance=new QLabel("Recent output (2 MiB) and up to 20,000 actual tool diagnostics. Only explicitly mapped absolute remote source locations can be opened locally.",dialog);guidance->setWordWrap(true);layout->addWidget(guidance);
    auto* output=new QPlainTextEdit(dialog);output->setReadOnly(true);output->setObjectName("wslCMakeOutput");layout->addWidget(output,2);
    auto* diagnostics=new QTableWidget(0,4,dialog);diagnostics->setObjectName("wslCMakeDiagnostics");diagnostics->setHorizontalHeaderLabels({"Tool source path","Local mapping (checked on open)","Line","Tool diagnostic"});diagnostics->setEditTriggers(QAbstractItemView::NoEditTriggers);diagnostics->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(diagnostics,1);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,dialog);layout->addWidget(buttons);
    auto* stopButton=buttons->addButton("Stop",QDialogButtonBox::ActionRole);stopButton->setObjectName("wslCMakeStop");
    connect(stopButton,&QPushButton::clicked,dialog,[stop,status]{stop->request_stop();status->setText("Stopping; waiting for actual remote cleanup acknowledgement…");});
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);connect(dialog,&QDialog::finished,dialog,[stop]{stop->request_stop();});
    auto view=std::make_shared<BuildView>();view->output=output;view->diagnostics=diagnostics;view->target=*target;
    for(auto& parser:view->parsers)parser=*edited->operation==build::WslCMakeOperation::Configure?build::makeCMakeOutputParser():build::makeCompilerOutputParser();
    const QPointer<Workbench> self(this);const QPointer<QDialog> owner(dialog);const QPointer<QLabel> statusGuard(status);const QPointer<QPushButton> stopGuard(stopButton);
    m_remoteBuildActive=true;const auto generation=++m_remoteBuildGeneration;
    connect(diagnostics,&QTableWidget::cellDoubleClicked,dialog,[self,owner,view,capture,root](int row,int){
        if(!self||!owner||row<0||static_cast<std::size_t>(row)>=view->rows.size())return;
        const auto diagnostic=view->rows[static_cast<std::size_t>(row)];
        auto result=[self,owner,root,diagnostic](Result<std::string> local){if(self)QMetaObject::invokeMethod(self.data(),[=]{if(self&&owner&&self->m_projectRoot==root&&local)self->openLocation(q(*local),diagnostic.line,std::max(1,diagnostic.column));},Qt::QueuedConnection);};
        auto navigate=[](io::IFileSystem* fs,project::TrustStore* trust,std::shared_ptr<RemoteCapture> captured,std::string remote,std::function<void(Result<std::string>)> done)->sched::Task<void>{
            if(auto current=admit(*fs,*trust,*captured);!current){done(std::unexpected(current.error()));co_return;}
            auto local=build::mapRemoteToLocal(captured->target,remote);if(!local){done(std::unexpected(local.error()));co_return;}
            auto physical=fs->physicalPath(*local);bool mapped=false;
            for(const auto& [base,identity]:captured->mappingIdentities)if(within(base,*local)&&physical&&within(identity,*physical)){mapped=true;break;}
            if(!mapped){done(fail(ErrorCode::Untrusted,"Remote diagnostic mapping escaped its captured physical local root"));co_return;}
            if(auto current=admit(*fs,*trust,*captured);!current){done(std::unexpected(current.error()));co_return;}
            done(std::move(*local));co_return;};
        self->m_pool->spawn(sched::Lane::Background,navigate(&self->m_services.fs,&self->m_services.trust,capture,diagnostic.file,std::move(result))).detach();
    });
    auto stream=[self,owner,view,root,generation](build::RunStream channel,std::string text){if(self)QMetaObject::invokeMethod(self.data(),[=]{if(self&&owner&&self->m_remoteBuildGeneration==generation&&self->m_projectRoot==root)view->feed(channel,text);},Qt::QueuedConnection);};
    auto done=[self,owner,statusGuard,stopGuard,view,generation](Result<build::RunExit> result){if(self)QMetaObject::invokeMethod(self.data(),[=]{
        if(!self||self->m_remoteBuildGeneration!=generation)return;self->m_remoteBuildActive=false;
        if(self->m_cmake)self->m_cmake->service().admissionAvailable();
        if(stopGuard)stopGuard->setEnabled(false);
        if(!owner||!statusGuard)return;view->finish();
        if(!result)statusGuard->setText(q(result.error().describe()));
        else if(result->waitFailed||result->cleanupFailed)statusGuard->setText("Remote result/cleanup could not be confirmed; inspect actual transport output.");
        else statusGuard->setText(QStringLiteral("Actual remote exit code: %1%2").arg(result->exitCode).arg(result->stopped?QStringLiteral(" (stopped)"):QString{}));
    },Qt::QueuedConnection);};
    dialog->show();
    m_pool->spawn(sched::Lane::Background,runWslBuild(&m_services.fs,&m_services.launcher,&m_services.trust,capture,edited->profile,*edited->operation,m_resourceGovernor,std::move(reserved),std::move(stream),std::move(done))).detach();
#endif
}
} // namespace frost::app
