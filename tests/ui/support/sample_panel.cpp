// SPDX-License-Identifier: Apache-2.0
//
// SamplePanel and StateGallery (sample_panel.hpp).

#include "support/sample_panel.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStyleOption>
#include <QTabBar>
#include <QVBoxLayout>

namespace frost::ui::test {

StateGallery::StateGallery(QWidget* parent) : QWidget(parent) { setFixedHeight(28); }

QRect StateGallery::cell(int index) const {
    const int width = this->width() / 4;
    return {index * width + 2, 2, width - 4, height() - 4};
}

void StateGallery::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QStyle::StateFlag states[] = {QStyle::State_MouseOver, QStyle::State_Sunken, QStyle::State_HasFocus};
    for (int i = 0; i < 3; ++i) {
        QStyleOptionButton option;
        option.initFrom(this);
        option.rect = cell(i);
        option.state = QStyle::State_Enabled | QStyle::State_Raised | states[i];
        style()->drawPrimitive(QStyle::PE_PanelButtonCommand, &option, &painter, this);
    }
    QStyleOptionFrame frame;
    frame.initFrom(this);
    frame.rect = cell(3);
    frame.state = QStyle::State_Enabled | QStyle::State_HasFocus;
    frame.lineWidth = 1;
    style()->drawPrimitive(QStyle::PE_PanelLineEdit, &frame, &painter, this);
}

