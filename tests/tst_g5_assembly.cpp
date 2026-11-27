// 层：测试壳
#include <QtTest>
#include "app/aiconversationwiring.h"
#include "app/capabilityhostwiring.h"
#include "../src/ai/chat/llmclient.h"
#include "../src/app/appcontext.h"
#include "../src/io/dataimportservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/ui/ai/aiconversationcard.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/ai/aicontextbar.h"
#include "../src/ui/paleodockmanager.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aioperationpresentation.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include "../src/ui/paleotheme.h"

// G5 W9：真实装配路径验证——AppContext + PaleoMainWindow +
// attachAiAssistant + installConversationPresentation 全链。普通 QWidget
// offscreen 测试不替代这条真实主窗装配证据；PALEO_G5_SHOT_DIR 设置时
// 走查独立 Agent 面板（发送→实时结果→历史恢复→操作详情）并落浅/暗/窄截图。
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
  void launcherKeepsDraftAndDocumentAcrossWorkflows();
  void savedLayoutRestoresIndependentAgentDock();
  void agentStartupLayout_data();
  void agentStartupLayout();
  void contextBarReflectsHostContext();
  void presenterIsIdempotentOnReinstall();
  void droppingAFileImportsItIntoTheOpenProjectWithoutAModel();
  void captureAssemblyScreenshots();

private:
  AppContext *m_ctx = nullptr;
  PaleoMainWindow *m_win = nullptr;
};

