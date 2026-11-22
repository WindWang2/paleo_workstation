// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextEdit>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <functional>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/chatsession.h"
#include "../src/ai/chat/chatsessionarchive.h"
#include "../src/ai/chat/llmclient.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aichatcontroller.h"

// H2（agent-session-archive）W7：最近会话 Combo 接入轻量元信息。
// 真实 checkpoint 信号链（sessionListChanged → refreshSessions → 元信息查询
// → Combo 重建）驱动，断言零旧正文解析；草稿/工具卡/当前选择不受刷新影响。
namespace {
const char kStream[] =
  "data: {\"choices\":[{\"delta\":{\"content\":\"砂岩为主\"}}]}\n\n"
  "data: {\"choices\":[{\"delta\":{}}],\"usage\":{\"prompt_tokens\":9,"
  "\"completion_tokens\":2}}\n\n"
  "data: [DONE]\n\n";

class FakeLlmServer : public QObject {
  Q_OBJECT
public:
  bool start() {
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      serve(m_server.nextPendingConnection());
    });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }

private:
  void serve(QTcpSocket *sock) {
    if (!sock)
      return;
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    connect(sock, &QTcpSocket::readyRead, sock, [sock] {
      if (!sock->readAll().contains("\r\n\r\n"))
        return;
      sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                  "Content-Type: text/event-stream\r\n\r\n");
      sock->write(kStream);
    });
  }
  QTcpServer m_server;
};

LlmConfig makeConfig(const QUrl &endpoint) {
  LlmConfig config;
  config.endpoint = endpoint;
  config.model = QStringLiteral("test-model");
  config.stream = true;
  config.timeoutMs = 5000;
  config.apiKey = "sk-test-fixture";
  return config;
}
} // namespace

class TestH2SessionCombo : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();
  void refreshBuildsComboFromSummaries();
  void brokenArchivesShowBoundedDiagnostics();
  void checkpointRefreshDoesNotParseBodies();
  void draftSurvivesRefreshAndSelectionStable();
  void deleteRemovesEntryFromCombo();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  QString seedSession(const QString &id, const QString &title, int ageSeconds);
  QStringList m_sessions;
};

void TestH2SessionCombo::initTestCase() {
  PaleoTheme::pinRenderEnvironment();
  QStandardPaths::setTestModeEnabled(true);
  cleanup();
}

