// 层：测试壳
#include <QtTest>
#include "app/aiconversationwiring.h"
#include "app/capabilityhostwiring.h"
#include "../src/ai/chat/llmclient.h"
#include "../src/app/appcontext.h"
#include "../src/ui/ai/aiconversationcard.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/ai/aicontextbar.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aioperationpresentation.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QTranslator>
#include "../src/ui/paleotheme.h"

// G5 W9：真实装配路径验证——AppContext + PaleoMainWindow +
// attachAiAssistant + installConversationPresentation 全链。普通 QWidget
// offscreen 测试不替代这条真实主窗装配证据；PALEO_G5_SHOT_DIR 设置时
// 走查真实 AI 页签（发送→实时结果→历史恢复→操作详情）并落浅/暗/窄截图。
namespace {
const char kShotStream[] =
  "data: {\"choices\":[{\"delta\":{\"content\":\"砂岩为主，泥岩次之\"}}]}\n\n"
  "data: [DONE]\n\n";

class FakeLlmServer : public QObject {
  Q_OBJECT
public:
  bool start() {
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { serve(m_server.nextPendingConnection()); });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }

private:
  void serve(QTcpSocket *sock) {
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    auto buf = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [sock, buf] {
      buf->append(sock->readAll());
      const int headerEnd = buf->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      const QString header = QString::fromUtf8(buf->left(headerEnd));
      const int contentLength =
        header.section("Content-Length:", 1, 1).section("\r", 0, 0).trimmed().toInt();
      if (buf->mid(headerEnd + 4).size() < contentLength)
        return;
      sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                  "Content-Type: text/event-stream\r\n\r\n");
      QTimer::singleShot(20, sock, [sock] {
        sock->write(kShotStream);
        sock->disconnectFromHost();
      });
    });
  }
  QTcpServer m_server;
};
bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < timeoutMs) {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return done();
}
} // namespace

class TestG5Assembly : public QObject {
  Q_OBJECT
public:
  explicit TestG5Assembly(AppContext *ctx, PaleoMainWindow *win)
    : m_ctx(ctx), m_win(win) {}

private slots:
  void dockAndPresenterAreWiredIntoTheRealWindow();
  void contextBarReflectsHostContext();
  void presenterIsIdempotentOnReinstall();
  void captureAssemblyScreenshots();

private:
  AppContext *m_ctx = nullptr;
  PaleoMainWindow *m_win = nullptr;
};

void TestG5Assembly::dockAndPresenterAreWiredIntoTheRealWindow() {
  auto *tabs = m_win->findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  QVERIFY(tabs);
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  QVERIFY2(dock, "attachAiAssistant 必须已建 AI 助手页签");
  QVERIFY(tabs->indexOf(dock) >= 0);
  QVERIFY(dock->controller() == m_ctx->aiChat());
  auto *presenter =
    m_win->findChild<paleo::chat::AiOperationPresentation *>(
      QStringLiteral("aiOperationPresenter"));
  QVERIFY2(presenter, "installConversationPresentation 必须注入 adapter");
  // 装配依赖来自真实组装根（不是测试自建服务）：adapter 生命周期挂主窗。
  QVERIFY(presenter->parent() == m_win);
  QVERIFY(presenter->currentContext().chatSessionId == m_ctx->aiChat()->session().id);
  // 未打开工程：上下文如实不可用，面板历史仍可读（不崩溃、不冒充）。
  auto *summary = dock->findChild<QLabel *>(QStringLiteral("aiContextSummary"));
  QVERIFY(summary);
  QVERIFY2(summary->text().contains(QStringLiteral("工程未打开")),
           qPrintable(summary->text()));
}

void TestG5Assembly::contextBarReflectsHostContext() {
  auto *presenter =
    m_win->findChild<paleo::chat::AiOperationPresentation *>(
      QStringLiteral("aiOperationPresenter"));
  QVERIFY(presenter);
  const auto view = presenter->currentContext();
  QVERIFY(!view.chatSessionId.isEmpty()); // 真实 controller 会话 id
  // 本用例不打开工程（真实工程打开路径属 tst_aiwiring 换工程回归）。
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  QVERIFY(dock);
  QVERIFY(dock->findChild<AiContextBar *>(QStringLiteral("aiAssistantContextBar")));
}

void TestG5Assembly::presenterIsIdempotentOnReinstall() {
  auto *first = installConversationPresentation(
    m_win, m_ctx->aiChat(), m_ctx->capabilityHost(), m_ctx->documentSession());
  auto *second = installConversationPresentation(
    m_win, m_ctx->aiChat(), m_ctx->capabilityHost(), m_ctx->documentSession());
  QCOMPARE(first, second); // 复用同一实例：重复装配不重复订阅
  const auto presenters =
    m_win->findChildren<paleo::chat::AiOperationPresentation *>();
  QCOMPARE(presenters.size(), 1);
}

