// SPDX-License-Identifier: Apache-2.0
//
// XML and solution readers/writers for ProjectPropertiesDialog, SolutionPropertiesDialog,
// ConfigurationManagerDialog, NuGetPackageManagerDialog, and Project Reference management.

#include "frost/io/path.hpp"

#include "project_properties_dialog.hpp"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QUuid>
#include <QXmlStreamReader>

#include <algorithm>

namespace frost::app {
namespace {

QString toQs(std::string_view sv) { return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size())); }

QString xmlEscape(const QString& text) {
    QString out = text;
    out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return out;
}

bool conditionMatchesConfig(const QString& condition, const QString& configPlatform) {
    if (condition.trimmed().isEmpty())
        return true;
    if (configPlatform.trimmed().isEmpty() ||
        configPlatform.compare(QStringLiteral("All Configurations"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return condition.contains(configPlatform, Qt::CaseInsensitive);
}

bool isItemDefinitionSection(const QString& tag) {
    return tag.compare(QStringLiteral("ClCompile"), Qt::CaseInsensitive) == 0 ||
           tag.compare(QStringLiteral("Link"), Qt::CaseInsensitive) == 0 ||
           tag.compare(QStringLiteral("Lib"), Qt::CaseInsensitive) == 0 ||
           tag.compare(QStringLiteral("ResourceCompile"), Qt::CaseInsensitive) == 0 ||
           tag.compare(QStringLiteral("PreBuildEvent"), Qt::CaseInsensitive) == 0 ||
           tag.compare(QStringLiteral("PreLinkEvent"), Qt::CaseInsensitive) == 0 ||
           tag.compare(QStringLiteral("PostBuildEvent"), Qt::CaseInsensitive) == 0;
}

void upsertTagInBlock(QString& block, const QString& tagName, const QString& escapedValue, const QString& closeParent,
                      const QString& indent) {
    const QRegularExpression selfClosingRe(QStringLiteral("<%1\\s*/>").arg(QRegularExpression::escape(tagName)),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression fullTagRe(QStringLiteral("<%1>([\\s\\S]*?)</%1>").arg(QRegularExpression::escape(tagName)),
                                       QRegularExpression::CaseInsensitiveOption);
    if (const auto m = fullTagRe.match(block); m.hasMatch()) {
        block.replace(m.capturedStart(0), m.capturedLength(0),
                      QStringLiteral("<%1>%2</%1>").arg(tagName, escapedValue));
        return;
    }
    if (const auto m = selfClosingRe.match(block); m.hasMatch()) {
        block.replace(m.capturedStart(0), m.capturedLength(0),
                      QStringLiteral("<%1>%2</%1>").arg(tagName, escapedValue));
        return;
    }
    const qsizetype closePos = block.lastIndexOf(closeParent, -1, Qt::CaseInsensitive);
    if (closePos >= 0) {
        block.insert(closePos, QStringLiteral("%1<%2>%3</%2>\n  ").arg(indent, tagName, escapedValue));
    }
}

void applyPropertyGroupUpdate(QString& xml, const QString& configPlatform, const QString& propName,
                              const QString& escapedVal) {
    const QRegularExpression groupRe(QStringLiteral("<PropertyGroup([^>]*)>([\\s\\S]*?)</PropertyGroup>"),
                                     QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = groupRe.globalMatch(xml);
    qsizetype bestStart = -1;
    qsizetype bestLen = 0;
    QString bestBlock;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString attrs = m.captured(1);
        const QString full = m.captured(0);
        if (attrs.contains(QStringLiteral("Label=\"Globals\""), Qt::CaseInsensitive) ||
            attrs.contains(QStringLiteral("Label=\"Configuration\""), Qt::CaseInsensitive)) {
            if (!configPlatform.isEmpty() && attrs.contains(configPlatform, Qt::CaseInsensitive)) {
                bestStart = m.capturedStart(0);
                bestLen = m.capturedLength(0);
                bestBlock = full;
                if (full.contains(QStringLiteral("<%1").arg(propName), Qt::CaseInsensitive))
                    break;
            }
            continue;
        }
        if (!configPlatform.isEmpty() && attrs.contains(configPlatform, Qt::CaseInsensitive)) {
            bestStart = m.capturedStart(0);
            bestLen = m.capturedLength(0);
            bestBlock = full;
            break;
        }
        if (bestStart < 0 && !attrs.contains(QStringLiteral("Condition"), Qt::CaseInsensitive)) {
            bestStart = m.capturedStart(0);
            bestLen = m.capturedLength(0);
            bestBlock = full;
        }
    }
    if (bestStart >= 0) {
        upsertTagInBlock(bestBlock, propName, escapedVal, QStringLiteral("</PropertyGroup>"), QStringLiteral("  "));
        xml.replace(bestStart, bestLen, bestBlock);
        return;
    }
    const QString condAttr =
        configPlatform.isEmpty()
            ? QString()
            : QStringLiteral(" Condition=\"'$(Configuration)|$(Platform)'=='%1'\"").arg(configPlatform);
    const QString newGroup = QStringLiteral("  <PropertyGroup%1>\n    <%2>%3</%2>\n  </PropertyGroup>\n")
                                 .arg(condAttr, propName, escapedVal);
    const qsizetype closeProj = xml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
    if (closeProj >= 0)
        xml.insert(closeProj, newGroup);
}

void applyItemDefinitionUpdate(QString& xml, const QString& configPlatform, const QString& section,
                               const QString& metaName, const QString& escapedVal) {
    const QRegularExpression idgRe(QStringLiteral("<ItemDefinitionGroup([^>]*)>([\\s\\S]*?)</ItemDefinitionGroup>"),
                                   QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = idgRe.globalMatch(xml);
    qsizetype bestStart = -1;
    qsizetype bestLen = 0;
    QString bestBlock;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString attrs = m.captured(1);
        if (!configPlatform.isEmpty() && attrs.contains(configPlatform, Qt::CaseInsensitive)) {
            bestStart = m.capturedStart(0);
            bestLen = m.capturedLength(0);
            bestBlock = m.captured(0);
            break;
        }
        if (bestStart < 0 && !attrs.contains(QStringLiteral("Condition"), Qt::CaseInsensitive)) {
            bestStart = m.capturedStart(0);
            bestLen = m.capturedLength(0);
            bestBlock = m.captured(0);
        }
    }
    if (bestStart >= 0) {
        const QRegularExpression secRe(QStringLiteral("<%1>([\\s\\S]*?)</%1>").arg(QRegularExpression::escape(section)),
                                       QRegularExpression::CaseInsensitiveOption);
        if (const auto sm = secRe.match(bestBlock); sm.hasMatch()) {
            QString secBlock = sm.captured(0);
            upsertTagInBlock(secBlock, metaName, escapedVal, QStringLiteral("</%1>").arg(section),
                             QStringLiteral("    "));
            bestBlock.replace(sm.capturedStart(0), sm.capturedLength(0), secBlock);
        } else {
            const qsizetype closeIdg =
                bestBlock.lastIndexOf(QStringLiteral("</ItemDefinitionGroup>"), -1, Qt::CaseInsensitive);
            if (closeIdg >= 0) {
                bestBlock.insert(
                    closeIdg,
                    QStringLiteral("  <%1>\n      <%2>%3</%2>\n    </%1>\n  ").arg(section, metaName, escapedVal));
            }
        }
        xml.replace(bestStart, bestLen, bestBlock);
        return;
    }
    const QString condAttr =
        configPlatform.isEmpty()
            ? QString()
            : QStringLiteral(" Condition=\"'$(Configuration)|$(Platform)'=='%1'\"").arg(configPlatform);
    const QString newIdg = QStringLiteral("  <ItemDefinitionGroup%1>\n    <%2>\n      <%3>%4</%3>\n    </%2>\n  "
                                          "</ItemDefinitionGroup>\n")
                               .arg(condAttr, section, metaName, escapedVal);
    const qsizetype closeProj = xml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
    if (closeProj >= 0)
        xml.insert(closeProj, newIdg);
}

} // namespace

ProjectPropertyMap parseVcxprojPropertiesForConfig(std::string_view xmlContent, std::string_view configPlatform) {
    ProjectPropertyMap result;
    const QString targetCfg = toQs(configPlatform);
    QXmlStreamReader reader(toQs(xmlContent));
    QString currentGroup;
    QString currentSection;
    bool groupActive = false;

    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = reader.name().toString();
            if (name.compare(QStringLiteral("PropertyGroup"), Qt::CaseInsensitive) == 0 ||
                name.compare(QStringLiteral("ItemDefinitionGroup"), Qt::CaseInsensitive) == 0) {
                currentGroup = name;
                const QString cond = reader.attributes().value(QStringLiteral("Condition")).toString();
                groupActive = conditionMatchesConfig(cond, targetCfg);
            } else if (groupActive && currentGroup.compare(QStringLiteral("PropertyGroup"), Qt::CaseInsensitive) == 0) {
                const QString propCond = reader.attributes().value(QStringLiteral("Condition")).toString();
                if (conditionMatchesConfig(propCond, targetCfg)) {
                    const QString val = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                    result["Prop." + name.toStdString()] = val.toStdString();
                }
            } else if (groupActive &&
                       currentGroup.compare(QStringLiteral("ItemDefinitionGroup"), Qt::CaseInsensitive) == 0) {
                if (currentSection.isEmpty() && isItemDefinitionSection(name)) {
                    currentSection = name;
                } else if (!currentSection.isEmpty()) {
                    const QString val = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                    result[currentSection.toStdString() + "." + name.toStdString()] = val.toStdString();
                }
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QString name = reader.name().toString();
            if (!currentSection.isEmpty() && name.compare(currentSection, Qt::CaseInsensitive) == 0) {
                currentSection.clear();
            } else if (name.compare(currentGroup, Qt::CaseInsensitive) == 0) {
                currentGroup.clear();
                groupActive = false;
            }
        }
    }
    return result;
}

std::string applyVcxprojPropertiesForConfig(std::string_view xmlContent, std::string_view configPlatform,
                                            const ProjectPropertyMap& updates) {
    QString xml = toQs(xmlContent);
    QString cfg = toQs(configPlatform);
    if (cfg.compare(QStringLiteral("All Configurations"), Qt::CaseInsensitive) == 0 ||
        cfg.startsWith(QStringLiteral("All "), Qt::CaseInsensitive)) {
        cfg.clear();
    }
    for (const auto& [key, value] : updates) {
        const std::size_t dot = key.find('.');
        if (dot == std::string::npos)
            continue;
        const QString prefix = toQs(std::string_view(key).substr(0, dot));
        const QString elem = toQs(std::string_view(key).substr(dot + 1));
        const QString escaped = xmlEscape(toQs(value));
        if (prefix.compare(QStringLiteral("Prop"), Qt::CaseInsensitive) == 0) {
            applyPropertyGroupUpdate(xml, cfg, elem, escaped);
        } else {
            applyItemDefinitionUpdate(xml, cfg, prefix, elem, escaped);
        }
    }
    return xml.toStdString();
}

ProjectPropertyMap parseCsprojProperties(std::string_view xmlContent) {
    ProjectPropertyMap result;
    QXmlStreamReader reader(toQs(xmlContent));
    bool inPropertyGroup = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = reader.name().toString();
            if (name.compare(QStringLiteral("PropertyGroup"), Qt::CaseInsensitive) == 0) {
                inPropertyGroup = true;
            } else if (inPropertyGroup) {
                const QString val = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                result["Prop." + name.toStdString()] = val.toStdString();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (reader.name().compare(QStringLiteral("PropertyGroup"), Qt::CaseInsensitive) == 0)
                inPropertyGroup = false;
        }
    }
    return result;
}

std::string applyCsprojProperties(std::string_view xmlContent, const ProjectPropertyMap& updates) {
    QString xml = toQs(xmlContent);
    const QRegularExpression selfClosingProj(QStringLiteral("<Project([^>]*)/>"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (const auto m = selfClosingProj.match(xml); m.hasMatch()) {
        xml.replace(
            m.capturedStart(0), m.capturedLength(0),
            QStringLiteral("<Project%1>\n  <PropertyGroup>\n  </PropertyGroup>\n</Project>").arg(m.captured(1)));
    }
    for (const auto& [key, value] : updates) {
        if (!key.starts_with("Prop."))
            continue;
        const QString propName = toQs(std::string_view(key).substr(5));
        applyPropertyGroupUpdate(xml, QString(), propName, xmlEscape(toQs(value)));
    }
    return xml.toStdString();
}

std::vector<NuGetPackageEntry> parseCsprojNuGetPackages(std::string_view xmlContent) {
    std::vector<NuGetPackageEntry> packages;
    QXmlStreamReader reader(toQs(xmlContent));
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement &&
            reader.name().compare(QStringLiteral("PackageReference"), Qt::CaseInsensitive) == 0) {
            const auto attrs = reader.attributes();
            QString id = attrs.value(QStringLiteral("Include")).toString();
            if (id.isEmpty())
                id = attrs.value(QStringLiteral("Update")).toString();
            QString version = attrs.value(QStringLiteral("Version")).toString();
            if (version.isEmpty()) {
                while (!(reader.isEndElement() &&
                         reader.name().compare(QStringLiteral("PackageReference"), Qt::CaseInsensitive) == 0) &&
                       !reader.atEnd()) {
                    if (reader.readNext() == QXmlStreamReader::StartElement &&
                        reader.name().compare(QStringLiteral("Version"), Qt::CaseInsensitive) == 0) {
                        version = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                    }
                }
            }
            if (!id.isEmpty())
                packages.push_back({id.toStdString(), version.toStdString()});
        }
    }
    return packages;
}

std::string upsertCsprojNuGetPackage(std::string_view xmlContent, std::string_view packageId,
                                     std::string_view version) {
    QString xml = toQs(xmlContent);
    const QString id = xmlEscape(toQs(packageId));
    const QString ver = xmlEscape(toQs(version));
    const QRegularExpression existingRe(
        QStringLiteral("<PackageReference\\s+Include=\"%1\"[^>]*(?:/>|>[\\s\\S]*?</PackageReference>)")
            .arg(QRegularExpression::escape(toQs(packageId))),
        QRegularExpression::CaseInsensitiveOption);
    const QString replacement = QStringLiteral("<PackageReference Include=\"%1\" Version=\"%2\" />").arg(id, ver);
    if (const auto m = existingRe.match(xml); m.hasMatch()) {
        xml.replace(m.capturedStart(0), m.capturedLength(0), replacement);
        return xml.toStdString();
    }
    const qsizetype closeProj = xml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
    if (closeProj >= 0) {
        xml.insert(closeProj, QStringLiteral("  <ItemGroup>\n    %1\n  </ItemGroup>\n").arg(replacement));
    }
    return xml.toStdString();
}

std::string removeCsprojNuGetPackage(std::string_view xmlContent, std::string_view packageId) {
    QString xml = toQs(xmlContent);
    const QRegularExpression existingRe(
        QStringLiteral("\\s*<PackageReference\\s+Include=\"%1\"[^>]*(?:/>|>[\\s\\S]*?</PackageReference>)")
            .arg(QRegularExpression::escape(toQs(packageId))),
        QRegularExpression::CaseInsensitiveOption);
    xml.remove(existingRe);
    return xml.toStdString();
}

std::string addProjectReferenceXml(std::string_view xmlContent, std::string_view relativeRefPath,
                                   std::string_view referencedGuid) {
    QString xml = toQs(xmlContent);
    const QString rel = xmlEscape(toQs(relativeRefPath));
    if (xml.contains(rel, Qt::CaseInsensitive))
        return xml.toStdString();
    QString refElement;
    if (!referencedGuid.empty()) {
        refElement = QStringLiteral("  <ItemGroup>\n    <ProjectReference Include=\"%1\">\n      "
                                    "<Project>%2</Project>\n    </ProjectReference>\n  </ItemGroup>\n")
                         .arg(rel, xmlEscape(toQs(referencedGuid)));
    } else {
        refElement =
            QStringLiteral("  <ItemGroup>\n    <ProjectReference Include=\"%1\" />\n  </ItemGroup>\n").arg(rel);
    }
    const qsizetype closeProj = xml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
    if (closeProj >= 0)
        xml.insert(closeProj, refElement);
    return xml.toStdString();
}

std::string removeProjectReferenceXml(std::string_view xmlContent, std::string_view relativeRefPath) {
    QString xml = toQs(xmlContent);
    const QString escapedPath = QRegularExpression::escape(toQs(relativeRefPath));
    QString altPath = toQs(relativeRefPath);
    altPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString escapedAlt = QRegularExpression::escape(altPath);
    const QRegularExpression re(
        QStringLiteral("\\s*<ProjectReference\\s+Include=\"(?:%1|%2)\"[^>]*(?:/>|>[\\s\\S]*?</ProjectReference>)")
            .arg(escapedPath, escapedAlt),
        QRegularExpression::CaseInsensitiveOption);
    xml.remove(re);
    return xml.toStdString();
}

Result<void> addExistingProjectToSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                              std::string_view projectPath) {
    auto read = fs.readFile(solutionPath);
    if (!read)
        return std::unexpected(read.error());
    QString text = toQs(*read);
    const QFileInfo slnInfo(toQs(solutionPath));
    const QFileInfo projInfo(toQs(projectPath));
    const QString relPath = QDir::toNativeSeparators(slnInfo.dir().relativeFilePath(projInfo.absoluteFilePath()));
    const QString projName = projInfo.completeBaseName();

    if (toQs(solutionPath).endsWith(QStringLiteral(".slnx"), Qt::CaseInsensitive)) {
        QString slashRel = relPath;
        slashRel.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (text.contains(slashRel, Qt::CaseInsensitive))
            return {};
        const qsizetype closeSln = text.lastIndexOf(QStringLiteral("</Solution>"), -1, Qt::CaseInsensitive);
        if (closeSln < 0)
            return fail(ErrorCode::Corrupt, "missing </Solution> tag in .slnx");
        text.insert(closeSln, QStringLiteral("  <Project Path=\"%1\" />\n").arg(xmlEscape(slashRel)));
        return fs.writeFileAtomic(solutionPath, text.toStdString());
    }

    if (text.contains(relPath, Qt::CaseInsensitive))
        return {};
    const bool isCsproj = projInfo.suffix().compare(QStringLiteral("csproj"), Qt::CaseInsensitive) == 0;
    const QString typeGuid = isCsproj ? QStringLiteral("{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}")
                                      : QStringLiteral("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}");
    const QString projGuid = QUuid::createUuid().toString(QUuid::WithBraces).toUpper();
    const QString projEntry = QStringLiteral("Project(\"%1\") = \"%2\", \"%3\", \"%4\"\r\nEndProject\r\n")
                                  .arg(typeGuid, projName, relPath, projGuid);
    const qsizetype globalPos = text.indexOf(QStringLiteral("Global"), 0, Qt::CaseInsensitive);
    if (globalPos >= 0)
        text.insert(globalPos, projEntry);
    else
        text.append(projEntry);
    return fs.writeFileAtomic(solutionPath, text.toStdString());
}

