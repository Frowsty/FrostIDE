// SPDX-License-Identifier: Apache-2.0
//
// Screenshot golden helpers (golden.hpp).

#include "support/golden.hpp"

#include "support/appearance_fixture.hpp"
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <format>

namespace frost::ui::test {

QString goldenOs() {
#ifdef _WIN32
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

QImage renderAt(QWidget& widget, qreal dpr) {
    const QSize device(qRound(widget.width() * dpr), qRound(widget.height() * dpr));
    QImage image(device, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

bool Comparison::passes() const {
    return sizesMatch && (maxChannelDiff <= kChannelTolerance || pixelsOverTolerance <= kMaxOutlierPixels);
}

std::string Comparison::describe() const {
    if (!sizesMatch) {
        return "the sizes differ";
    }
    return std::format("max channel difference {}, {} pixel(s) over {}", maxChannelDiff, pixelsOverTolerance,
                       kChannelTolerance);
}

Comparison compareImages(const QImage& actual, const QImage& expected) {
    Comparison result;
    result.sizesMatch = actual.size() == expected.size();
    if (!result.sizesMatch) {
        return result;
    }
    const QImage a = actual.convertToFormat(QImage::Format_ARGB32);
    const QImage b = expected.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y) {
        const auto* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const int diff = std::max({std::abs(qRed(ra[x]) - qRed(rb[x])), std::abs(qGreen(ra[x]) - qGreen(rb[x])),
                                       std::abs(qBlue(ra[x]) - qBlue(rb[x])), std::abs(qAlpha(ra[x]) - qAlpha(rb[x]))});
            result.maxChannelDiff = std::max(result.maxChannelDiff, diff);
            result.pixelsOverTolerance += diff > kChannelTolerance ? 1 : 0;
        }
    }
    return result;
}

namespace {

QString goldenPath(const QString& file) {
    return QDir(QString::fromStdString(sourceDir()))
        .filePath(QStringLiteral("tests/ui/goldens/") + goldenOs() + "/" + file);
}

QImage diffImage(const QImage& actual, const QImage& expected) {
    QImage diff(actual.size(), QImage::Format_ARGB32);
    const QImage a = actual.convertToFormat(QImage::Format_ARGB32);
    const QImage b = expected.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < diff.height(); ++y) {
        for (int x = 0; x < diff.width(); ++x) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            const int d = std::max({std::abs(qRed(pa) - qRed(pb)), std::abs(qGreen(pa) - qGreen(pb)),
                                    std::abs(qBlue(pa) - qBlue(pb)), std::abs(qAlpha(pa) - qAlpha(pb))});
            diff.setPixel(
                x, y, d > kChannelTolerance ? qRgb(255, 0, 255) : qRgb(qGray(pa) / 3, qGray(pa) / 3, qGray(pa) / 3));
        }
    }
    return diff;
}

} // namespace

QImage loadGolden(const QString& file) {
    QImage golden(goldenPath(file));
    return golden;
}

void expectMatchesGolden(const QImage& actual, const QString& file) {
    const QString path = goldenPath(file);
    const QImage golden(path);
    const bool update = qEnvironmentVariable("FROST_UPDATE_GOLDENS") == QStringLiteral("1");
    const Comparison comparison = golden.isNull() ? Comparison{} : compareImages(actual, golden);
    if (!golden.isNull() && comparison.passes()) {
        return;
    }
    if (update) {
        QString dir = qEnvironmentVariable("FROST_GOLDEN_DIR");
        if (dir.isEmpty()) {
            dir = QFileInfo(path).absolutePath();
        }
        QDir().mkpath(dir);
        QImage copy = actual.convertToFormat(QImage::Format_ARGB32);
        copy.setDevicePixelRatio(1.0);
        const QString target = QDir(dir).filePath(file);
        ASSERT_TRUE(copy.save(target, "PNG")) << target.toStdString();
        ::testing::Test::RecordProperty("golden-updated", target.toStdString());
        FAIL() << "goldens updated (" << target.toStdString() << "); re-run without FROST_UPDATE_GOLDENS";
    }
    if (golden.isNull()) {
        FAIL() << "missing golden " << path.toStdString() << "; run with FROST_UPDATE_GOLDENS=1";
    }
    const QString out = QDir::current().filePath(QStringLiteral("ui-screenshots/") + goldenOs());
    QDir().mkpath(out);
    const QString stem = QFileInfo(file).completeBaseName();
    QImage copy = actual.convertToFormat(QImage::Format_ARGB32);
    copy.setDevicePixelRatio(1.0);
    copy.save(QDir(out).filePath(stem + ".actual.png"), "PNG");
    if (comparison.sizesMatch) {
        diffImage(actual, golden).save(QDir(out).filePath(stem + ".diff.png"), "PNG");
    }
    ADD_FAILURE() << file.toStdString() << " differs from its golden: " << comparison.describe() << "; see "
                  << out.toStdString();
}

} // namespace frost::ui::test
