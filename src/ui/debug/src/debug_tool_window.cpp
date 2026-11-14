// SPDX-License-Identifier: Apache-2.0
//
// DebugToolWindow and EvaluateDialog (see frost/ui/debug_tool_window.hpp): the widgets, the toolbar's
// enabled states and the wiring between the views, the models and the controller.

#include "frost/ui/debug_tool_window.hpp"

#include "frost/core/log.hpp"
#include "frost/debug/msvc_throw.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_models.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/run_console.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <charconv>
#include <format>

namespace frost::ui {

namespace {

constexpr int kRoleBreakpointId = Qt::UserRole;
// The Breakpoints tab's rows: what a row is, and an exception row's filter id.
constexpr int kRoleBreakpointKind = Qt::UserRole + 1;
constexpr int kRoleExceptionFilter = Qt::UserRole + 2;
enum class BreakpointRow : int { Line = 0, Function = 1, Exception = 2, Note = 3 };

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

QString formatHexDump(const debug::dap::ReadMemoryResult& mem) {
    std::uint64_t baseAddr = 0;
    std::string_view addrStr = mem.address;
    if (addrStr.starts_with("0x") || addrStr.starts_with("0X")) {
        addrStr.remove_prefix(2);
    }
    std::from_chars(addrStr.data(), addrStr.data() + addrStr.size(), baseAddr, 16);

    const std::string& bytes = mem.decodedBytes;
    if (bytes.empty()) {
        return QStringLiteral("(0 bytes read at %1)").arg(qs(mem.address));
    }
    std::string out;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
        out += std::format("0x{:016X} | ", baseAddr + offset);
        std::string ascii;
        for (std::size_t i = 0; i < 16; ++i) {
            if (offset + i < bytes.size()) {
                const auto b = static_cast<unsigned char>(bytes[offset + i]);
                out += std::format("{:02X} ", b);
                ascii.push_back((b >= 0x20U && b <= 0x7EU) ? static_cast<char>(b) : '.');
            } else {
                out += "   ";
                ascii.push_back(' ');
            }
        }
        out += std::format("| {}\n", ascii);
    }
    return qs(out);
}

// A tree view for variables: three columns, lazy children, Copy Value / Set Value / Remove Watch in context menu.
QTreeView* makeVariablesView(QWidget* parent, VariablesModel* model, DebugToolWindow* owner = nullptr,
                             bool isWatchesView = false) {
    auto* view = new QTreeView(parent);
    view->setModel(model);
    view->setUniformRowHeights(true);
    view->setAlternatingRowColors(false);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->header()->setStretchLastSection(true);
    view->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    view->setColumnWidth(0, 180);
    view->setColumnWidth(1, 220);
    QObject::connect(view, &QTreeView::activated, model, [model](const QModelIndex& index) {
        if (model->isLoadMoreRow(index)) {
            model->loadMore(index);
        }
    });
    QObject::connect(view, &QTreeView::clicked, model, [model](const QModelIndex& index) {
        if (model->isLoadMoreRow(index)) {
            model->loadMore(index);
        }
    });
    QObject::connect(
        view, &QWidget::customContextMenuRequested, view, [view, model, owner, isWatchesView](const QPoint& at) {
            const QModelIndex index = view->indexAt(at);
            if (!index.isValid()) {
                return;
            }
            const QString value = model->valueAt(index.siblingAtColumn(1));
            QMenu menu(view);
            menu.addAction(QStringLiteral("Copy Value"), view, [value] { QApplication::clipboard()->setText(value); });
            if (owner != nullptr && model->parentReferenceAt(index) > 0 && model->isVariableRow(index)) {
                QAction* setVal = menu.addAction(QStringLiteral("Set Value..."));
                setVal->setShortcut(QKeySequence(Qt::Key_F2));
                QObject::connect(setVal, &QAction::triggered, owner,
                                 [owner, view, model, index] { owner->promptSetVariableValue(view, model, index); });
            }
            if (isWatchesView && model->isWatchRoot(index)) {
                const int row = index.row();
                QAction* rmWatch = menu.addAction(QStringLiteral("Remove Watch"));
                rmWatch->setShortcut(QKeySequence(Qt::Key_Delete));
                QObject::connect(rmWatch, &QAction::triggered, model, [model, row] { model->removeWatch(row); });
            }
            menu.exec(view->viewport()->mapToGlobal(at));
        });
    if (owner != nullptr) {
        auto* f2Shortcut = new QShortcut(QKeySequence(Qt::Key_F2), view);
        f2Shortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(f2Shortcut, &QShortcut::activated, owner, [owner, view, model] {
            const QModelIndex current = view->currentIndex();
            if (current.isValid() && model->parentReferenceAt(current) > 0 && model->isVariableRow(current)) {
                owner->promptSetVariableValue(view, model, current);
            }
        });
    }
    if (isWatchesView) {
        auto* delShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), view);
        delShortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(delShortcut, &QShortcut::activated, model, [view, model] {
            const QModelIndex current = view->currentIndex();
            if (current.isValid() && model->isWatchRoot(current)) {
                model->removeWatch(current.row());
            }
        });
    }
    return view;
}

} // namespace

