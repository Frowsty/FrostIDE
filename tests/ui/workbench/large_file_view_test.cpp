// SPDX-License-Identifier: Apache-2.0
// Deferred UI tests for actual paged view state, byte/line seeking and close/cancel generations.
#include "frost/ui/workbench/large_file_view.hpp"
#include "frost/ui/workbench/document_editor.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/core/clock.hpp"
#include "support/scratch_dir.hpp"
#include <filesystem>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QThreadPool>
#include <gtest/gtest.h>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>
namespace frost::ui::test {
namespace {
struct ReaderState {
    std::string data="first\nsecond\n";
    std::uint64_t size=0;
    std::atomic<int> calls=0,cancelled=0,destroyed=0;
    std::atomic<bool> block=false;
    std::optional<Error> error;
    std::mutex mutex;
    std::condition_variable_any changed;
};
class Reader final : public io::IRandomAccessReader {
public:
    explicit Reader(std::shared_ptr<ReaderState> state):m_state(std::move(state)) {}
    ~Reader() override { ++m_state->destroyed; }
    std::uint64_t size() const noexcept override { return m_state->size ? m_state->size : m_state->data.size(); }
    Result<std::string> read(std::uint64_t offset,std::size_t count,std::stop_token stop) override {
        ++m_state->calls;
        EXPECT_LE(count,kMaximumReadBytes);
        if (m_state->block) {
            std::unique_lock lock(m_state->mutex);
            m_state->changed.wait(lock,stop,[&] { return !m_state->block.load(); });
        }
        if (stop.stop_requested()) { ++m_state->cancelled; return fail(ErrorCode::Cancelled,"cancelled fixture"); }
        if (m_state->error) return std::unexpected(*m_state->error);
        if (offset>size()) return fail(ErrorCode::OutOfRange,"outside fixture");
        const auto length=static_cast<std::size_t>(std::min<std::uint64_t>(count,size()-offset));
        if (offset>=m_state->data.size()) return std::string(length,'p');
        return m_state->data.substr(static_cast<std::size_t>(offset),length);
    }
private:
    std::shared_ptr<ReaderState> m_state;
};
// Only the lifetime-independent opener is usable. Whole-file reads are an assertion failure.
class ReaderFs final : public io::IFileSystem {
public:
    std::shared_ptr<ReaderState> state=std::make_shared<ReaderState>();
    io::RandomAccessOpener randomAccessOpener() const override {
        return [state=state](std::string_view)->Result<std::unique_ptr<io::IRandomAccessReader>> {
            return std::unique_ptr<io::IRandomAccessReader>(new Reader(state));
        };
    }
    Result<std::string> readFile(std::string_view) override { ADD_FAILURE()<<"Whole-file read forbidden"; return fail(ErrorCode::Internal); }
    Result<void> writeFileAtomic(std::string_view,std::string_view) override { return fail(ErrorCode::Unsupported); }
    Result<io::FileStatus> status(std::string_view) override { return io::FileStatus{io::FileType::Regular,std::uint64_t{1}<<30,{}}; }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view) override { return fail(ErrorCode::Unsupported); }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view) override { return fail(ErrorCode::Unsupported); }
    Result<void> createDirectories(std::string_view) override { return fail(ErrorCode::Unsupported); }
    Result<void> removeFile(std::string_view) override { return fail(ErrorCode::Unsupported); }
    Result<void> removeDirectory(std::string_view) override { return fail(ErrorCode::Unsupported); }
    Result<bool> exists(std::string_view) override { return true; }
};
struct Host final : IEditorHost {
    int changes=0;
    void editorStatusChanged(IEditorView&) override { ++changes; }
};
template<class Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer deadline; deadline.start();
    while (!predicate() && deadline.elapsed()<3000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return predicate();
}
struct LargeFileViewTest : ::testing::Test {
    void TearDown() override { QThreadPool::globalInstance()->waitForDone(); }
};
TEST_F(LargeFileViewTest, NativeLargeFixtureReachesProductFactoryWithoutEditableDocumentOrInnerEditor) {
    frost::test::ScratchDir scratch("large-file-view"); const auto path=scratch.join("large.txt");
    auto& fs=io::realFileSystem(); const std::string header="actual file first screen\n";
    const std::string firstPage=header+std::string(io::IRandomAccessReader::kMaximumReadBytes-header.size(),'a');
    ASSERT_EQ(firstPage.size(),65536U); ASSERT_TRUE(fs.writeFileAtomic(path,firstPage));
    std::error_code error;
    std::filesystem::resize_file(std::filesystem::u8path(path),std::uint64_t{128}<<20,error); ASSERT_FALSE(error);
    ManualClock clock;
    document::SaveManagerServices services; services.fs=&fs; services.clock=&clock;
    document::SaveManager manager(services,{}); nav::DocumentRegistry registry(manager,fs);
    struct Inner final : IEditorFactory {
        int calls=0;
        Result<std::unique_ptr<IEditorView>> open(const std::string&,IEditorHost&) override {
            ++calls; return fail(ErrorCode::Internal,"Large fixture reached editable factory");
        }
    } inner;
    DocumentEditorFactory factory(inner,registry); Host host;
    auto opened=factory.open(path,host); ASSERT_TRUE(opened);
    EXPECT_EQ(inner.calls,0); EXPECT_TRUE(manager.documents().empty());
    EXPECT_EQ((*opened)->widget()->objectName(),QStringLiteral("largeFileView"));
    ASSERT_TRUE(waitUntil([&] { return host.changes>0; }));
    EXPECT_EQ((*opened)->widget()->findChild<QPlainTextEdit*>()->toPlainText(),QString::fromStdString(firstPage));
    opened->reset(); QThreadPool::globalInstance()->waitForDone();
}
TEST_F(LargeFileViewTest, DisplaysBoundedPageAndSeeksLineAcrossPageBoundary) {
    ReaderFs fs; Host host;
    fs.state->data=std::string(io::IRandomAccessReader::kMaximumReadBytes-1,'x')+"\nneedle\nlast";
    auto view=makeLargeFileView(fs,"/fixture.txt",host);
    auto* text=view->widget()->findChild<QPlainTextEdit*>(QStringLiteral("largeFilePage")); ASSERT_NE(text,nullptr);
    ASSERT_TRUE(waitUntil([&] { return host.changes>0; }));
    EXPECT_EQ(text->toPlainText().size(),65536);
    EXPECT_TRUE(text->isReadOnly()); EXPECT_TRUE(view->status().readOnly); EXPECT_FALSE(view->status().dirty);
    EXPECT_TRUE(view->contextKeys().has(input::ContextKey::EditorReadOnly));
    view->goToLine(2,1);
    ASSERT_TRUE(waitUntil([&] { return host.changes>1; }));
    EXPECT_EQ(text->toPlainText(),QStringLiteral("needle\nlast")); EXPECT_EQ(view->status().line,2);
}
TEST_F(LargeFileViewTest, ByteSeekUsesHigh64BitOffsetAndUnknownLineBanner) {
    ReaderFs fs; Host host; fs.state->size=(std::uint64_t{1}<<33)+100;
    auto view=makeLargeFileView(fs,"/fixture.txt",host);
    ASSERT_TRUE(waitUntil([&] { return host.changes>0; }));
    auto* offset=view->widget()->findChild<QLineEdit*>(QStringLiteral("largeFileByteOffset")); ASSERT_NE(offset,nullptr);
    offset->setText(QStringLiteral("8589934592"));
    ASSERT_TRUE(QMetaObject::invokeMethod(offset,"returnPressed",Qt::DirectConnection));
    ASSERT_TRUE(waitUntil([&] { return host.changes>1; }));
    auto* label=view->widget()->findChild<QLabel*>(QStringLiteral("largeFileState"));
    EXPECT_TRUE(label->text().contains(QStringLiteral("8589934592")));
    EXPECT_TRUE(label->text().contains(QStringLiteral("Global line unknown")));
}
TEST_F(LargeFileViewTest, NewGenerationCancelsBlockedOldPageAndCloseReleasesReaderWithoutHostCallbacks) {
    auto fs=std::make_unique<ReaderFs>(); Host host; auto state=fs->state; state->block=true;
    auto view=makeLargeFileView(*fs,"/fixture.txt",host);
    ASSERT_TRUE(waitUntil([&] { return state->calls>0; }));
    view->goToLine(2,1);
    ASSERT_TRUE(waitUntil([&] { return state->cancelled>0; }));
    // Both filesystem and view can die before the worker finishes: captured opener owns its state.
    fs.reset(); view.reset();
    ASSERT_TRUE(waitUntil([&] { return state->destroyed>0; }));
    EXPECT_EQ(host.changes,0);
}
TEST_F(LargeFileViewTest, ExplicitCancelSuppressesDeliveryAndLineSeekResumesWithNewGeneration) {
    ReaderFs fs; Host host; fs.state->block=true;
    auto view=makeLargeFileView(fs,"/fixture.txt",host);
    ASSERT_TRUE(waitUntil([&] { return fs.state->calls>0; }));
    auto* cancel=view->widget()->findChild<QPushButton*>(QStringLiteral("largeFileCancel")); ASSERT_NE(cancel,nullptr);
    cancel->click(); ASSERT_TRUE(waitUntil([&] { return fs.state->cancelled>0; }));
    EXPECT_EQ(host.changes,0);
    EXPECT_TRUE(view->widget()->findChild<QLabel*>()->text().contains(QStringLiteral("cancelled")));
    fs.state->block=false; fs.state->changed.notify_all(); view->goToLine(2,1);
    ASSERT_TRUE(waitUntil([&] { return host.changes>0; }));
    EXPECT_EQ(view->widget()->findChild<QPlainTextEdit*>()->toPlainText(),QStringLiteral("second\n"));
    EXPECT_EQ(view->status().line,2);
}
TEST_F(LargeFileViewTest, IdentityFailureClearsOldPageAndSurfacesError) {
    ReaderFs fs; Host host; fs.state->error=Error(ErrorCode::Busy,"identity changed; reopen");
    auto view=makeLargeFileView(fs,"/fixture.txt",host);
    auto* label=view->widget()->findChild<QLabel*>(QStringLiteral("largeFileState"));
    ASSERT_TRUE(waitUntil([&] { return label->text().contains(QStringLiteral("identity changed")); }));
    EXPECT_TRUE(view->widget()->findChild<QPlainTextEdit*>()->toPlainText().isEmpty()); EXPECT_EQ(host.changes,0);
}
TEST_F(LargeFileViewTest, InvalidInteriorUtf8AndUtf16AreRefused) {
    for (const auto& bytes : {std::string("hello\xffworld"),std::string("\xff\xfeX\0",4)}) {
        ReaderFs fs; Host host; fs.state->data=bytes;
        auto view=makeLargeFileView(fs,"/fixture.txt",host);
        auto* label=view->widget()->findChild<QLabel*>(QStringLiteral("largeFileState"));
        ASSERT_TRUE(waitUntil([&] { return label->text().contains(QStringLiteral("unsupported")) || label->text().contains(QStringLiteral("refused")); }));
        EXPECT_TRUE(view->widget()->findChild<QPlainTextEdit*>()->toPlainText().isEmpty());
    }
}
TEST_F(LargeFileViewTest, Utf8SplitAtPageEndIsPreviewedButTruncatedSequenceAtEofIsRefused) {
    ReaderFs fs; Host host;
    fs.state->data=std::string(65535,'a')+"\xe2\x82\xac";
    auto view=makeLargeFileView(fs,"/fixture.txt",host);
    ASSERT_TRUE(waitUntil([&] { return host.changes>0; }));
    EXPECT_EQ(view->widget()->findChild<QPlainTextEdit*>()->toPlainText().size(),65536);
    view.reset(); QThreadPool::globalInstance()->waitForDone();
    fs.state=std::make_shared<ReaderState>(); fs.state->data="bad eof \xe2\x82";
    view=makeLargeFileView(fs,"/fixture.txt",host);
    auto* label=view->widget()->findChild<QLabel*>(QStringLiteral("largeFileState"));
    ASSERT_TRUE(waitUntil([&] { return label->text().contains(QStringLiteral("unsupported")); }));
}
}
}
