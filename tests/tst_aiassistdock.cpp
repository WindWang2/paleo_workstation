// 层：测试壳
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextTable>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <functional>

#include "../src/ai/chat/llmclient.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/ai/aicontextbar.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aichatcontroller.h"

// 方向51：AI 助手 dock（视图层）——禁用态、流式渲染、工具调用占位卡片、
// 错误态。方向62 追加：会话切换 round-trip（内容/滚动）、markdown 定型
// 渲染、复制/重发、会话行。端点一律走 127.0.0.1 假服务器（本机），
// 不发外网请求。
namespace {
const char kChunkA[] = "data: {\"choices\":[{\"delta\":{\"content\":\"砂岩\"}}]}\n\n";
const char kChunkB[] = "data: {\"choices\":[{\"delta\":{\"content\":\"为主\"}}]}\n\n";
const char kDone[] =
  "data: {\"choices\":[{\"delta\":{\"content\":\"已完成\"}}]}\n\n"
  "data: [DONE]\n\n";
const char kToolChunk[] =
  "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
  "\"function\":{\"name\":\"paleo.query_project\",\"arguments\":\"{\\\"topic\\\":\\\"well_facies_models\\\"}\"}}]}}]}\n\n"
  "data: [DONE]\n\n";
// 流式响应和截图共用一个示例，便于核对实际 QTextDocument 与历史重放。
const QString kComparisonQuestion = QStringLiteral("对比 A1、B2 两井 S1 段的砂岩厚度。");
const char kMarkdownChunk[] =
  R"(data: {"choices":[{"delta":{"content":"## S1 砂岩厚度对比\n\n**A1 井**比 B2 井厚 6.5 m。\n\n)"
  R"(| 井号 | 砂岩厚度（m） | 优势岩性 |\n|---|---:|---|\n| A1 | 18.5 | 粗砂岩 |\n| B2 | 12.0 | 细砂岩 |\n\n)"
  R"(> 两井均采用 S1 顶、底界内的井深厚度。\n\n- 复核 B2 井的薄层识别。\n- 对照 `GR` 曲线检查层界。\n\n)"
  R"(```python\ndelta_m = 18.5 - 12.0\n```"}}]})" "\n\n"
  "data: [DONE]\n\n";
} // namespace

class FakeLlmServer : public QObject {
  Q_OBJECT
public:
  enum class Mode { Stream, ToolCalls, ToolCallsOnce, Unauthorized, Markdown, PausedMarkdown };
  bool start(Mode mode) {
    m_mode = mode;
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { serve(m_server.nextPendingConnection()); });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1")
                  .arg(m_server.serverPort()));
  }
  int requestCount() const { return m_requests; }
  void finishPausedMarkdown() {
    if (m_paused) {
      m_paused->write(kDone);
      m_paused->disconnectFromHost();
    }
  }

private:
  void serve(QTcpSocket *sock) {
    if (!sock)
      return;
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    auto buf = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
      buf->append(sock->readAll());
      const int headerEnd = buf->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      const QString header = QString::fromUtf8(buf->left(headerEnd));
      const int contentLength =
        header.section("Content-Length:", 1, 1).section("\r", 0, 0).trimmed().toInt();
      if (buf->mid(headerEnd + 4).size() < contentLength)
        return;
      *buf = buf->mid(headerEnd + 4 + contentLength);
      respond(sock);
    });
  }
  void respond(QTcpSocket *sock) {
    ++m_requests;
    if (m_mode == Mode::Unauthorized) {
      const QByteArray body = "{\"error\":{\"message\":\"bad key\"}}";
      sock->write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n"
                  "Content-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
      return;
    }
    sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                "Content-Type: text/event-stream\r\n\r\n");
    if (m_mode == Mode::PausedMarkdown) {
      m_paused = sock;
      sock->write(R"(data: {"choices":[{"delta":{"content":"| 井号 | 厚度 |\n|---|---|\n| A1 | 18.5 |\n\n"}}]})" "\n\n");
      return;
    }
    if (m_mode == Mode::Markdown) {
      // markdown 回答一整块下发（表头必须在行首，不能被前缀文本污染）。
      QTimer::singleShot(20, sock, [sock] {
        sock->write(kMarkdownChunk);
        sock->disconnectFromHost();
      });
      return;
    }
    const bool toolRound =
      m_mode == Mode::ToolCalls ||
      (m_mode == Mode::ToolCallsOnce && m_requestCount++ == 0);
    const QByteArray tail = toolRound ? QByteArray(kToolChunk) : QByteArray(kDone);
    QTimer::singleShot(20, sock, [sock] { sock->write(kChunkA); });
    QTimer::singleShot(50, sock, [sock, tail] {
      sock->write(tail);
      sock->disconnectFromHost();
    });
  }
  Mode m_mode = Mode::Stream;
  int m_requestCount = 0;
  QTcpServer m_server;
  QPointer<QTcpSocket> m_paused;
  int m_requests = 0;
};

