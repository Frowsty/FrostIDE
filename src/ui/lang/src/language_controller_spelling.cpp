// SPDX-License-Identifier: Apache-2.0
// Explicit selected-prose spellcheck uses installed dictionaries and normal encoded editor edits.
#include "frost/ui/lang/language_controller.hpp"
#include "frost/lang/spelling.hpp"
#include "language_controller_internal.hpp"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMetaObject>
#include <QProgressDialog>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <thread>

namespace frost::ui::lang {
void LanguageController::checkSelectionSpelling() {
    QPointer<LanguageController> owner(this);
    const auto snapshot = m_services.host.snapshot();
    const auto range = m_services.host.selection();
    if (!snapshot || !range || range->empty()) { unavailable(QStringLiteral("Select comment, documentation or string text to check its spelling")); return; }
    if (range->end.value()-range->begin.value()>65536) { unavailable(QStringLiteral("Select at most 64 KiB of prose")); return; }
    const auto identity=m_services.host.structureIdentity();
    const auto version=m_services.host.documentVersion();
    auto languages=::frost::lang::spellingLanguages();
    if (!languages) { report(languages.error(),"Spelling dictionaries"); return; }
    if (languages->empty()) { unavailable(QStringLiteral("No spelling dictionaries are installed")); return; }
    QStringList choices; for(const auto& language:*languages) choices.push_back(detail::qs(language));
    bool accepted=false;
    const auto language=QInputDialog::getItem(m_services.host.editorWidget(),QStringLiteral("Spelling language"),
        QStringLiteral("Installed dictionary:"),choices,0,false,&accepted);
    if (!owner || !accepted) return;
    const auto selected=snapshot->text(*range);
    QProgressDialog progress(QStringLiteral("Checking selected prose…"),QString(),0,0);
    progress.setWindowModality(Qt::ApplicationModal); progress.setMinimumDuration(0);
    std::optional<Result<std::vector<::frost::lang::SpellingIssue>>> result;
    std::jthread worker([&] {
        auto checked=::frost::lang::checkSpelling(selected,language.toUtf8().toStdString());
        QMetaObject::invokeMethod(&progress,[&,checked=std::move(checked)]() mutable { result=std::move(checked); progress.done(0); },Qt::QueuedConnection);
    });
    progress.exec(); worker.join();
    if (!owner) return;
    if (!result) return;
    if (!*result) { report(result->error(),"Check Spelling"); return; }
    if ((**result).empty()) { emit statusMessage(QStringLiteral("The installed dictionary reports no spelling errors in this selection")); return; }
    QDialog dialog; dialog.setWindowModality(Qt::ApplicationModal); dialog.setWindowTitle(QStringLiteral("Spelling corrections"));
    connect(this,&QObject::destroyed,&dialog,&QDialog::reject);
    auto* layout=new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Choose corrections from the installed dictionary. Keep leaves an occurrence unchanged."),&dialog));
    auto* table=new QTableWidget(static_cast<int>((**result).size()),2,&dialog);
    table->setHorizontalHeaderLabels({QStringLiteral("Word"),QStringLiteral("Correction")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    std::vector<QComboBox*> corrections;
    for(std::size_t i=0;i<(**result).size();++i) {
        const auto& issue=(**result)[i];
        auto* word=new QTableWidgetItem(detail::qs(issue.word)); word->setFlags(word->flags() & ~Qt::ItemIsEditable); table->setItem(static_cast<int>(i),0,word);
        auto* choice=new QComboBox(table); choice->addItem(QStringLiteral("Keep"));
        for(const auto& suggestion:issue.suggestions) choice->addItem(suggestion.empty()?QStringLiteral("Delete"):detail::qs(suggestion));
        corrections.push_back(choice); table->setCellWidget(static_cast<int>(i),1,choice);
    }
    layout->addWidget(table);
    auto* add=new QPushButton(QStringLiteral("Add selected word to dictionary"),&dialog); layout->addWidget(add);
    connect(add,&QPushButton::clicked,&dialog,[&] {
        if (!owner) return;
        const int row=table->currentRow(); if(row<0) return;
        auto saved=::frost::lang::addSpellingWord((**result)[static_cast<std::size_t>(row)].word,language.toUtf8().toStdString());
        if (!saved) report(saved.error(),"Add dictionary word");
        else emit statusMessage(QStringLiteral("Word added to the installed user dictionary"));
    });
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); layout->addWidget(buttons);
    dialog.resize(600,380); if(dialog.exec()!=QDialog::Accepted) return;
    if (!owner) return;
    if(m_services.host.structureIdentity()!=identity || m_services.host.documentVersion()!=version) { unavailable(QStringLiteral("Document changed; check the selection again")); return; }
    std::vector<::frost::lang::ByteEdit> edits;
    for(std::size_t i=0;i<corrections.size();++i) {
        const int option=corrections[i]->currentIndex(); if(option<=0) continue;
        const auto& issue=(**result)[i];
        edits.push_back({{ByteOffset{range->begin.value()+issue.begin},ByteOffset{range->begin.value()+issue.end}},issue.suggestions[static_cast<std::size_t>(option-1)]});
    }
    std::ranges::sort(edits,{},[](const auto& edit) { return edit.range.begin; });
    if(!edits.empty()) if(auto applied=m_services.host.applyEdits(edits,"Spelling corrections",std::nullopt); !applied) report(applied.error(),"Spelling corrections");
}
} // namespace frost::ui::lang