SamplePanel makeSamplePanel(const QString& monoFamily) {
    SamplePanel panel;
    const FrostStyle* style = FrostStyle::current();
    panel.root = std::make_unique<IslandHost>(QBoxLayout::LeftToRight);
    panel.root->setContentsMargins(0, kTopBand, 0, 0);
    panel.left = new IslandFrame;
    panel.right = new IslandFrame;
    panel.root->addIsland(panel.left, 3);
    panel.root->addIsland(panel.right, 2);

    // Left island: buttons, text fields, check boxes, radios, a tab bar and labels.
    auto* left = new QVBoxLayout(panel.left);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(new QPushButton(QStringLiteral("Button")));
    panel.defaultButton = new QPushButton(QStringLiteral("Default"));
    panel.defaultButton->setDefault(true);
    buttons->addWidget(panel.defaultButton);
    auto* off = new QPushButton(QStringLiteral("Off"));
    off->setEnabled(false);
    buttons->addWidget(off);
    left->addLayout(buttons);
    panel.text = new QLineEdit(QStringLiteral("Glacier text"));
    left->addWidget(panel.text);
    auto* fields = new QHBoxLayout;
    auto* placeholder = new QLineEdit;
    placeholder->setPlaceholderText(QStringLiteral("Search"));
    fields->addWidget(placeholder);
    auto* combo = new QComboBox;
    combo->addItems({QStringLiteral("Debug"), QStringLiteral("Release")});
    fields->addWidget(combo);
    left->addLayout(fields);
    auto* checks = new QHBoxLayout;
    panel.checked = new QCheckBox(QStringLiteral("On"));
    panel.checked->setChecked(true);
    checks->addWidget(panel.checked);
    checks->addWidget(new QCheckBox(QStringLiteral("Off")));
    panel.checkedRadio = new QRadioButton(QStringLiteral("A"));
    panel.checkedRadio->setChecked(true);
    checks->addWidget(panel.checkedRadio);
    checks->addWidget(new QRadioButton(QStringLiteral("B")));
    left->addLayout(checks);
    auto* tabs = new QTabBar;
    tabs->addTab(QStringLiteral("main.cpp"));
    tabs->addTab(QStringLiteral("style.cpp"));
    tabs->setDrawBase(false);
    left->addWidget(tabs);
    auto* labels = new QHBoxLayout;
    panel.label = new QLabel(QStringLiteral("Primary"));
    labels->addWidget(panel.label);
    auto* secondary = new QLabel(QStringLiteral("Secondary"));
    if (style != nullptr) {
        QPalette palette = secondary->palette();
        palette.setColor(QPalette::WindowText, style->color(ColorKey::LabelSecondaryForeground));
        secondary->setPalette(palette);
    }
    labels->addWidget(secondary);
    auto* disabled = new QLabel(QStringLiteral("Disabled"));
    disabled->setEnabled(false);
    labels->addWidget(disabled);
    left->addLayout(labels);
    panel.monoLabel = new QLabel(QStringLiteral("int main() { return 0; }"));
    QFont mono(monoFamily);
    mono.setPointSizeF(QApplication::font().pointSizeF());
    mono.setHintingPreference(QFont::PreferNoHinting);
    mono.setStyleStrategy(QFont::NoSubpixelAntialias);
    panel.monoLabel->setFont(mono);
    left->addWidget(panel.monoLabel);
    left->addStretch(1);

    // Right island: tool buttons, a tree with a scroll bar, and the state gallery.
    auto* right = new QVBoxLayout(panel.right);
    auto* tools = new QHBoxLayout;
    const std::string_view toolIcons[] = {icons::kActionsSearch, icons::kActionsRefresh, icons::kRunRun,
                                          icons::kRunStop};
    for (const std::string_view id : toolIcons) {
        auto* tool = new QToolButton;
        tool->setAutoRaise(true);
        if (style != nullptr) {
            tool->setIcon(style->icon(id, IconSlot::Toolbar));
            const int px = style->metricPx(MetricKey::ToolbarIconSize);
            tool->setIconSize(QSize(px, px));
        }
        tools->addWidget(tool);
        panel.disabledTool = tool;
    }
    panel.disabledTool->setEnabled(false);
    tools->addStretch(1);
    right->addLayout(tools);
    auto* treeRow = new QHBoxLayout;
    auto* model = new QStandardItemModel(panel.root.get());
    const std::pair<std::string_view, const char*> rows[] = {{icons::kNodesFolderOpen, "src"},
                                                             {icons::kNodesFolder, "tests"},
                                                             {icons::kNodesFileCode, "main.cpp"},
                                                             {icons::kNodesFile, "README"}};
    for (const auto& [id, text] : rows) {
        auto* item = new QStandardItem(QString::fromLatin1(text));
        if (style != nullptr) {
            item->setIcon(style->icon(id));
        }
        model->appendRow(item);
    }
    panel.tree = new QTreeView;
    panel.tree->setModel(model);
    panel.tree->setHeaderHidden(true);
    panel.tree->setRootIsDecorated(false);
    panel.tree->setFrameShape(QFrame::NoFrame);
    panel.tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panel.tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panel.tree->setFocusPolicy(Qt::NoFocus);
    panel.tree->selectionModel()->select(model->index(1, 0), QItemSelectionModel::Select);
    treeRow->addWidget(panel.tree, 1);
    auto* scroll = new QScrollBar(Qt::Vertical);
    scroll->setRange(0, 100);
    scroll->setPageStep(20);
    scroll->setValue(30);
    treeRow->addWidget(scroll);
    right->addLayout(treeRow, 1);
    panel.gallery = new StateGallery;
    right->addWidget(panel.gallery);

    // No widget holds focus; nothing blinks.
    for (QWidget* child : panel.root->findChildren<QWidget*>()) {
        child->setFocusPolicy(Qt::NoFocus);
    }
    QApplication::setCursorFlashTime(0);
    /*
     * On the offscreen platform (no desktop window can appear) the panel is shown for real and
     * activated, so the goldens draw the Active palette group a user normally sees. On any other
     * platform it stays WA_DontShowOnScreen, never activates, and draws the Inactive group.
     */
    const bool offscreen = QGuiApplication::platformName() == QStringLiteral("offscreen");
    panel.root->setAttribute(Qt::WA_DontShowOnScreen, !offscreen);
    panel.root->resize(kPanelWidth, kPanelHeight);
    panel.root->show();
    if (offscreen) {
        panel.root->activateWindow();
        QElapsedTimer timer;
        timer.start();
        while (!panel.root->isActiveWindow() && timer.elapsed() < 2000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }
    QApplication::processEvents();
    return panel;
}

} // namespace frost::ui::test
