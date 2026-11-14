// SPDX-License-Identifier: Apache-2.0
// Production template preview/Create/global history, with no generated-file prewrites.
#include "workbench_rig.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/rpc/json.hpp"
#include <QByteArray>
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <gtest/gtest.h>

namespace frost::app::test {
namespace {
struct PreviewAnswer {
    QTimer timer,deadline;
    int visits=0;
    explicit PreviewAnswer(bool accept,std::function<void()> before={}) {
        timer.setInterval(1);
        QObject::connect(&timer,&QTimer::timeout,[this,accept,before=std::move(before)] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if(!dialog || dialog->objectName()!=QStringLiteral("sourceTemplatePreview")) return;
            ++visits; timer.stop(); if(before) before(); if(accept) dialog->accept(); else dialog->reject();
        });
        deadline.setSingleShot(true); deadline.setInterval(20000);
        QObject::connect(&deadline,&QTimer::timeout,[] {
            ADD_FAILURE()<<"Template preparation/preview did not complete within the existing UI wait budget";
            if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
        });
        timer.start(); deadline.start();
    }
};
RigOptions optionsFor(const WorkDir& dir) {
    RigOptions options; options.workbench.refactoringStateDirectory=dir.join("private-refactoring"); return options;
}
SourceTemplateSpec classSpec(const WorkDir& dir) {
    SourceTemplateSpec spec; spec.directory=dir.join("workspace"); spec.file={lang::FileTemplateKind::Class,"sample::Widget","Actual Author","Widget documentation"}; return spec;
}
TEST(WorkbenchTemplates, ClassPreviewHasNoFilesThenOneUndoRestoresBothAbsencesAndOpenView) {
    WorkDir dir("templates-class-history"); dir.write("workspace/keep.txt","unchanged"); WorkbenchRig rig(dir,optionsFor(dir));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace"))); ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    PreviewAnswer preview(true,[&] {
        EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/Widget.hpp")).value_or(true));
        EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/Widget.cpp")).value_or(true));
    });
    auto created=rig.workbench->createSourceTemplate(classSpec(dir)); ASSERT_TRUE(created) << created.error().describe();
    EXPECT_EQ(preview.visits,1); ASSERT_EQ(created->size(),2);
    EXPECT_NE(dir.read("workspace/Widget.hpp").find("@author Actual Author"),std::string::npos);
    EXPECT_NE(dir.read("workspace/Widget.cpp").find("sample::Widget::Widget()"),std::string::npos);
    ASSERT_NE(rig.workbench->currentView(),nullptr); EXPECT_EQ(rig.workbench->currentView()->path(),dir.join("workspace/Widget.hpp"));
    auto undone=rig.workbench->undoRefactoring(); ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_EQ(undone->status,document::RefactoringStatus::Undone);
    EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/Widget.hpp")).value_or(true));
    EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/Widget.cpp")).value_or(true));
    EXPECT_EQ(rig.workbench->currentView()->document().diskState(),document::DiskState::Deleted);
    auto redone=rig.workbench->redoRefactoring(); ASSERT_TRUE(redone); EXPECT_EQ(redone->status,document::RefactoringStatus::Redone);
    EXPECT_EQ(rig.workbench->currentView()->document().diskState(),document::DiskState::InSync);
    EXPECT_EQ(dir.read("workspace/keep.txt"),"unchanged");
}
TEST(WorkbenchTemplates, DeclinedPreviewAndExistingHeaderCannotLeaveAHalfGeneratedClass) {
    WorkDir dir("templates-class-refusal"); dir.write("workspace/keep.txt","unchanged"); WorkbenchRig rig(dir,optionsFor(dir));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace"))); ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    { PreviewAnswer preview(false); auto created=rig.workbench->createSourceTemplate(classSpec(dir)); ASSERT_FALSE(created); EXPECT_EQ(created.error().code(),ErrorCode::Cancelled); EXPECT_EQ(preview.visits,1); }
    EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/Widget.hpp")).value_or(true)); EXPECT_FALSE(rig.workbench->canUndoRefactoring());
    dir.write("workspace/Widget.hpp","user-owned bytes");
    auto collision=rig.workbench->createSourceTemplate(classSpec(dir)); ASSERT_FALSE(collision);
    EXPECT_EQ(dir.read("workspace/Widget.hpp"),"user-owned bytes");
    EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/Widget.cpp")).value_or(true));
}
TEST(WorkbenchTemplates, NewProjectScaffoldsRenderOnlyTheActualProvidedAuthorAndDescription) {
    for(const auto kind:{NewProjectTemplate::CMakeStaticLibrary,NewProjectTemplate::VsCppStaticLibrary}) {
        WorkDir dir("template-project-author"); WorkbenchRig rig(dir);
        NewProjectSpec spec; spec.name="Library"; spec.location=dir.path(); spec.kind=kind; spec.author="Actual Author"; spec.description="Actual description";
        auto created=rig.workbench->createNewProject(spec); ASSERT_TRUE(created) << created.error().describe();
        EXPECT_NE(dir.read("Library/Library.cpp").find("Author: Actual Author"),std::string::npos);
        EXPECT_NE(dir.read("Library/Library.cpp").find("Actual description"),std::string::npos);
    }
}
TEST(WorkbenchTemplates, SettledCallbackClosingOwnerPreservesSuccessfulCreationWithoutOpeningViews) {
    WorkDir dir("templates-owner-closed-on-settlement"); dir.write("workspace/keep.txt","unchanged");
    WorkbenchRig rig(dir,optionsFor(dir)); ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    bool settled=false;
    QObject::connect(rig.workbench.get(),&Workbench::refactoringSettled,[&](const document::RefactoringOutcome& outcome) {
        if(outcome.status!=document::RefactoringStatus::Applied) return;
        settled=true; rig.workbench->shutdown(); rig.window.reset(); rig.workbench.reset();
    });
    PreviewAnswer preview(true);
    auto created=rig.workbench->createSourceTemplate(classSpec(dir));
    ASSERT_TRUE(created) << created.error().describe(); EXPECT_TRUE(settled); EXPECT_FALSE(rig.workbench);
    EXPECT_EQ(created->size(),2);
    EXPECT_NE(dir.read("workspace/Widget.hpp").find("class Widget"),std::string::npos);
    EXPECT_NE(dir.read("workspace/Widget.cpp").find("Widget::Widget()"),std::string::npos);
}
#ifdef _WIN32
TEST(WorkbenchTemplates, ScriptedHostMembershipAndClassFilesShareOneProductionApplyAndUndo) {
    WorkDir dir("templates-membership-composition");
    const std::string guid="{12345678-1234-1234-1234-123456789012}";
    dir.write("workspace/Demo.sln","Microsoft Visual Studio Solution File, Format Version 12.00\nProject(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"App\", \"App\\App.vcxproj\", \""+guid+"\"\nEndProject\nGlobal\nGlobalSection(SolutionConfigurationPlatforms) = preSolution\nDebug|x64 = Debug|x64\nEndGlobalSection\nGlobalSection(ProjectConfigurationPlatforms) = postSolution\n"+guid+".Debug|x64.ActiveCfg = Debug|x64\n"+guid+".Debug|x64.Build.0 = Debug|x64\nEndGlobalSection\nEndGlobal\n");
    const std::string before="<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"><ItemGroup><ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup><PropertyGroup><ProjectGuid>"+guid+"</ProjectGuid><ConfigurationType>StaticLibrary</ConfigurationType></PropertyGroup></Project>";
    auto after=before; after.insert(after.rfind("</Project>"),"<ItemGroup><ClInclude Include=\"Widget.hpp\"/><ClCompile Include=\"Widget.cpp\"/></ItemGroup>");
    dir.write("workspace/App/App.vcxproj",before);
    const auto encoded=[](const std::string& value){return QByteArray::fromStdString(value).toBase64().toStdString();};
    auto answer=rpc::Json::object().set("Error",nullptr).set("Edit",rpc::Json::object()
        .set("PlanVersion",1).set("Mode","BeforeFileCreateAdd").set("ProjectPath",dir.join("workspace/App/App.vcxproj"))
        .set("ProjectBefore",encoded(before)).set("Project",encoded(after)).set("FiltersBefore",nullptr).set("Filters",nullptr)
        .set("Configurations",rpc::Json::array().push("Debug|x64")).set("Environment","scripted-unit-receipt").set("InputBytes",rpc::Json::array()));
    io::ScriptedProcessLauncher processes; processes.expect({.stdoutBytes=answer.dump()+"\n",.outputHeld=true});
    auto options=optionsFor(dir); options.launcher=&processes; options.disableProjectEvaluation=true;
    options.workbench.projectEditHostPath=dir.join("tools/ProjectHost.exe"); options.workbench.msbuildPath=dir.join("tools/MSBuild.exe");
    WorkbenchRig rig(dir,options); ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openSolution(dir.join("workspace/Demo.sln")));
    auto spec=classSpec(dir); spec.directory=dir.join("workspace/App"); spec.vcxprojPath=dir.join("workspace/App/App.vcxproj");
    PreviewAnswer preview(true,[&] { EXPECT_EQ(dir.read("workspace/App/App.vcxproj"),before); EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/App/Widget.hpp")).value_or(true)); });
    auto created=rig.workbench->createSourceTemplate(spec); ASSERT_TRUE(created)<<created.error().describe();
    EXPECT_EQ(dir.read("workspace/App/App.vcxproj"),after);
    ASSERT_EQ(processes.launches().size(),1); EXPECT_NE(processes.launches()[0].stdinBytes.find("BeforeFileCreate"),std::string::npos);
    EXPECT_EQ(processes.launches()[0].stdinBytes.find("Decision"),std::string::npos);
    auto undo=rig.workbench->undoRefactoring(); ASSERT_TRUE(undo)<<undo.error().describe();
    EXPECT_EQ(dir.read("workspace/App/App.vcxproj"),before);
    EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/App/Widget.hpp")).value_or(true));
    EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/App/Widget.cpp")).value_or(true));
    auto redo=rig.workbench->redoRefactoring(); ASSERT_TRUE(redo);
    EXPECT_EQ(dir.read("workspace/App/App.vcxproj"),after); EXPECT_TRUE(io::realFileSystem().exists(dir.join("workspace/App/Widget.cpp")).value_or(false));
}
#endif
}
}
