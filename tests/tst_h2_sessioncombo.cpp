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
#include <QJsonDocument>
#include <QStandardItemModel>
#include <functional>
#include <memory>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/chatsession.h"
#include "../src/ai/chat/chatsessionarchive.h"
#include "../src/ai/chat/llmclient.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/paleotheme.h"
#include "pichattestcontroller.h"

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
    auto buf = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [sock, buf] {
      // 等到完整请求（头 + Content-Length 指明的体）再应答一次；流式无
      // Content-Length，写完即关连接，客户端以 EOF 判定响应结束。
      buf->append(sock->readAll());
      const int headerEnd = buf->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      const QString header = QString::fromUtf8(buf->left(headerEnd));
      const int contentLength =
        header.section("Content-Length:", 1, 1, QString::SectionCaseInsensitiveSeps)
          .section("\r", 0, 0).trimmed().toInt();
      if (buf->mid(headerEnd + 4).size() < contentLength)
        return;
      sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                  "Content-Type: text/event-stream\r\n\r\n");
      sock->write(kStream);
      sock->flush();
      sock->disconnectFromHost();
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
  void incompleteListIsMarkedUntilRepaired();

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
  // 选中较旧会话经权威载入切换：Combo 回到新当前会话首位；该会话自身的
  // 元信息不再重复入列（当前 + 其余 2 个存量 = 3 项）。
  QString error;
  QVERIFY(controller.loadSession(middle, &error));
  QVERIFY(spinUntil([&] { return combo->count() == 3; }));
  QCOMPARE(combo->currentIndex(), 0);
  QVERIFY(combo->itemText(0).contains(QStringLiteral("较早会话")));
  QCOMPARE(combo->itemText(1), QStringLiteral("最新会话"));
  QCOMPARE(combo->itemText(2), QStringLiteral("最旧会话"));
  Q_UNUSED(newest);
}