class TestAiAssistDock : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();
  void unconfiguredControllerShowsDisabledState();
  void suggestedQuestionsOpenEditableDrafts();
  void openingAgentRevealsComposerInScrolledDock();
  void fileDropUsesCurrentProjectAndPreservesDraft();
  void filePickerOpensWithoutModelAndRejectsChangedProject();
  void streamingRendersIntoTranscript();
  void completedTableIsReadableWhileReplyStreams();
  void toolCallRendersPlaceholderCard();
  void errorStateIsRenderedWithReason();
  void markdownAnswerIsFormattedAfterStreamEnds();
  void sessionSwitchRoundTripsContentAndScroll();
  void renameAndDeleteKeepComboInSync();
  void copyButtonPutsSelectionOrWholeTranscriptOnClipboard();
  void resendIssuesNewRequestWithoutDuplicatingHistory();
  void captureLedgerScreenshots();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  LlmConfig makeConfig(const QUrl &endpoint) const {
    LlmConfig config;
    config.endpoint = endpoint;
    config.model = QStringLiteral("test-model");
    config.stream = true;
    config.timeoutMs = 5000;
    config.apiKey = "sk-test-fixture";
    return config;
  }
};

void TestAiAssistDock::initTestCase() {
  PaleoTheme::pinRenderEnvironment(); // 字体/主题钉死（渲染断言稳定）
  // 会话存储重定向 .qttest 沙箱：Windows 上 XDG env 不改写 QStandardPaths，
  // 不重定向的话 combo/列表断言会被本机真实会话存量污染。
  QStandardPaths::setTestModeEnabled(true);
  cleanup();
}

void TestAiAssistDock::cleanup() {
  // QtTest 即使用例提前退出也会调用 cleanup，避免残留会话污染计数断言。
  for (const QString &id : ChatSessionStore::listIds()) {
    QString error;
    QVERIFY2(ChatSessionStore::remove(id, &error), qPrintable(error));
  }
}

void TestAiAssistDock::unconfiguredControllerShowsDisabledState() {
  AiChatController controller; // 未配置：端点/模型/密钥都没有
  AiAssistDock dock(&controller);
  dock.show();
  auto *status = dock.findChild<QLabel *>(QStringLiteral("aiAssistantStatus"));
  auto *send = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *stop = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantStop"));
  QVERIFY(status && send && input && stop);
  // 诚实禁用态：状态行写明原因，输入与发送都不可用——不冒充能用。
  QVERIFY2(status->text().contains(QStringLiteral("尚未配置")),
           qPrintable(status->text()));
  QVERIFY(!send->isEnabled());
  QVERIFY(!input->isEnabled());
  QVERIFY(!stop->isEnabled());
  auto *empty = dock.findChild<QLabel *>(QStringLiteral("aiAssistantEmptyState"));
  auto *configure = dock.findChild<QPushButton *>("aiAssistantWelcomeConfigure");
  auto *localImport = dock.findChild<QPushButton *>("aiAssistantWelcomeImport");
  QVERIFY(empty && configure && localImport);
  QVERIFY(empty->isVisible());
  QVERIFY2(empty->text().contains(QStringLiteral("配置")), qPrintable(empty->text()));
  QVERIFY(configure->isVisible() && !localImport->isVisible());
  QSignalSpy configureRequested(&dock, &AiAssistDock::configureRequested);
  QTest::mouseClick(configure, Qt::LeftButton);
  QCOMPARE(configureRequested.count(), 1);
  dock.setFileImportContext(7, {});
  QTRY_VERIFY(localImport->isVisible() && localImport->isEnabled());
}

