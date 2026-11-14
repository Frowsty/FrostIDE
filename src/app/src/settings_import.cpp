// SPDX-License-Identifier: Apache-2.0
// Supported .vssettings profile: explicit FontsAndColors category and mapped Global/Text Editor shortcuts.
// Container contract: https://learn.microsoft.com/visualstudio/ide/reference/import-and-export-settings-command
#include "settings_import.hpp"
#include "frost/input/keymap.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/migration.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <algorithm>
#include <map>
#include <set>
namespace frost::app {
namespace {
std::string utf8(QStringView value) { return value.toString().toUtf8().toStdString(); }
std::string attr(const QXmlStreamAttributes& attributes, const char* name) { return utf8(attributes.value(QLatin1StringView(name))); }
std::optional<std::string> colorRef(std::string_view text) {
    if (text.size() != 10 || !text.starts_with("0x00")) return std::nullopt;
    for (char c : text.substr(4)) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return std::nullopt;
    return "#" + std::string(text.substr(8, 2)) + std::string(text.substr(6, 2)) + std::string(text.substr(4, 2));
}
void putColor(settings::RecordList& colors, std::string name, std::string_view raw) {
    auto color = colorRef(raw);
    if (!color) return; // Automatic/system COLORREF values are not literal RGB values.
    auto record = settings::makeRecord({{"name", std::move(name)}, {"color", std::move(*color)}});
    const auto* field = settings::findField(record, "name");
    const auto& key = std::get<std::string>(*field);
    const auto existing = std::ranges::find_if(colors, [&](const auto& old) {
        const auto* oldName = settings::findField(old, "name");
        return oldName && std::get<std::string>(*oldName) == key;
    });
    if (existing == colors.end()) colors.push_back(std::move(record));
    else *existing = std::move(record);
}
constexpr std::pair<std::string_view, std::string_view> commands[] = {
    {"Edit.Copy", "editor.copy"}, {"Edit.Cut", "editor.cut"}, {"Edit.Paste", "editor.paste"},
    {"Edit.Undo", "editor.undo"}, {"Edit.Redo", "editor.redo"}, {"Edit.SelectAll", "editor.selectAll"},
    {"Edit.Find", "editor.find"}, {"Edit.Replace", "editor.replace"}, {"File.SaveSelectedItems", "file.saveDocument"},
    {"File.SaveAll", "file.saveAll"}, {"Edit.GoTo", "editor.gotoLine"}, {"Edit.GoToAll", "search.everywhere"}
};
}
Result<ImportedVisualStudioSettings> parseVisualStudioSettings(std::string_view bytes) {
    if (bytes.size() > 1024 * 1024) return fail(ErrorCode::OutOfRange, "Settings import is limited to 1 MiB");
    QXmlStreamReader xml(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
    ImportedVisualStudioSettings out;
    int depth = 0, fontsDepth = 0, categoryDepth = 0;
    std::optional<std::size_t> category;
    std::set<std::string> categoryIds;
    std::map<std::string, settings::StringList> shortcuts;
    unsigned tokens = 0;
    bool root = false;
    const auto warn = [&](std::string value) { if (out.warnings.size() < 64) out.warnings.push_back(std::move(value)); };
    while (!xml.atEnd()) {
        const auto type = xml.readNext();
        if (++tokens > 50000) return fail(ErrorCode::OutOfRange, "Settings XML token limit exceeded");
        if (type == QXmlStreamReader::DTD || type == QXmlStreamReader::EntityReference)
            return fail(ErrorCode::Unsupported, "Settings import does not accept DTDs or entities");
        if (type == QXmlStreamReader::StartElement) {
            if (++depth > 64) return fail(ErrorCode::OutOfRange, "Settings XML nesting limit exceeded");
            const auto name = xml.name();
            const auto attributes = xml.attributes();
            if (!root) {
                if (name != QLatin1StringView("UserSettings")) return fail(ErrorCode::InvalidArgument, "Expected a .vssettings UserSettings root");
                root = true;
            }
            if (name == QLatin1StringView("FontsAndColors")) fontsDepth = depth;
            if (fontsDepth && name == QLatin1StringView("Category") && attributes.hasAttribute(QLatin1StringView("GUID"))) {
                auto id = attr(attributes, "GUID");
                if (out.fonts.size() >= 128 || !categoryIds.insert(id).second)
                    return fail(ErrorCode::InvalidArgument, "Too many or duplicate font categories");
                ImportedFontCategory font;
                font.id = std::move(id);
                font.font = attr(attributes, "FontName");
                if (attributes.hasAttribute(QLatin1StringView("FontSize"))) {
                    bool ok = false;
                    const auto size = attributes.value(QLatin1StringView("FontSize")).toLongLong(&ok);
                    if (!ok || size < 4 || size > 96) return fail(ErrorCode::InvalidArgument, "Invalid imported font size");
                    font.size = size;
                }
                out.fonts.push_back(std::move(font));
                category = out.fonts.size() - 1;
                categoryDepth = depth;
            }
            if (category && name == QLatin1StringView("Item")) {
                auto& colors = out.fonts[*category].colors;
                const auto item = attr(attributes, "Name");
                std::string key;
                if (item == "Plain Text") { key = "Editor.foreground"; putColor(colors, "Editor.background", attr(attributes, "Background")); }
                else if (item == "Keyword") key = "Syntax.keyword";
                else if (item == "Comment") key = "Syntax.comment";
                else if (item == "String") key = "Syntax.string";
                else if (item == "Number") key = "Syntax.number";
                else if (item == "Identifier") key = "Syntax.identifier";
                else if (item == "Operator") key = "Syntax.operator";
                else if (item == "Line Number") key = "Editor.lineNumberForeground";
                else warn("Unsupported color item: " + item);
                if (!key.empty()) putColor(colors, key, attr(attributes, "Foreground"));
                if (attr(attributes, "BoldFont") == "Yes") warn("Bold item styles are not imported: " + item);
            }
            if (name == QLatin1StringView("Shortcut")) {
                const auto command = attr(attributes, "Command");
                const auto scope = attr(attributes, "Scope");
                const auto mapping = std::ranges::find(commands, command, [](const auto& item) { return item.first; });
                const auto sequenceText = xml.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement).trimmed();
                --depth; // readElementText consumed this element's end.
                if (mapping == std::end(commands) || (scope != "Global" && scope != "Text Editor")) {
                    warn("Unsupported shortcut command/scope: " + command + " / " + scope);
                    continue;
                }
                auto sequence = input::parseSequence(sequenceText.toUtf8().toStdString(), input::SyntaxMode::Lenient);
                if (!sequence) { warn("Unsupported shortcut: " + sequenceText.toStdString()); continue; }
                auto& values = shortcuts[std::string(mapping->second)];
                const auto formatted = input::formatSequence(*sequence);
                if (values.size() < 8 && std::ranges::find(values, formatted) == values.end()) values.push_back(formatted);
            }
        } else if (type == QXmlStreamReader::EndElement) {
            if (depth == categoryDepth) { category.reset(); categoryDepth = 0; }
            if (depth == fontsDepth) fontsDepth = 0;
            --depth;
        }
    }
    if (xml.hasError() || !root) return fail(ErrorCode::InvalidArgument, "Malformed Visual Studio settings XML");
    for (auto& [action, values] : shortcuts)
        out.shortcuts.push_back(settings::makeRecord({{"action", action}, {"shortcuts", std::move(values)}}));
    warn("Only the selected font/color category and mapped Global/Text Editor shortcuts are imported; other package settings remain unchanged.");
    return out;
}
Result<std::vector<settings::ConditionalSettingEdit>> prepareVisualStudioImport(settings::SettingsStore& store,
    const ImportedVisualStudioSettings& imported, std::size_t category) {
    auto doc = store.readScopeDocument(settings::Scope::Application);
    if (!doc) return std::unexpected(doc.error());
    std::vector<settings::ConditionalSettingEdit> edits;
    const auto add = [&](std::string_view key, settings::Value value) -> Result<void> {
        const auto* spec = store.registry().find(key);
        if (!spec) return fail(ErrorCode::NotFound, "Imported setting is not registered");
        if (auto valid = settings::validateValue(spec->schema, value); !valid) return valid;
        const auto* member = doc->find(key);
        edits.push_back({std::string(key), member ? std::optional<std::string>(doc->rawValue(*member)) : std::nullopt, std::move(value)});
        return {};
    };
    if (!imported.fonts.empty()) {
        if (category >= imported.fonts.size()) return fail(ErrorCode::InvalidArgument, "Select an imported font category");
        const auto& font = imported.fonts[category];
        if (!font.font.empty()) if (auto value = add(settings::key::editorFontFamily.name, font.font); !value) return std::unexpected(value.error());
        if (font.size) if (auto value = add(settings::key::editorFontSize.name, *font.size); !value) return std::unexpected(value.error());
        if (!font.colors.empty()) if (auto value = add(settings::key::editorSchemeColors.name, font.colors); !value) return std::unexpected(value.error());
    }
    if (!imported.shortcuts.empty()) {
        settings::RecordList records;
        if (const auto* member = doc->find(settings::key::keymapOverrides.name)) {
            const auto* spec = store.registry().find(settings::key::keymapOverrides.name);
            auto current = doc->value(settings::key::keymapOverrides.name, spec->schema);
            if (!current) return std::unexpected(current.error());
            records = std::get<settings::RecordList>(*current);
        }
        for (const auto& row : imported.shortcuts) {
            const auto action = std::get<std::string>(*settings::findField(row, "action"));
            std::erase_if(records, [&](const auto& old) {
                const auto* field = settings::findField(old, "action");
                return field && std::get<std::string>(*field) == action;
            });
            records.push_back(row);
        }
        if (auto value = add(settings::key::keymapOverrides.name, std::move(records)); !value) return std::unexpected(value.error());
    }
    if (edits.empty()) return fail(ErrorCode::Unsupported, "No supported settings were found in the selected category");
    return edits;
}
bool showSettingsImport(settings::SettingsStore& store, QWidget* parent) {
    const bool hadParent = parent != nullptr;
    const QPointer<QWidget> owner(parent);
    const auto path = QFileDialog::getOpenFileName(parent, QStringLiteral("Import Visual Studio Settings"), {}, QStringLiteral("Visual Studio settings (*.vssettings)"));
    if (path.isEmpty() || (hadParent && !owner)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return false;
    const auto bytes = file.read(1024 * 1024 + 1);
    auto imported = parseVisualStudioSettings(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    // A nested event loop may destroy the settings owner. A parent must not delete this
    // stack object, and no callback may touch its borrowed store after that owner is gone.
    QDialog dialog;
    if (owner) QObject::connect(owner.data(), &QObject::destroyed, &dialog, &QDialog::reject);
    ui::installDialogChrome(&dialog, true);
    dialog.setWindowTitle(QStringLiteral("Review Settings Import"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* categories = new QComboBox(&dialog);
    auto* preview = new QPlainTextEdit(&dialog);
    preview->setReadOnly(true);
    auto* status = new QLabel(&dialog);
    status->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(new QLabel(QStringLiteral("Choose the Visual Studio font/color category to apply to FrostIDE's editor. GUIDs are preserved; no category is guessed."), &dialog));
    layout->addWidget(categories); layout->addWidget(preview); layout->addWidget(status); layout->addWidget(buttons);
    std::vector<settings::ConditionalSettingEdit> captured;
    if (imported) for (const auto& font : imported->fonts)
        categories->addItem(QString::fromStdString(font.id + "  " + font.font + "  (" + std::to_string(font.colors.size()) + " colors)"));
    if (imported && imported->fonts.empty()) categories->addItem(QStringLiteral("Shortcuts only"));
    const auto refresh = [&] {
        if (hadParent && !owner) { dialog.reject(); return; }
        captured.clear();
        if (!imported) { status->setText(QString::fromStdString(imported.error().message())); buttons->button(QDialogButtonBox::Apply)->setEnabled(false); return; }
        auto proposal = prepareVisualStudioImport(store, *imported, static_cast<std::size_t>(std::max(0, categories->currentIndex())));
        if (!proposal) { status->setText(QString::fromStdString(proposal.error().message())); buttons->button(QDialogButtonBox::Apply)->setEnabled(false); return; }
        captured = std::move(*proposal);
        QStringList lines;
        auto proposed = settings::SettingsDocument::create(settings::kCurrentSchemaVersion);
        for (const auto& edit : captured) if (edit.value) (void)proposed.set(edit.key, *edit.value);
        lines << QString::fromStdString(proposed.bytes());
        for (const auto& warning : imported->warnings) lines << QString::fromStdString(warning);
        preview->setPlainText(lines.join(QLatin1Char('\n')));
        status->setText(QStringLiteral("Apply replaces only these settings in one atomic application-file write. Existing editor scheme colors are replaced; unrelated shortcuts are retained."));
        buttons->button(QDialogButtonBox::Apply)->setEnabled(true);
    };
    QObject::connect(categories, &QComboBox::currentIndexChanged, &dialog, [&](int) { refresh(); });
    bool applied = false;
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        if (hadParent && !owner) { dialog.reject(); return; }
        if (auto result = store.applyConditional(settings::Scope::Application, captured); !result) {
            status->setText(QString::fromStdString(result.error().message()));
            buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
            return;
        }
        applied = true;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    refresh();
    dialog.resize(700, 480);
    dialog.exec();
    return applied;
}
} // namespace frost::app
