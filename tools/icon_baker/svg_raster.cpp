// SPDX-License-Identifier: Apache-2.0
//
// SVG preparation and rasterisation for frost-icon-baker (svg_raster.hpp).

#include "svg_raster.hpp"

#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>
#include <QSvgRenderer>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtSvg/qtsvgglobal.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>

namespace frost::tools::baker {

namespace {

// The message sink: a plain pointer (no global constructor), set once by main() before any
// rendering and only touched on the GUI thread.
QStringList* g_svgMessages = nullptr;

void messageHandler(QtMsgType, const QMessageLogContext& context, const QString& message) {
    const QByteArray text = message.toLocal8Bit();
    std::fprintf(stderr, "frost-icon-baker: qt: [%s] %s\n", context.category != nullptr ? context.category : "",
                 text.constData());
    if (g_svgMessages != nullptr && context.category != nullptr && std::strncmp(context.category, "qt.svg", 6) == 0) {
        g_svgMessages->push_back(message);
    }
}

Result<QByteArray> invalid(std::string message) { return fail(ErrorCode::InvalidArgument, std::move(message)); }

// The size of a square viewBox ("0 0 24 24"), or nullopt.
std::optional<double> squareViewBox(const QString& viewBox) {
    const QStringList parts =
        viewBox.simplified().replace(QLatin1Char(','), QLatin1Char(' ')).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 4) {
        return std::nullopt;
    }
    bool okW = false;
    bool okH = false;
    const double w = parts[2].toDouble(&okW);
    const double h = parts[3].toDouble(&okH);
    if (!okW || !okH || !(w > 0) || w != h) {
        return std::nullopt;
    }
    return w;
}

} // namespace

void installSvgMessageHandler(QStringList* sink) noexcept {
    g_svgMessages = sink;
    qInstallMessageHandler(messageHandler);
}

Result<QByteArray> prepareSvg(const QByteArray& svg, std::optional<double> strokePx, std::uint8_t designSize,
                              bool tint) {
    QXmlStreamReader reader(svg);
    QByteArray out;
    QXmlStreamWriter writer(&out);
    int depth = 0;
    bool sawRoot = false;
    while (!reader.atEnd()) {
        switch (reader.readNext()) {
        case QXmlStreamReader::StartDocument: writer.writeStartDocument(QStringLiteral("1.0")); break;
        case QXmlStreamReader::EndDocument: writer.writeEndDocument(); break;
        case QXmlStreamReader::DTD: return invalid("DTDs are not allowed in icon sources");
        case QXmlStreamReader::EntityReference: return invalid("entity references are not allowed in icon sources");
        case QXmlStreamReader::StartElement: {
            const QString name = reader.name().toString();
            if (depth == 0) {
                if (sawRoot || name != QLatin1String("svg")) {
                    return invalid("the root element must be <svg>");
                }
                sawRoot = true;
            }
            if (name == QLatin1String("text") || name == QLatin1String("tspan") || name == QLatin1String("textArea")) {
                return invalid("<text> is not allowed in icon sources (fonts are not baked)");
            }
            writer.writeStartElement(reader.qualifiedName().toString());
            for (const QXmlStreamNamespaceDeclaration& ns : reader.namespaceDeclarations()) {
                writer.writeAttribute(ns.prefix().isEmpty() ? QStringLiteral("xmlns")
                                                            : QStringLiteral("xmlns:") + ns.prefix().toString(),
                                      ns.namespaceUri().toString());
            }
            const QXmlStreamAttributes attributes = reader.attributes();
            double viewBoxSize = 0;
            if (depth == 0) {
                const auto size = squareViewBox(attributes.value(QLatin1String("viewBox")).toString());
                if (!size) {
                    return invalid("the root <svg> needs a square viewBox \"x y size size\"");
                }
                viewBoxSize = *size;
                if (strokePx && !attributes.hasAttribute(QLatin1String("stroke-width"))) {
                    return invalid("strokePx is set but the root <svg> declares no stroke-width");
                }
            } else if (strokePx && attributes.hasAttribute(QLatin1String("stroke-width"))) {
                return invalid(
                    std::format("<{}> sets its own stroke-width, which strokePx cannot normalise", name.toStdString()));
            }
            for (const QXmlStreamAttribute& attribute : attributes) {
                const QString attributeName = attribute.qualifiedName().toString();
                QString value = attribute.value().toString();
                if (depth == 0 && strokePx && attributeName == QLatin1String("stroke-width")) {
                    // Lucide's absoluteStrokeWidth: strokeWidth * 24 / size for its 24-unit grid.
                    value = QString::number(*strokePx * viewBoxSize / designSize, 'g', 9);
                }
                if (tint && value.trimmed() == QLatin1String("currentColor")) {
                    value = QStringLiteral("#000000");
                }
                writer.writeAttribute(attributeName, value);
            }
            ++depth;
            break;
        }
        case QXmlStreamReader::EndElement:
            writer.writeEndElement();
            --depth;
            break;
        case QXmlStreamReader::Characters:
            if (!reader.isWhitespace()) {
                writer.writeCharacters(reader.text().toString());
            }
            break;
        case QXmlStreamReader::Comment:
        case QXmlStreamReader::ProcessingInstruction:
        case QXmlStreamReader::NoToken:
        case QXmlStreamReader::Invalid: break;
        }
    }
    if (reader.hasError()) {
        return invalid(
            std::format("malformed SVG at line {}: {}", reader.lineNumber(), reader.errorString().toStdString()));
    }
    if (!sawRoot) {
        return invalid("no <svg> element");
    }
    return out;
}

Result<ui::AtlasCell> rasterizeSvg(const QByteArray& svg, std::uint16_t devicePx, bool alphaMask) {
    if (g_svgMessages != nullptr) {
        g_svgMessages->clear();
    }
    QSvgRenderer renderer;
    renderer.setOptions(QtSvg::Tiny12FeaturesOnly | QtSvg::DisableSMILAnimations | QtSvg::DisableCSSAnimations);
    const bool loaded = renderer.load(svg);
    if (!loaded || !renderer.isValid()) {
        return fail(ErrorCode::InvalidArgument, "QSvgRenderer cannot load the SVG");
    }
    QImage image(devicePx, devicePx, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, devicePx, devicePx));
    }
    if (g_svgMessages != nullptr && !g_svgMessages->isEmpty()) {
        return fail(ErrorCode::InvalidArgument,
                    "Qt Svg reported: " + g_svgMessages->join(QStringLiteral("; ")).toStdString());
    }
    if (alphaMask) {
        image = image.convertToFormat(QImage::Format_Alpha8);
    }
    const std::size_t rowBytes = std::size_t{devicePx} * (alphaMask ? 1U : 4U);
    ui::AtlasCell cell{devicePx, devicePx, {}};
    cell.pixels.resize(rowBytes * devicePx);
    for (int y = 0; y < devicePx; ++y) {
        std::memcpy(cell.pixels.data() + static_cast<std::size_t>(y) * rowBytes, image.constScanLine(y), rowBytes);
    }
    return cell;
}

} // namespace frost::tools::baker