void TestG5Assembly::captureAssemblyScreenshots() {
  const QString dir = QString::fromUtf8(qgetenv("PALEO_G5_SHOT_DIR"));
  if (dir.isEmpty())
    QSKIP("截图按需采集（PALEO_G5_SHOT_DIR 未设）");
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  QVERIFY(dock);
  auto *tabs = m_win->findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  tabs->setCurrentWidget(dock);
  m_win->resize(1280, 800);
  m_win->show();

  // 真实主窗走查（W9.4）：上下文→发送（实时工具卡）→落盘→历史恢复→操作详情。
  // 端点为 127.0.0.1 loopback（真实 controller/runner/渲染链，不发外网）。
  FakeLlmServer server;
  QVERIFY(server.start());
  LlmConfig config;
  config.endpoint = server.endpoint();
  config.model = QStringLiteral("g5-shot-model");
  config.stream = true;
  config.timeoutMs = 5000;
  config.apiKey = "sk-g5-shot";
  m_ctx->aiChat()->setConfig(config);
  auto *input = dock->findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *send = dock->findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  auto *cards = dock->findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  QVERIFY(input && send && cards);
  auto *transcript =
    dock->findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  QVERIFY(transcript);
  input->setPlainText(QStringLiteral("列表对比一下岩性"));
  send->click();
  QVERIFY2(spinUntil([this] { return !m_ctx->aiChat()->streaming(); }),
           "走查：实时一轮必须完成");
  QVERIFY2(transcript->toPlainText().contains(QStringLiteral("砂岩")),
           qPrintable(transcript->toPlainText()));

  // 历史会话注入（operation 三轴卡 + 详情入口；宿主未开工程 → 刷新被
  // adapter 如实拒绝，UI 呈现拒绝原因——这本身是走查断言的一部分）。
  ChatSession seeded;
  seeded.id = ChatSessionStore::newSessionId();
  seeded.title = QStringLiteral("G5 操作走查");
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QStringLiteral("上次的重分类做完了吗");
  user.hostTurnId = QStringLiteral("turn-shot");
  ChatMessage assistant;
  assistant.role = ChatRole::Assistant;
  ChatToolCall call;
  call.id = QStringLiteral("call-shot");
  call.name = QStringLiteral("paleo.tile_classification_v2");
  call.argumentsJson = QStringLiteral("{}");
  assistant.toolCalls.append(call);
  ChatMessage tool;
  tool.role = ChatRole::Tool;
  tool.toolCallId = call.id;
  tool.content = QStringLiteral(
    R"({"contractVersion":"paleo.tool-result.v1","status":"succeeded","code":"ok",)"
    R"("payloadContract":"legacy.unversioned",)"
    R"("transport":{"status":"complete","code":"ok"},"coverage":{"complete":true},)"
    R"("data":{"contract":"paleo.operation.v2","ok":true,"code":"ok",)"
    R"("operationId":"op-g5-shot","execution":"succeeded","commitState":"committed",)"
    R"("presentation":"pending","revision":"9","computeCount":1,)"
    R"("cancelRequested":false,"commitReceiptRef":"rcpt-shot"}})");
  seeded.messages << user << assistant << tool;
  QString error;
  QVERIFY2(ChatSessionStore::save(seeded, &error), qPrintable(error));
  m_ctx->aiChat()->loadSession(seeded.id);
  QVERIFY2(spinUntil([cards] {
             return !cards->findChildren<AiConversationCard *>(
                       QStringLiteral("aiConversationCard"))
                       .isEmpty();
           }),
           "走查：历史操作卡必须恢复");
  const auto historyCards = cards->findChildren<AiConversationCard *>(
    QStringLiteral("aiConversationCard"));
  historyCards.first()->setExpanded(true);
  auto *detailButton = historyCards.first()->findChild<QPushButton *>(
    QStringLiteral("aiCardOperationDetail"));
  QVERIFY2(detailButton, "走查：operation 历史卡必须提供详情入口");
  QTest::mouseClick(detailButton, Qt::LeftButton);
  QVERIFY2(spinUntil([dock] {
             return !dock->findChild<QWidget *>(QStringLiteral("aiOperationDetail"))
                       ->isHidden();
           }),
           "走查：操作详情面必须打开");

  PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  QVERIFY2(m_win->grab().save(dir + QStringLiteral("/g5-assembly-light.png")),
           "浅色主窗截图失败");
  PaleoTheme::applyTheme(PaleoTheme::Theme::Dark);
  QVERIFY2(m_win->grab().save(dir + QStringLiteral("/g5-assembly-dark.png")),
           "暗色主窗截图失败");
  PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  m_win->resize(720, 600);
  QVERIFY2(m_win->grab().save(dir + QStringLiteral("/g5-assembly-narrow.png")),
           "窄窗主窗截图失败");
}

int main(int argc, char *argv[]) {
#ifdef PALEO_TEST_HAS_TRANSLATIONS
  Q_INIT_RESOURCE(translations);
#endif
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  static QTemporaryDir settingsDir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
  AppContext ctx(qEnvironmentVariable("QGIS_PREFIX_PATH"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");
  PaleoMainWindow win(ctx.canvasCtl(), ctx.projectSvc(), ctx.layerSvc(),
                      ctx.toolSvc(), ctx.selection());
  win.attachWorkflows(ctx.predictionWf(), ctx.constraintWf(), ctx.compositionWf(),
                      ctx.validationWf(), ctx.importSvc(), ctx.seismicLink(),
                      ctx.processingSvc(), ctx.store(), ctx.editingSvc(),
                      ctx.layoutSvc(), ctx.taskSvc());
  win.attachDocumentSession(ctx.documentSession());
  bindCapabilityDispatcher(win.commandDispatcher(), ctx.capabilityHost());
  win.attachAiAssistant(ctx.aiChat());
  // 生产接线同 main.cpp：G5 adapter 装配。
  installConversationPresentation(&win, ctx.aiChat(), ctx.capabilityHost(),
                                  ctx.documentSession());
  TestG5Assembly test(&ctx, &win);
  return QTest::qExec(&test, argc, argv);
}

#include "tst_g5_assembly.moc"