DebugToolWindow::DebugToolWindow(DebugController& controller, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_reason(new QLabel(this)), m_tabs(new QTabWidget(this)),
      m_console(new RunConsole(m_tabs)) {
    setObjectName(QStringLiteral("debugToolWindow"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    buildToolbar(layout);
    m_reason->setObjectName(QStringLiteral("debugReason"));
    m_reason->setContentsMargins(8, 2, 8, 2);
    m_reason->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_reason);
    m_tabs->setDocumentMode(true);
    layout->addWidget(m_tabs, 1);

    buildThreadsAndVariablesTab();
    m_tabs->addTab(m_console, QStringLiteral("Console"));
    m_controller.setConsole(m_console);

    buildBreakpointsTab();
    buildMemoryAndDisassemblyTab();
    wireControllerSignals();
    updateBreakpointsList();
    updateActions();
}

DebugToolWindow::~DebugToolWindow() = default;

void DebugToolWindow::buildToolbar(QVBoxLayout* layout) {
    m_toolbar = new QToolBar(this);
    m_toolbar->setIconSize(QSize(16, 16));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    const auto add = [this](const QString& text, const QString& shortcut, auto slot) {
        QAction* action = m_toolbar->addAction(text);
        action->setToolTip(shortcut.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, shortcut));
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    m_resume = add(QStringLiteral("Resume"), QStringLiteral("F9"), [this] { m_controller.resume(); });
    m_pause = add(QStringLiteral("Pause"), QString(), [this] { m_controller.pause(); });
    m_stop = add(QStringLiteral("Stop"), QStringLiteral("Ctrl+F2"), [this] { m_controller.stop(); });
    m_toolbar->addSeparator();
    m_stepOver = add(QStringLiteral("Step Over"), QStringLiteral("F8"), [this] { m_controller.stepOver(); });
    m_stepInto = add(QStringLiteral("Step Into"), QStringLiteral("F7"), [this] { m_controller.stepInto(); });
    m_stepOut = add(QStringLiteral("Step Out"), QStringLiteral("Shift+F8"), [this] { m_controller.stepOut(); });
    m_runToCursor =
        add(QStringLiteral("Run to Cursor"), QStringLiteral("Alt+F9"), [this] { m_controller.runToCursor(); });
    m_toolbar->addSeparator();
    m_evaluate = add(QStringLiteral("Evaluate Expression"), QStringLiteral("Alt+F8"), [this] { openEvaluate(); });
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        m_resume->setIcon(style->icon(icons::kRunRun, IconSlot::Small, IconRole::Success));
        m_stop->setIcon(style->icon(icons::kRunStop, IconSlot::Small, IconRole::Error));
        m_stepOver->setIcon(style->icon(icons::kNavForward));
        m_stepInto->setIcon(style->icon(icons::kNavChevronDown));
        m_stepOut->setIcon(style->icon(icons::kNavChevronUp));
        m_runToCursor->setIcon(style->icon(icons::kNavChevronRight));
        m_evaluate->setIcon(style->icon(icons::kActionsSearch));
    }
    layout->addWidget(m_toolbar);
}

void DebugToolWindow::buildThreadsAndVariablesTab() {
    auto* splitter = new QSplitter(Qt::Horizontal, m_tabs);
    auto* framesPane = new QWidget(splitter);
    auto* framesLayout = new QVBoxLayout(framesPane);
    framesLayout->setContentsMargins(0, 0, 0, 0);
    m_threads = new QComboBox(framesPane);
    framesLayout->addWidget(m_threads);
    m_frames = new FramesModel(m_controller, this);
    m_framesView = new QListView(framesPane);
    m_framesView->setModel(m_frames);
    m_framesView->setUniformItemSizes(true);
    framesLayout->addWidget(m_framesView, 1);
    m_loadedLabel = new QLabel(framesPane);
    m_loadedLabel->setObjectName(QStringLiteral("debugLoadedSourceLabel"));
    m_loadedLabel->hide();
    framesLayout->addWidget(m_loadedLabel);
    m_loadedSource = new QPlainTextEdit(framesPane);
    m_loadedSource->setObjectName(QStringLiteral("debugLoadedSource"));
    m_loadedSource->setReadOnly(true);
    m_loadedSource->hide();
    framesLayout->addWidget(m_loadedSource, 2);

    m_variables = new VariablesModel(m_controller, VariablesModel::Mode::Scopes, this);
    m_variablesView = makeVariablesView(splitter, m_variables, this, false);

    auto* watchesPane = new QWidget(splitter);
    auto* watchesLayout = new QVBoxLayout(watchesPane);
    watchesLayout->setContentsMargins(0, 0, 0, 0);
    auto* watchRow = new QHBoxLayout();
    m_watchInput = new QLineEdit(watchesPane);
    m_watchInput->setPlaceholderText(QStringLiteral("Add watch: an expression, then Enter"));
    auto* removeWatch = new QToolButton(watchesPane);
    removeWatch->setText(QStringLiteral("Remove"));
    removeWatch->setToolTip(QStringLiteral("Remove the selected watch"));
    watchRow->addWidget(m_watchInput, 1);
    watchRow->addWidget(removeWatch);
    watchesLayout->addLayout(watchRow);
    m_watches = new VariablesModel(m_controller, VariablesModel::Mode::Watches, this);
    m_watchesView = makeVariablesView(watchesPane, m_watches, this, true);
    watchesLayout->addWidget(m_watchesView, 1);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 2);
    m_tabs->addTab(splitter, QStringLiteral("Threads && Variables"));

    connect(m_watchInput, &QLineEdit::returnPressed, this, [this] {
        m_watches->addWatch(m_watchInput->text());
        m_watchInput->clear();
    });
    connect(removeWatch, &QToolButton::clicked, this, [this] {
        const QModelIndex current = m_watchesView->currentIndex();
        if (current.isValid() && !current.parent().isValid()) {
            m_watches->removeWatch(current.row());
        }
    });
    connect(m_framesView, &QListView::clicked, this,
            [this](const QModelIndex& index) { onFrameActivated(index.row()); });
    connect(m_framesView, &QListView::activated, this,
            [this](const QModelIndex& index) { onFrameActivated(index.row()); });
    connect(m_threads, &QComboBox::activated, this, [this](int index) {
        if (!m_fillingThreads && index >= 0) {
            m_controller.selectThread(m_threads->itemData(index).toLongLong());
        }
    });
}

