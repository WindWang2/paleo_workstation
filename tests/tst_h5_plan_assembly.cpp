// 层：测试壳
#include <QtTest>
#include "app/aiconversationwiring.h"
#include "app/capabilityhostwiring.h"
#include "../src/ai/chat/llmclient.h"
#include "../src/app/appcontext.h"
#include "../src/ui/ai/aiconversationcard.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/ai/aiplanreviewwidget.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aiplanpresentation.h"
#include "../src/workflow/aioperationpresentation.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
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
#include <QListWidget>
#include <QTableWidget>
#include "../src/ui/paleotheme.h"
#include "../src/ai/chat/chatsession.h"

// H5 W9：真实装配路径验证——AppContext + PaleoMainWindow + attachAiAssistant +
// installConversationPresentation（内部追加 installPlanWorkbench）全链。
// 真实 controller → runner → 卡片 → 审阅面板 用 127.0.0.1 loopback 的确定性
// plan preview tool call 走通（无真实云模型/凭据/外网）。
namespace {

QByteArray sseEvent(const QJsonObject &event) {
  return "data: " + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n";
}
QByteArray doneTail() {
  QJsonObject usage;
  usage.insert(QStringLiteral("prompt_tokens"), 80);
  usage.insert(QStringLiteral("completion_tokens"), 30);
  QJsonObject choice;
  choice.insert(QStringLiteral("delta"), QJsonObject());
  QJsonObject event;
  event.insert(QStringLiteral("choices"), QJsonArray{choice});
  event.insert(QStringLiteral("usage"), usage);
  return sseEvent(event) + QByteArrayLiteral("data: [DONE]\n\n");
}

QString planPreviewArguments() {
  QJsonArray points;
  for (int i = 0; i < 4; ++i)
    points.append(QJsonObject{{"x", (i % 2) * 100.0}, {"y", (i / 2) * 100.0},
                              {"z", (i / 2) * 10.0}, {"well_id", QString::number(i)}});
  const QJsonObject idw{{"id", "idw"},
                        {"capability", "constraint.idw"},
                        {"contract_version", 1},
                        {"output_port", "OUTPUT"},
                        {"intent", "compute-only"},
                        {"parameters", QJsonObject{{"FIELD", "z"}, {"CELL_SIZE", 25.0}}},
                        {"inputs", QJsonObject{{"points", points},
                                               {"subinterval_id", "S1"}}};
  const QJsonObject contour{{"id", "contours"},
                            {"capability", "singlefactor.contours"},
                            {"contract_version", 1},
                            {"output_port", "LINES"},
                            {"parameters", QJsonObject{{"CONTOUR_LEVEL", 5.0}}},
                            {"inputs", QJsonObject{}}};
  const QJsonObject arguments{
    {"title", QStringLiteral("装配走查计划")},
    {"nodes", QJsonArray{idw, contour}},
    {"edges", QJsonArray{QJsonObject{{"from", "idw"}, {"from_port", "OUTPUT"},
                                     {"to", "contours"}, {"to_port", "RASTER"}}}}};
  return QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
}

// 脚本化假端点：请求 1 回 plan preview tool_calls，请求 2 回终答文本。
class ScriptedLlmServer : public QObject {
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
  int requestCount() const { return m_bodies.size(); }
  QJsonObject body(int index) const { return m_bodies.value(index); }

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
        header.section("Content-Length:", 1, 1, QString::SectionCaseInsensitiveSeps)
          .section("\r", 0, 0)
          .trimmed()
          .toInt();
      if (buf->mid(headerEnd + 4).size() < contentLength)
        return;
      const QByteArray bodyBytes = buf->mid(headerEnd + 4, contentLength);
      m_bodies.append(QJsonDocument::fromJson(bodyBytes).object());
      QByteArray response;
      if (m_bodies.size() == 1) {
        QJsonObject function;
        function.insert(QStringLiteral("name"), QStringLiteral("paleo.workflow_plan_preview"));
        function.insert(QStringLiteral("arguments"), planPreviewArguments());
        QJsonObject call;
        call.insert(QStringLiteral("index"), 0);
        call.insert(QStringLiteral("id"), QStringLiteral("call-h5-assembly"));
        call.insert(QStringLiteral("type"), QStringLiteral("function"));
        call.insert(QStringLiteral("function"), function);
        QJsonObject delta;
        delta.insert(QStringLiteral("tool_calls"), QJsonArray{call});
        QJsonObject choice;
        choice.insert(QStringLiteral("delta"), delta);
        response = sseEvent(QJsonObject{{QStringLiteral("choices"), QJsonArray{choice}}}) + doneTail();
      } else {
        QJsonObject delta;
        delta.insert(QStringLiteral("content"), QStringLiteral("已提出两步计划，请在面板中审阅。"));
        QJsonObject choice;
        choice.insert(QStringLiteral("delta"), delta);
        response = sseEvent(QJsonObject{{QStringLiteral("choices"), QJsonArray{choice}}}) + doneTail();
      }
      sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                  "Content-Type: text/event-stream\r\n\r\n");
      QTimer::singleShot(15, sock, [sock, response] {
        sock->write(response);
        sock->flush();
        sock->disconnectFromHost();
      });
    });
  }
  QTcpServer m_server;
  QVector<QJsonObject> m_bodies;
};