void TestAiAssistDock::suggestedQuestionsOpenEditableDrafts() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::PausedMarkdown));
  AiChatController controller;
  auto config = makeConfig(server.endpoint());
  config.timeoutMs = 30000;
  controller.setConfig(config);
  AiAssistDock dock(&controller);
  dock.setFileImportContext(1, {});
  dock.resize(400, 650);
  dock.show();
  dock.activateWindow();
  auto *project = dock.findChild<QPushButton *>("aiAssistantProjectPrompt");
  auto *tools = dock.findChild<QPushButton *>("aiAssistantToolsPrompt");
  auto *input = dock.findChild<QTextEdit *>("aiAssistantInput");
  auto *stop = dock.findChild<QPushButton *>("aiAssistantStop");
  auto *activity = dock.findChild<QProgressBar *>("aiAssistantActivity");
  QVERIFY(project && tools && input && stop && activity);
  QTRY_VERIFY(project->isVisible() && tools->isVisible());
  QVERIFY(!activity->isVisible());
  QTest::mouseClick(project, Qt::LeftButton);
  QTRY_VERIFY(input->hasFocus());
  const QString projectDraft = input->toPlainText();
  QVERIFY(!projectDraft.trimmed().isEmpty());
  QCOMPARE(server.requestCount(), 0);
  QVERIFY(!project->isVisible() && !tools->isVisible());
  input->clear();
  QTRY_VERIFY(tools->isVisible());
  QTest::mouseClick(tools, Qt::LeftButton);
  const QString toolsDraft = input->toPlainText();
  QVERIFY(!toolsDraft.trimmed().isEmpty() && toolsDraft != projectDraft);
  QVERIFY(controller.messages().isEmpty());
  QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
  QVERIFY(input->toPlainText().contains(QLatin1Char('\n')));
  QVERIFY(!controller.streaming());
  QTest::keyClick(input, Qt::Key_Return);
  QTRY_COMPARE(server.requestCount(), 1);
  QVERIFY(controller.streaming());
  QTRY_VERIFY(activity->isVisible() && stop->isVisible() && stop->isEnabled());
  QCOMPARE(controller.messages().first().content, toolsDraft.trimmed());
  server.finishPausedMarkdown();
  QTRY_VERIFY(!controller.streaming() && !activity->isVisible());
  QVERIFY(!project->isVisible() && !tools->isVisible());
}

void TestAiAssistDock::openingAgentRevealsComposerInScrolledDock() {
  AiChatController controller;
  controller.setConfig(makeConfig(QUrl(QStringLiteral("http://127.0.0.1:1/v1"))));
  QScrollArea host;
  host.resize(320, 240);
  host.setWidgetResizable(true);
  auto *dock = new AiAssistDock(&controller);
  dock->setMinimumHeight(host.height() * 3);
  host.setWidget(dock);
  host.show();
  host.activateWindow();
  auto *input = dock->findChild<QTextEdit *>("aiAssistantInput");
  auto *configure = dock->findChild<QPushButton *>("aiAssistantConfigure");
  QVERIFY(input && configure);
  auto inViewport = [&](QWidget *target) {
    return host.viewport()->rect().contains(target->mapTo(host.viewport(), target->rect().center()));
  };
  QTRY_VERIFY(host.verticalScrollBar()->maximum() > 0);
  host.verticalScrollBar()->setValue(0);
  QVERIFY(!inViewport(input));
  dock->focusComposer();
  QTRY_VERIFY(input->hasFocus() && inViewport(input));
  input->setPlainText(QStringLiteral("draft"));
  controller.setConfig({});
  host.verticalScrollBar()->setValue(0);
  dock->focusComposer();
  QTRY_VERIFY(configure->hasFocus() && inViewport(configure));
  QCOMPARE(input->toPlainText(), QStringLiteral("draft"));
}

