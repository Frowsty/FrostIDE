// SPDX-License-Identifier: Apache-2.0
// Registry-backed keymap settings with staged edits and explicit conflicts.
#include "settings_keymap_page.hpp"

#include "frost/settings/keys.hpp"
#include "frost/ui/tr.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <ranges>

namespace frost::app {
namespace {
QString text(std::string_view s) { return ui::qtr("frost::app::SettingsKeymapPage", s); }
QString str(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }
template<class T>
const T* field(const settings::Record& record, std::string_view name) {
    for (const auto& f : record)
        if (f.name == name)
            return std::get_if<T>(&f.value);
    return nullptr;
}
QString actionName(const input::ActionInfo& info) { return str(info.name); }
} // namespace
SettingsKeymapPage::SettingsKeymapPage(const input::ActionRegistry& registry, input::KeymapService& keymap,
                                       std::function<void()> changed, QWidget* parent)
    : QWidget(parent), m_registry(registry), m_keymap(keymap), m_changed(std::move(changed)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* help = new QLabel(text("Select an action to edit its shortcuts. Separate alternatives with |."), this);
    help->setWordWrap(true);
    layout->addWidget(help);
    m_actions = new QTreeWidget(this);
    m_actions->setObjectName(QStringLiteral("keymapActions"));
    m_actions->setAccessibleName(text("Actions and shortcuts"));
    m_actions->setColumnCount(3);
    m_actions->setHeaderLabels({text("Action"), text("Shortcuts"), text("Source")});
    m_actions->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_actions->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_actions->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_actions->setRootIsDecorated(false);
    layout->addWidget(m_actions, 1);
    auto* entry = new QHBoxLayout();
    auto* label = new QLabel(text("Shortcut:"), this);
    m_shortcut = new QLineEdit(this);
    m_shortcut->setObjectName(QStringLiteral("keymapShortcut"));
    m_shortcut->setAccessibleName(text("Shortcut"));
    m_shortcut->setPlaceholderText(text("For example Ctrl+K, Ctrl+C"));
    label->setBuddy(m_shortcut);
    entry->addWidget(label);
    entry->addWidget(m_shortcut, 1);
    layout->addLayout(entry);
    auto* buttons = new QHBoxLayout();
    auto* set = new QPushButton(text("Set shortcuts"), this);
    set->setObjectName(QStringLiteral("keymapSet"));
    auto* remove = new QPushButton(text("Remove shortcuts"), this);
    remove->setObjectName(QStringLiteral("keymapRemove"));
    auto* reset = new QPushButton(text("Restore default"), this);
    reset->setObjectName(QStringLiteral("keymapReset"));
    buttons->addWidget(set);
    buttons->addWidget(remove);
    buttons->addWidget(reset);
    buttons->addStretch();
    layout->addLayout(buttons);
    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("keymapMessage"));
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    connect(m_actions, &QTreeWidget::itemSelectionChanged, this, [this] { selected(); });
    connect(set, &QPushButton::clicked, this, [this] { setShortcut(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeShortcuts(); });
    connect(reset, &QPushButton::clicked, this, [this] { resetShortcut(); });
}
void SettingsKeymapPage::load(const settings::SettingsDocument& document, const settings::Registry& registry) {
    const auto* spec = registry.find(settings::key::keymapOverrides.name);
    m_preferences = m_keymap.preferences();
    m_preferences.overrides.clear();
    m_records.clear();
    m_expected.reset();
    m_valid = false;
    const auto* member = document.find(settings::key::keymapOverrides.name);
    if (member) {
        m_expected = std::string(document.rawValue(*member));
        auto value = document.value(settings::key::keymapOverrides.name, spec->schema);
        if (!value) {
            m_message->setText(text("Custom shortcuts are invalid. Repair the settings file before editing them."));
            setEnabled(false);
            return;
        }
        m_records = std::get<settings::RecordList>(*value);
    }
    for (const auto& record : m_records) {
        const auto* action = field<std::string>(record, "action");
        const auto* shortcuts = field<settings::StringList>(record, "shortcuts");
        if (!action || !shortcuts) {
            setEnabled(false);
            return;
        }
        input::ActionOverride item{*action, {}};
        for (const auto& shortcut : *shortcuts) {
            auto parsed = input::parseSequence(shortcut, input::SyntaxMode::Strict);
            if (!parsed) {
                m_message->setText(text("Custom shortcuts are invalid. Repair the settings file before editing them."));
                setEnabled(false);
                return;
            }
            item.sequences.push_back(*parsed);
        }
        m_preferences.overrides.push_back(std::move(item));
    }
    if (!input::validate(m_preferences)) {
        m_message->setText(text("Custom shortcuts are invalid. Repair the settings file before editing them."));
        setEnabled(false);
        return;
    }
    m_original = m_records;
    m_valid = true;
    setEnabled(true);
    m_message->clear();
    rebuild();
}
void SettingsKeymapPage::setKeymapId(std::string_view keymapId) {
    if (m_preferences.keymapId == keymapId)
        return;
    m_preferences.keymapId = std::string(keymapId);
    if (m_valid)
        rebuild();
}
bool SettingsKeymapPage::dirty() const noexcept { return m_valid && m_records != m_original; }
std::optional<settings::ConditionalSettingEdit> SettingsKeymapPage::edit() const {
    if (!dirty())
        return std::nullopt;
    return settings::ConditionalSettingEdit{std::string(settings::key::keymapOverrides.name), m_expected,
                                            settings::Value(m_records)};
}
input::BindingIndex SettingsKeymapPage::index() const {
    const auto activeKeymap = input::builtinKeymap(m_preferences.keymapId);
    const input::Keymap& baseKeymap = activeKeymap ? *activeKeymap : m_keymap.keymap();
    auto defaults = baseKeymap.bindings(m_keymap.platform());
    std::vector<input::ActionOverride> overrides;
    for (const auto& module : m_keymap.moduleDefaults()) {
        if (std::ranges::none_of(defaults, [&](const auto& item) { return item.actionId == module.actionId; }))
            overrides.push_back({module.actionId, module.sequences});
    }
    overrides.insert(overrides.end(), m_preferences.overrides.begin(), m_preferences.overrides.end());
    return input::BindingIndex::build(baseKeymap, m_keymap.platform(), overrides, m_registry);
}
void SettingsKeymapPage::rebuild() {
    const auto selectedId =
        m_actions->currentItem() ? m_actions->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_actions->clear();
    const auto bindings = index();
    for (auto id : m_registry.all()) {
        const auto& info = m_registry.info(id);
        QStringList shortcuts;
        for (const auto& sequence : bindings.shortcutsFor(id))
            shortcuts << str(input::formatSequence(sequence));
        const bool overridden =
            std::ranges::any_of(m_preferences.overrides, [&](const auto& item) { return item.actionId == info.id; });
        auto* item = new QTreeWidgetItem(m_actions, {actionName(info), shortcuts.join(QStringLiteral(" | ")),
                                                     overridden ? text("Custom") : text("Default")});
        item->setData(0, Qt::UserRole, str(info.id));
        item->setToolTip(0, str(info.description));
        if (str(info.id) == selectedId)
            m_actions->setCurrentItem(item);
    }
    if (!m_actions->currentItem() && m_actions->topLevelItemCount())
        m_actions->setCurrentItem(m_actions->topLevelItem(0));
    filter(m_filter);
    selected();
}
void SettingsKeymapPage::selected() {
    auto* item = m_actions->currentItem();
    m_shortcut->setText(item ? item->text(1) : QString());
}
bool SettingsKeymapPage::matches(const QString& query) const {
    if (query.isEmpty() || text("Keymap").contains(query, Qt::CaseInsensitive))
        return true;
    for (auto id : m_registry.all()) {
        const auto& info = m_registry.info(id);
        if (actionName(info).contains(query, Qt::CaseInsensitive) || str(info.id).contains(query, Qt::CaseInsensitive))
            return true;
    }
    return false;
}
void SettingsKeymapPage::filter(const QString& query) {
    m_filter = query;
    const bool showAll = query.isEmpty() || text("Keymap").contains(query, Qt::CaseInsensitive);
    QTreeWidgetItem* first = nullptr;
    for (int i = 0; i < m_actions->topLevelItemCount(); ++i) {
        auto* item = m_actions->topLevelItem(i);
        item->setHidden(!showAll && !item->text(0).contains(query, Qt::CaseInsensitive) &&
                        !item->data(0, Qt::UserRole).toString().contains(query, Qt::CaseInsensitive));
        if (!item->isHidden() && !first)
            first = item;
    }
    if (!m_actions->currentItem() || m_actions->currentItem()->isHidden())
        m_actions->setCurrentItem(first);
}
void SettingsKeymapPage::setShortcut() {
    std::vector<input::Sequence> sequences;
    for (const auto& part : m_shortcut->text().split(QLatin1Char('|'))) {
        auto parsed = input::parseSequence(utf8(part.trimmed()), input::SyntaxMode::Lenient);
        if (!parsed ||
            !input::parseSequence(parsed ? input::formatSequence(*parsed) : std::string{}, input::SyntaxMode::Strict)) {
            m_message->setText(text("Enter a valid shortcut, such as Ctrl+K, Ctrl+C. Use Remove shortcuts to unbind."));
            return;
        }
        sequences.push_back(*parsed);
    }
    change(std::move(sequences));
}
void SettingsKeymapPage::removeShortcuts() { change(std::vector<input::Sequence>{}); }
void SettingsKeymapPage::resetShortcut() { change(std::nullopt); }
void SettingsKeymapPage::change(std::optional<std::vector<input::Sequence>> sequences) {
    auto* item = m_actions->currentItem();
    if (!m_valid || !item)
        return;
    const auto action = utf8(item->data(0, Qt::UserRole).toString());
    auto preferences = m_preferences;
    auto preference = std::ranges::find(preferences.overrides, action, &input::ActionOverride::actionId);
    if (preference == preferences.overrides.end()) {
        if (!sequences)
            return;
        preferences.overrides.push_back({action, *sequences});
    } else if (sequences) {
        if (preference->sequences == *sequences)
            return;
        preference->sequences = *sequences;
    } else {
        preferences.overrides.erase(preference);
    }
    if (auto valid = input::validate(preferences); !valid) {
        m_message->setText(text("Cannot set shortcut: %1").arg(str(valid.error().message())));
        return;
    }
    auto record = std::ranges::find_if(m_records, [&](const auto& old) {
        const auto* id = field<std::string>(old, "action");
        return id && *id == action;
    });
    if (sequences) {
        settings::StringList values;
        for (const auto& sequence : *sequences)
            values.push_back(input::formatSequence(sequence));
        auto replacement = settings::makeRecord({settings::RecordField{"action", settings::FieldValue(action)},
                                                 settings::RecordField{"shortcuts", settings::FieldValue(values)}});
        if (record == m_records.end())
            m_records.push_back(std::move(replacement));
        else
            *record = std::move(replacement);
    } else if (record != m_records.end()) {
        m_records.erase(record);
    }
    m_preferences = std::move(preferences);
    const auto bindings = index();
    const auto actionId = m_registry.find(action);
    QStringList conflicts;
    if (actionId)
        for (const auto& conflict : bindings.conflicts()) {
            if (std::ranges::find(conflict.actions, *actionId) == conflict.actions.end())
                continue;
            for (auto other : conflict.actions)
                if (other != *actionId)
                    conflicts << actionName(m_registry.info(other));
        }
    conflicts.removeDuplicates();
    m_message->setText(
        conflicts.isEmpty()
            ? text("Shortcut staged. Apply to save changes.")
            : text("Conflicts with: %1. These actions may share a context.").arg(conflicts.join(QStringLiteral(", "))));
    rebuild();
    m_changed();
}
} // namespace frost::app