bool spinUntil(const std::function<bool()> &done, int timeoutMs = 20000) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < timeoutMs) {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return done();
}
} // namespace

class TestH5PlanAssembly : public QObject {
  Q_OBJECT
public:
  explicit TestH5PlanAssembly(AppContext *ctx, PaleoMainWindow *win)
    : m_ctx(ctx), m_win(win) {}

private slots:
  void planWorkbenchIsWiredIntoTheRealWindow();
  void reinstallIsIdempotentForBothPresenters();
  void liveToolCallProducesReviewableCard();
  void captureAssemblyScreenshots();

private:
  AppContext *m_ctx = nullptr;
  PaleoMainWindow *m_win = nullptr;
};

void TestH5PlanAssembly::planWorkbenchIsWiredIntoTheRealWindow() {
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  QVERIFY2(dock, "attachAiAssistant 必须已建 AI 助手页签");
  auto *presenter = m_win->findChild<paleo::chat::AiPlanPresentation *>(
    QStringLiteral("aiPlanPresenter"));
  QVERIFY2(presenter, "installConversationPresentation 必须追加计划装配");
  QVERIFY(presenter->parent() == m_win);
  QVERIFY(dock->findChild<AiPlanReviewWidget *>(QStringLiteral("aiPlanReview")));
}

void TestH5PlanAssembly::reinstallIsIdempotentForBothPresenters() {
  const auto first = installConversationPresentation(
    m_win, m_ctx->aiChat(), m_ctx->capabilityHost(), m_ctx->documentSession());
  const auto second = installConversationPresentation(
    m_win, m_ctx->aiChat(), m_ctx->capabilityHost(), m_ctx->documentSession());
  QCOMPARE(first, second);
  QCOMPARE(m_win->findChildren<paleo::chat::AiPlanPresentation *>().size(), 1);
  QCOMPARE(m_win->findChildren<paleo::chat::AiOperationPresentation *>().size(), 1);
}