void DebugToolWindow::buildBreakpointsTab() {
    auto* bpPane = new QWidget(m_tabs);
    auto* bpLayout = new QVBoxLayout(bpPane);
    bpLayout->setContentsMargins(6, 6, 6, 6);
    bpLayout->setSpacing(6);

    auto* bpControls = new QHBoxLayout();
    m_muteBreakpointsButton = new QPushButton(QStringLiteral("Mute All Breakpoints"), bpPane);
    m_muteBreakpointsButton->setObjectName(QStringLiteral("debugMuteBreakpointsButton"));
    m_muteBreakpointsButton->setCheckable(true);
    m_muteBreakpointsButton->setChecked(m_controller.breakpointsMuted());

    m_addFunctionBreakpointButton = new QPushButton(QStringLiteral("Add Function Breakpoint..."), bpPane);
    m_addFunctionBreakpointButton->setObjectName(QStringLiteral("debugAddFunctionBreakpointButton"));

    m_editBreakpointButton = new QPushButton(QStringLiteral("Edit Breakpoint..."), bpPane);
    m_editBreakpointButton->setObjectName(QStringLiteral("debugEditBreakpointButton"));
    m_removeBreakpointButton = new QPushButton(QStringLiteral("Remove Breakpoint"), bpPane);
    m_removeBreakpointButton->setObjectName(QStringLiteral("debugRemoveBreakpointButton"));

    bpControls->addWidget(m_muteBreakpointsButton);
    bpControls->addWidget(m_addFunctionBreakpointButton);
    bpControls->addStretch(1);
    bpControls->addWidget(m_editBreakpointButton);
    bpControls->addWidget(m_removeBreakpointButton);
    bpLayout->addLayout(bpControls);

    m_breakpointsTree = new QTreeWidget(bpPane);
    m_breakpointsTree->setObjectName(QStringLiteral("debugBreakpointsTree"));
    m_breakpointsTree->setRootIsDecorated(false);
    m_breakpointsTree->setUniformRowHeights(true);
    m_breakpointsTree->setAlternatingRowColors(true);
    m_breakpointsTree->setHeaderLabels({QStringLiteral("Enabled"), QStringLiteral("File:Line"),
                                        QStringLiteral("Condition"), QStringLiteral("Hit Count"),
                                        QStringLiteral("Log Message (Tracepoint)")});
    m_breakpointsTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_breakpointsTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_breakpointsTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_breakpointsTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_breakpointsTree->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    bpLayout->addWidget(m_breakpointsTree, 1);

    m_tabs->addTab(bpPane, QStringLiteral("Breakpoints"));

    connect(m_muteBreakpointsButton, &QPushButton::toggled, this,
            [this](bool checked) { m_controller.setMutedBreakpoints(checked); });
    connect(m_addFunctionBreakpointButton, &QPushButton::clicked, this, [this] { openAddFunctionBreakpoint(); });
    connect(m_editBreakpointButton, &QPushButton::clicked, this, [this] { openEditBreakpoint(0); });
    connect(m_removeBreakpointButton, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem* current = m_breakpointsTree->currentItem()) {
            const auto id = static_cast<debug::BreakpointId>(current->data(0, kRoleBreakpointId).toULongLong());
            m_controller.breakpoints().remove(id);
        }
    });
    connect(m_breakpointsTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_fillingBreakpoints || item == nullptr || column != 0) {
            return;
        }
        const auto kind = static_cast<BreakpointRow>(item->data(0, kRoleBreakpointKind).toInt());
        const bool checked = item->checkState(0) == Qt::Checked;
        // The change rebuilds the list; inside this signal that would delete `item` under Qt.
        m_editingBreakpoint = true;
        if (kind == BreakpointRow::Exception) {
            m_controller.setExceptionFilterEnabled(item->data(0, kRoleExceptionFilter).toString().toStdString(),
                                                   checked);
        } else if (kind != BreakpointRow::Note) {
            const auto id = static_cast<debug::BreakpointId>(item->data(0, kRoleBreakpointId).toULongLong());
            m_controller.breakpoints().setEnabled(id, checked);
        }
        m_editingBreakpoint = false;
    });
    connect(m_breakpointsTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item != nullptr) {
            openEditBreakpoint(static_cast<debug::BreakpointId>(item->data(0, kRoleBreakpointId).toULongLong()));
        }
    });
}