void TestAiAssistDock::fileDropUsesCurrentProjectAndPreservesDraft() {
  AiChatController controller;
  controller.setConfig(makeConfig(QUrl(QStringLiteral("http://127.0.0.1:1/v1"))));
  AiAssistDock dock(&controller);
  dock.show();
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *button = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantImportFiles"));
  QVERIFY(input && button);
  QVERIFY(!button->isEnabled());
  input->setPlainText(QStringLiteral("unsent draft"));
  QSignalSpy imports(&dock, &AiAssistDock::importFilesRequested);
  QTemporaryFile source;
  QVERIFY(source.open());
  const QStringList files{source.fileName()};
  QMimeData mime;
  mime.setUrls({QUrl::fromLocalFile(files.front())});
  auto enter = [&] {
    QDragEnterEvent event(QPoint(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(input->viewport(), &event);
    return event.isAccepted();
  };
  auto drop = [&] {
    QDropEvent event(QPointF(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(input->viewport(), &event);
  };
  dock.setFileImportContext(4, {});
  QVERIFY(enter());
  drop();
  QCOMPARE(imports.count(), 1);
  QCOMPARE(imports.first().at(0).toStringList(), files);
  QCOMPARE(imports.first().at(1).toULongLong(), quint64(4));
  QVERIFY(enter());
  dock.setFileImportContext(5, {});
  drop();
  QCOMPARE(imports.count(), 1);
  mime.setUrls({QUrl(QStringLiteral("https://example.invalid/file.las"))});
  QVERIFY(!enter());
  dock.setFileImportContext(5, QStringLiteral("read only"));
  QVERIFY(!button->isEnabled());
  QCOMPARE(button->toolTip(), QStringLiteral("read only"));
  QCOMPARE(input->toPlainText(), QStringLiteral("unsent draft"));
  QVERIFY(controller.messages().isEmpty());
}

void TestAiAssistDock::filePickerOpensWithoutModelAndRejectsChangedProject() {
  const bool old = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  const auto restore = qScopeGuard([old] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, old); });
  AiChatController controller;
  AiAssistDock dock(&controller);
  auto *button = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantImportFiles"));
  auto *starter = dock.findChild<QPushButton *>("aiAssistantWelcomeImport");
  QVERIFY(button && starter);
  QVERIFY(!controller.enabled());
  dock.setFileImportContext(7, {});
  QVERIFY(button->isEnabled());
  QSignalSpy imports(&dock, &AiAssistDock::importFilesRequested);
  QTemporaryFile source;
  QVERIFY(source.open());
  source.close();
  for (bool changeProject : {false, true}) {
    (changeProject ? button : starter)->click();
    auto *picker = dock.findChild<QFileDialog *>();
    QVERIFY(picker);
    QCOMPARE(picker->fileMode(), QFileDialog::ExistingFiles);
    auto *fileName = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    QVERIFY(fileName);
    fileName->setText(source.fileName());
    if (changeProject) dock.setFileImportContext(8, {});
    QMetaObject::invokeMethod(picker, "accept");
    QVERIFY(!picker->isVisible());
    QCOMPARE(imports.count(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  QCOMPARE(imports.first().at(0).toStringList(), QStringList{source.fileName()});
  QCOMPARE(imports.first().at(1).toULongLong(), quint64(7));
  QCOMPARE(controller.messages().size(), 0);
}

void TestAiAssistDock::streamingRendersIntoTranscript() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  dock.show();
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *send = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  QVERIFY(input && send && transcript);
  QVERIFY(!send->isEnabled());
  input->setPlainText(QStringLiteral("  \n "));
  QVERIFY(!send->isEnabled());
  auto *empty = dock.findChild<QLabel *>(QStringLiteral("aiAssistantEmptyState"));
  QVERIFY(empty && empty->isVisible());

  input->setPlainText(QStringLiteral("这段是什么岩性？"));
  QVERIFY(send->isEnabled());
  send->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "流式必须结束（或超时报错）");
  const QString text = transcript->toPlainText();
  QVERIFY2(text.contains(QStringLiteral("砂岩")), qPrintable(text));
  QVERIFY2(text.contains(QStringLiteral("这段是什么岩性？")),
           qPrintable(text)); // 用户那条也要上屏
  QVERIFY(!empty->isVisible());
}

void TestAiAssistDock::completedTableIsReadableWhileReplyStreams() {
  const auto original = PaleoTheme::currentTheme();
  const auto restore = qScopeGuard([original] { PaleoTheme::applyTheme(original); });
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::PausedMarkdown));
  AiChatController controller;
  auto config = makeConfig(server.endpoint());
  config.timeoutMs = 30000;
  controller.setConfig(config);
  AiAssistDock dock(&controller);
  dock.show();
  auto *transcript = dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  QVERIFY(transcript);
  controller.sendUserText(QStringLiteral("列出各井的厚度。"));
  const auto tableVisible = [transcript] {
    const auto cell = transcript->document()->find(QStringLiteral("18.5"));
    return !cell.isNull() && cell.currentTable();
  };
  QVERIFY(spinUntil(tableVisible));
  QVERIFY(controller.streaming());
  const QString beforeStyle = transcript->document()->defaultStyleSheet();
  const auto selectedQuestion = transcript->document()->find(QStringLiteral("各井"));
  QVERIFY(!selectedQuestion.isNull());
  transcript->setTextCursor(selectedQuestion);
  PaleoTheme::applyTheme(original == PaleoTheme::Theme::Dark
                          ? PaleoTheme::Theme::Light : PaleoTheme::Theme::Dark);
  QTRY_VERIFY(transcript->document()->defaultStyleSheet() != beforeStyle);
  QVERIFY(tableVisible());
  QCOMPARE(transcript->textCursor().selectedText(), QStringLiteral("各井"));
  QCOMPARE(transcript->textCursor().currentFrame()->frameFormat().background().color(),
           PaleoTheme::tokens().surfaceAlt);
  AiAssistDock reopened(&controller);
  auto *reopenedTranscript = reopened.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  QVERIFY(reopenedTranscript);
  transcript->setTextCursor(transcript->document()->find(QStringLiteral("18.5")));
  server.finishPausedMarkdown();
  QVERIFY(spinUntil([&controller] { return !controller.streaming(); }));
  QVERIFY(tableVisible());
  QCOMPARE(transcript->textCursor().selectedText(), QStringLiteral("18.5"));
  QCOMPARE(transcript->toPlainText().count(QStringLiteral("18.5")), 1);
  QCOMPARE(transcript->toPlainText().count(QStringLiteral("已完成")), 1);
  QCOMPARE(reopenedTranscript->toPlainText(), transcript->toPlainText());
}

void TestAiAssistDock::toolCallRendersPlaceholderCard() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::ToolCallsOnce));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *cards = dock.findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  QVERIFY(cards);
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("查一下有哪些测井相模型"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&cards] {
             return !cards->findChildren<QFrame *>(
                       QStringLiteral("aiAssistantToolCard")).isEmpty();
           }),
           "工具调用必须出占位卡片");
  // 方向61：卡片两态翻面——未配置远端 → 终态「失败」+ 如实摘要，然后模型
  // 拿到错误结果给出终答（ToolCallsOnce：工具一轮、文字一轮）。
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }, 20000),
           "诚实失败轮必须收敛（错误回灌→终答）");
  const auto cardList =
    cards->findChildren<QFrame *>(QStringLiteral("aiAssistantToolCard"));
  QCOMPARE(cardList.size(), 1); // 同 id 帧不叠卡
  QString cardText;
  for (QLabel *label : cardList.first()->findChildren<QLabel *>())
    cardText += label->text() + QLatin1Char('\n');
  QVERIFY2(cardText.contains(QStringLiteral("失败")), qPrintable(cardText));
  QVERIFY2(cardText.contains(QStringLiteral("well_endpoint_unavailable")), qPrintable(cardText));
  QCOMPARE(server.requestCount(), 2);
}