void TestH2SessionCombo::incompleteListIsMarkedUntilRepaired() {
  // 未建索引的存量会话（H2 升级后首启的形态）：无任务服务时不在 GUI 线程
  // 补读正文，列表如实标注「可能不完整」且该行不可选；接上任务服务后
  // 冷修复完成，标注消失、会话进入列表。
  seedSession(QStringLiteral("h2-combo-indexed"), QStringLiteral("已有索引的会话"), 3);
  ChatSession raw;
  raw.id = QStringLiteral("h2-combo-unindexed");
  raw.title = QStringLiteral("尚未建索引的会话");
  raw.model = QStringLiteral("test-model");
  raw.createdAt = raw.updatedAt = QDateTime::currentDateTimeUtc();
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QStringLiteral("升级前写入的正文");
  raw.messages.append(user);
  m_sessions.append(raw.id);
  {
    // 直接落盘且不留 sidecar：等价于旧版本写出的会话文件。
    QFile file(ChatSessionStore::pathFor(raw.id));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(raw.toJson()).toJson(QJsonDocument::Compact));
  }
  const auto rowEnabled = [](const QComboBox *combo, int row) {
    const auto *model = qobject_cast<const QStandardItemModel *>(combo->model());
    return model && model->item(row) && model->item(row)->isEnabled();
  };
  {
    AiChatController controller; // 未接任务服务：只发布可信缓存的部分结果
    AiAssistDock dock(&controller);
    auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
    QVERIFY(combo);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(combo->count(), 3); // 当前 + 已建索引 1 条 + 说明行
    QCOMPARE(combo->itemText(1), QStringLiteral("已有索引的会话"));
    QVERIFY2(combo->itemText(2).contains(QStringLiteral("列表可能不完整")),
             qPrintable(combo->itemText(2)));
    QVERIFY(!rowEnabled(combo, 2)); // 说明行不可选
    QVERIFY(rowEnabled(combo, 1));
    for (int i = 0; i < combo->count(); ++i)
      QVERIFY2(!combo->itemText(i).contains(QStringLiteral("尚未建索引")),
               "未校验的会话不得冒充已列出的完整最近集合");
  }
  {
    PaleoTaskService service;
    AiChatController controller;
    controller.toolRunner()->setTaskService(&service);
    AiAssistDock dock(&controller);
    auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
    QVERIFY(combo);
    // 冷修复完成后的第二次发布：说明行消失，两条存量按时间倒序入列。
    QVERIFY2(spinUntil([&] { return combo->count() == 3 &&
                                     !combo->itemText(2).contains(QStringLiteral("列表可能不完整")); }),
             qPrintable(QStringLiteral("count=%1 last=%2").arg(combo->count()).arg(combo->itemText(combo->count() - 1))));
    QCOMPARE(combo->currentIndex(), 0);
    QCOMPARE(combo->itemText(1), QStringLiteral("尚未建索引的会话"));
    QCOMPARE(combo->itemText(2), QStringLiteral("已有索引的会话"));
  }
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
  // 路径连接）→ 权威 load 拒绝（Combo 回到真实当前会话）。loadSession 载入
  // 失败只发 sessionListChanged，原因经 out 参数交给宿主显示——与生产同形，
  // 这里经出参断言拒绝原因。
  QString loadError;
  connect(&dock, &AiAssistDock::openSessionRequested, &controller,
          [&controller, &loadError](const QString &id) { controller.loadSession(id, &loadError); });
  QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1)); // 信号直连：同步完成
  QVERIFY2(loadError.contains(QStringLiteral("上限")), qPrintable(loadError));
  QCOMPARE(controller.session().id != big.id, true);
  QCOMPARE(combo->currentIndex(), 0); // Combo 回到真实当前会话
  QVERIFY(spinUntil([&] { return combo->count() == 2; }));
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
  // 下拉是「有界最近列表」：当前 + 最近 20 个（requestRecentSessionSummaries(20)）。
  // 磁盘上有 60 个存量会话，列表只取 updatedAt 最新的 20 个（按时间倒序）。
  const auto comboRows = [combo] {
    QStringList rows;
    for (int i = 0; i < combo->count(); ++i)
      rows << combo->itemText(i);
    return rows;
  };
  QVERIFY2(spinUntil([&] { return combo->count() == 21; }), // 当前 + 20 个最近存量
           qPrintable(QStringLiteral("count=%1\n%2").arg(combo->count()).arg(comboRows().join(QLatin1Char('\n')))));
  QCOMPARE(comboRows().at(1), QStringLiteral("存量会话 0"));  // 最新
  QCOMPARE(comboRows().at(20), QStringLiteral("存量会话 19")); // 第 20 近
  QVERIFY(!comboRows().contains(QStringLiteral("存量会话 20"))); // 更旧的不在最近 20 内

  // 真实流式回合：checkpoint 保存 → sessionListChanged → refreshSessions
  // → 元信息查询。热刷新零旧正文解析（保存自己的 sidecar 顺带更新）。
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY(spinUntil([&] { return !controller.streaming(); }));
  m_sessions.append(controller.session().id);
  QVERIFY2(spinUntil([&] { return !controller.recentSummaryRepairInFlight(); }),
           "checkpoint 后的修复应已收尾");
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(paleo::chat::chatArchiveFullLoadCount(), qint64(0));
  // 回合后当前会话已落盘并进入「最近 20 条」之首：dock 过滤掉结果里的当前会话
  // 再固定首位，所以是 20 项 = 当前 + 19 个存量（与改造前 recentSessions(20)
  // + 过滤当前会话的条数语义一致）。当前会话不重复入列，选择仍在其上。
  QVERIFY2(combo->count() == 20, qPrintable(QStringLiteral("count=%1\n%2")
             .arg(combo->count()).arg(comboRows().join(QLatin1Char('\n')))));
  QCOMPARE(combo->currentIndex(), 0);
  QCOMPARE(comboRows().at(0), QStringLiteral("新问题")); // 当前会话（首条用户消息作标题）
  QCOMPARE(comboRows().at(1), QStringLiteral("存量会话 0"));
  QCOMPARE(comboRows().at(19), QStringLiteral("存量会话 18"));
  QVERIFY(comboRows().filter(QStringLiteral("新问题")).size() == 1); // 不重复入列

  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2SessionCombo::draftSurvivesRefreshAndSelectionStable() {
  seedSession(QStringLiteral("h2-combo-draft"), QStringLiteral("草稿存活验证"), 5);
  FakeLlmServer server;
  QVERIFY(server.start());
  PiTestChatController controller;
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