void DebugToolWindow::buildMemoryAndDisassemblyTab() {
    auto* lowPane = new QWidget(m_tabs);
    auto* lowLayout = new QVBoxLayout(lowPane);
    lowLayout->setContentsMargins(6, 6, 6, 6);
    lowLayout->setSpacing(6);

    auto* topBar = new QHBoxLayout();
    m_memoryAddressInput = new QLineEdit(lowPane);
    m_memoryAddressInput->setObjectName(QStringLiteral("debugMemoryAddressInput"));
    m_memoryAddressInput->setPlaceholderText(QStringLiteral("0x... or symbol/expression"));
    m_disassembleButton = new QPushButton(QStringLiteral("Disassemble"), lowPane);
    m_disassembleButton->setObjectName(QStringLiteral("debugDisassembleButton"));
    m_readMemoryButton = new QPushButton(QStringLiteral("Read Memory (Hex)"), lowPane);
    m_readMemoryButton->setObjectName(QStringLiteral("debugReadMemoryButton"));
    topBar->addWidget(new QLabel(QStringLiteral("Address / Symbol:"), lowPane));
    topBar->addWidget(m_memoryAddressInput, 1);
    topBar->addWidget(m_disassembleButton);
    topBar->addWidget(m_readMemoryButton);
    lowLayout->addLayout(topBar);

    auto* lowSplitter = new QSplitter(Qt::Horizontal, lowPane);
    m_disassemblyTree = new QTreeWidget(lowSplitter);
    m_disassemblyTree->setObjectName(QStringLiteral("debugDisassemblyTree"));
    m_disassemblyTree->setRootIsDecorated(false);
    m_disassemblyTree->setUniformRowHeights(true);
    m_disassemblyTree->setAlternatingRowColors(true);
    m_disassemblyTree->setHeaderLabels(
        {QStringLiteral("Address"), QStringLiteral("Bytes"), QStringLiteral("Instruction")});
    m_disassemblyTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_disassemblyTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_disassemblyTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);

    m_memoryView = new QPlainTextEdit(lowSplitter);
    m_memoryView->setObjectName(QStringLiteral("debugMemoryView"));
    m_memoryView->setReadOnly(true);
    m_memoryView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    lowSplitter->addWidget(m_disassemblyTree);
    lowSplitter->addWidget(m_memoryView);
    lowSplitter->setStretchFactor(0, 1);
    lowSplitter->setStretchFactor(1, 1);
    lowLayout->addWidget(lowSplitter, 1);

    m_tabs->addTab(lowPane, QStringLiteral("Memory && Disassembly"));

    connect(m_disassembleButton, &QPushButton::clicked, this, &DebugToolWindow::requestDisassembly);
    connect(m_readMemoryButton, &QPushButton::clicked, this, &DebugToolWindow::requestMemoryDump);
    connect(m_memoryAddressInput, &QLineEdit::returnPressed, this, [this] {
        requestDisassembly();
        requestMemoryDump();
    });
}