Result<void> removeProjectFromSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                           std::string_view projectPath) {
    auto read = fs.readFile(solutionPath);
    if (!read)
        return std::unexpected(read.error());
    QString text = toQs(*read);
    const QFileInfo slnInfo(toQs(solutionPath));
    const QFileInfo projInfo(toQs(projectPath));
    const QString relNative = QDir::toNativeSeparators(slnInfo.dir().relativeFilePath(projInfo.absoluteFilePath()));
    QString relSlash = relNative;
    relSlash.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (toQs(solutionPath).endsWith(QStringLiteral(".slnx"), Qt::CaseInsensitive)) {
        const QRegularExpression re(
            QStringLiteral("\\s*<Project\\s+Path=\"(?:%1|%2)\"[^>]*(?:/>|>[\\s\\S]*?</Project>)")
                .arg(QRegularExpression::escape(relNative), QRegularExpression::escape(relSlash)),
            QRegularExpression::CaseInsensitiveOption);
        text.remove(re);
        return fs.writeFileAtomic(solutionPath, text.toStdString());
    }

    const QRegularExpression projBlockRe(
        QStringLiteral("Project\\(\"[^\"]+\"\\)\\s*=\\s*\"[^\"]+\",\\s*\"(?:%1|%2)\",\\s*\"(\\{[^\\}]+\\})\"[\\s\\S]*?"
                       "EndProject\\r?\\n?")
            .arg(QRegularExpression::escape(relNative), QRegularExpression::escape(relSlash)),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto m = projBlockRe.match(text); m.hasMatch()) {
        const QString guid = m.captured(1);
        text.remove(m.capturedStart(0), m.capturedLength(0));
        if (!guid.isEmpty()) {
            const QRegularExpression cfgLineRe(
                QStringLiteral("[ \\t]*%1\\.[^\\r\\n]+\\r?\\n?").arg(QRegularExpression::escape(guid)),
                QRegularExpression::CaseInsensitiveOption);
            text.remove(cfgLineRe);
        }
    }
    return fs.writeFileAtomic(solutionPath, text.toStdString());
}