void TestH5PlanAssembly::liveToolCallProducesReviewableCard() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  LlmConfig config;
  config.endpoint = server.endpoint();
  config.model = QStringLiteral("h5-assembly-model");
  config.stream = true;
  config.timeoutMs = 8000;
  config.apiKey = "sk-h5-assembly";
  m_ctx->aiChat()->setConfig(config);
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  QVERIFY(dock);
  auto *input = dock->findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *send = dock->findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  QVERIFY(input && send);
  input->setPlainText(QStringLiteral("帮我构造一张两口井的构造图计划"));
  send->click();
  QVERIFY2(spinUntil([this, &server] {
    return server.requestCount() >= 2 && !m_ctx->aiChat()->streaming();
  }), "走查：tool_call 轮 + 终答轮必须完成");
  // 工具确实在协商表里上送（请求 1 的 tools[]）。
  const auto tools = server.body(0).value(QStringLiteral("tools")).toArray();
  bool declared = false;
  for (const auto &entry : tools)
    if (entry.toObject().value(QStringLiteral("function")).toObject()
            .value(QStringLiteral("name")).toString() ==
        QStringLiteral("paleo.workflow_plan_preview"))
      declared = true;
  QVERIFY2(declared, "paleo.workflow_plan_preview 必须出现在 tools[] 上送");
  // 请求 2 回灌 role=tool 应答本提案（协议对账）。
  const auto messages = server.body(1).value(QStringLiteral("messages")).toArray();
  bool toolPaired = false;
  for (const auto &entry : messages)
    if (entry.toObject().value(QStringLiteral("role")).toString() == QLatin1String("tool") &&
        entry.toObject().value(QStringLiteral("tool_call_id")).toString() ==
            QLatin1String("call-h5-assembly"))
      toolPaired = true;
  QVERIFY2(toolPaired, "tool_call_id 必须有 role=tool 应答回灌");
  // 实时卡出现且带「审阅此计划」入口；点击打开真实面板。
  auto *cards = dock->findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  QVERIFY(cards);
  QVERIFY2(spinUntil([cards] {
    return !cards->findChildren<AiConversationCard *>().isEmpty();
  }), "走查：实时计划卡必须出现");
  auto *card = cards->findChildren<AiConversationCard *>().first();
  card->setExpanded(true);
  auto *review = card->findChild<QPushButton *>(QStringLiteral("aiCardPlanReview"));
  QVERIFY2(review, "走查：计划卡必须提供审阅入口");
  QTest::mouseClick(review, Qt::LeftButton);
  auto *panel = dock->findChild<AiPlanReviewWidget *>(QStringLiteral("aiPlanReview"));
  QVERIFY2(panel && !panel->isHidden(), "走查：审阅面板必须打开");
  QVERIFY(panel->reviewing());
  // 纯预检入口：打开/审阅期间宿主零执行链（未开工程时计算被拒，预检也需
  // 绑定——面板如实呈现，不冒充）。
  auto *presenter = m_win->findChild<paleo::chat::AiPlanPresentation *>(
    QStringLiteral("aiPlanPresenter"));
  QCOMPARE(presenter->prepareAttempts(), 0u);
  QCOMPARE(presenter->startAttempts(), 0u);
}

void TestH5PlanAssembly::captureAssemblyScreenshots() {
  const QString dir = QString::fromUtf8(qgetenv("PALEO_H5_SHOT_DIR"));
  if (dir.isEmpty())
    QSKIP("截图按需采集（PALEO_H5_SHOT_DIR 未设）");
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  QVERIFY(dock);
  auto *tabs = m_win->findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  tabs->setCurrentWidget(dock);
  m_win->resize(1280, 800);
  m_win->show();

  // 修正无效下游参数 → 有效两步图：直接注入编辑走查（面板真实控件）。
  ScriptedLlmServer server;
  QVERIFY(server.start());
  LlmConfig config;
  config.endpoint = server.endpoint();
  config.model = QStringLiteral("h5-shot-model");
  config.stream = true;
  config.timeoutMs = 8000;
  config.apiKey = "sk-h5-shot";
  m_ctx->aiChat()->setConfig(config);
  auto *input = dock->findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *send = dock->findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  input->setPlainText(QStringLiteral("再提一次计划"));
  send->click();
  QVERIFY2(spinUntil([&server, this] {
    return server.requestCount() >= 2 && !m_ctx->aiChat()->streaming();
  }), "截图走查：两轮必须完成");
  auto *cards = dock->findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  auto *card = cards->findChildren<AiConversationCard *>().first();
  card->setExpanded(true);
  QTest::mouseClick(card->findChild<QPushButton *>(QStringLiteral("aiCardPlanReview")),
                    Qt::LeftButton);
  auto *panel = dock->findChild<AiPlanReviewWidget *>(QStringLiteral("aiPlanReview"));
  QVERIFY(panel && panel->reviewing());
  // 用户修改一个点值（修订走查）。
  auto *points = panel->findChild<QTableWidget *>(QStringLiteral("aiPlanReviewPoints"));
  points->item(0, 2)->setText(QStringLiteral("3.5"));
 PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  QVERIFY(m_win->grab().save(dir + QStringLiteral("/h5-assembly-light.png")));
  PaleoTheme::applyTheme(PaleoTheme::Theme::Dark);
  QVERIFY(m_win->grab().save(dir + QStringLiteral("/h5-assembly-dark.png")));
  PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  m_win->resize(720, 600);
  QVERIFY(m_win->grab().save(dir + QStringLiteral("/h5-assembly-narrow.png")));
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
  // 生产接线同 main.cpp：installConversationPresentation 内部追加计划装配。
  installConversationPresentation(&win, ctx.aiChat(), ctx.capabilityHost(),
                                  ctx.documentSession());
  TestH5PlanAssembly test(&ctx, &win);
  return QTest::qExec(&test, argc, argv);
}

#include "tst_h5_plan_assembly.moc"
