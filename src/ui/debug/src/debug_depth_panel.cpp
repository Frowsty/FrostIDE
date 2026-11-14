// SPDX-License-Identifier: Apache-2.0
#include "frost/ui/debug_depth_panel.hpp"
#include "frost/ui/debug_controller.hpp"
#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QStringList>
#include <QVBoxLayout>

namespace frost::ui {
namespace {
QString text(const rpc::Json& json, std::string_view key) {
    const auto* value = json.find(key);
    return value && value->asString() ? QString::fromStdString(*value->asString()) : QString();
}
// Reader-thread results cross the Qt boundary before consulting the controller. Drop replies
// after a session, stop or selected-frame transition (DAP may reuse every numeric reference).
template<class T = rpc::Json, class Done>
auto deliver(DebugController& controller, QWidget* owner, Done done, bool selectionBound = true) {
    QPointer<QWidget> guard(owner);
    const auto generation = controller.sessionGeneration();
    const auto frame = controller.selectedFrame();
    const auto thread = controller.selectedThread();
    const auto epoch = controller.session() ? controller.session()->inspectionGeneration() : 0;
    return [guard, &controller, generation, epoch, frame, thread, selectionBound, done = std::move(done)](Result<T> result) mutable {
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, &controller, generation, epoch, frame, thread, selectionBound, done, result = std::move(result)]() mutable {
            if (!guard || controller.sessionGeneration() != generation || !controller.session()) return;
            if (selectionBound && (!controller.paused() || controller.session()->inspectionGeneration() != epoch)) return;
            const auto current = controller.selectedFrame();
            if (selectionBound && controller.selectedThread() != thread) return;
            if (selectionBound && (bool(current) != bool(frame) || (frame && current->id != frame->id))) return;
            done(std::move(result));
        }, Qt::QueuedConnection);
    };
}
}
DebugDepthPanel::DebugDepthPanel(DebugController& controller, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("debugDepthPanel"));
    auto* layout = new QVBoxLayout(this);
    auto* hex = new QCheckBox(QStringLiteral("Hexadecimal values"), this);
    hex->setChecked(controller.hexadecimalValues());
    connect(hex, &QCheckBox::toggled, &controller, &DebugController::setHexadecimalValues);
    auto* status = new QLabel(this);
    status->setWordWrap(true);
    auto* calls = new QComboBox(this);
    calls->setObjectName(QStringLiteral("debugCallTargets"));
    auto* refresh = new QPushButton(QStringLiteral("Load call targets"), this);
    auto* step = new QPushButton(QStringLiteral("Step into selected call"), this);
    auto* registers = new QTreeWidget(this);
    registers->setObjectName(QStringLiteral("debugRegisters"));
    registers->setHeaderLabels({QStringLiteral("Register"), QStringLiteral("Value"), QStringLiteral("Type")});
    auto* instruction = new QPushButton(QStringLiteral("Step one instruction"), this);
    connect(instruction, &QPushButton::clicked, this, [&controller, status] {
        if (!controller.session()) return;
        if (auto result = controller.session()->stepInstruction(); !result)
            status->setText(QString::fromStdString(result.error().message()));
    });
    auto* freeze = new QPushButton(QStringLiteral("Freeze selected thread"), this);
    auto* thaw = new QPushButton(QStringLiteral("Thaw selected thread"), this);
    auto setFrozen = [this, &controller, status](bool frozen) {
        if (!controller.session() || !controller.selectedThread()) return;
        controller.session()->setThreadFrozen(*controller.selectedThread(), frozen, deliver(controller, this,
            [status, frozen](Result<rpc::Json> body) {
                if (!body) { status->setText(QString::fromStdString(body.error().message())); return; }
                if (const auto state = body->boolAt("frozen"))
                    status->setText(*state ? QStringLiteral("Adapter reports the thread is frozen.") : QStringLiteral("Adapter reports the thread is thawed."));
                else status->setText(frozen ? QStringLiteral("Freeze command completed; current freeze state is unavailable.")
                                           : QStringLiteral("Thaw command completed; current freeze state is unavailable."));
            }, false));
    };
    connect(freeze, &QPushButton::clicked, this, [setFrozen] { setFrozen(true); });
    connect(thaw, &QPushButton::clicked, this, [setFrozen] { setFrozen(false); });
    auto* loadRegisters = new QPushButton(QStringLiteral("Load registers"), this);
    auto* form = new QFormLayout;
    auto* reference = m_reference = new QLineEdit(this);
    reference->hide();
    auto* name = m_name = new QLineEdit(this);
    name->setReadOnly(true);
    name->setPlaceholderText(QStringLiteral("Use Watch Hardware Access on a variable"));
    auto* access = new QComboBox(this);
    auto* info = new QPushButton(QStringLiteral("Inspect hardware watchpoint support"), this);
    auto* install = new QPushButton(QStringLiteral("Replace hardware watchpoints with this variable"), this);
    auto* clear = new QPushButton(QStringLiteral("Clear hardware watchpoints"), this);
    auto dataId = std::make_shared<std::string>();
    auto selection = std::make_shared<std::uint64_t>(0);
    form->addRow(QStringLiteral("Variable name"), name);
    form->addRow(QStringLiteral("Access"), access);
    layout->addWidget(status);
    layout->addWidget(hex);
    layout->addWidget(refresh); layout->addWidget(calls); layout->addWidget(step);
    layout->addWidget(instruction);
    layout->addWidget(freeze); layout->addWidget(thaw);
    layout->addWidget(loadRegisters); layout->addWidget(registers, 1);
    layout->addLayout(form); layout->addWidget(info); layout->addWidget(install); layout->addWidget(clear);
    auto update = [&controller, status, refresh, calls, step, loadRegisters, registers, info, install, clear, access, dataId, selection, reference, name, hex, instruction, freeze, thaw] {
        ++*selection; reference->clear(); name->clear();
        calls->clear(); registers->clear(); access->clear(); dataId->clear();
        const auto* session = controller.session();
        const auto caps = session ? session->capabilities() : debug::dap::Capabilities{};
        hex->setEnabled(caps.supportsValueFormattingOptions);
        hex->setToolTip(QStringLiteral("Requires debugger value formatting support"));
        const bool stopped = session && controller.paused();
        const bool mutableStop = stopped && !controller.readOnlySession();
        freeze->setEnabled(mutableStop && caps.frostSupportsThreadFreeze && controller.selectedThread().has_value());
        thaw->setEnabled(freeze->isEnabled());
        freeze->setToolTip(QStringLiteral("Requires debugger thread freeze support"));
        thaw->setToolTip(freeze->toolTip());
        instruction->setEnabled(mutableStop && caps.supportsSteppingGranularity);
        refresh->setEnabled(mutableStop && caps.supportsStepInTargetsRequest);
        step->setEnabled(false);
        loadRegisters->setEnabled(stopped && controller.selectedFrame().has_value());
        info->setEnabled(mutableStop && caps.supportsDataBreakpoints);
        install->setEnabled(false); clear->setEnabled(mutableStop && caps.supportsDataBreakpoints);
        status->setText(QStringLiteral("Unavailable actions are disabled for this debugger. "
            "Select a variable and choose Watch Hardware Access to configure a hardware watchpoint. "
            "Freeze and thaw require debugger support; thread state is shown only when the debugger reports it."));
    };
    connect(&controller, &DebugController::stateChanged, this, update);
    connect(&controller, &DebugController::frameSelected, this, update);
    connect(refresh, &QPushButton::clicked, this, [this, &controller, calls, step, status] {
        if (!controller.session()) return;
        controller.session()->stepInTargets(deliver(controller, this, [calls, step, status](Result<rpc::Json> body) {
            calls->clear(); step->setEnabled(false);
            if (!body) { status->setText(QString::fromStdString(body.error().message())); return; }
            if (const auto* targets = body->find("targets")) for (const auto& target : targets->items()) {
                if (const auto id = target.integerAt("id")) calls->addItem(text(target, "label"), QVariant::fromValue<qlonglong>(*id));
            }
            step->setEnabled(calls->count() > 0);
            if (!calls->count()) status->setText(QStringLiteral("The adapter returned no call targets for this frame."));
        }));
    });
    connect(step, &QPushButton::clicked, this, [this, &controller, calls, status] {
        if (!controller.session() || calls->currentIndex() < 0) return;
        if (auto result = controller.session()->stepIntoTarget(calls->currentData().toLongLong()); !result)
            status->setText(QString::fromStdString(result.error().message()));
    });
    connect(loadRegisters, &QPushButton::clicked, this, [this, &controller, registers, status] {
        if (!controller.selectedFrame()) return;
        registers->clear();
        controller.scopes(controller.selectedFrame()->id, deliver<debug::ScopeList>(controller, this, [this, &controller, registers, status](Result<debug::ScopeList> scopes) {
            if (!scopes) { status->setText(QString::fromStdString(scopes.error().message())); return; }
            bool found = false;
            for (const auto& scope : *scopes) if (scope.presentationHint == "registers") {
                found = true;
                controller.variables(scope.variablesReference, 0, 256, deliver<debug::VariableList>(controller, this, [registers, status](Result<debug::VariableList> values) {
                    if (!values) { status->setText(QString::fromStdString(values.error().message())); return; }
                    for (const auto& value : *values) new QTreeWidgetItem(registers, {QString::fromStdString(value.name),
                        QString::fromStdString(value.value), QString::fromStdString(value.type)});
                }));
            }
            if (!found) status->setText(QStringLiteral("The adapter exposes no registers scope for this frame."));
        }));
    });
    auto invalidate = [dataId, install, access, selection] { ++*selection; dataId->clear(); install->setEnabled(false); access->clear(); };
    connect(name, &QLineEdit::textChanged, this, invalidate);
    connect(reference, &QLineEdit::textChanged, this, invalidate);
    connect(info, &QPushButton::clicked, this, [this, &controller, reference, name, access, dataId, install, status, selection] {
        if (!controller.session()) return;
        bool ok = false; const auto ref = reference->text().toLongLong(&ok);
        if (!ok || ref <= 0) { status->setText(QStringLiteral("Choose a positive parent variablesReference.")); return; }
        const auto serial = ++*selection;
        controller.session()->dataBreakpointInfo(ref, name->text().toStdString(), deliver(controller, this,
            [access, dataId, install, status, selection, serial](Result<rpc::Json> body) {
                if (*selection != serial) return;
                dataId->clear(); access->clear(); install->setEnabled(false);
                if (!body) { status->setText(QString::fromStdString(body.error().message())); return; }
                *dataId = text(*body, "dataId").toStdString();
                status->setText(text(*body, "description"));
                if (const auto* types = body->find("accessTypes")) for (const auto& type : types->items())
                    if (type.asString()) access->addItem(QString::fromStdString(*type.asString()));
                // DAP default is write when accessTypes is omitted.
                if (!access->count()) access->addItem(QStringLiteral("write"));
                install->setEnabled(!dataId->empty());
            }));
    });
    auto replace = [this, &controller, status](rpc::Json list) {
        if (!controller.session()) return;
        controller.session()->setDataBreakpoints(std::move(list), deliver(controller, this, [status](Result<rpc::Json> body) {
            if (!body) { status->setText(QString::fromStdString(body.error().message())); return; }
            QStringList messages;
            if (const auto* results = body->find("breakpoints")) for (const auto& result : results->items()) {
                const auto parsed = debug::dap::parseBreakpoint(result);
                messages << (parsed.verified ? QStringLiteral("Hardware watchpoint verified") : QStringLiteral("Hardware watchpoint unverified"))
                    + QStringLiteral(": ") + QString::fromStdString(parsed.message);
            }
            status->setText(messages.isEmpty() ? QStringLiteral("Adapter acknowledged an empty hardware watchpoint set.") : messages.join('\n'));
        }, false));
    };
    connect(install, &QPushButton::clicked, this, [replace, dataId, access] {
        if (dataId->empty()) return;
        auto list = rpc::Json::array();
        list.push(rpc::Json::object().set("dataId", *dataId).set("accessType", access->currentText().toStdString()));
        replace(std::move(list));
    });
    connect(clear, &QPushButton::clicked, this, [replace] { replace(rpc::Json::array()); });
    update();
}
void DebugDepthPanel::chooseVariable(std::int64_t reference, const QString& name) {
    m_reference->setText(QString::number(reference));
    m_name->setText(name);
}
}