namespace {

QString normalizeFilterSeparators(QString filterPath) {
    filterPath.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (filterPath.startsWith(QLatin1Char('\\'))) {
        filterPath.remove(0, 1);
    }
    while (filterPath.endsWith(QLatin1Char('\\'))) {
        filterPath.chop(1);
    }
    return filterPath.trimmed();
}

QString classifyVcxprojItemTag(const QString& filePath) {
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (suffix == QStringLiteral("c") || suffix == QStringLiteral("cc") || suffix == QStringLiteral("cpp") ||
        suffix == QStringLiteral("cxx") || suffix == QStringLiteral("c++") || suffix == QStringLiteral("cppm") ||
        suffix == QStringLiteral("ixx") || suffix == QStringLiteral("cu")) {
        return QStringLiteral("ClCompile");
    }
    if (suffix == QStringLiteral("h") || suffix == QStringLiteral("hh") || suffix == QStringLiteral("hpp") ||
        suffix == QStringLiteral("hxx") || suffix == QStringLiteral("h++") || suffix == QStringLiteral("inl") ||
        suffix == QStringLiteral("ipp") || suffix == QStringLiteral("tpp")) {
        return QStringLiteral("ClInclude");
    }
    if (suffix == QStringLiteral("rc")) {
        return QStringLiteral("ResourceCompile");
    }
    if (suffix == QStringLiteral("natvis")) {
        return QStringLiteral("Natvis");
    }
    return QStringLiteral("None");
}

QString readOrDefaultFiltersXml(io::IFileSystem& fs, const std::string& filtersPath) {
    if (auto read = fs.readFile(filtersPath)) {
        return toQs(*read);
    }
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
        "</Project>\n");
}

