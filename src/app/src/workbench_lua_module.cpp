// SPDX-License-Identifier: Apache-2.0
// Complete bounded workspace require-user capture; no source is moved before REF1 admission.
#include "frost/app/workbench.hpp"
#include "frost/lang/lua_module_rename.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/document.hpp"
#include "frost/text/text_file.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/lane.hpp"
#include "workbench_source_rename.hpp"
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPointer>
#include <algorithm>
#include <map>
namespace frost::app {
namespace {
struct OpenLuaInput {std::string path,physical; text::Snapshot snapshot;};
struct DirectoryReceipt {std::string path,physical;std::vector<io::DirEntry> entries;};
struct PreparedLuaModule {document::RefactoringPlan plan;std::vector<DirectoryReceipt> directories;std::vector<std::pair<std::string,std::string>> identities;};
void sortEntries(std::vector<io::DirEntry>& entries) {std::ranges::sort(entries,{},&io::DirEntry::name);}
Result<PreparedLuaModule> captureModule(io::IFileSystem& fs,lang::LuaModuleRenameRequest request,
    const std::vector<OpenLuaInput>& open,lang::Json configuration,std::stop_token stop) {
    PreparedLuaModule captured;captured.plan.label="Rename Lua module and resolved workspace require users";
    std::map<std::string,RefactoringInputSnapshot> inputs;std::map<std::string,text::DocumentEncoding> encodings;
    std::map<std::string,const OpenLuaInput*> openIndex;
    for(const auto& input:open) {
        auto [found,inserted]=openIndex.emplace(lang::documentKey(input.physical),&input);
        if(!inserted&&(lang::documentKey(found->second->path)!=lang::documentKey(input.path)||found->second->snapshot.text()!=input.snapshot.text()))
            return fail(ErrorCode::Busy,"Ambiguous open Lua input aliases");
    }
    const auto capture=[&](std::string path)->Result<RefactoringInputSnapshot> {
        std::optional<std::string> buffer;
        auto exists=fs.exists(path);if(!exists)return std::unexpected(exists.error());
        if(*exists) {
            auto physical=fs.physicalPath(path);if(!physical)return std::unexpected(physical.error());
            const auto found=openIndex.find(lang::documentKey(*physical));
            const OpenLuaInput* selected=found==openIndex.end()?nullptr:found->second;
            captured.identities.emplace_back(path,*physical);
            if(selected) {path=selected->path;buffer=selected->snapshot.text();}
        }
        return captureRefactoringInput(fs,std::move(path),std::move(buffer));
    };
    // Project-native config overrides service defaults. Dirty config is not a server-observed receipt.
    for(const auto* filename:{".luarc.json",".luarc.jsonc"}) {
        auto path=io::joinPath(request.workspaceRoot,filename);if(!path)return std::unexpected(path.error());
        auto input=capture(*path);if(!input)return std::unexpected(input.error());
        if(auto dependency=appendRefactoringDependency(captured.plan,*input);!dependency)return std::unexpected(dependency.error());
        if(!input->disk)continue;
        if(std::string_view(filename).ends_with("jsonc"))return fail(ErrorCode::Unsupported,"Module rename currently requires JSON runtime.path configuration, not JSONC");
        if(auto evaluated=refactoringInputMatchesEvaluation(*input,input->disk);!evaluated)return std::unexpected(evaluated.error());
        auto decoded=text::decodeBytes(*input->disk);if(!decoded||decoded->encoding.fidelity.loss!=text::LossKind::None)return fail(ErrorCode::Unsupported,"Lua config cannot be decoded losslessly");
        auto parsed=lang::parseJson(decoded->payloadIsText?std::string_view(*input->disk).substr(decoded->payloadOffset):std::string_view(decoded->text));
        if(!parsed||!parsed->isObject())return fail(ErrorCode::Corrupt,"Invalid .luarc.json");
        const lang::Json* paths=parsed->find("Lua.runtime.path");if(!paths)paths=parsed->find("runtime.path");
        if(!paths)if(const auto* runtime=parsed->find("runtime"))paths=runtime->find("path");
        if(paths)configuration.set("runtime",lang::Json::object().set("path",*paths));
    }
    request.runtimePaths={"?.lua","?/init.lua"};
    if(const auto* runtime=configuration.find("runtime"))if(const auto* paths=runtime->find("path")) {
        if(!paths->isArray()||paths->size()>32)return fail(ErrorCode::Unsupported,"runtime.path is not a bounded literal list");
        request.runtimePaths.clear();for(const auto& path:paths->items()) {
            const auto* value=path.asString();if(!value)return fail(ErrorCode::Unsupported,"runtime.path must contain strings");request.runtimePaths.push_back(*value);
        }
    }
    std::vector<std::string> pending{request.workspaceRoot};std::size_t entriesSeen=0,sourceBytes=0;
    while(!pending.empty()) {
        if(stop.stop_requested())return fail(ErrorCode::Cancelled,"Lua workspace capture cancelled");
        auto path=std::move(pending.back());pending.pop_back();
        if(captured.directories.size()>=1024)return fail(ErrorCode::OutOfRange,"Module rename workspace exceeds 1024 directories; use a narrower workspace");
        auto listing=fs.listDirectoryWithDiagnostics(path);if(!listing)return std::unexpected(listing.error());
        if(!listing->issues.empty())return fail(ErrorCode::Unsupported,"Workspace contains unrepresentable names; module-user inventory would be incomplete");
        entriesSeen+=listing->entries.size();if(entriesSeen>20000)return fail(ErrorCode::OutOfRange,"Module rename workspace exceeds 20000 entries");
        auto physical=fs.physicalPath(path);if(!physical)return std::unexpected(physical.error());
        sortEntries(listing->entries);captured.directories.push_back({path,*physical,listing->entries});
        for(const auto& entry:listing->entries) {
            if(stop.stop_requested())return fail(ErrorCode::Cancelled,"Lua workspace capture cancelled");
            if(entry.name==".git")continue;
            auto child=io::joinPath(path,entry.name);if(!child)return std::unexpected(child.error());
            if(entry.type==io::FileType::Symlink)return fail(ErrorCode::Unsupported,"Module rename does not silently skip or follow workspace links: "+*child);
            if(entry.type==io::FileType::Directory) {pending.push_back(*child);continue;}
            if(entry.type!=io::FileType::Regular||syntax::languageForFileName(entry.name)!=syntax::LanguageId::Lua)continue;
            auto input=capture(*child);if(!input)return std::unexpected(input.error());if(!input->disk)return fail(ErrorCode::Busy,"Lua source disappeared during capture");
            sourceBytes+=input->disk->size();if(sourceBytes>64*1024*1024||request.sources.size()>=10000)return fail(ErrorCode::OutOfRange,"Lua source inventory exceeds 64 MiB/10000 files");
            auto decoded=text::decodeBytes(*input->disk);if(!decoded||decoded->encoding.fidelity.loss!=text::LossKind::None||decoded->binaryLikely)return fail(ErrorCode::Unsupported,"Lua source cannot be decoded losslessly: "+input->path);
            auto logical=input->buffer?*input->buffer:decoded->payloadIsText?input->disk->substr(decoded->payloadOffset):decoded->text;
            auto document=text::Document::fromUtf8(std::move(logical));if(!document)return std::unexpected(document.error());
            request.sources.push_back({input->path,document->text()});encodings.emplace(input->path,decoded->encoding);
            if(auto dependency=appendRefactoringDependency(captured.plan,*input);!dependency)return std::unexpected(dependency.error());
            const auto inputPath=input->path;inputs.emplace(inputPath,std::move(*input));
        }
    }
    auto proposal=lang::planLuaModuleRename(fs,request,stop);if(!proposal)return std::unexpected(proposal.error());
    for(const auto& probe:proposal->probes) {
        auto input=capture(probe.path);if(!input)return std::unexpected(input.error());
        if(input->disk!=probe.bytes)return fail(ErrorCode::Busy,"Lua module lookup changed during analysis");
        if(probe.bytes) {
            auto physical=fs.physicalPath(probe.path);
            if(!physical||lang::documentKey(*physical)!=lang::documentKey(probe.physicalPath))
                return fail(ErrorCode::Busy,"Lua module lookup physical identity changed during analysis");
            captured.identities.emplace_back(probe.path,probe.physicalPath);
        }
        if(auto dependency=appendRefactoringDependency(captured.plan,*input);!dependency)return std::unexpected(dependency.error());
    }
    for(const auto& file:proposal->files) {
        auto after=file.original.text();for(auto edit=file.edits.rbegin();edit!=file.edits.rend();++edit)
            after.replace(edit->range.begin.value(),edit->range.end.value()-edit->range.begin.value(),edit->replacement);
        auto replacement=text::Document::fromUtf8(std::move(after));if(!replacement)return std::unexpected(replacement.error());
        auto beforeBytes=text::encodeForSave(file.original,encodings.at(file.path));if(!beforeBytes)return std::unexpected(beforeBytes.error());
        auto afterBytes=text::encodeForSave(replacement->text(),encodings.at(file.path));if(!afterBytes)return std::unexpected(afterBytes.error());
        if(auto edit=appendRefactoringReplacement(captured.plan,inputs.at(file.path),*beforeBytes,std::move(*afterBytes));!edit)return std::unexpected(edit.error());
    }
    return captured;
}
sched::Task<void> captureModuleTask(io::IFileSystem* fs,lang::LuaModuleRenameRequest request,std::vector<OpenLuaInput> open,
    lang::Json configuration,std::stop_token stop,std::function<void(Result<PreparedLuaModule>)> done) {
    done(captureModule(*fs,std::move(request),open,std::move(configuration),stop));co_return;
}
QString moduleText(std::string_view text){return QString::fromUtf8(text.data(),static_cast<qsizetype>(text.size()));}
}
void Workbench::renameLuaModule() {
    auto* view=m_actionHost.active();if(!view||syntax::languageForFileName(view->path())!=syntax::LanguageId::Lua||!m_lua||m_projectRoot.empty())return;
    const auto root=m_projectRoot,from=view->path();const auto provider=m_lua;const auto trust=m_services.trust.snapshot(root);
    if(!trust.trusted)return;
    auto caps=provider->capabilities();if(!caps||caps->serverName!="sumneko.lua"||caps->serverVersion!=lang::pinnedLuaLanguageServerVersion()) {
        // Actual pinned provider.lua initialize response. Other providers need their own resolver profile.
        QMessageBox::warning(m_window,"Rename Lua Module","This resolver profile requires the pinned LuaLS service.");return;
    }
    if(!provider->folders().empty()) {QMessageBox::warning(m_window,"Rename Lua Module","Multi-root module resolution is not yet supported; no file was renamed.");return;}
    const auto configuration=provider->configuration();const auto configurationReceipt=configuration.dump();
    const QPointer<Workbench> owner(this);bool accepted=false;
    const auto newName=QInputDialog::getText(m_window,"Rename Lua Module","New same-directory module filename (.lua):",QLineEdit::Normal,moduleText(io::fileName(from)),&accepted).toStdString();
    if(!owner||!accepted)return;auto to=io::joinPath(io::parentPath(from),newName);if(!to)return;
    const auto current=m_services.trust.snapshot(root);
    if(m_projectRoot!=root||m_lua!=provider||!current.trusted||current.revision!=trust.revision||provider->configuration().dump()!=configurationReceipt)return;
    std::vector<OpenLuaInput> open;
    for(auto* document:m_editors->views()) {
        if(document->path().empty())continue;if(open.size()>=4096)return;
        auto physical=m_services.fs.physicalPath(document->path());if(!physical) {QMessageBox::warning(m_window,"Rename Lua Module",moduleText(physical.error().message()));return;}
        open.push_back({document->path(),*physical,document->document().document().text()});
    }
    const auto sameContext=[owner,root,provider,trust,configurationReceipt,open]()->Result<void> {
        if(!owner||owner->m_shutDown||owner->m_projectRoot!=root||owner->m_lua!=provider||provider->configuration().dump()!=configurationReceipt)return fail(ErrorCode::Busy,"Lua module context changed");
        const auto permission=owner->m_services.trust.snapshot(root);if(!permission.trusted||permission.revision!=trust.revision)return fail(ErrorCode::Untrusted,"Workspace trust changed");
        std::size_t count=0;for(auto* view:owner->m_editors->views())if(!view->path().empty()) {
            ++count;const auto found=std::ranges::find(open,view->path(),&OpenLuaInput::path);
            if(found==open.end()||view->document().document().text().text()!=found->snapshot.text())return fail(ErrorCode::Busy,"Open source/configuration layout changed");
            auto physical=owner->m_services.fs.physicalPath(view->path());if(!physical||lang::documentKey(*physical)!=lang::documentKey(found->physical))return fail(ErrorCode::Busy,"Open input physical identity changed");
        }
        if(count!=open.size())return fail(ErrorCode::Busy,"Open document layout changed");return {};
    };
    auto cancellation=std::make_shared<std::stop_source>();auto* progress=new QProgressDialog("Resolving actual workspace require calls…","Cancel",0,0,m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);progress->setWindowModality(Qt::NonModal);progress->show();const QPointer<QProgressDialog> pending(progress);
    connect(progress,&QProgressDialog::canceled,progress,[cancellation]{cancellation->request_stop();});
    lang::LuaModuleRenameRequest request{root,from,*to,{},{}};
    m_retiredLua.push_back(m_pool->spawn(sched::Lane::UserVisible,captureModuleTask(&m_services.fs,std::move(request),std::move(open),configuration,cancellation->get_token(),
        [owner,from,to=*to,sameContext,cancellation,pending](Result<PreparedLuaModule> prepared) mutable {
            if(!owner)return;
            QMetaObject::invokeMethod(owner.data(),[owner,from,to,sameContext,cancellation,pending,prepared=std::move(prepared)]() mutable {
                if(pending){pending->reset();pending->deleteLater();}if(!owner||cancellation->stop_requested())return;
                const auto context=sameContext();if(!context)return;
                if(!prepared){QMessageBox::warning(owner->m_window,"Module rename refused",moduleText(prepared.error().message()));return;}
                auto rename=owner->prepareProjectSourceRename(from,to);if(!owner)return;
                if(!rename){QMessageBox::warning(owner->m_window,"Module rename refused",moduleText(rename.error().message()));return;}
                auto& plan=prepared->plan;
                plan.disk.insert(plan.disk.end(),rename->disk.begin(),rename->disk.end());plan.buffers.insert(plan.buffers.end(),rename->buffers.begin(),rename->buffers.end());
                plan.dependencies.insert(plan.dependencies.end(),rename->dependencies.begin(),rename->dependencies.end());plan.bufferDependencies.insert(plan.bufferDependencies.end(),rename->bufferDependencies.begin(),rename->bufferDependencies.end());plan.renames=std::move(rename->renames);
                const auto renameContext=rename->revalidateContext;
                plan.revalidateContext=[owner,sameContext,renameContext,dirs=prepared->directories,identities=prepared->identities]()->Result<void> {
                    if(auto context=sameContext();!context)return context;
                    if(renameContext)if(auto context=renameContext();!context)return context;
                    for(const auto& dir:dirs){auto listing=owner->m_services.fs.listDirectoryWithDiagnostics(dir.path);if(!listing||!listing->issues.empty())return fail(ErrorCode::Busy,"Lua workspace namespace changed");sortEntries(listing->entries);auto physical=owner->m_services.fs.physicalPath(dir.path);if(listing->entries!=dir.entries||!physical||lang::documentKey(*physical)!=lang::documentKey(dir.physical))return fail(ErrorCode::Busy,"Lua workspace namespace changed");}
                    for(const auto& [path,expected]:identities){auto physical=owner->m_services.fs.physicalPath(path);if(!physical||lang::documentKey(*physical)!=lang::documentKey(expected))return fail(ErrorCode::Busy,"Lua input identity changed");}
                    return {};
                };
                if(!owner->previewCppRefactoring(plan)||!owner)return;
                auto outcome=owner->applyRefactoring(plan);if(!owner)return;
                if(!outcome)QMessageBox::warning(owner->m_window,"Module rename refused",moduleText(outcome.error().message()));
                else if(outcome->status==document::RefactoringStatus::PendingRecovery)QMessageBox::warning(owner->m_window,"Module rename committed","Settlement is pending recovery; do not retry.");
            },Qt::QueuedConnection);
        })));
}
}