void DebugToolWindow::wireControllerSignals() {
    connect(&m_controller, &DebugController::loadedSourceChanged, this,
            [this](const QString& name, const QString& content, int line) {
                const bool available = !name.isEmpty();
                m_loadedLabel->setVisible(available);
                m_loadedSource->setVisible(available);
                if (!available) {
                    m_loadedSource->clear();
                    return;
                }
                m_loadedLabel->setText(QStringLiteral("Loaded source: %1 (read-only)").arg(name));
                m_loadedSource->setPlainText(content);
                if (line > 0) {
                    const QTextBlock block = m_loadedSource->document()->findBlockByNumber(line - 1);
                    if (block.isValid()) {
                        QTextCursor cursor(block);
                        cursor.select(QTextCursor::LineUnderCursor);
                        m_loadedSource->setTextCursor(cursor);
                        m_loadedSource->centerCursor();
                    }
                }
            });
    connect(&m_controller, &DebugController::stateChanged, this, &DebugToolWindow::updateActions);
    connect(&m_controller, &DebugController::threadsChanged, this, &DebugToolWindow::updateThreads);
    connect(&m_controller, &DebugController::breakpointsChanged, this, &DebugToolWindow::updateBreakpointsList);
    connect(&m_controller, &DebugController::editBreakpointRequested, this,
            [this](debug::BreakpointId id) { openEditBreakpoint(id); });
    connect(&m_controller, &DebugController::viewBreakpointsRequested, this, [this] {
        if (m_tabs != nullptr && m_breakpointsTree != nullptr) {
            m_tabs->setCurrentIndex(2);
            m_breakpointsTree->setFocus();
        }
    });
    connect(&m_controller, &DebugController::frameSelected, this, [this] {
        const int row = m_frames->selectedRow();
        if (row >= 0) {
            const QSignalBlocker blocker(m_framesView->selectionModel());
            m_framesView->setCurrentIndex(m_frames->index(row));
        }
    });
    connect(&m_controller, &DebugController::stopped, this, [this](const QString& summary, bool crash) {
        m_reason->setText(summary);
        m_reason->setStyleSheet(crash ? QStringLiteral("color: #E06C75;") : QString());
        m_tabs->setCurrentIndex(0);
    });
    connect(&m_controller, &DebugController::sessionEnded, this, [this](const QString& summary) {
        m_reason->setText(summary);
        m_reason->setStyleSheet(QString());
    });
    connect(&m_controller, &DebugController::evaluateRequested, this,
            [this](const QString& expression) { openEvaluate(expression); });
    connect(m_variables, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex& parent, int first, int) {
        if (!parent.isValid() && first == 0) {
            m_variablesView->expand(m_variables->index(0, 0));
        }
    });
}

void DebugToolWindow::updateActions() {
    const bool live = m_controller.debugging();
    const bool paused = m_controller.paused();
    m_resume->setEnabled(paused);
    m_pause->setEnabled(live && m_controller.state() == debug::DebugState::Running);
    m_stop->setEnabled(live);
    m_stepOver->setEnabled(paused);
    m_stepInto->setEnabled(paused);
    m_stepOut->setEnabled(paused);
    m_runToCursor->setEnabled(paused);
    m_evaluate->setEnabled(paused);
    if (m_disassembleButton != nullptr) {
        m_disassembleButton->setEnabled(paused);
    }
    if (m_readMemoryButton != nullptr) {
        m_readMemoryButton->setEnabled(paused);
    }
    if (live && !paused) {
        m_reason->setText(m_controller.statusText());
        m_reason->setStyleSheet(QString());
    }
}

void DebugToolWindow::updateThreads() {
    m_fillingThreads = true;
    m_threads->clear();
    int current = -1;
    for (const debug::DebugThread& thread : m_controller.threads()) {
        const QString name = thread.name.empty()
                                 ? QStringLiteral("Thread %1").arg(thread.id)
                                 : QStringLiteral("%1 (%2)").arg(QString::fromStdString(thread.name)).arg(thread.id);
        m_threads->addItem(name, QVariant::fromValue<qlonglong>(thread.id));
        if (m_controller.selectedThread() == thread.id) {
            current = m_threads->count() - 1;
        }
    }
    if (current >= 0) {
        m_threads->setCurrentIndex(current);
    }
    m_fillingThreads = false;
}