void ensureFilterDeclarationsInXml(QString& filtersXml, const QString& fullFilterPath) {
    const QString clean = normalizeFilterSeparators(fullFilterPath);
    if (clean.isEmpty()) {
        return;
    }
    const QStringList parts = clean.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    QString prefix;
    QString toInsert;
    for (const QString& part : parts) {
        if (!prefix.isEmpty()) {
            prefix += QLatin1Char('\\');
        }
        prefix += part;
        const QString escapedPrefix = xmlEscape(prefix);
        const QRegularExpression existingRe(
            QStringLiteral("<Filter\\s+Include=\"%1\"\\s*(?:/>|>)").arg(QRegularExpression::escape(escapedPrefix)),
            QRegularExpression::CaseInsensitiveOption);
        if (!existingRe.match(filtersXml).hasMatch()) {
            const QString guid = QUuid::createUuid().toString(QUuid::WithBraces).toLower();
            toInsert += QStringLiteral("    <Filter Include=\"%1\">\n      "
                                       "<UniqueIdentifier>%2</UniqueIdentifier>\n    </Filter>\n")
                            .arg(escapedPrefix, guid);
        }
    }
    if (toInsert.isEmpty()) {
        return;
    }
    const qsizetype closeProj = filtersXml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
    if (closeProj >= 0) {
        filtersXml.insert(closeProj, QStringLiteral("  <ItemGroup>\n%1  </ItemGroup>\n").arg(toInsert));
    }
}

bool xmlContainsItemInclude(const QString& xml, const QString& relNative) {
    const QString escBack = QRegularExpression::escape(xmlEscape(relNative));
    QString relSlash = relNative;
    relSlash.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString escSlash = QRegularExpression::escape(xmlEscape(relSlash));
    const QRegularExpression re(QStringLiteral("Include=\"(?:%1|%2)\"").arg(escBack, escSlash),
                                QRegularExpression::CaseInsensitiveOption);
    return re.match(xml).hasMatch();
}

bool removeItemIncludeFromXml(QString& xml, const QString& relNative) {
    const QString escBack = QRegularExpression::escape(xmlEscape(relNative));
    QString relSlash = relNative;
    relSlash.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString escSlash = QRegularExpression::escape(xmlEscape(relSlash));
    const QRegularExpression re(
        QStringLiteral("[ \\t]*<(?:ClCompile|ClInclude|ResourceCompile|Natvis|None|CustomBuild|Text|Image|Media)"
                       "\\s+Include=\"(?:%1|%2)\"[^>]*(?:/>|>[\\s\\S]*?</"
                       "(?:ClCompile|ClInclude|ResourceCompile|Natvis|None|CustomBuild|Text|Image|Media)>)\\r?\\n?")
            .arg(escBack, escSlash),
        QRegularExpression::CaseInsensitiveOption);
    if (!re.match(xml).hasMatch()) {
        return false;
    }
    xml.remove(re);
    return true;
}

