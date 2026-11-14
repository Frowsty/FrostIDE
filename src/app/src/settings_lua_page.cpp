// SPDX-License-Identifier: Apache-2.0
// The Settings dialog's Lua page (settings_lua_page.hpp, LUA-4): controls over the project's
// .luarc.json, loaded and written through lang::readLuaRc and lang::writeLuaRc.
#include "settings_lua_page.hpp"

#include "frost/ui/tr.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include <array>
#include <string_view>
#include <utility>

namespace frost::app {
namespace {
QString text(std::string_view source) { return ui::qtr("frost::app::SettingsLuaPage", source); }
QString str(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

// The runtimes LuaLS 3.19.1 knows (its Lua.runtime.version values); "" leaves the key out.
constexpr std::array<std::string_view, 5> kRuntimes{"Lua 5.1", "Lua 5.2", "Lua 5.3", "Lua 5.4", "LuaJIT"};

// The labels the dialog's search looks at.
constexpr std::array<std::string_view, 7> kLabels{"Lua",     ".luarc.json",        "Runtime version",     "Libraries",
                                                  "Globals", "Enable diagnostics", "Disabled diagnostics"};

std::vector<std::string> lines(const QPlainTextEdit* edit) {
    std::vector<std::string> out;
    for (const QString& line : edit->toPlainText().split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            out.push_back(trimmed.toStdString());
    }
    return out;
}

QString joined(const std::vector<std::string>& items) {
    QStringList out;
    for (const std::string& item : items)
        out.push_back(str(item));
    return out.join(QLatin1Char('\n'));
}

QPlainTextEdit* listEdit(const QString& name, const QString& placeholder, QWidget* parent) {
    auto* edit = new QPlainTextEdit(parent);
    edit->setObjectName(name);
    edit->setPlaceholderText(placeholder);
    edit->setTabChangesFocus(true);
    edit->setMinimumHeight(64);
    edit->setMaximumHeight(110);
    return edit;
}
} // namespace

SettingsLuaPage::SettingsLuaPage(io::IFileSystem& fs, std::string projectRoot, std::function<void()> dirtyChanged,
                                 std::function<void()> applied, QWidget* parent)
    : QWidget(parent), m_fs(fs), m_root(std::move(projectRoot)), m_dirtyChanged(std::move(dirtyChanged)),
      m_applied(std::move(applied)), m_path(new QLabel(this)), m_status(new QLabel(this)),
      m_version(new QComboBox(this)) {
    setObjectName(QStringLiteral("settingsLuaPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_path->setObjectName(QStringLiteral("luarc.rcPath"));
    m_path->setWordWrap(true);
    layout->addWidget(m_path);
    auto* form = new QFormLayout();
    m_version->setObjectName(QStringLiteral("luarc.runtimeVersion"));
    m_version->addItem(text("Not set (Tools > Lua runtime version applies)"), QString());
    for (const std::string_view runtime : kRuntimes)
        m_version->addItem(str(runtime), str(runtime));
    form->addRow(text("Runtime version"), m_version);
    m_libraries = listEdit(QStringLiteral("luarc.libraries"), text("One folder or file per line"), this);
    form->addRow(text("Libraries"), m_libraries);
    m_globals = listEdit(QStringLiteral("luarc.globals"), text("One global name per line"), this);
    form->addRow(text("Globals"), m_globals);
    m_diagnostics = new QCheckBox(text("Enable diagnostics"), this);
    m_diagnostics->setObjectName(QStringLiteral("luarc.diagnosticsEnabled"));
    form->addRow(QString(), m_diagnostics);
    m_disabled =
        listEdit(QStringLiteral("luarc.disabledDiagnostics"), text("LuaLS diagnostic codes, one per line"), this);
    form->addRow(text("Disabled diagnostics"), m_disabled);
    layout->addLayout(form);
    m_status->setObjectName(QStringLiteral("luarc.rcStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(m_version, &QComboBox::currentIndexChanged, this, [this] { edited(); });
    connect(m_diagnostics, &QCheckBox::toggled, this, [this] { edited(); });
    for (QPlainTextEdit* edit : {m_libraries, m_globals, m_disabled})
        connect(edit, &QPlainTextEdit::textChanged, this, [this] { edited(); });
    load();
}

void SettingsLuaPage::load() {
    m_loading = true;
    m_loaded.reset();
    lang::LuaRcSettings settings;
    QString status;
    if (m_root.empty()) {
        m_path->setText(text("Open a project to edit its .luarc.json, the Lua configuration LuaLS reads."));
    } else if (auto file = lang::readLuaRc(m_fs, m_root); !file) {
        m_path->setText(text("Project file: %1").arg(str(m_root)));
        status = text("The project's .luarc.json cannot be edited here: %1. Edit it in the editor.")
                     .arg(str(file.error().describe()));
    } else {
        m_path->setText(
            text("Project file: %1 (LuaLS reads it; it has priority over the Tools page)").arg(str(file->path)) +
            (file->exists ? QString() : text(" - not created yet")));
        settings = file->settings;
        m_loaded = std::move(*file);
    }
    const QString version = str(settings.runtimeVersion);
    int index = m_version->findData(version);
    if (index < 0) {
        m_version->addItem(version, version); // A value this page does not list stays as it is.
        index = m_version->count() - 1;
    }
    m_version->setCurrentIndex(index);
    m_libraries->setPlainText(joined(settings.libraries));
    m_globals->setPlainText(joined(settings.globals));
    m_diagnostics->setChecked(settings.diagnosticsEnabled);
    m_disabled->setPlainText(joined(settings.disabledDiagnostics));
    const bool editable = m_loaded.has_value();
    for (QWidget* control :
         std::initializer_list<QWidget*>{m_version, m_libraries, m_globals, m_diagnostics, m_disabled})
        control->setEnabled(editable);
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_loading = false;
}

lang::LuaRcSettings SettingsLuaPage::current() const {
    lang::LuaRcSettings settings;
    settings.runtimeVersion = m_version->currentData().toString().toStdString();
    settings.libraries = lines(m_libraries);
    settings.globals = lines(m_globals);
    settings.diagnosticsEnabled = m_diagnostics->isChecked();
    settings.disabledDiagnostics = lines(m_disabled);
    return settings;
}

bool SettingsLuaPage::dirty() const { return m_loaded && current() != m_loaded->settings; }

void SettingsLuaPage::edited() {
    if (!m_loading && m_dirtyChanged)
        m_dirtyChanged();
}

Result<void> SettingsLuaPage::apply() {
    if (!dirty())
        return {};
    if (auto written = lang::writeLuaRc(m_fs, *m_loaded, current()); !written)
        return written;
    load();
    if (m_applied)
        m_applied();
    return {};
}

bool SettingsLuaPage::matches(const QString& query) const {
    if (query.isEmpty())
        return true;
    return std::ranges::any_of(
        kLabels, [&](std::string_view label) { return text(label).contains(query, Qt::CaseInsensitive); });
}

} // namespace frost::app