void DebugToolWindow::updateBreakpointsList() {
    if (m_breakpointsTree == nullptr) {
        return;
    }
    if (m_editingBreakpoint) {
        QMetaObject::invokeMethod(this, &DebugToolWindow::updateBreakpointsList, Qt::QueuedConnection);
        return;
    }
    m_fillingBreakpoints = true;
    if (m_muteBreakpointsButton != nullptr) {
        const QSignalBlocker blocker(m_muteBreakpointsButton);
        m_muteBreakpointsButton->setChecked(m_controller.breakpointsMuted());
    }
    m_breakpointsTree->clear();
    const debug::BreakpointManager& manager = m_controller.breakpoints();
    for (const debug::LineBreakpoint& bp : manager.all()) {
        auto* item = new QTreeWidgetItem(m_breakpointsTree);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, bp.enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(0, kRoleBreakpointId, QVariant::fromValue<qulonglong>(bp.id));
        item->setData(0, kRoleBreakpointKind, static_cast<int>(BreakpointRow::Line));
        const std::string_view fileName = io::fileName(bp.path);
        const QString displayPath = fileName.empty() ? qs(bp.path) : qs(fileName);
        item->setText(1, QStringLiteral("%1:%2").arg(displayPath).arg(bp.line));
        item->setToolTip(1, QStringLiteral("%1:%2").arg(qs(bp.path)).arg(bp.line));
        item->setText(2, qs(bp.condition));
        item->setText(3, qs(bp.hitCondition));
        item->setText(4, qs(bp.logMessage));
    }
    for (const debug::FunctionBreakpointEntry& bp : manager.functionBreakpoints()) {
        auto* item = new QTreeWidgetItem(m_breakpointsTree);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, bp.enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(0, kRoleBreakpointId, QVariant::fromValue<qulonglong>(bp.id));
        item->setData(0, kRoleBreakpointKind, static_cast<int>(BreakpointRow::Function));
        item->setText(1, QStringLiteral("Function: %1").arg(qs(bp.name)));
        // The adapter's verdict (an unknown name stays unverified), never a guess.
        item->setToolTip(1, bp.status == debug::BreakpointStatus::Unverified && !bp.message.empty() ? qs(bp.message)
                                                                                                    : qs(bp.name));
        item->setText(2, qs(bp.condition));
        item->setText(3, qs(bp.hitCondition));
    }
    const std::vector<debug::ExceptionFilterEntry> filters = manager.exceptionFilters();
    for (const debug::ExceptionFilterEntry& filter : filters) {
        auto* item = new QTreeWidgetItem(m_breakpointsTree);
        item->setData(0, kRoleBreakpointKind, static_cast<int>(BreakpointRow::Exception));
        item->setData(0, kRoleExceptionFilter, qs(filter.filter));
        item->setText(1, QStringLiteral("Exception: %1").arg(qs(filter.label)));
        if (filter.unavailable.empty()) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, filter.enabled ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(1, qs(filter.filter));
            if (!filter.types.empty()) {
                // DBG-2b: the throw types it stops for, in the Condition column.
                QStringList types;
                for (const std::string& type : filter.types) {
                    types << qs(type);
                }
                item->setText(2, QStringLiteral("Types: %1").arg(types.join(QStringLiteral(", "))));
            }
        } else {
            // Unavailable for this session's programs: shown with the reason, and not checkable.
            item->setFlags(item->flags() & ~(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled));
            item->setText(4, QStringLiteral("Unavailable: %1").arg(qs(filter.unavailable)));
            item->setToolTip(1, qs(filter.unavailable));
        }
    }
    if (filters.empty()) {
        // No adapter has listed its exception filters yet: say so rather than offer made-up ones.
        auto* item = new QTreeWidgetItem(m_breakpointsTree);
        item->setData(0, kRoleBreakpointKind, static_cast<int>(BreakpointRow::Note));
        item->setFlags(item->flags() & ~(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable));
        item->setText(1, QStringLiteral("Exception breakpoints: start a debug session to list the debugger's filters"));
    }
    m_fillingBreakpoints = false;
}

