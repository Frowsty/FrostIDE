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

} // namespace frost::app