void TestAiAssistDock::errorStateIsRenderedWithReason() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Unauthorized));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("hi"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([transcript] {
             return transcript->toPlainText().contains(QStringLiteral("密钥"));
           }),
           qPrintable(transcript->toPlainText()));
  QVERIFY(!controller.streaming());
}

// 流式回复和历史回放应产生相同的文档结构，包括标题、表格和列表。
void TestAiAssistDock::markdownAnswerIsFormattedAfterStreamEnds() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Markdown));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  const QString secondParagraph = QStringLiteral("请按井列出数值，并标注仍需人工确认的地层界面。");
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))->setPlainText(
      kComparisonQuestion + QLatin1Char('\n') + secondParagraph);
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "markdown 流必须结束");
  const QString sessionId = controller.session().id;
  const QString liveText = transcript->toPlainText();
  for (const bool replay : {false, true}) {
    if (replay) {
      controller.newSession();
      QVERIFY(transcript->toPlainText().isEmpty());
      QVERIFY(controller.loadSession(sessionId));
      QCOMPARE(transcript->toPlainText(), liveText);
    }
    QTextDocument *doc = transcript->document();
    const auto question = doc->find(kComparisonQuestion);
    const auto heading = doc->find(QStringLiteral("S1 砂岩厚度对比"));
    QVERIFY(!question.isNull() && !heading.isNull());
    QCOMPARE(question.block().text(), kComparisonQuestion);
    const auto *userFrame = question.currentFrame();
    QVERIFY(userFrame != doc->rootFrame());
    QCOMPARE(userFrame->frameFormat().background().color(), PaleoTheme::tokens().surfaceAlt);
    QCOMPARE(doc->find(secondParagraph).currentFrame(), userFrame);
    const auto agent = doc->find(QStringLiteral("AI Agent"));
    QVERIFY(!agent.isNull());
    QCOMPARE(agent.currentFrame(), doc->rootFrame());
    QCOMPARE(agent.blockFormat().headingLevel(), 2);
    QCOMPARE(heading.blockFormat().headingLevel(), 2);
    QVERIFY(question.blockNumber() < heading.blockNumber());
    const auto cell = doc->find(QStringLiteral("粗砂岩"));
    QVERIFY(!cell.isNull());
    const auto *table = cell.currentTable();
    QVERIFY(table);
    QCOMPARE(table->rows(), 3);
    QCOMPARE(table->columns(), 3);
    QVERIFY(doc->find(QStringLiteral("复核 B2 井的薄层识别。")).currentList());
    const auto quote = doc->find(QStringLiteral("两井均采用 S1 顶、底界内的井深厚度。"));
    QVERIFY(!quote.isNull());
    QCOMPARE(quote.block().text().trimmed(), quote.selectedText());
    QVERIFY(!doc->find(QStringLiteral("delta_m = 18.5 - 12.0")).isNull());
    QVERIFY(doc->find(QStringLiteral("```")).isNull());
    QVERIFY(doc->find(QStringLiteral("**")).isNull());
  }
}

