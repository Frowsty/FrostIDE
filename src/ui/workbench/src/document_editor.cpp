// SPDX-License-Identifier: Apache-2.0
//
// DocumentEditorFactory and its views (frost/ui/workbench/document_editor.hpp): every view acquires
// its document in the registry, forwards the inner editor, and keeps a banner area above it in step
// with the document's state.

#include "frost/ui/workbench/document_editor.hpp"

#include "frost/document/load_service.hpp"
#include "frost/ui/icon_ids.hpp"

#include "workbench_util.hpp"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace frost::ui {

using workbench_detail::fromUtf8;
using workbench_detail::text;
using workbench_detail::themedIcon;

namespace workbench_detail {

QIcon fileIcon(std::string_view name) {
    constexpr std::array kCode{".c",   ".cc",  ".cpp",  ".cxx", ".c++", ".h",     ".hh", ".hpp", ".hxx", ".inl",
                               ".ipp", ".ixx", ".cppm", ".cs",  ".py",  ".cmake", ".rs", ".js",  ".ts",  ".java"};
    const std::size_t dot = name.rfind('.');
    if (dot != std::string_view::npos) {
        std::string extension(name.substr(dot));
        std::ranges::transform(extension, extension.begin(),
                               [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        if (std::ranges::find(kCode, std::string_view(extension)) != kCode.end()) {
            return themedIcon(icons::kNodesFileCode);
        }
    }
    if (name == "CMakeLists.txt") {
        return themedIcon(icons::kNodesFileCode);
    }
    return themedIcon(icons::kNodesFile);
}

} // namespace workbench_detail

std::vector<BannerKind> bannersFor(const document::FileDocument& document) {
    std::vector<BannerKind> banners;
    switch (document.diskState()) {
    case document::DiskState::OutOfDate: banners.push_back(BannerKind::OutOfDate); break;
    case document::DiskState::Changed: banners.push_back(BannerKind::MergePending); break;
    case document::DiskState::Deleted: banners.push_back(BannerKind::Deleted); break;
    case document::DiskState::InSync:
    case document::DiskState::Unsaved: break;
    }
    if (document.readOnly() == document::ReadOnlyReason::LongLine) {
        banners.push_back(BannerKind::LongLine);
    }
    return banners;
}

EditorBanner::EditorBanner(BannerKind kind, const QString& message, QWidget* parent)
    : QFrame(parent), m_kind(kind), m_message(new QLabel(message, this)), m_layout(new QHBoxLayout(this)) {
    setObjectName(QStringLiteral("editorBanner"));
    setProperty("bannerKind", static_cast<int>(kind));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    m_message->setWordWrap(true);
    m_layout->setContentsMargins(8, 4, 8, 4);
    m_layout->addWidget(m_message, 1);
}

void EditorBanner::addAction(const QString& label, std::function<void()> run) {
    auto* button = new QPushButton(label, this);
    button->setObjectName(QStringLiteral("bannerAction"));
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, this, [run = std::move(run)] {
        if (run) {
            run();
        }
    });
    m_layout->addWidget(button);
}

/*
 * One tab's view: a container with the banner column above the inner editor's widget. It is the
 * inner editor's host, so status changes reach EditorTabs with the wrapper as the editor. The
 * document stays acquired in the registry for the view's lifetime.
 */
class DocumentEditorView final : public IEditorView, private IEditorHost {
public:
    DocumentEditorView(DocumentEditorFactory& factory, std::string path, IEditorHost& host)
        : m_factory(factory), m_path(std::move(path)), m_host(host), m_container(new QWidget()),
          m_banners(new QVBoxLayout()) {
        m_container->setObjectName(QStringLiteral("documentEditor"));
        auto* layout = new QVBoxLayout(m_container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        m_banners->setContentsMargins(0, 0, 0, 0);
        m_banners->setSpacing(0);
        layout->addLayout(m_banners);
        m_factory.documents().acquire(m_path);
    }

    DocumentEditorView(const DocumentEditorView&) = delete ("an editor view has identity");
    DocumentEditorView& operator=(const DocumentEditorView&) = delete ("an editor view has identity");

    ~DocumentEditorView() override {
        m_factory.detach(this);
        // The inner view deletes its widget; take it out of the container first.
        if (m_inner != nullptr) {
            m_inner->widget()->setParent(nullptr);
        }
        m_inner.reset();
        delete m_container;
        m_factory.documents().release(m_path);
    }

    // The inner editor, opened as this view's host. Called once by the factory.
    [[nodiscard]] Result<void> openInner(IEditorFactory& inner) {
        auto opened = inner.open(m_path, *this);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        m_inner = std::move(*opened);
        static_cast<QVBoxLayout*>(m_container->layout())->addWidget(m_inner->widget(), 1);
        m_container->setFocusProxy(m_inner->widget());
        refreshBanners();
        m_ready = true;
        return {};
    }

    [[nodiscard]] QWidget* widget() override { return m_container; }
    [[nodiscard]] const std::string& path() const override { return m_path; }
    void setPath(std::string path) {
        m_path = std::move(path);
        refreshBanners();
    }

    [[nodiscard]] EditorStatus status() const override {
        EditorStatus status = m_inner != nullptr ? m_inner->status() : EditorStatus{};
        if (const document::FileDocument* document = m_factory.documents().find(m_path); document != nullptr) {
            status.dirty = status.dirty || document->dirty();
            status.readOnly = status.readOnly || document->readOnly() != document::ReadOnlyReason::None;
        }
        return status;
    }

    void goToLine(int line, int column) override {
        if (m_inner != nullptr) {
            m_inner->goToLine(line, column);
        }
    }
    void goToStatusPosition(int line, int column) override {
        if (m_inner != nullptr)
            m_inner->goToStatusPosition(line, column);
    }
    [[nodiscard]] input::ContextKeys contextKeys() const override {
        return m_inner != nullptr ? m_inner->contextKeys() : input::ContextKeys{};
    }
    [[nodiscard]] bool requestClose() override { return m_inner == nullptr || m_inner->requestClose(); }
    [[nodiscard]] EditorViewState viewState() const override {
        return m_inner != nullptr ? m_inner->viewState() : EditorViewState{};
    }
    void restoreViewState(const EditorViewState& state) override {
        if (m_inner != nullptr) {
            m_inner->restoreViewState(state);
        }
    }
    void setBookmarks(std::span<const int> oneBasedLines) override {
        if (m_inner != nullptr) {
            m_inner->setBookmarks(oneBasedLines);
        }
    }

    // Rebuilds the banners from the document's state.
    void refreshBanners() {
        for (EditorBanner* banner : m_bannerWidgets) {
            delete banner;
        }
        m_bannerWidgets.clear();
        const document::FileDocument* document = m_factory.documents().find(m_path);
        if (document == nullptr) {
            return;
        }
        const BannerActions& actions = m_factory.m_actions;
        const std::string path = m_path;
        for (const BannerKind kind : bannersFor(*document)) {
            EditorBanner* banner = nullptr;
            switch (kind) {
            case BannerKind::LongLine:
                banner = new EditorBanner(kind, text("This file has a very long line and is open read-only."));
                break;
            case BannerKind::OutOfDate: {
                QString reason = text("The file changed on disk but could not be reloaded.");
                if (const auto& why = document->outOfDateReason(); why) {
                    reason += QStringLiteral(" ") + fromUtf8(why->message());
                }
                banner = new EditorBanner(kind, reason);
                banner->addAction(text("Reload"), [reload = actions.reload, path] {
                    if (reload) {
                        reload(path);
                    }
                });
                banner->addAction(text("Overwrite"), [overwrite = actions.overwrite, path] {
                    if (overwrite) {
                        overwrite(path);
                    }
                });
                break;
            }
            case BannerKind::MergePending:
                banner = new EditorBanner(kind, text("The file changed on disk while it has unsaved changes."));
                banner->addAction(text("Compare..."), [compare = actions.compare, path] {
                    if (compare) {
                        compare(path);
                    }
                });
                break;
            case BannerKind::Deleted:
                banner = new EditorBanner(kind, text("The file was deleted on disk. Saving creates it again."));
                break;
            }
            m_banners->addWidget(banner);
            m_bannerWidgets.push_back(banner);
        }
        if (m_ready) {
            m_host.editorStatusChanged(*this);
        }
    }

    [[nodiscard]] const std::vector<EditorBanner*>& bannerWidgets() const noexcept { return m_bannerWidgets; }

private:
    void editorStatusChanged(IEditorView&) override {
        if (m_ready) {
            m_host.editorStatusChanged(*this);
        }
    }

    DocumentEditorFactory& m_factory;
    std::string m_path;
    IEditorHost& m_host;
    QWidget* m_container;
    QVBoxLayout* m_banners;
    std::unique_ptr<IEditorView> m_inner;
    std::vector<EditorBanner*> m_bannerWidgets;
    bool m_ready = false;
};

DocumentEditorFactory::DocumentEditorFactory(IEditorFactory& inner, nav::DocumentRegistry& documents) noexcept
    : m_inner(&inner), m_documents(&documents) {}

DocumentEditorFactory::~DocumentEditorFactory() = default;

Result<std::unique_ptr<IEditorView>> DocumentEditorFactory::open(const std::string& path, IEditorHost& host) {
    auto opened = m_documents->open(path);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    auto view = std::make_unique<DocumentEditorView>(*this, opened->path, host);
    if (auto inner = view->openInner(*m_inner); !inner) {
        return std::unexpected(std::move(inner.error()));
    }
    attach(view.get());
    return std::unique_ptr<IEditorView>(std::move(view));
}

std::string DocumentEditorFactory::canonicalPath(const std::string& path) const {
    return m_documents->canonicalize(path);
}

void DocumentEditorFactory::refreshBanners(const std::string& path) {
    // A copy: a banner rebuild may not open or close views, but be safe against it.
    const std::vector<DocumentEditorView*> views = m_views;
    for (DocumentEditorView* view : views) {
        if (view->path() == path) {
            view->refreshBanners();
        }
    }
}

void DocumentEditorFactory::renamePath(const std::string& oldPath, const std::string& newPath) {
    const std::vector<DocumentEditorView*> views = m_views;
    for (DocumentEditorView* view : views) {
        if (view->path() == oldPath) {
            view->setPath(newPath);
        }
    }
}

void DocumentEditorFactory::setBookmarks(const std::string& path, std::span<const int> oneBasedLines) {
    const std::vector<DocumentEditorView*> views = m_views;
    for (DocumentEditorView* view : views) {
        if (view->path() == path) {
            view->setBookmarks(oneBasedLines);
        }
    }
}

std::vector<EditorBanner*> DocumentEditorFactory::banners(const std::string& path) const {
    for (const DocumentEditorView* view : m_views) {
        if (view->path() == path) {
            return view->bannerWidgets();
        }
    }
    return {};
}

void DocumentEditorFactory::attach(DocumentEditorView* view) { m_views.push_back(view); }

void DocumentEditorFactory::detach(DocumentEditorView* view) { std::erase(m_views, view); }

} // namespace frost::ui