void TestG5Assembly::dockAndPresenterAreWiredIntoTheRealWindow() {
  auto *tabs = m_win->findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  QVERIFY(tabs);
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  auto *shell = m_win->findChild<QDockWidget *>("aiAgentDock", Qt::FindDirectChildrenOnly);
  QVERIFY(dock && shell && shell->isAncestorOf(dock));
  QVERIFY(!tabs->isAncestorOf(dock));
  QVERIFY(shell->features().testFlag(QDockWidget::DockWidgetFloatable));
  QVERIFY(shell->features().testFlag(QDockWidget::DockWidgetClosable));
  QVERIFY(shell->features().testFlag(QDockWidget::DockWidgetMovable));
  auto *manager = m_win->findChild<PaleoDockManager *>(QString(), Qt::FindDirectChildrenOnly);
  QVERIFY(manager);
  QScopedPointer<QMenu> layoutMenu(manager->createMenu());
  QVERIFY(layoutMenu->actions().contains(shell->toggleViewAction()));
  m_win->attachAiAssistant(m_ctx->aiChat());
  QCOMPARE(m_win->findChildren<AiAssistDock *>().size(), 1);
  QCOMPARE(m_win->findChildren<QDockWidget *>("aiAgentDock").size(), 1);
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

void TestG5Assembly::savedLayoutRestoresIndependentAgentDock() {
  auto *shell = m_win->findChild<QDockWidget *>("aiAgentDock");
  auto *manager = m_win->findChild<PaleoDockManager *>(QString(), Qt::FindDirectChildrenOnly);
  QVERIFY(shell && manager);
  m_win->show();
  const QByteArray originalLayout = m_win->saveState();
  const QVariantMap originalIntent = manager->captureIntent();
  const auto reset = qScopeGuard([&] {
    manager->restoreLayout(originalLayout, &originalIntent);
  });
  m_win->addDockWidget(Qt::LeftDockWidgetArea, shell);
  shell->show();
  shell->raise();
  const QByteArray leftVisible = m_win->saveState();
  const QVariantMap visibleIntent = manager->captureIntent();
  shell->close();
  m_win->addDockWidget(Qt::BottomDockWidgetArea, shell);
  QVERIFY(shell->isHidden());
  QVERIFY(manager->restoreLayout(leftVisible, &visibleIntent));
  QTRY_VERIFY(shell->isVisible());
  QCOMPARE(m_win->dockWidgetArea(shell), Qt::LeftDockWidgetArea);
}

void TestG5Assembly::agentStartupLayout_data() {
  QTest::addColumn<QString>("selected");
  QTest::newRow("layout-before-agent") << QStringLiteral("legacy");
  QTest::newRow("agent-front") << QStringLiteral("aiAgentDock");
  QTest::newRow("inspector-front") << QStringLiteral("pagePanelDock");
}

void TestG5Assembly::agentStartupLayout() {
  QFETCH(QString, selected);
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  QVariantMap backup;
  const QStringList keys{"windowState", "windowDockIntent", "windowGeometry"};
  for (const auto &key : keys) backup.insert(key, settings.value(key));
  const auto reset = qScopeGuard([&] {
    for (const auto &key : keys) {
      if (backup.value(key).isValid()) settings.setValue(key, backup.value(key));
      else settings.remove(key);
    }
  });
  for (const auto &key : keys) settings.remove(key);
  const bool legacy = selected == QLatin1String("legacy");
  if (legacy) {
    QMainWindow previous;
    auto *inspector = new QDockWidget(&previous);
    inspector->setObjectName("pagePanelDock");
    previous.addDockWidget(Qt::RightDockWidgetArea, inspector);
    settings.setValue("windowState", previous.saveState());
  } else {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow previous(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(),
                             m_ctx->toolSvc(), m_ctx->selection());
    previous.show();
    previous.showPage(QStringLiteral("compose"));
    auto *inspector = previous.findChild<QDockWidget *>("pagePanelDock");
    auto *agent = previous.findChild<QDockWidget *>("aiAgentDock");
    QVERIFY(inspector && agent);
    previous.tabifyDockWidget(inspector, agent);
    inspector->show();
    agent->show();
    auto *front = previous.findChild<QDockWidget *>(selected);
    front->raise();
    QTRY_VERIFY(!front->visibleRegion().isEmpty());
    previous.saveWindowState();
  }

  QgisCanvasController canvas(m_ctx);
  PaleoMainWindow recovered(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(),
                            m_ctx->toolSvc(), m_ctx->selection());
  recovered.attachAiAssistant(m_ctx->aiChat());
  recovered.show();
  auto *agent = recovered.findChild<QDockWidget *>("aiAgentDock");
  auto *button = recovered.findChild<QToolButton *>("aiAgentButton");
  QVERIFY(agent && button && button->isEnabled());
  QCOMPARE(agent->isHidden(), legacy);
  recovered.showPage(QStringLiteral("compose"));
  if (!legacy) {
    auto *inspector = recovered.findChild<QDockWidget *>("pagePanelDock");
    QVERIFY(recovered.tabifiedDockWidgets(inspector).contains(agent));
    auto *front = recovered.findChild<QDockWidget *>(selected);
    QTRY_VERIFY(!front->visibleRegion().isEmpty());
    auto *behind = front == agent ? inspector : agent;
    QTRY_VERIFY(behind->visibleRegion().isEmpty());
  }
  QTest::mouseClick(button, Qt::LeftButton);
  QTRY_VERIFY(!agent->visibleRegion().isEmpty());
}

void TestG5Assembly::launcherKeepsDraftAndDocumentAcrossWorkflows() {
  auto *shell = m_win->findChild<QDockWidget *>("aiAgentDock");
  auto *launcher = m_win->findChild<QToolButton *>("aiAgentButton");
  auto *input = m_win->findChild<QTextEdit *>("aiAssistantInput");
  auto *documents = m_win->findChild<QTabBar *>("visualizationTabs");
  auto *focus = m_win->findChild<QToolButton *>("workspaceFocusButton");
  QVERIFY(shell && launcher && input && documents && focus);
  QVERIFY(!launcher->icon().isNull());

  QTemporaryDir project;
  QVERIFY(project.isValid());
  const auto closeProject = qScopeGuard([this] { m_ctx->closeProject(); });
  const QSize originalSize = m_win->size();
  const auto restoreSize = qScopeGuard([&] { m_win->resize(originalSize); });
  auto *chat = m_ctx->aiChat();
  const auto previousConfig = chat->config();
  const auto reset = qScopeGuard([&] {
    if (focus->isChecked()) focus->click();
    input->clear();
    shell->setFloating(false);
    shell->close();
    chat->setConfig(previousConfig);
    m_win->showStartup();
  });
  LlmConfig config;
  config.endpoint = QUrl("http://127.0.0.1:1/v1");
  config.model = "draft-only";
  config.apiKey = "fixture";
  chat->setConfig(config);
  m_win->show();
  m_win->activateWindow();
  m_win->resize(640, 600);
  QTRY_VERIFY(launcher->isVisible() && launcher->isEnabled());
  QTRY_COMPARE(m_win->childAt(launcher->mapTo(m_win, launcher->rect().center())),
               static_cast<QWidget *>(launcher));
  QVERIFY(shell->isHidden());
  const auto page = m_win->currentPage();
  const int document = documents->currentIndex();
  QTest::mouseClick(launcher, Qt::LeftButton);
  QTRY_VERIFY(shell->isVisible() && input->hasFocus());
  QCOMPARE(m_win->currentPage(), page);
  QCOMPARE(documents->currentIndex(), document);
  m_win->resize(originalSize);
  QVERIFY(m_ctx->projectSvc()->createProject(project.filePath("agent-dock.qgz")));
  const QString draft = QStringLiteral("按当前层位核对井资料");
  input->setPlainText(draft);

  m_win->showPage(QStringLiteral("correlation"));
  QTRY_VERIFY(shell->isVisible());
  shell->setFloating(true);
  QTRY_VERIFY(shell->isFloating());
  shell->close();
  m_win->showPage(QStringLiteral("compose"));
  QVERIFY(shell->isHidden());
  m_win->activateWindow();
  QTRY_VERIFY(m_win->isActiveWindow());
  const int mapTab = documents->currentIndex();
  QTest::keyClick(m_win, Qt::Key_J, Qt::ControlModifier);
  QTRY_VERIFY(shell->isVisible() && shell->isFloating());
  QCOMPARE(input->toPlainText(), draft);
  QCOMPARE(m_win->currentPage(), QStringLiteral("compose"));
  QCOMPARE(documents->currentIndex(), mapTab);

  shell->setFloating(false);
  QTRY_VERIFY(focus->isVisible());
  focus->click();
  QTRY_VERIFY(focus->isChecked() && shell->isHidden());
  QVERIFY(launcher->isVisible());
  QTest::mouseClick(launcher, Qt::LeftButton);
  QTRY_VERIFY(!focus->isChecked() && shell->isVisible() && input->hasFocus());
  QCOMPARE(documents->currentIndex(), mapTab);
  QCOMPARE(input->toPlainText(), draft);
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

void TestG5Assembly::droppingAFileImportsItIntoTheOpenProjectWithoutAModel() {
  QTemporaryDir project, files;
  QVERIFY(project.isValid() && files.isValid());
  const auto closeProject = qScopeGuard([this] { m_ctx->closeProject(); });
  QVERIFY2(m_ctx->projectSvc()->createProject(project.filePath("import.qgz")),
           qPrintable(m_ctx->projectSvc()->lastErrors().join(';')));
  auto *chat = m_ctx->aiChat();
  chat->setConfig({});
  chat->newSession();
  const auto session = chat->session().id;
  const auto removeChat = qScopeGuard([chat, session] { chat->removeSession(session); });
  auto *dock = m_win->findChild<AiAssistDock *>("aiAssistantDock");
  QVERIFY(dock);
  const QString source = files.filePath("survey.dat");
  QVERIFY(QFile::copy(QFINDTESTDATA("../testdata/project_area/ExportWellHead.dat"), source));

  bool prompted = false;
  QTimer dismissUnexpectedPrompt;
  connect(&dismissUnexpectedPrompt, &QTimer::timeout, this, [&] {
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      prompted = true;
      dialog->reject();
    }
  });
  dismissUnexpectedPrompt.start(50);
  QMimeData data;
  data.setUrls({QUrl::fromLocalFile(source)});
  QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(dock, &enter);
  QVERIFY(enter.isAccepted());
  QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(dock, &drop);
  QVERIFY(drop.isAccepted());
  QVERIFY(spinUntil([chat] { return chat->messages().size() >= 2; }));
  QVERIFY2(!prompted, "A self-describing well-head table should import directly");
  const auto assets = m_ctx->importSvc()->assets("well_head");
  QCOMPARE(assets.size(), 1);
  const auto receipt = chat->messages().last();
  QVERIFY(receipt.localImportReceipt);
  QVERIFY(receipt.content.contains(assets.first()));
  QVERIFY(receipt.content.contains("survey.dat"));
  QVERIFY(!receipt.content.contains(files.path()));
  QVERIFY(!chat->streaming());
}

void TestG5Assembly::captureAssemblyScreenshots() {
  const QString dir = QString::fromUtf8(qgetenv("PALEO_G5_SHOT_DIR"));
  if (dir.isEmpty())
    QSKIP("截图按需采集（PALEO_G5_SHOT_DIR 未设）");
  auto *dock = m_win->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  QVERIFY(dock);
  m_win->resize(1280, 800);
  m_win->show();
  auto *launcher = m_win->findChild<QToolButton *>("aiAgentButton");
  QVERIFY(launcher && launcher->isVisible());
  launcher->click();
  QTRY_VERIFY(dock->isVisible());

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
  QStandardPaths::setTestModeEnabled(true);
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