// 方向62：会话切换 round-trip——openSessionRequested 真实 emit + 装配侧
// load 接线（测试内模拟宿主的同一连接），内容回放一致、滚动位置还原。
void TestAiAssistDock::sessionSwitchRoundTripsContentAndScroll() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  auto *combo =
    dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  QVERIFY(transcript && combo);
  const QString idA = controller.session().id;

  // 会话 A 跑一轮（finished 即落盘）。
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "会话 A 的一轮必须结束");
  QVERIFY2(transcript->toPlainText().contains(QStringLiteral("砂岩")),
           qPrintable(transcript->toPlainText()));

  // 宿主接线（生产在 paleomainwindow_attach_shell，测试同路径连接）。
  connect(&dock, &AiAssistDock::openSessionRequested, &controller,
          [&controller](const QString &id) { controller.loadSession(id); });

  // 新会话：A 进列表（当前未命名固定首位）。
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantNew"))->click();
  QCOMPARE(combo->count(), 2); // 未命名（当前）+ A
  QVERIFY(transcript->toPlainText().isEmpty()); // 新会话清屏

  // 用户在拉列表里选 A：activated → openSessionRequested → load → 回放。
  QSignalSpy openSpy(&dock, &AiAssistDock::openSessionRequested);
  QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1));
  QCOMPARE(openSpy.size(), 1);
  QCOMPARE(openSpy.at(0).at(0).toString(), idA);
  QCOMPARE(controller.session().id, idA);
  const QString replayed = transcript->toPlainText();
  QVERIFY2(replayed.contains(QStringLiteral("这段是什么岩性？")),
           qPrintable(replayed));
  QVERIFY2(replayed.contains(QStringLiteral("砂岩")), qPrintable(replayed));

  // 滚动位置还原：长会话 B 滚到中间，切走再切回，位置一致。
  ChatSession longSession;
  longSession.id = ChatSessionStore::newSessionId();
  longSession.title = QStringLiteral("长会话");
  longSession.createdAt = longSession.updatedAt = QDateTime::currentDateTime();
  ChatMessage long1;
  long1.role = ChatRole::Assistant;
  long1.content = QStringLiteral("分层描述：\n");
  for (int i = 0; i < 60; ++i)
    long1.content += QStringLiteral("第%1层：砂岩与泥岩互层，厚度约 %2 米。\n")
                       .arg(i + 1)
                       .arg(120 - i);
  longSession.messages.append(long1);
  QString error;
  QVERIFY2(ChatSessionStore::save(longSession, &error), qPrintable(error));

  dock.resize(300, 160);
  dock.show();
  controller.loadSession(longSession.id);
  QScrollBar *scroll = transcript->verticalScrollBar();
  const int stored = qMax(1, scroll->maximum() / 2);
  scroll->setValue(stored);
  const int effective = scroll->value(); // 布局未撑开时为 0（断言仍成立）
  controller.loadSession(idA);
  controller.loadSession(longSession.id);
  QCOMPARE(scroll->value(), effective);
  QVERIFY(transcript->toPlainText().contains(QStringLiteral("第1层")));
}

