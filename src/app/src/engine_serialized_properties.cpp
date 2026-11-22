// SPDX-License-Identifier: Apache-2.0
// FEC-6: an actual live-engine reflected-properties editor, not an IDE-side scene serializer.
#include "frost/engine/serialized_properties.hpp"
#include "frost/ui/dialog_chrome.hpp"

#include "engine_integration_internal.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
namespace frost::app {
namespace {
QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
struct PropertyDialog {
    QPointer<EngineIntegration> owner;
    QPointer<QDialog> dialog;
    QPointer<QLabel> status;
    QPointer<QPlainTextEdit> fields;
    QPointer<QPushButton> load, apply;
    std::shared_ptr<const engine::AssetIndex> index;
    std::optional<engine::SerializedComponent> snapshot;
    std::function<bool()> current;
    std::function<void(std::uint64_t)> cancel;
    std::uint64_t request{}, generation{};
    bool expectedSupported{}, closed{};
};
void requestProperty(const std::shared_ptr<PropertyDialog>& state, const std::string& method, lang::Json params,
                     std::function<void(Result<lang::Json>)> done) {
    if (state->closed || !state->dialog || !state->owner || !state->current())
        return;
    const auto generation = state->generation;
    state->load->setEnabled(false);
    state->apply->setEnabled(false);
    auto started = state->owner->contentCommand(
        method, std::move(params), [state, generation, done = std::move(done)](Result<lang::Json> result) mutable {
            if (state->closed || !state->dialog || generation != state->generation || !state->current())
                return;
            state->request = 0;
            state->load->setEnabled(true);
            done(std::move(result));
        });
    if (started)
        state->request = *started;
    else {
        state->status->setText(qs(started.error().message()));
        state->load->setEnabled(true);
    }
}
void publishProperties(const std::shared_ptr<PropertyDialog>& state, engine::SerializedComponent component) {
    const auto text = component.fields.dump();
    state->snapshot = std::move(component);
    state->fields->setPlainText(qs(text));
    const bool editable = state->expectedSupported && text.size() <= 512 * 1024;
    state->fields->setReadOnly(!editable);
    state->apply->setEnabled(editable);
    state->status->setText(
        editable
            ? QStringLiteral(
                  "Actual engine fields. Only changed values are submitted; engine validation and engine Undo apply.")
            : QStringLiteral("Read-only: this engine lacks expected-before component.set, or the component exceeds the "
                             "editor limit."));
}
} // namespace
Result<void> EngineIntegration::showSerializedProperties() {
    auto& impl = *m_impl;
    if (!impl.assetIndex || !impl.identity || !impl.session || !impl.session->connected())
        return fail(ErrorCode::NotFound, "Pair an editor and refresh its actual asset index first");
    auto schema = engine::parseSchemaDocument(impl.identity->schema);
    if (!schema)
        return std::unexpected(schema.error());
    if (schema->components.empty())
        return fail(ErrorCode::Unsupported, "The engine exports no reflected component metadata");
    auto state = std::make_shared<PropertyDialog>();
    state->owner = this;
    state->index = impl.assetIndex;
    const auto epoch = impl.epoch, revision = impl.trustRevision, sessionEpoch = impl.sessionEpoch;
    const auto root = impl.root;
    const auto identity = *impl.identity;
    auto* exact = impl.session.get();
    const QPointer<EngineIntegration> self(this);
    const auto index = state->index;
    state->current = [self, epoch, revision, sessionEpoch, root, identity, exact, index] {
        return self && self->m_impl->admitted(epoch, root, revision) && self->m_impl->sessionEpoch == sessionEpoch &&
               self->m_impl->session.get() == exact && self->m_impl->currentIdentity(identity) &&
               self->m_impl->assetIndex == index;
    };
    state->cancel = [self, exact, sessionEpoch](std::uint64_t id) {
        if (self && self->m_impl->session.get() == exact && self->m_impl->sessionEpoch == sessionEpoch)
            exact->cancel(id);
    };
    auto* dialog = new QDialog(impl.workbench.m_window);
    state->dialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("engineSerializedProperties"));
    dialog->setWindowTitle(QStringLiteral("Serialized component properties — live FrostEditor"));
    dialog->resize(720, 520);
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    auto* scene = new QComboBox(dialog);
    scene->setObjectName(QStringLiteral("enginePropertyScene"));
    for (const auto& a : index->content().assets)
        if (a.type == "Scene")
            scene->addItem(qs(a.path));
    auto* entity = new QLineEdit(dialog);
    entity->setObjectName(QStringLiteral("enginePropertyEntity"));
    entity->setPlaceholderText(QStringLiteral("Entity name or exact unsigned ID"));
    auto* component = new QComboBox(dialog);
    component->setObjectName(QStringLiteral("enginePropertyComponent"));
    for (const auto& c : schema->components)
        component->addItem(qs(c.name));
    form->addRow(QStringLiteral("Scene"), scene);
    form->addRow(QStringLiteral("Entity"), entity);
    form->addRow(QStringLiteral("Component"), component);
    layout->addLayout(form);
    auto* load = new QPushButton(QStringLiteral("Read from engine"), dialog);
    load->setObjectName(QStringLiteral("enginePropertyRead"));
    state->load = load;
    layout->addWidget(load);
    auto* fields = new QPlainTextEdit(dialog);
    fields->setObjectName(QStringLiteral("enginePropertyFields"));
    fields->setReadOnly(true);
    state->fields = fields;
    layout->addWidget(fields, 1);
    auto* status = new QLabel(
        QStringLiteral("Choose an engine-listed scene and actual entity; read values before editing."), dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    state->status = status;
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* apply = buttons->addButton(QStringLiteral("Apply to live engine"), QDialogButtonBox::ApplyRole);
    apply->setObjectName(QStringLiteral("enginePropertyApply"));
    apply->setEnabled(false);
    state->apply = apply;
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto changed = [state] {
        ++state->generation;
        if (state->request)
            state->cancel(state->request);
        state->request = 0;
        state->snapshot.reset();
        state->fields->clear();
        state->fields->setReadOnly(true);
        state->apply->setEnabled(false);
        state->load->setEnabled(state->current());
    };
    connect(scene, &QComboBox::currentIndexChanged, dialog, changed);
    connect(component, &QComboBox::currentIndexChanged, dialog, changed);
    connect(entity, &QLineEdit::textEdited, dialog, changed);
    connect(load, &QPushButton::clicked, dialog, [state, scene, entity, component] {
        if (!state->current())
            return;
        state->snapshot.reset();
        ++state->generation;
        state->fields->setReadOnly(true);
        const auto scenePath = scene->currentText().toStdString();
        const auto componentName = component->currentText().toStdString();
        auto params = lang::Json::object()
                          .set("scene", scenePath)
                          .set("entity", entity->text().toStdString())
                          .set("component", componentName);
        requestProperty(state, "commands.describe", lang::Json::object().set("command", "component.set"),
                        [state, params, scenePath, componentName](Result<lang::Json> descriptor) {
                            if (!descriptor) {
                                state->status->setText(qs(descriptor.error().message()));
                                return;
                            }
                            state->expectedSupported = engine::supportsExpectedComponentSet(*descriptor);
                            requestProperty(state, "component.get", params,
                                            [state, scenePath, componentName](Result<lang::Json> actual) {
                                                if (!actual) {
                                                    state->status->setText(qs(actual.error().message()));
                                                    return;
                                                }
                                                auto parsed = engine::parseSerializedComponent(*state->index, scenePath,
                                                                                               componentName, *actual);
                                                if (!parsed) {
                                                    state->status->setText(qs(parsed.error().message()));
                                                    return;
                                                }
                                                publishProperties(state, std::move(*parsed));
                                            });
                        });
    });
    connect(apply, &QPushButton::clicked, dialog, [state] {
        if (!state->snapshot || !state->expectedSupported || !state->current())
            return;
        auto text = state->fields->toPlainText().toUtf8();
        if (text.size() > 512 * 1024) {
            state->status->setText(QStringLiteral("Serialized property edit exceeds 512 KiB"));
            return;
        }
        auto after = lang::parseJson(std::string_view(text.constData(), static_cast<std::size_t>(text.size())));
        if (!after) {
            state->status->setText(qs(after.error().message()));
            return;
        }
        auto params = engine::serializedComponentEdit(*state->snapshot, *after);
        if (!params) {
            state->status->setText(qs(params.error().message()));
            return;
        }
        const auto scenePath = state->snapshot->scene, componentName = state->snapshot->component;
        const auto entityId = state->snapshot->entity;
        state->fields->setReadOnly(true);
        requestProperty(
            state, "component.set", std::move(*params),
            [state, scenePath, componentName, entityId](Result<lang::Json> actual) {
                if (!actual) {
                    state->status->setText(qs(actual.error().message()));
                    state->fields->setReadOnly(false);
                    return;
                }
                auto parsed =
                    engine::parseSerializedComponent(*state->index, scenePath, componentName, *actual, entityId);
                if (!parsed) {
                    state->status->setText(qs(parsed.error().message()));
                    return;
                }
                auto expected = std::make_shared<engine::SerializedComponent>(std::move(*parsed));
                auto read = lang::Json::object()
                                .set("scene", expected->scene)
                                .set("entity", engine::toJson(expected->entity))
                                .set("component", expected->component);
                requestProperty(
                    state, "component.get", std::move(read), [state, expected](Result<lang::Json> actualRead) {
                        if (!actualRead) {
                            state->status->setText(qs(actualRead.error().message()));
                            return;
                        }
                        auto current = engine::parseSerializedComponent(
                            *state->index, expected->scene, expected->component, *actualRead, expected->entity);
                        if (!current) {
                            state->status->setText(qs(current.error().message()));
                            return;
                        }
                        const bool same = current->fields == expected->fields;
                        publishProperties(state, std::move(*current));
                        state->status->setText(
                            same ? QStringLiteral("Edit round-tripped through the live engine. Scene persistence and "
                                                  "Undo remain engine-owned.")
                                 : QStringLiteral("Engine fields changed again after the edit; current values are "
                                                  "shown. Reload before another change."));
                    });
            });
    });
    connect(dialog, &QDialog::finished, dialog, [state] {
        state->closed = true;
        if (state->request)
            state->cancel(state->request);
    });
    auto* watch = new QTimer(dialog);
    watch->setInterval(250);
    connect(watch, &QTimer::timeout, dialog, [state] {
        if (!state->current()) {
            if (state->request)
                state->cancel(state->request);
            state->request = 0;
            state->load->setEnabled(false);
            state->apply->setEnabled(false);
            state->fields->setReadOnly(true);
            state->status->setText(QStringLiteral("Engine/project/trust changed. Reopen this inspector."));
        }
    });
    watch->start();
    ui::installDialogChrome(dialog, false);
    dialog->open();
    return {};
}
} // namespace frost::app