void DebugToolWindow::openAddFunctionBreakpoint() {
    QDialog dialog(window());
    dialog.setWindowTitle(QStringLiteral("Add Function Breakpoint"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* nameEdit = new QLineEdit(&dialog);
    nameEdit->setObjectName(QStringLiteral("debugFunctionBreakpointName"));
    nameEdit->setPlaceholderText(QStringLiteral("e.g. MyClass::update or main"));
    form->addRow(QStringLiteral("Function:"), nameEdit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    nameEdit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        if (auto added = m_controller.addFunctionBreakpoint(nameEdit->text()); !added) {
            log::warn("debug", "the function breakpoint was not added: {}", added.error().message());
        }
    }
}

void DebugToolWindow::requestDisassembly() {
    if (!m_controller.paused() || m_memoryAddressInput == nullptr) {
        return;
    }
    const QString ref = m_memoryAddressInput->text().trimmed();
    if (ref.isEmpty()) {
        return;
    }
    QPointer<DebugToolWindow> self(this);
    m_controller.disassemble(ref, 64, [self](Result<std::vector<debug::dap::DisassembledInstruction>> res) {
        if (!self || self->m_disassemblyTree == nullptr) {
            return;
        }
        self->m_disassemblyTree->clear();
        if (!res) {
            auto* errItem = new QTreeWidgetItem(self->m_disassemblyTree);
            errItem->setText(2, qs(res.error().message()));
            return;
        }
        for (const debug::dap::DisassembledInstruction& inst : *res) {
            auto* item = new QTreeWidgetItem(self->m_disassemblyTree);
            item->setText(0, qs(inst.address));
            item->setText(1, qs(inst.instructionBytes));
            item->setText(2, inst.symbol.empty()
                                 ? qs(inst.instruction)
                                 : QStringLiteral("<%1> %2").arg(qs(inst.symbol), qs(inst.instruction)));
        }
    });
}

void DebugToolWindow::requestMemoryDump() {
    if (!m_controller.paused() || m_memoryAddressInput == nullptr) {
        return;
    }
    const QString ref = m_memoryAddressInput->text().trimmed();
    if (ref.isEmpty()) {
        return;
    }
    QPointer<DebugToolWindow> self(this);
    m_controller.readMemory(ref, 256, [self](Result<debug::dap::ReadMemoryResult> res) {
        if (!self || self->m_memoryView == nullptr) {
            return;
        }
        if (!res) {
            self->m_memoryView->setPlainText(qs(res.error().message()));
            return;
        }
        self->m_memoryView->setPlainText(formatHexDump(*res));
    });
}

void DebugToolWindow::promptSetVariableValue(QTreeView* view, VariablesModel* model, const QModelIndex& index) {
    if (!m_controller.paused() || model == nullptr || !index.isValid()) {
        return;
    }
    const std::int64_t parentRef = model->parentReferenceAt(index);
    const QString name = model->nameAt(index);
    const QString currentVal = model->valueAt(index);
    if (parentRef <= 0 || name.isEmpty()) {
        return;
    }
    QDialog dialog(view != nullptr ? view->window() : window());
    dialog.setWindowTitle(QStringLiteral("Set Value: %1").arg(name));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* valueEdit = new QLineEdit(currentVal, &dialog);
    form->addRow(QStringLiteral("New value:"), valueEdit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    valueEdit->selectAll();
    valueEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString newValue = valueEdit->text();
    QPointer<VariablesModel> safeModel(model);
    m_controller.setVariableValue(parentRef, name, newValue, [safeModel, index](Result<debug::dap::Variable> updated) {
        if (safeModel && updated) {
            safeModel->updateValueAt(index, qs(updated->value), qs(updated->type));
        }
    });
}

void DebugToolWindow::openEditBreakpoint(debug::BreakpointId id) {
    if (m_tabs != nullptr && m_breakpointsTree != nullptr) {
        m_tabs->setCurrentIndex(2);
    }
    if (id == 0 && m_breakpointsTree != nullptr && m_breakpointsTree->currentItem() != nullptr) {
        QTreeWidgetItem* current = m_breakpointsTree->currentItem();
        if (static_cast<BreakpointRow>(current->data(0, kRoleBreakpointKind).toInt()) == BreakpointRow::Exception) {
            const std::string filter = current->data(0, kRoleExceptionFilter).toString().toStdString();
            for (const debug::ExceptionFilterEntry& entry : m_controller.breakpoints().exceptionFilters()) {
                if (entry.filter == filter && filter == "cpp_throw" && entry.unavailable.empty()) {
                    openEditThrowTypes(entry);
                }
            }
            return;
        }
        id = static_cast<debug::BreakpointId>(current->data(0, kRoleBreakpointId).toULongLong());
    }
    if (const auto function = m_controller.breakpoints().findFunction(id)) {
        openEditFunctionBreakpoint(*function);
        return;
    }
    const auto bp = m_controller.breakpoints().find(id);
    if (!bp) {
        return;
    }
    QDialog dialog(window());
    dialog.setWindowTitle(QStringLiteral("Edit Breakpoint (%1:%2)").arg(qs(io::fileName(bp->path))).arg(bp->line));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* conditionEdit = new QLineEdit(qs(bp->condition), &dialog);
    conditionEdit->setPlaceholderText(QStringLiteral("e.g. i == 42 && ptr != nullptr"));
    auto* hitCountEdit = new QLineEdit(qs(bp->hitCondition), &dialog);
    hitCountEdit->setPlaceholderText(QStringLiteral("e.g. >= 10"));
    auto* logMessageEdit = new QLineEdit(qs(bp->logMessage), &dialog);
    logMessageEdit->setPlaceholderText(QStringLiteral("e.g. Value of x is {x} (Tracepoint)"));
    form->addRow(QStringLiteral("Condition:"), conditionEdit);
    form->addRow(QStringLiteral("Hit Count:"), hitCountEdit);
    form->addRow(QStringLiteral("Log Message:"), logMessageEdit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    conditionEdit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        m_controller.breakpoints().setProperties(id, conditionEdit->text().trimmed().toStdString(),
                                                 hitCountEdit->text().trimmed().toStdString(),
                                                 logMessageEdit->text().trimmed().toStdString());
        updateBreakpointsList();
    }
}

void DebugToolWindow::openEditFunctionBreakpoint(const debug::FunctionBreakpointEntry& bp) {
    QDialog dialog(window());
    dialog.setWindowTitle(QStringLiteral("Edit Function Breakpoint (%1)").arg(qs(bp.name)));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* conditionEdit = new QLineEdit(qs(bp.condition), &dialog);
    conditionEdit->setPlaceholderText(QStringLiteral("e.g. value > 1"));
    auto* hitCountEdit = new QLineEdit(qs(bp.hitCondition), &dialog);
    hitCountEdit->setPlaceholderText(QStringLiteral("e.g. 10"));
    form->addRow(QStringLiteral("Condition:"), conditionEdit);
    form->addRow(QStringLiteral("Hit Count:"), hitCountEdit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    conditionEdit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        m_controller.breakpoints().setProperties(bp.id, conditionEdit->text().trimmed().toStdString(),
                                                 hitCountEdit->text().trimmed().toStdString(), {});
        updateBreakpointsList();
    }
}

void DebugToolWindow::openEditThrowTypes(const debug::ExceptionFilterEntry& filter) {
    QDialog dialog(window());
    dialog.setWindowTitle(QStringLiteral("Edit Exception Breakpoint (%1)").arg(qs(filter.label)));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    QStringList current;
    for (const std::string& type : filter.types) {
        current << qs(type);
    }
    auto* typesEdit = new QLineEdit(current.join(QStringLiteral(", ")), &dialog);
    typesEdit->setObjectName(QStringLiteral("debugThrowTypes"));
    typesEdit->setPlaceholderText(QStringLiteral("e.g. std::runtime_error, MyError (empty: every type)"));
    form->addRow(QStringLiteral("Only these types:"), typesEdit);
    layout->addLayout(form);
    // The type list works through the MSVC runtime's ThrowInfo (debug::DebugSession, "Thrown types").
    layout->addWidget(
        new QLabel(QStringLiteral("Stops only for a throw of exactly these types (not their base or derived classes). "
                                  "Programs built for the MSVC ABI (Windows) only."),
                   &dialog));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    typesEdit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        m_controller.breakpoints().setExceptionFilterTypes(filter.filter,
                                                           debug::msvc::splitTypeList(typesEdit->text().toStdString()));
        updateBreakpointsList();
    }
}

void DebugToolWindow::onFrameActivated(int row) {
    if (m_frames->isLoadMoreRow(row)) {
        m_controller.loadMoreFrames();
        return;
    }
    const QVariant id = m_frames->data(m_frames->index(row), FramesModel::kFrameIdRole);
    if (id.isValid() && id.toLongLong() >= 0) {
        m_controller.selectFrame(id.toLongLong());
    }
}

void DebugToolWindow::copySelectedValue() {
    QTreeView* view = m_watchesView->hasFocus() ? m_watchesView : m_variablesView;
    auto* model = static_cast<VariablesModel*>(view->model());
    const QModelIndex current = view->currentIndex();
    if (current.isValid()) {
        QApplication::clipboard()->setText(model->valueAt(current.siblingAtColumn(1)));
    }
}

void DebugToolWindow::openEvaluate(const QString& expression) {
    if (!m_controller.paused()) {
        return;
    }
    auto* dialog = new EvaluateDialog(m_controller, expression, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

// --- EvaluateDialog ---

EvaluateDialog::EvaluateDialog(DebugController& controller, const QString& expression, QWidget* parent)
    : QDialog(parent), m_expression(new QLineEdit(expression, this)),
      m_result(new VariablesModel(controller, VariablesModel::Mode::Watches, this)) {
    setWindowTitle(QStringLiteral("Evaluate"));
    auto* layout = new QVBoxLayout(this);
    m_expression->setPlaceholderText(QStringLiteral("Expression to evaluate in the selected frame"));
    layout->addWidget(m_expression);
    m_result->setEvaluateContext(QStringLiteral("repl"));
    // A new stop does not evaluate the dialog's expression again by itself: only Evaluate does.
    disconnect(&controller, &DebugController::frameSelected, m_result, nullptr);
    m_resultView = makeVariablesView(this, m_result);
    layout->addWidget(m_resultView, 1);
    auto* buttons = new QDialogButtonBox(this);
    QPushButton* evaluateButton = buttons->addButton(QStringLiteral("Evaluate"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(evaluateButton, &QPushButton::clicked, this, &EvaluateDialog::evaluate);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_expression, &QLineEdit::returnPressed, this, &EvaluateDialog::evaluate);
    // A button box's Accept role would close the dialog; Evaluate keeps it open.
    disconnect(buttons, &QDialogButtonBox::accepted, this, nullptr);
    installDialogChrome(this, true);
    resize(560, 360);
    if (!expression.trimmed().isEmpty()) {
        evaluate();
    }
}

EvaluateDialog::~EvaluateDialog() = default;

void EvaluateDialog::evaluate() {
    while (m_result->rowCount() > 0) {
        m_result->removeWatch(0);
    }
    m_result->addWatch(m_expression->text());
}

} // namespace frost::ui