// 方向62：重命名/删除后下拉同步（sessionListChanged → refreshSessions）。
void TestAiAssistDock::renameAndDeleteKeepComboInSync() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *combo =
    dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  const QString idA = controller.session().id;
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "一轮必须结束（落盘后才能出现在列表里）");
  QCOMPARE(combo->count(), 1); // 只有 A（空的新会话不落盘不进列表）

  QString error;
  QVERIFY(controller.renameSession(idA, QStringLiteral("岩性讨论"), &error));
  QCOMPARE(combo->itemText(0), QStringLiteral("岩性讨论"));
  ChatSession stored;
  QVERIFY(ChatSessionStore::load(idA, &stored, &error));
  QCOMPARE(stored.title, QStringLiteral("岩性讨论"));

  // 删除当前会话：列表回到未命名新会话，文件消失。
  QVERIFY(controller.removeSession(idA, &error));
  QVERIFY(!ChatSessionStore::load(idA, &stored, &error));
  QCOMPARE(combo->count(), 1);
  QVERIFY(combo->itemText(0).contains(QStringLiteral("未命名")));
}

// 方向62：复制——有选区复制选区（单条/局部），无选区复制整段。
void TestAiAssistDock::copyButtonPutsSelectionOrWholeTranscriptOnClipboard() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  auto *more = dock.findChild<QToolButton *>(QStringLiteral("aiAssistantMore"));
  auto *copy = dock.findChild<QAction *>(QStringLiteral("aiAssistantCopy"));
  QVERIFY(more && more->menu() && copy && more->menu()->actions().contains(copy));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "一轮必须结束");
  QVERIFY(copy->isEnabled());

  // 无选区：整段（用户消息 + 回答都在）。
  copy->trigger();
  QVERIFY(QApplication::clipboard()->mimeData()->hasHtml());
  const QString whole = QApplication::clipboard()->text();
  QVERIFY2(whole.contains(QStringLiteral("这段是什么岩性？")), qPrintable(whole));
  QVERIFY2(whole.contains(QStringLiteral("砂岩")), qPrintable(whole));

  // 有选区：只复制选中的两个字。
  const auto cursor = transcript->document()->find(QStringLiteral("岩性"));
  QVERIFY(!cursor.isNull());
  transcript->setTextCursor(cursor);
  copy->trigger();
  QVERIFY(QApplication::clipboard()->mimeData()->hasHtml());
  QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("岩性"));
}