void TestH2SessionCombo::cleanup() {
  for (const QString &id : ChatSessionStore::listIds()) {
    QString error;
    ChatSessionStore::remove(id, &error);
  }
  m_sessions.clear();
  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

QString TestH2SessionCombo::seedSession(const QString &id, const QString &title,
                                        int ageSeconds) {
  ChatSession session;
  session.id = id;
  session.title = title;
  session.model = QStringLiteral("test-model");
  session.createdAt = QDateTime::currentDateTimeUtc().addSecs(-ageSeconds - 60);
  session.updatedAt = QDateTime::currentDateTimeUtc().addSecs(-ageSeconds);
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = title + QStringLiteral("的问题");
  user.timestamp = session.createdAt;
  session.messages.append(user);
  QString error;
  if (!ChatSessionStore::save(session, &error))
    qWarning("seed save failed: %s", qPrintable(error));
  m_sessions.append(session.id);
  return session.id;
}

void TestH2SessionCombo::refreshBuildsComboFromSummaries() {
  const QString newest = seedSession(QStringLiteral("h2-combo-new"), QStringLiteral("最新会话"), 1);
  const QString middle = seedSession(QStringLiteral("h2-combo-mid"), QStringLiteral("较早会话"), 50);
  seedSession(QStringLiteral("h2-combo-old"), QStringLiteral("最旧会话"), 100);
  PaleoTaskService service;
  AiChatController controller;
  controller.toolRunner()->setTaskService(&service);
  AiAssistDock dock(&controller);
  auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  QVERIFY(combo);
  // 冷修复 + Combo 重建（经真实 ready → rebuildSessionCombo 链）。
  QVERIFY(spinUntil([&] { return combo->count() == 4; }));
  // 当前会话永远首位；其余按元信息时间倒序；选择稳定在当前会话。
  QCOMPARE(combo->currentIndex(), 0);
  QCOMPARE(combo->itemText(1), QStringLiteral("最新会话"));
  QCOMPARE(combo->itemText(2), QStringLiteral("较早会话"));
  QCOMPARE(combo->itemText(3), QStringLiteral("最旧会话"));
  // 选中较旧会话经权威载入切换：Combo 回到新当前会话首位。
  QString error;
  QVERIFY(controller.loadSession(middle, &error));
  QVERIFY(spinUntil([&] { return combo->count() == 4; }));
  QCOMPARE(combo->currentIndex(), 0);
  QVERIFY(combo->itemText(0).contains(QStringLiteral("较早会话")));
  Q_UNUSED(newest);
}

void TestH2SessionCombo::brokenArchivesShowBoundedDiagnostics() {
  // 超限旧档案：有界诊断文案（选择后由权威入口拒绝，不伪称可载入）。
  const auto production = paleo::chat::ChatArchiveBudget::production();
  paleo::chat::setChatArchiveBudgetForTesting(production);
  ChatSession big;
  big.id = QStringLiteral("h2-combo-over");
  big.title = QStringLiteral("超限旧档案");
  big.createdAt = big.updatedAt = QDateTime::currentDateTimeUtc();
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QString(20000, QLatin1Char('x'));
  big.messages.append(user);
  QString error;
  QVERIFY(ChatSessionStore::save(big, &error));
  m_sessions.append(big.id);
  paleo::chat::ChatArchiveBudget scaled = production;
  scaled.maxArchiveBytes = 8192;
  scaled.maxTitleChars = 64;
  scaled.maxTitleCharsLoad = 4096;
  scaled.maxMessages = 2000;
  paleo::chat::setChatArchiveBudgetForTesting(scaled);

  PaleoTaskService service;
  AiChatController controller;
  controller.toolRunner()->setTaskService(&service);
  AiAssistDock dock(&controller);
  auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  QVERIFY(combo);
  QVERIFY(spinUntil([&] { return combo->count() == 2; }));
  QVERIFY(combo->itemText(1).contains(QStringLiteral("超限会话")));
  // 选择超限档案：宿主接线（生产在 paleomainwindow_attach_shell，测试同
  // 路径连接）→ 权威 load 拒绝（Combo 回到真实当前会话）。
  connect(&dock, &AiAssistDock::openSessionRequested, &controller,
          [&controller](const QString &id) { controller.loadSession(id); });
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1));
  QVERIFY(spinUntil([&] { return errors.count() >= 1; }));
  QVERIFY(errors.first().first().toString().contains(QStringLiteral("上限")));
  QCOMPARE(controller.session().id != big.id, true);
  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2SessionCombo::checkpointRefreshDoesNotParseBodies() {
  for (int i = 0; i < 60; ++i)
    seedSession(QStringLiteral("h2-combo-bulk-%1").arg(i, 2, 10, QLatin1Char('0')),
                QStringLiteral("存量会话 %1").arg(i), i);
  FakeLlmServer server;
  QVERIFY(server.start());
  PaleoTaskService service;
  AiChatController controller;
  controller.toolRunner()->setTaskService(&service);
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  QVERIFY(combo);
  QVERIFY(spinUntil([&] { return combo->count() == 61; })); // 当前 + 60 存量

  // 真实流式回合：checkpoint 保存 → sessionListChanged → refreshSessions
  // → 元信息查询。热刷新零旧正文解析（保存自己的 sidecar 顺带更新）。
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY(spinUntil([&] { return !controller.streaming(); }));
  m_sessions.append(controller.session().id);
  spinUntil([&] { return !controller.recentSummaryRepairInFlight(); });
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(paleo::chat::chatArchiveFullLoadCount(), qint64(0));
  QVERIFY(spinUntil([&] { return combo->count() == 61; })); // 当前 + 60 存量

  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2SessionCombo::draftSurvivesRefreshAndSelectionStable() {
  seedSession(QStringLiteral("h2-combo-draft"), QStringLiteral("草稿存活验证"), 5);
  FakeLlmServer server;
  QVERIFY(server.start());
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  QVERIFY(input && combo);
  QVERIFY(spinUntil([&] { return combo->count() == 2; }));
  input->setPlainText(QStringLiteral("还没发出去的草稿"));
  QCOMPARE(combo->currentIndex(), 0);
  // 刷新（程序化 sessionListChanged）不清草稿、不改选择。
  controller.sendUserText(QStringLiteral("占位一轮"));
  QVERIFY(spinUntil([&] { return !controller.streaming(); }));
  m_sessions.append(controller.session().id);
  QVERIFY(spinUntil([&] { return combo->count() == 2; })); // 当前 + 1 存量（自身不重复入列）
  QCOMPARE(input->toPlainText(), QStringLiteral("还没发出去的草稿"));
  QCOMPARE(combo->currentIndex(), 0);
}

void TestH2SessionCombo::deleteRemovesEntryFromCombo() {
  const QString victim = seedSession(QStringLiteral("h2-combo-del"), QStringLiteral("待删除会话"), 5);
  seedSession(QStringLiteral("h2-combo-keep"), QStringLiteral("另一会话"), 6);
  AiChatController controller;
  AiAssistDock dock(&controller);
  auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  QVERIFY(combo);
  QVERIFY(spinUntil([&] { return combo->count() == 3; }));
  QString error;
  QVERIFY(controller.removeSession(victim, &error));
  QVERIFY(spinUntil([&] { return combo->count() == 2; }));
  for (int i = 0; i < combo->count(); ++i) {
    const QString text = combo->itemText(i);
    QVERIFY2(!text.contains(QStringLiteral("待删除会话")), "已删除会话不得复活");
  }
}

QTEST_MAIN(TestH2SessionCombo)
#include "tst_h2_sessioncombo.moc"
