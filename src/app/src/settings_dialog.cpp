// SPDX-License-Identifier: Apache-2.0
// Registry-backed application settings dialog and optimistic application-file edits.
#include "settings_dialog.hpp"

#include "frost/core/assert.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/tr.hpp"

#include "settings_keymap_page.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace frost::app {
namespace {
QString text(std::string_view source) { return ui::qtr("frost::app::SettingsDialog", source); }
QString str(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
struct Entry {
    std::string_view key;
    int page;
};
constexpr std::array kEntries{
    Entry{settings::key::uiTheme.name, 0},
    Entry{settings::key::uiCompactMode.name, 0},
    Entry{settings::key::uiZoom.name, 0},
    Entry{settings::key::editorFontFamily.name, 1},
    Entry{settings::key::editorFontSize.name, 1},
    Entry{settings::key::editorLineSpacing.name, 1},
    Entry{settings::key::editorFontLigatures.name, 1},
    Entry{settings::key::editorSoftWrap.name, 1},
    Entry{settings::key::editorLineNumbers.name, 1},
    Entry{settings::key::editorTabSize.name, 2},
    Entry{settings::key::editorIndentSize.name, 2},
    Entry{settings::key::editorInsertSpaces.name, 2},
    Entry{settings::key::editorEditorConfig.name, 2},
    Entry{settings::key::filesEncoding.name, 3},
    Entry{settings::key::filesUtf8Bom.name, 3},
    Entry{settings::key::filesLineSeparator.name, 3},
    Entry{settings::key::editorInsertFinalNewline.name, 3},
    Entry{settings::key::toolsCmakePath.name, 4},
    Entry{settings::key::toolsNinjaPath.name, 4},
    Entry{settings::key::toolsGitPath.name, 4},
    Entry{settings::key::toolsClangdPath.name, 4},
    Entry{settings::key::toolsMsbuildPath.name, 4},
    Entry{settings::key::debugSourceMapFrom.name, 4},
    Entry{settings::key::debugSourceMapTo.name, 4},
    Entry{settings::key::networkOffline.name, 5},
    Entry{settings::key::updateChannel.name, 5},
    Entry{settings::key::keymapId.name, 6},
};
constexpr std::array<std::string_view, 7> kPages{"Appearance", "Editor",  "Code Style", "Files",
                                                 "Tools",      "Updates", "Keymap"};
constexpr std::size_t kKeymapPage = 6;

settings::Value controlValue(const QWidget* control, const settings::Schema& schema) {
    using settings::ValueType;
    switch (schema.type) {
    case ValueType::Bool: return qobject_cast<const QCheckBox*>(control)->isChecked();
    case ValueType::Integer: return std::int64_t{qobject_cast<const QSpinBox*>(control)->value()};
    case ValueType::Number: return qobject_cast<const QDoubleSpinBox*>(control)->value();
    case ValueType::Enum: return utf8(qobject_cast<const QComboBox*>(control)->currentData().toString());
    case ValueType::String:
        if (const auto* combo = qobject_cast<const QComboBox*>(control)) {
            if (!combo->isEditable() ||
                (combo->currentIndex() >= 0 && combo->currentText() == combo->itemText(combo->currentIndex())))
                return utf8(combo->currentData().toString());
            return utf8(combo->currentText());
        }
        return utf8(qobject_cast<const QLineEdit*>(control)->text());
    case ValueType::StringList:
    case ValueType::RecordList: break;
    }
    FROST_PRE(false);
    return false;
}
void setControl(QWidget* control, const settings::Schema& schema, const settings::Value& value) {
    using settings::ValueType;
    switch (schema.type) {
    case ValueType::Bool: qobject_cast<QCheckBox*>(control)->setChecked(std::get<bool>(value)); break;
    case ValueType::Integer:
        qobject_cast<QSpinBox*>(control)->setValue(static_cast<int>(std::get<std::int64_t>(value)));
        break;
    case ValueType::Number: qobject_cast<QDoubleSpinBox*>(control)->setValue(std::get<double>(value)); break;
    case ValueType::Enum:
    case ValueType::String:
        if (auto* combo = qobject_cast<QComboBox*>(control)) {
            const QString selected = str(std::get<std::string>(value));
            int index = combo->findData(selected);
            if (index < 0) {
                combo->addItem(selected, selected);
                index = combo->count() - 1;
            }
            combo->setCurrentIndex(index);
        } else
            qobject_cast<QLineEdit*>(control)->setText(str(std::get<std::string>(value)));
        break;
    case ValueType::StringList:
    case ValueType::RecordList: FROST_PRE(false); break;
    }
}
} // namespace

struct SettingsDialog::Row {
    const settings::KeySpec* spec = nullptr;
    int page = 0;
    QWidget* container = nullptr;
    QWidget* control = nullptr;
    QLabel* title = nullptr;
    QLabel* source = nullptr;
    std::optional<std::string> expectedRaw;
    settings::Value initial;
    bool reset = false;
    bool dirty = false;
};

SettingsDialog::SettingsDialog(settings::SettingsStore& store, const input::ActionRegistry& registry,
                               input::KeymapService& keymap, QWidget* parent)
    : QDialog(parent), m_store(store), m_registry(registry), m_keymap(keymap) {
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(text("Settings"));
    resize(920, 680);
    build();
    reload();
}
SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::build() {
    auto* outer = new QVBoxLayout(this);
    auto* title = new QLabel(text("Settings"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);
    outer->addWidget(title);
    auto* scope = new QLabel(
        text("Application settings apply to all projects. Project or file settings can override them."), this);
    scope->setWordWrap(true);
    outer->addWidget(scope);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("settingsSearch"));
    m_search->setPlaceholderText(text("Search settings"));
    outer->addWidget(m_search);
    auto* content = new QHBoxLayout();
    m_categories = new QListWidget(this);
    m_categories->setObjectName(QStringLiteral("settingsCategories"));
    m_categories->setMaximumWidth(200);
    m_categories->setMinimumWidth(140);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("settingsPages"));
    content->addWidget(m_categories);
    content->addWidget(m_pages, 1);
    outer->addLayout(content, 1);
    std::array<QVBoxLayout*, kPages.size()> layouts{};
    for (std::size_t i = 0; i < kPages.size(); ++i) {
        m_categories->addItem(text(kPages[i]));
        auto* scroll = new QScrollArea(m_pages);
        scroll->setWidgetResizable(true);
        auto* page = new QWidget(scroll);
        layouts[i] = new QVBoxLayout(page);
        layouts[i]->setAlignment(Qt::AlignTop);
        auto* heading = new QLabel(text(kPages[i]), page);
        QFont font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        layouts[i]->addWidget(heading);
        scroll->setWidget(page);
        m_pages->addWidget(scroll);
    }
    m_keymapPage = new SettingsKeymapPage(
        m_registry, m_keymap,
        [this] {
            m_apply->setEnabled(m_keymapPage->dirty() ||
                                std::ranges::any_of(m_rows, [](const auto& row) { return row->dirty; }));
        },
        this);
    auto* noResults = new QLabel(text("No settings found"), m_pages);
    noResults->setObjectName(QStringLiteral("settingsNoResults"));
    noResults->setAlignment(Qt::AlignCenter);
    m_pages->addWidget(noResults);
    for (const auto& entry : kEntries) {
        const auto* spec = m_store.registry().find(entry.key);
        FROST_PRE(spec != nullptr);
        auto row = std::make_unique<Row>();
        row->spec = spec;
        row->page = entry.page;
        row->container = new QWidget(this);
        auto* rowLayout = new QVBoxLayout(row->container);
        rowLayout->setContentsMargins(0, 5, 0, 5);
        rowLayout->setSpacing(3);
        auto* line = new QHBoxLayout();
        auto* label = new QLabel(text(spec->title), row->container);
        row->title = label;
        line->addWidget(label, 1);
        const auto type = spec->schema.type;
        if (type == settings::ValueType::Bool)
            row->control = new QCheckBox(row->container);
        else if (type == settings::ValueType::Integer) {
            auto* spin = new QSpinBox(row->container);
            spin->setRange(static_cast<int>(spec->schema.minInteger), static_cast<int>(spec->schema.maxInteger));
            row->control = spin;
        } else if (type == settings::ValueType::Number) {
            auto* spin = new QDoubleSpinBox(row->container);
            spin->setRange(spec->schema.minNumber, spec->schema.maxNumber);
            spin->setDecimals(2);
            spin->setSingleStep(0.05);
            row->control = spin;
        } else if (type == settings::ValueType::Enum ||
                   (type == settings::ValueType::String && !spec->schema.options.empty())) {
            auto* combo = new QComboBox(row->container);
            for (const auto& option : spec->schema.options)
                combo->addItem(text(option.label), str(option.value));
            if (type == settings::ValueType::String && spec->schema.hint != settings::StringHint::ThemeId)
                combo->setEditable(true);
            row->control = combo;
        } else {
            FROST_PRE(type == settings::ValueType::String);
            auto* edit = new QLineEdit(row->container);
            row->control = edit;
        }
        row->control->setObjectName(QStringLiteral("setting.") + str(spec->key));
        row->control->setAccessibleName(text(spec->title));
        row->control->setToolTip(text(spec->description));
        row->control->setMinimumWidth(200);
        label->setBuddy(row->control);
        line->addWidget(row->control);
        auto* reset = new QPushButton(text("Reset"), row->container);
        reset->setObjectName(QStringLiteral("reset.") + str(spec->key));
        reset->setAccessibleName(text("Reset %1").arg(text(spec->title)));
        reset->setToolTip(text("Remove this application override"));
        line->addWidget(reset);
        rowLayout->addLayout(line);
        auto* description = new QLabel(text(spec->description), row->container);
        description->setWordWrap(true);
        rowLayout->addWidget(description);
        row->source = new QLabel(row->container);
        row->source->setWordWrap(true);
        rowLayout->addWidget(row->source);
        const std::size_t index = m_rows.size();
        if (auto* check = qobject_cast<QCheckBox*>(row->control))
            connect(check, &QCheckBox::toggled, this, [this, index] { changed(index); });
        else if (auto* spin = qobject_cast<QSpinBox*>(row->control))
            connect(spin, &QSpinBox::valueChanged, this, [this, index] { changed(index); });
        else if (auto* doubleSpin = qobject_cast<QDoubleSpinBox*>(row->control))
            connect(doubleSpin, &QDoubleSpinBox::valueChanged, this, [this, index] { changed(index); });
        else if (auto* combo = qobject_cast<QComboBox*>(row->control)) {
            connect(combo, &QComboBox::currentIndexChanged, this, [this, index] { changed(index); });
            if (combo->isEditable())
                connect(combo, &QComboBox::editTextChanged, this, [this, index] { changed(index); });
        } else
            connect(qobject_cast<QLineEdit*>(row->control), &QLineEdit::textChanged, this,
                    [this, index] { changed(index); });
        connect(reset, &QPushButton::clicked, this, [this, index] { resetRow(index); });
        layouts[static_cast<std::size_t>(entry.page)]->addWidget(row->container);
        m_rows.push_back(std::move(row));
    }
    layouts[kKeymapPage]->addWidget(m_keymapPage);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("settingsStatus"));
    m_status->setWordWrap(true);
    outer->addWidget(m_status);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    auto* reloadButton = buttons->addButton(text("Reload"), QDialogButtonBox::ActionRole);
    reloadButton->setToolTip(text("Discard pending changes and reload settings from disk"));
    m_apply = buttons->button(QDialogButtonBox::Apply);
    outer->addWidget(buttons);
    connect(m_categories, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_search, &QLineEdit::textChanged, this, [this] { refreshSearch(); });
    connect(reloadButton, &QPushButton::clicked, this, [this] { reload(); });
    connect(m_apply, &QPushButton::clicked, this, [this] { (void)apply(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_categories->setCurrentRow(0);
}

void SettingsDialog::reload() {
    auto document = m_store.readScopeDocument(settings::Scope::Application);
    if (!document) {
        m_loaded = false;
        m_pages->setEnabled(false);
        m_apply->setEnabled(false);
        m_status->setText(text("Cannot read settings: %1").arg(str(document.error().describe())));
        return;
    }
    const auto snapshot = m_store.snapshot();
    m_loading = true;
    for (auto& row : m_rows) {
        const auto* member = document->find(row->spec->key);
        row->expectedRaw = member ? std::optional<std::string>(document->rawValue(*member)) : std::nullopt;
        auto value = document->value(row->spec->key, row->spec->schema);
        row->initial = value ? std::move(*value) : row->spec->defaultValue;
        row->reset = false;
        row->dirty = false;
        row->title->setText(text(row->spec->title));
        setControl(row->control, row->spec->schema, row->initial);
        const auto source = snapshot->source(row->spec->key);
        QString note = member && !value
                           ? text("The stored application value is invalid; change or reset it to repair it.")
                           : QString();
        if (source == settings::Scope::ProjectShared || source == settings::Scope::ProjectPersonal) {
            if (!note.isEmpty())
                note += QLatin1Char('\n');
            note += text("The open project overrides this application setting.");
        }
        row->source->setText(note);
    }
    m_keymapPage->load(*document, m_store.registry());
    m_loading = false;
    m_loaded = true;
    m_pages->setEnabled(true);
    m_apply->setEnabled(false);
    m_status->clear();
}

void SettingsDialog::changed(std::size_t index) {
    if (m_loading)
        return;
    auto& row = *m_rows[index];
    row.reset = false;
    const settings::Value current = controlValue(row.control, row.spec->schema);
    row.dirty = current != row.initial;
    row.title->setText(text(row.spec->title) + (row.dirty ? QStringLiteral(" *") : QString()));
    if (row.spec->key == settings::key::keymapId.name) {
        m_keymapPage->setKeymapId(std::get<std::string>(current));
    }
    m_apply->setEnabled(m_keymapPage->dirty() ||
                        std::ranges::any_of(m_rows, [](const auto& item) { return item->dirty; }));
}
void SettingsDialog::resetRow(std::size_t index) {
    auto& row = *m_rows[index];
    m_loading = true;
    setControl(row.control, row.spec->schema, row.spec->defaultValue);
    m_loading = false;
    row.reset = true;
    row.dirty = row.expectedRaw.has_value();
    row.title->setText(text(row.spec->title) + (row.dirty ? QStringLiteral(" *") : QString()));
    if (row.spec->key == settings::key::keymapId.name) {
        m_keymapPage->setKeymapId(std::get<std::string>(row.spec->defaultValue));
    }
    m_apply->setEnabled(m_keymapPage->dirty() ||
                        std::ranges::any_of(m_rows, [](const auto& item) { return item->dirty; }));
}
void SettingsDialog::refreshSearch() {
    const QString query = m_search->text().trimmed();
    std::array<bool, kPages.size()> matches{};
    for (auto& row : m_rows) {
        const bool match = query.isEmpty() || text(row->spec->title).contains(query, Qt::CaseInsensitive) ||
                           text(row->spec->description).contains(query, Qt::CaseInsensitive) ||
                           text(kPages[static_cast<std::size_t>(row->page)]).contains(query, Qt::CaseInsensitive);
        row->container->setVisible(match);
        matches[static_cast<std::size_t>(row->page)] |= match;
    }
    matches[kKeymapPage] |= m_keymapPage->matches(query);
    m_keymapPage->filter(query);
    int first = -1;
    for (std::size_t i = 0; i < matches.size(); ++i) {
        m_categories->item(static_cast<int>(i))->setHidden(!matches[i]);
        if (first < 0 && matches[i])
            first = static_cast<int>(i);
    }
    if (first < 0) {
        m_pages->setCurrentIndex(static_cast<int>(kPages.size()));
    } else if (m_categories->currentRow() < 0 || m_categories->currentItem()->isHidden()) {
        m_categories->setCurrentRow(first);
    } else {
        m_pages->setCurrentIndex(m_categories->currentRow());
    }
}
bool SettingsDialog::apply() {
    if (!m_loaded)
        return false;
    std::vector<settings::ConditionalSettingEdit> edits;
    for (const auto& row : m_rows) {
        if (!row->dirty)
            continue;
        std::optional<settings::Value> value;
        if (!row->reset)
            value = controlValue(row->control, row->spec->schema);
        if (value) {
            if (auto valid = settings::validateValue(row->spec->schema, *value); !valid) {
                m_status->setText(text("%1: %2").arg(text(row->spec->title), str(valid.error().message())));
                return false;
            }
        }
        edits.push_back({row->spec->key, row->expectedRaw, std::move(value)});
    }
    if (auto keymapEdit = m_keymapPage->edit())
        edits.push_back(std::move(*keymapEdit));
    if (auto written = m_store.applyConditional(settings::Scope::Application, edits); !written) {
        m_status->setText(text("Cannot apply settings: %1. Reload to discard your changes and read the current file.")
                              .arg(str(written.error().describe())));
        return false;
    }
    reload();
    return m_loaded;
}
} // namespace frost::app