// 方向62：重发最后一条用户消息——新请求发出（服务器计数 +1），历史不
// 重复（无第二条 user 消息），transcript 只有一份回答。
void TestAiAssistDock::resendIssuesNewRequestWithoutDuplicatingHistory() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  auto *more = dock.findChild<QToolButton *>(QStringLiteral("aiAssistantMore"));
  auto *resend = dock.findChild<QAction *>(QStringLiteral("aiAssistantResend"));
  QVERIFY(more && more->menu() && resend && more->menu()->actions().contains(resend));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "第一轮必须结束");
  QCOMPARE(server.requestCount(), 1);
  // 本文件假服务器的 Stream 模式答案 = kChunkA 两段（「砂岩砂岩」）。
  const QString firstAnswer = controller.messages().last().content;
  QVERIFY(!firstAnswer.isEmpty());
  QVERIFY(resend->isEnabled());
  resend->trigger();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "重发的一轮必须结束");
  QCOMPARE(server.requestCount(), 2);
  QCOMPARE(controller.messages().size(), 2); // user + assistant（无重复 user）
  QCOMPARE(controller.messages().last().content, firstAnswer); // 同源同答
  QCOMPARE(transcript->toPlainText().count(firstAnswer), 1);   // 旧回答被截掉
}

// 可选截图沿用实际面板，覆盖现有内容换主题和收窄后的排版。
void TestAiAssistDock::captureLedgerScreenshots() {
  const QString dir =
    QString::fromUtf8(qgetenv("PALEO_AI_SHOT_DIR"));
  if (dir.isEmpty())
    QSKIP("截图按需采集（PALEO_AI_SHOT_DIR 未设）");
  QVERIFY(QDir().mkpath(dir));
  const auto originalTheme = PaleoTheme::currentTheme();
  const auto restoreTheme = qScopeGuard([originalTheme] { PaleoTheme::applyTheme(originalTheme); });
  PaleoTheme::applyLightTheme();
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Markdown));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  dock.resize(400, 640);
  dock.show();
  QCoreApplication::processEvents();
  QVERIFY(dock.grab().save(QDir(dir).filePath(QStringLiteral("ai-agent-welcome.png"))));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))->setPlainText(kComparisonQuestion);
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "markdown 流必须结束（定型渲染后截图）");
  auto *transcript = dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  QVERIFY(transcript);
  auto *context = dock.findChild<AiContextBar *>();
  QVERIFY(context);
  context->present(QStringLiteral("示例工区 · S1 层段"), false, {}, true, false,
                   QStringLiteral("S1"), QStringLiteral("S1"), QStringLiteral("sample"), 1, {}, {});
  dock.setFileImportContext(1, {});
  auto *composer = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  QVERIFY(composer);
  composer->setPlainText(QStringLiteral("列出这两口井的有效厚度区间。"));
  dock.resize(720, 640);
  dock.show();
  composer->setFocus();
  const auto capture = [&](const QString &name) {
    QCoreApplication::processEvents();
    transcript->verticalScrollBar()->setValue(0);
    return dock.grab().save(QDir(dir).filePath(name));
  };
  QVERIFY(capture(QStringLiteral("ai-ux-dock-markdown.png")));
  dock.resize(400, 520);
  QVERIFY(capture(QStringLiteral("ai-ux-dock-narrow.png")));
  QCOMPARE(dock.width(), 400);
  PaleoTheme::applyDarkTheme();
  dock.resize(720, 640);
  QVERIFY(capture(QStringLiteral("ai-ux-dock-dark.png")));
}

QTEST_MAIN(TestAiAssistDock)
#include "tst_aiassistdock.moc"
