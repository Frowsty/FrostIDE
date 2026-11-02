// SPDX-License-Identifier: Apache-2.0
//
// DebugToolWindow and EvaluateDialog (see frost/ui/debug_tool_window.hpp): the widgets, the toolbar's
// enabled states and the wiring between the views, the models and the controller.

#include "frost/ui/debug_tool_window.hpp"

#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_models.hpp"
#include "frost/ui/run_console.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace frost::ui {

namespace {

// A tree view for variables: three columns, lazy children, Copy Value in the context menu.
QTreeView* makeVariablesView(QWidget* parent, VariablesModel* model) {
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
    QObject::connect(view, &QWidget::customContextMenuRequested, view, [view, model](const QPoint& at) {
        const QModelIndex index = view->indexAt(at);
        if (!index.isValid()) {
            return;
        }
        const QString value = model->valueAt(index.siblingAtColumn(1));
        QMenu menu(view);
        menu.addAction(QStringLiteral("Copy Value"), view, [value] { QApplication::clipboard()->setText(value); });
        menu.exec(view->viewport()->mapToGlobal(at));
    });
    return view;
}

} // namespace

DebugToolWindow::DebugToolWindow(DebugController& controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName(QStringLiteral("debugToolWindow"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The toolbar: the commands with the keymap's default shortcuts in their tooltips.
    m_toolbar = new QToolBar(this);
    m_toolbar->setIconSize(QSize(16, 16));
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
    layout->addWidget(m_toolbar);

    m_reason = new QLabel(this);
    m_reason->setObjectName(QStringLiteral("debugReason"));
    m_reason->setContentsMargins(8, 2, 8, 2);
    m_reason->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_reason);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    layout->addWidget(m_tabs, 1);

    // "Threads & Variables": frames on the left, variables and watches on the right.
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

    m_variables = new VariablesModel(m_controller, VariablesModel::Mode::Scopes, this);
    m_variablesView = makeVariablesView(splitter, m_variables);

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
    m_watchesView = makeVariablesView(watchesPane, m_watches);
    watchesLayout->addWidget(m_watchesView, 1);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 2);
    m_tabs->addTab(splitter, QStringLiteral("Threads && Variables"));

    m_console = new RunConsole(m_tabs);
    m_tabs->addTab(m_console, QStringLiteral("Console"));
    m_controller.setConsole(m_console);

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

    connect(&m_controller, &DebugController::stateChanged, this, &DebugToolWindow::updateActions);
    connect(&m_controller, &DebugController::threadsChanged, this, &DebugToolWindow::updateThreads);
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
    // The first scope (Locals) opens by itself, as in Rider.
    connect(m_variables, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex& parent, int first, int) {
        if (!parent.isValid() && first == 0) {
            m_variablesView->expand(m_variables->index(0, 0));
        }
    });
    updateActions();
}

DebugToolWindow::~DebugToolWindow() = default;

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
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Evaluate"));
    auto* layout = new QVBoxLayout(this);
    m_expression = new QLineEdit(expression, this);
    m_expression->setPlaceholderText(QStringLiteral("Expression to evaluate in the selected frame"));
    layout->addWidget(m_expression);
    m_result = new VariablesModel(controller, VariablesModel::Mode::Watches, this);
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