bool isIgnoredSourceDirectory(std::string_view name) {
    QString qname = toQs(name);
    return qname.compare(QStringLiteral(".git"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral(".vs"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral(".vscode"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("obj"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("x64"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("Win32"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("ARM64"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("Debug"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("Release"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("build"), Qt::CaseInsensitive) == 0 ||
           qname.compare(QStringLiteral("out"), Qt::CaseInsensitive) == 0;
}

bool isIgnoredMetaFile(std::string_view name) {
    const QString suffix = QFileInfo(toQs(name)).suffix().toLower();
    return suffix == QStringLiteral("vcxproj") || suffix == QStringLiteral("filters") ||
           suffix == QStringLiteral("user") || suffix == QStringLiteral("sln") || suffix == QStringLiteral("slnx") ||
           suffix == QStringLiteral("obj") || suffix == QStringLiteral("pdb") || suffix == QStringLiteral("ilk") ||
           suffix == QStringLiteral("exe") || suffix == QStringLiteral("dll") || suffix == QStringLiteral("lib") ||
           suffix == QStringLiteral("exp") || suffix == QStringLiteral("tlog") || suffix == QStringLiteral("log") ||
           suffix == QStringLiteral("idb") || suffix == QStringLiteral("ipdb") || suffix == QStringLiteral("iobj");
}

} // namespace

Result<void> addFilterToVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath, std::string_view filterPath) {
    const QString clean = normalizeFilterSeparators(toQs(filterPath));
    if (clean.isEmpty()) {
        return fail(ErrorCode::InvalidArgument, "enter a non-empty filter name");
    }
    const std::string filtersPath = std::string(vcxprojPath) + ".filters";
    QString filtersXml = readOrDefaultFiltersXml(fs, filtersPath);
    ensureFilterDeclarationsInXml(filtersXml, clean);
    auto written = fs.safeWrite(filtersPath, filtersXml.toStdString(), {});
    if (!written) {
        return std::unexpected(written.error());
    }
    return {};
}

Result<void> renameFilterInVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath,
                                          std::string_view oldFilterPath, std::string_view newFilterPath) {
    const QString oldClean = normalizeFilterSeparators(toQs(oldFilterPath));
    const QString newClean = normalizeFilterSeparators(toQs(newFilterPath));
    if (oldClean.isEmpty() || newClean.isEmpty()) {
        return fail(ErrorCode::InvalidArgument, "enter a valid filter name");
    }
    if (oldClean == newClean) {
        return {};
    }
    const std::string filtersPath = std::string(vcxprojPath) + ".filters";
    QString filtersXml = readOrDefaultFiltersXml(fs, filtersPath);
    const QString oldEsc = xmlEscape(oldClean);
    const QString newEsc = xmlEscape(newClean);

    filtersXml.replace(QStringLiteral("<Filter Include=\"%1\"").arg(oldEsc),
                       QStringLiteral("<Filter Include=\"%1\"").arg(newEsc), Qt::CaseInsensitive);
    filtersXml.replace(QStringLiteral("<Filter Include=\"%1\\").arg(oldEsc),
                       QStringLiteral("<Filter Include=\"%1\\").arg(newEsc), Qt::CaseInsensitive);
    filtersXml.replace(QStringLiteral("<Filter>%1</Filter>").arg(oldEsc),
                       QStringLiteral("<Filter>%1</Filter>").arg(newEsc), Qt::CaseInsensitive);
    filtersXml.replace(QStringLiteral("<Filter>%1\\").arg(oldEsc), QStringLiteral("<Filter>%1\\").arg(newEsc),
                       Qt::CaseInsensitive);

    ensureFilterDeclarationsInXml(filtersXml, newClean);
    auto written = fs.safeWrite(filtersPath, filtersXml.toStdString(), {});
    if (!written) {
        return std::unexpected(written.error());
    }
    return {};
}

Result<void> removeFilterFromVcxprojFilters(io::IFileSystem& fs, std::string_view vcxprojPath,
                                            std::string_view filterPath) {
    const QString clean = normalizeFilterSeparators(toQs(filterPath));
    if (clean.isEmpty()) {
        return fail(ErrorCode::InvalidArgument, "select a filter to remove");
    }
    const std::string filtersPath = std::string(vcxprojPath) + ".filters";
    if (!fs.exists(filtersPath).value_or(false)) {
        return {};
    }
    auto read = fs.readFile(filtersPath);
    if (!read) {
        return std::unexpected(read.error());
    }
    QString filtersXml = toQs(*read);
    const QString esc = QRegularExpression::escape(xmlEscape(clean));
    const QRegularExpression filterDeclRe(
        QStringLiteral("[ \\t]*<Filter\\s+Include=\"%1(?:\\\\[^\"]*)?\"[^>]*(?:/>|>[\\s\\S]*?</Filter>)\\r?\\n?")
            .arg(esc),
        QRegularExpression::CaseInsensitiveOption);
    filtersXml.remove(filterDeclRe);

    const QRegularExpression itemFilterRe(QStringLiteral("[ \\t]*<Filter>%1(?:\\\\[^<]*)?</Filter>\\r?\\n?").arg(esc),
                                          QRegularExpression::CaseInsensitiveOption);
    filtersXml.remove(itemFilterRe);

    auto written = fs.safeWrite(filtersPath, filtersXml.toStdString(), {});
    if (!written) {
        return std::unexpected(written.error());
    }
    return {};
}

Result<void> moveFilesToVcxprojFilter(io::IFileSystem& fs, std::string_view vcxprojPath,
                                      std::span<const std::string> filePaths, std::string_view targetFilterPath) {
    if (filePaths.empty()) {
        return {};
    }
    auto projRead = fs.readFile(vcxprojPath);
    if (!projRead) {
        return std::unexpected(projRead.error());
    }
    QString projXml = toQs(*projRead);
    const std::string filtersPath = std::string(vcxprojPath) + ".filters";
    QString filtersXml = readOrDefaultFiltersXml(fs, filtersPath);
    const QString cleanFilter = normalizeFilterSeparators(toQs(targetFilterPath));
    if (!cleanFilter.isEmpty()) {
        ensureFilterDeclarationsInXml(filtersXml, cleanFilter);
    }

    const QFileInfo projInfo(toQs(vcxprojPath));
    const QDir projDir = projInfo.dir();
    bool projChanged = false;
    QString projInsertions;
    QString filterInsertions;

    for (const std::string& rawFile : filePaths) {
        const QFileInfo fi(toQs(rawFile));
        const QString relNative = QDir::toNativeSeparators(projDir.relativeFilePath(fi.absoluteFilePath()));
        if (relNative.isEmpty()) {
            continue;
        }
        const QString tag = classifyVcxprojItemTag(fi.fileName());
        const QString escRel = xmlEscape(relNative);

        if (!xmlContainsItemInclude(projXml, relNative)) {
            projInsertions += QStringLiteral("    <%1 Include=\"%2\" />\n").arg(tag, escRel);
            projChanged = true;
        }

        removeItemIncludeFromXml(filtersXml, relNative);
        if (cleanFilter.isEmpty()) {
            filterInsertions += QStringLiteral("    <%1 Include=\"%2\" />\n").arg(tag, escRel);
        } else {
            filterInsertions += QStringLiteral("    <%1 Include=\"%2\">\n      <Filter>%3</Filter>\n    </%1>\n")
                                    .arg(tag, escRel, xmlEscape(cleanFilter));
        }
    }

    if (projChanged && !projInsertions.isEmpty()) {
        const qsizetype closeProj = projXml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
        if (closeProj >= 0) {
            projXml.insert(closeProj, QStringLiteral("  <ItemGroup>\n%1  </ItemGroup>\n").arg(projInsertions));
            if (auto w = fs.safeWrite(vcxprojPath, projXml.toStdString(), {}); !w) {
                return std::unexpected(w.error());
            }
        }
    }

    if (!filterInsertions.isEmpty()) {
        const qsizetype closeFilt = filtersXml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
        if (closeFilt >= 0) {
            filtersXml.insert(closeFilt, QStringLiteral("  <ItemGroup>\n%1  </ItemGroup>\n").arg(filterInsertions));
        }
    }

    if (auto w = fs.safeWrite(filtersPath, filtersXml.toStdString(), {}); !w) {
        return std::unexpected(w.error());
    }
    return {};
}

Result<std::size_t> addExistingFilesToVcxproj(io::IFileSystem& fs, std::string_view vcxprojPath,
                                              std::span<const std::string> paths, std::string_view targetFilterPath) {
    if (paths.empty()) {
        return std::size_t{0};
    }
    auto projRead = fs.readFile(vcxprojPath);
    if (!projRead) {
        return std::unexpected(projRead.error());
    }
    QString projXml = toQs(*projRead);
    const std::string filtersPath = std::string(vcxprojPath) + ".filters";
    QString filtersXml = readOrDefaultFiltersXml(fs, filtersPath);
    const QString baseFilter = normalizeFilterSeparators(toQs(targetFilterPath));

    const QFileInfo projInfo(toQs(vcxprojPath));
    const QDir projDir = projInfo.dir();
    const QString projDirPath = QDir::cleanPath(projDir.absolutePath());

    struct FileToAdd {
        QString absPath;
        QString filterPath;
    };
    std::vector<FileToAdd> filesToAdd;

    const auto collectDirectory = [&](const auto& self, const std::string& dirAbs,
                                      const QString& currentFilter) -> void {
        auto entries = fs.listDirectory(dirAbs);
        if (!entries) {
            return;
        }
        std::ranges::sort(*entries, [](const io::DirEntry& a, const io::DirEntry& b) { return a.name < b.name; });
        for (const io::DirEntry& entry : *entries) {
            auto childAbs = io::joinPath(dirAbs, entry.name);
            if (!childAbs) {
                continue;
            }
            if (entry.type == io::FileType::Directory) {
                if (isIgnoredSourceDirectory(entry.name)) {
                    continue;
                }
                const QString subFilter =
                    currentFilter.isEmpty() ? toQs(entry.name) : (currentFilter + QLatin1Char('\\') + toQs(entry.name));
                ensureFilterDeclarationsInXml(filtersXml, subFilter);
                self(self, *childAbs, subFilter);
            } else if (entry.type == io::FileType::Regular) {
                if (isIgnoredMetaFile(entry.name)) {
                    continue;
                }
                filesToAdd.push_back({toQs(*childAbs), currentFilter});
            }
        }
    };

    for (const std::string& rawPath : paths) {
        auto st = fs.status(rawPath);
        if (!st) {
            continue;
        }
        if (st->type == io::FileType::Directory) {
            const QFileInfo dirFi(toQs(rawPath));
            const QString cleanDir = QDir::cleanPath(dirFi.absoluteFilePath());
            QString folderFilter = baseFilter;
            if (folderFilter.isEmpty()) {
                if (cleanDir.startsWith(projDirPath + QLatin1Char('/'), Qt::CaseInsensitive)) {
                    folderFilter = normalizeFilterSeparators(cleanDir.mid(projDirPath.size() + 1));
                } else {
                    folderFilter = dirFi.fileName();
                }
            } else if (!dirFi.fileName().isEmpty() && !folderFilter.endsWith(dirFi.fileName(), Qt::CaseInsensitive)) {
                folderFilter += QLatin1Char('\\') + dirFi.fileName();
            }
            ensureFilterDeclarationsInXml(filtersXml, folderFilter);
            collectDirectory(collectDirectory, rawPath, folderFilter);
        } else if (st->type == io::FileType::Regular) {
            const QFileInfo fi(toQs(rawPath));
            QString itemFilter = baseFilter;
            if (itemFilter.isEmpty()) {
                const QString cleanParent = QDir::cleanPath(fi.absolutePath());
                if (cleanParent.startsWith(projDirPath + QLatin1Char('/'), Qt::CaseInsensitive)) {
                    itemFilter = normalizeFilterSeparators(cleanParent.mid(projDirPath.size() + 1));
                }
            }
            filesToAdd.push_back({fi.absoluteFilePath(), itemFilter});
        }
    }

    QString projInsertions;
    QString filterInsertions;
    std::size_t addedCount = 0;

    for (const FileToAdd& item : filesToAdd) {
        const QFileInfo fi(item.absPath);
        const QString relNative = QDir::toNativeSeparators(projDir.relativeFilePath(fi.absoluteFilePath()));
        if (relNative.isEmpty() || xmlContainsItemInclude(projXml, relNative)) {
            continue;
        }
        const QString tag = classifyVcxprojItemTag(fi.fileName());
        const QString escRel = xmlEscape(relNative);
        projInsertions += QStringLiteral("    <%1 Include=\"%2\" />\n").arg(tag, escRel);

        removeItemIncludeFromXml(filtersXml, relNative);
        if (!item.filterPath.isEmpty()) {
            ensureFilterDeclarationsInXml(filtersXml, item.filterPath);
            filterInsertions += QStringLiteral("    <%1 Include=\"%2\">\n      <Filter>%3</Filter>\n    </%1>\n")
                                    .arg(tag, escRel, xmlEscape(item.filterPath));
        } else {
            filterInsertions += QStringLiteral("    <%1 Include=\"%2\" />\n").arg(tag, escRel);
        }
        ++addedCount;
    }

    if (addedCount == 0) {
        return std::size_t{0};
    }

    const qsizetype closeProj = projXml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
    if (closeProj < 0) {
        return fail(ErrorCode::Corrupt, "missing </Project> tag in .vcxproj");
    }
    projXml.insert(closeProj, QStringLiteral("  <ItemGroup>\n%1  </ItemGroup>\n").arg(projInsertions));
    if (auto w = fs.safeWrite(vcxprojPath, projXml.toStdString(), {}); !w) {
        return std::unexpected(w.error());
    }

    const qsizetype closeFilt = filtersXml.lastIndexOf(QStringLiteral("</Project>"), -1, Qt::CaseInsensitive);
    if (closeFilt >= 0 && !filterInsertions.isEmpty()) {
        filtersXml.insert(closeFilt, QStringLiteral("  <ItemGroup>\n%1  </ItemGroup>\n").arg(filterInsertions));
    }
    if (auto w = fs.safeWrite(filtersPath, filtersXml.toStdString(), {}); !w) {
        return std::unexpected(w.error());
    }
    return addedCount;
}

Result<std::size_t> removeFilesFromVcxproj(io::IFileSystem& fs, std::string_view vcxprojPath,
                                           std::span<const std::string> filePaths) {
    if (filePaths.empty()) {
        return std::size_t{0};
    }
    auto projRead = fs.readFile(vcxprojPath);
    if (!projRead) {
        return std::unexpected(projRead.error());
    }
    QString projXml = toQs(*projRead);
    const std::string filtersPath = std::string(vcxprojPath) + ".filters";
    const bool hasFilters = fs.exists(filtersPath).value_or(false);
    QString filtersXml = hasFilters ? readOrDefaultFiltersXml(fs, filtersPath) : QString();

    const QFileInfo projInfo(toQs(vcxprojPath));
    const QDir projDir = projInfo.dir();
    std::size_t removedCount = 0;

    for (const std::string& rawFile : filePaths) {
        const QFileInfo fi(toQs(rawFile));
        const QString relNative = QDir::toNativeSeparators(projDir.relativeFilePath(fi.absoluteFilePath()));
        if (relNative.isEmpty()) {
            continue;
        }
        if (removeItemIncludeFromXml(projXml, relNative)) {
            ++removedCount;
        }
        if (hasFilters) {
            removeItemIncludeFromXml(filtersXml, relNative);
        }
    }

    if (removedCount == 0) {
        return std::size_t{0};
    }
    if (auto w = fs.safeWrite(vcxprojPath, projXml.toStdString(), {}); !w) {
        return std::unexpected(w.error());
    }
    if (hasFilters) {
        (void)fs.safeWrite(filtersPath, filtersXml.toStdString(), {});
    }
    return removedCount;
}

namespace {

void upsertSlnNestedProject(QString& slnText, const QString& childGuid, const QString& parentFolderGuid) {
    const QRegularExpression existingLineRe(
        QStringLiteral("[ \\t]*%1\\s*=\\s*\\{[^\\}]+\\}\\r?\\n?").arg(QRegularExpression::escape(childGuid)),
        QRegularExpression::CaseInsensitiveOption);
    slnText.remove(existingLineRe);
    if (parentFolderGuid.isEmpty()) {
        return;
    }
    const QString entryLine = QStringLiteral("\t\t%1 = %2\r\n").arg(childGuid, parentFolderGuid);
    const QRegularExpression sectionRe(QStringLiteral("(GlobalSection\\(NestedProjects\\)\\s*=\\s*preSolution\\r?\\n)"),
                                       QRegularExpression::CaseInsensitiveOption);
    if (const auto m = sectionRe.match(slnText); m.hasMatch()) {
        slnText.insert(m.capturedEnd(1), entryLine);
        return;
    }
    const QString newSection =
        QStringLiteral("\tGlobalSection(NestedProjects) = preSolution\r\n%1\tEndGlobalSection\r\n").arg(entryLine);
    const qsizetype endGlobal = slnText.lastIndexOf(QStringLiteral("EndGlobal"), -1, Qt::CaseInsensitive);
    if (endGlobal >= 0) {
        slnText.insert(endGlobal, newSection);
    } else {
        slnText.append(QStringLiteral("Global\r\n%1EndGlobal\r\n").arg(newSection));
    }
}

} // namespace

Result<std::string> addSolutionFolderToSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                    std::string_view parentFolderId, std::string_view folderName) {
    const QString cleanName = toQs(folderName).trimmed();
    if (cleanName.isEmpty()) {
        return fail(ErrorCode::InvalidArgument, "solution folder name cannot be empty");
    }
    auto read = fs.readFile(solutionPath);
    if (!read) {
        return std::unexpected(read.error());
    }
    QString text = toQs(*read);

    if (toQs(solutionPath).endsWith(QStringLiteral(".slnx"), Qt::CaseInsensitive)) {
        QString parentPrefix = toQs(parentFolderId).trimmed();
        parentPrefix.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (parentPrefix.isEmpty()) {
            parentPrefix = QStringLiteral("/");
        } else {
            if (!parentPrefix.startsWith(QLatin1Char('/'))) {
                parentPrefix.prepend(QLatin1Char('/'));
            }
            if (!parentPrefix.endsWith(QLatin1Char('/'))) {
                parentPrefix.append(QLatin1Char('/'));
            }
        }
        const QString fullPath = parentPrefix + cleanName + QLatin1Char('/');
        const qsizetype closeSln = text.lastIndexOf(QStringLiteral("</Solution>"), -1, Qt::CaseInsensitive);
        if (closeSln < 0) {
            return fail(ErrorCode::Corrupt, "missing </Solution> tag in .slnx");
        }
        text.insert(closeSln, QStringLiteral("  <Folder Name=\"%1\" />\n").arg(xmlEscape(fullPath)));
        if (auto w = fs.safeWrite(solutionPath, text.toStdString(), {}); !w) {
            return std::unexpected(w.error());
        }
        return fullPath.toStdString();
    }

    const QString folderGuid = QUuid::createUuid().toString(QUuid::WithBraces).toUpper();
    const QString projEntry =
        QStringLiteral("Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \"%1\", \"%1\", \"%2\"\r\nEndProject\r\n")
            .arg(cleanName, folderGuid);
    const qsizetype globalPos = text.indexOf(QStringLiteral("Global"), 0, Qt::CaseInsensitive);
    if (globalPos >= 0) {
        text.insert(globalPos, projEntry);
    } else {
        text.append(projEntry);
    }
    if (!parentFolderId.empty()) {
        upsertSlnNestedProject(text, folderGuid, toQs(parentFolderId));
    }
    if (auto w = fs.safeWrite(solutionPath, text.toStdString(), {}); !w) {
        return std::unexpected(w.error());
    }
    return folderGuid.toStdString();
}

Result<void> renameSolutionFolderInSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                std::string_view folderId, std::string_view newName) {
    const QString targetName = toQs(newName).trimmed();
    if (targetName.isEmpty() || folderId.empty()) {
        return fail(ErrorCode::InvalidArgument, "solution folder name cannot be empty");
    }
    auto read = fs.readFile(solutionPath);
    if (!read) {
        return std::unexpected(read.error());
    }
    QString text = toQs(*read);
    const QString idQs = toQs(folderId);

    if (toQs(solutionPath).endsWith(QStringLiteral(".slnx"), Qt::CaseInsensitive)) {
        QString normOld = idQs;
        normOld.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (!normOld.startsWith(QLatin1Char('/'))) {
            normOld.prepend(QLatin1Char('/'));
        }
        if (!normOld.endsWith(QLatin1Char('/'))) {
            normOld.append(QLatin1Char('/'));
        }
        QStringList segments = normOld.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (segments.isEmpty()) {
            return {};
        }
        segments.back() = targetName;
        const QString normNew = QLatin1Char('/') + segments.join(QLatin1Char('/')) + QLatin1Char('/');
        text.replace(QStringLiteral("Name=\"%1").arg(xmlEscape(normOld)),
                     QStringLiteral("Name=\"%1").arg(xmlEscape(normNew)), Qt::CaseInsensitive);
        return fs.writeFileAtomic(solutionPath, text.toStdString());
    }

    const QRegularExpression folderRe(
        QStringLiteral("(Project\\(\"\\{2150E333-8FDC-42A3-9474-1A3956D46DE8\\}\"\\)\\s*=\\s*\")[^\"]+(\",\\s*\")"
                       "[^\"]+(\",\\s*\"%1\")")
            .arg(QRegularExpression::escape(idQs)),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(folderRe, QStringLiteral("\\1%1\\2%1\\3").arg(targetName));
    return fs.writeFileAtomic(solutionPath, text.toStdString());
}

Result<void> removeSolutionFolderFromSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                  std::string_view folderId) {
    if (folderId.empty()) {
        return {};
    }
    auto read = fs.readFile(solutionPath);
    if (!read) {
        return std::unexpected(read.error());
    }
    QString text = toQs(*read);
    const QString idQs = toQs(folderId);

    if (toQs(solutionPath).endsWith(QStringLiteral(".slnx"), Qt::CaseInsensitive)) {
        const QRegularExpression selfCloseRe(QStringLiteral("[ \\t]*<Folder\\s+Name=\"%1\"\\s*/>\\r?\\n?")
                                                 .arg(QRegularExpression::escape(xmlEscape(idQs))),
                                             QRegularExpression::CaseInsensitiveOption);
        text.remove(selfCloseRe);
        const QRegularExpression openTagRe(
            QStringLiteral("[ \\t]*<Folder\\s+Name=\"%1\"\\s*>([\\s\\S]*?)</Folder>\\r?\\n?")
                .arg(QRegularExpression::escape(xmlEscape(idQs))),
            QRegularExpression::CaseInsensitiveOption);
        text.replace(openTagRe, QStringLiteral("\\1"));
        return fs.writeFileAtomic(solutionPath, text.toStdString());
    }

    const QRegularExpression folderBlockRe(
        QStringLiteral("Project\\(\"\\{2150E333-8FDC-42A3-9474-1A3956D46DE8\\}\"\\)\\s*=\\s*\"[^\"]+\",\\s*\"[^\"]+\","
                       "\\s*\"%1\"[\\s\\S]*?EndProject\\r?\\n?")
            .arg(QRegularExpression::escape(idQs)),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(folderBlockRe);

    const QRegularExpression nestedLineRe(
        QStringLiteral("[ \\t]*(?:%1\\s*=\\s*\\{[^\\}]+\\}|\\{[^\\}]+\\}\\s*=\\s*%1)\\r?\\n?")
            .arg(QRegularExpression::escape(idQs)),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(nestedLineRe);
    return fs.writeFileAtomic(solutionPath, text.toStdString());
}

Result<void> moveProjectToSolutionFolderInSolutionFile(io::IFileSystem& fs, std::string_view solutionPath,
                                                       std::string_view projectPath, std::string_view targetFolderId) {
    auto read = fs.readFile(solutionPath);
    if (!read) {
        return std::unexpected(read.error());
    }
    QString text = toQs(*read);
    const QFileInfo slnInfo(toQs(solutionPath));
    const QFileInfo projInfo(toQs(projectPath));
    const QString relNative = QDir::toNativeSeparators(slnInfo.dir().relativeFilePath(projInfo.absoluteFilePath()));
    QString relSlash = relNative;
    relSlash.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (toQs(solutionPath).endsWith(QStringLiteral(".slnx"), Qt::CaseInsensitive)) {
        const QRegularExpression projRe(
            QStringLiteral("[ \\t]*<Project\\s+Path=\"(?:%1|%2)\"[^>]*(?:/>|>[\\s\\S]*?</Project>)\\r?\\n?")
                .arg(QRegularExpression::escape(relNative), QRegularExpression::escape(relSlash)),
            QRegularExpression::CaseInsensitiveOption);
        text.remove(projRe);

        if (targetFolderId.empty()) {
            const qsizetype closeSln = text.lastIndexOf(QStringLiteral("</Solution>"), -1, Qt::CaseInsensitive);
            if (closeSln >= 0) {
                text.insert(closeSln, QStringLiteral("  <Project Path=\"%1\" />\n").arg(xmlEscape(relSlash)));
            }
            return fs.writeFileAtomic(solutionPath, text.toStdString());
        }

        const QString folderQs = toQs(targetFolderId);
        const QRegularExpression selfCloseFolderRe(QStringLiteral("([ \\t]*)<Folder\\s+Name=\"%1\"\\s*/>")
                                                       .arg(QRegularExpression::escape(xmlEscape(folderQs))),
                                                   QRegularExpression::CaseInsensitiveOption);
        if (const auto m = selfCloseFolderRe.match(text); m.hasMatch()) {
            const QString indent = m.captured(1);
            const QString expanded = QStringLiteral("%1<Folder Name=\"%2\">\n%1  <Project Path=\"%3\" />\n%1</Folder>")
                                         .arg(indent, xmlEscape(folderQs), xmlEscape(relSlash));
            text.replace(m.capturedStart(0), m.capturedLength(0), expanded);
            return fs.writeFileAtomic(solutionPath, text.toStdString());
        }
        const QRegularExpression openFolderRe(
            QStringLiteral("<Folder\\s+Name=\"%1\"\\s*>").arg(QRegularExpression::escape(xmlEscape(folderQs))),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = openFolderRe.match(text); m.hasMatch()) {
            text.insert(m.capturedEnd(0), QStringLiteral("\n    <Project Path=\"%1\" />").arg(xmlEscape(relSlash)));
            return fs.writeFileAtomic(solutionPath, text.toStdString());
        }
        return fs.writeFileAtomic(solutionPath, text.toStdString());
    }

    const QRegularExpression projGuidRe(
        QStringLiteral("Project\\(\"[^\"]+\"\\)\\s*=\\s*\"[^\"]+\",\\s*\"(?:%1|%2)\",\\s*\"(\\{[^\\}]+\\})\"")
            .arg(QRegularExpression::escape(relNative), QRegularExpression::escape(relSlash)),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = projGuidRe.match(text);
    if (!m.hasMatch()) {
        return fail(ErrorCode::NotFound, "project entry not found in .sln");
    }
    const QString projGuid = m.captured(1);
    upsertSlnNestedProject(text, projGuid, toQs(targetFolderId));
    return fs.writeFileAtomic(solutionPath, text.toStdString());
}

} // namespace frost::app
