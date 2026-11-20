// SPDX-License-Identifier: Apache-2.0
// New engine assets use the authenticated engine's actual generators, sidecars and Undo stack.
#include "engine_integration_internal.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
namespace frost::app {
namespace {
QString templateText(std::string_view value) { return QString::fromUtf8(value.data(),static_cast<qsizetype>(value.size())); }
}
void EngineIntegration::registerTemplateActions() {
    input::ActionSpec action;action.id="engine.template.new";action.name="New FrostEngine Asset from Template";action.group="FrostEngine";
    action.handler=[this](const input::ActionContext&,const input::InvokeInfo&) {
        auto& impl=*m_impl;
        if(!impl.session||!impl.identity||!impl.state||impl.state->state!="Edit") {
            impl.setStatus(QStringLiteral("Pair an engine in Edit state before creating an asset"));return;
        }
        const auto root=impl.root;const auto epoch=impl.epoch,sessionEpoch=impl.sessionEpoch,revision=impl.trustRevision;
        QPointer<EngineIntegration> self(this);QDialog dialog;
        dialog.setObjectName("engineAssetTemplateDialog");dialog.setWindowTitle("New FrostEngine Asset");
        dialog.setWindowModality(Qt::ApplicationModal);
        connect(this,&QObject::destroyed,&dialog,&QDialog::reject);
        auto* layout=new QVBoxLayout(&dialog);auto* form=new QFormLayout();
        auto* kind=new QComboBox(&dialog);kind->setObjectName("engineAssetTemplateKind");
        kind->addItems({"Scene","Prefab","Material","Behaviour","Physics","Test","Module"});
        auto* path=new QLineEdit(QStringLiteral("Assets/Scenes/NewScene.fscene"),&dialog);path->setObjectName("engineAssetTemplatePath");
        form->addRow("Engine template:",kind);form->addRow("New asset path:",path);layout->addLayout(form);
        auto* explanation=new QLabel("Create uses the paired engine's generator and registers its real asset and .fmeta sidecar. Existing files are refused. Undo belongs to the paired FrostEditor.",&dialog);
        explanation->setWordWrap(true);layout->addWidget(explanation);
        connect(kind,&QComboBox::currentIndexChanged,&dialog,[path](int index) {
            static const char* defaults[]={"Assets/Scenes/NewScene.fscene","Assets/Prefabs/NewPrefab.fprefab","Assets/Materials/NewMaterial.fmat","Assets/Scripts/NewBehaviour.lua","Assets/Scripts/NewPhysics.lua","Assets/Scripts/NewTest.lua","Assets/Scripts/NewModule.lua"};
            if(index>=0&&index<7)path->setText(QString::fromLatin1(defaults[index]));
        });
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
        layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.resize(620,220);
        if(dialog.exec()!=QDialog::Accepted||!self)return;
        auto& current=*self->m_impl;
        if(current.sessionEpoch!=sessionEpoch||!current.admitted(epoch,root,revision)) {
            current.setStatus(QStringLiteral("Engine project/session changed; prepare the new asset again"));return;
        }
        auto params=lang::Json::object().set("kind",kind->currentText().toUtf8().toStdString())
            .set("path",path->text().trimmed().toUtf8().toStdString());
        auto sent=self->contentCommand("document.create-template",std::move(params),[self](Result<lang::Json> reply) {
            if(!self)return;auto& observed=*self->m_impl;
            if(!reply){observed.setStatus(templateText(reply.error().message()));return;}
            const auto created=reply->stringAt("path");
            if(created.empty()){observed.setStatus(QStringLiteral("Engine returned no created asset path; inspect its actual reply"));return;}
            observed.setStatus(QStringLiteral("Engine created and registered ")+templateText(created));
            observed.openAsset(std::string(created));
            self->invalidateAssetUsages();
        });
        if(!sent)current.setStatus(templateText(sent.error().message()));
    };
    (void)m_impl->workbench.m_registry.add(std::move(action));
}
}
