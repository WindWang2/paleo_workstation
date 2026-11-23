#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <memory>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/chatsession.h"
#include "../src/ai/chat/chatsessionarchive.h"
#include "../src/workflow/aichatcontroller.h"

// H2（agent-session-archive）W3：发送/重试/重命名前的纯档案容量准入。
// 端点一律 127.0.0.1 假服务器；断言只认语义（零 append/零 HTTP/状态不变），
// 不认墙钟。预算用缩放档（结构同生产，数值缩小），边界用例可精确构造。
namespace {
class FakeLlmServer : public QObject {
  Q_OBJECT
public:
  // HeavyEscape：单条回答由 1024 个 U+0001 组成——SSE 传输侧合法 JSON
  // （\u0001 转义），落盘侧每个码元最坏 6 字节，是预留公式的真实压力形态。
  enum class Mode { Plain, HeavyEscape };
  bool start(Mode mode) {
    m_mode = mode;
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      serve(m_server.nextPendingConnection());
    });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }
  int requestCount() const { return m_requests; }

private:
  void serve(QTcpSocket *sock) {
    if (!sock)
      return;
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    auto buf = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
      buf->append(sock->readAll());
      // 等到完整请求（头 + Content-Length 指明的体）再应答，同一连接只应答
      // 一次（同 tst_aichatcontroller 假服务器的口径）。
      const int headerEnd = buf->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      const QString header = QString::fromUtf8(buf->left(headerEnd));
      const int contentLength =
        header.section("Content-Length:", 1, 1, QString::SectionCaseInsensitiveSeps)
          .section("\r", 0, 0).trimmed().toInt();
      if (buf->mid(headerEnd + 4).size() < contentLength)
        return;
      ++m_requests;
      QJsonObject delta;
      if (m_mode == Mode::HeavyEscape) {
        QString content(1024, QChar(0x0001));
        delta.insert(QStringLiteral("content"), content);
      } else {
        delta.insert(QStringLiteral("content"), QStringLiteral("砂岩为主"));
      }
      const QByteArray frame =
        "data: " +
        QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"delta", delta}}}}})
          .toJson(QJsonDocument::Compact) +
        "\n\n"
        "data: {\"choices\":[{\"delta\":{}}],\"usage\":{\"prompt_tokens\":9,"
        "\"completion_tokens\":2}}\n\n"
        "data: [DONE]\n\n";
      // 流式不写 Content-Length：写完即关连接，客户端以 EOF 判定响应结束。
      sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                  "Content-Type: text/event-stream\r\n\r\n");
      sock->write(frame);
      sock->flush();
      sock->disconnectFromHost();
    });
  }
  QTcpServer m_server;
  Mode m_mode = Mode::Plain;
  int m_requests = 0;
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

paleo::chat::ChatArchiveBudget scaledBudget() {
  paleo::chat::ChatArchiveBudget budget;
  budget.toolRounds = 1;
  budget.maxResponseBytes = 1024;
  budget.callsPerTurn = 2;
  budget.toolArgumentBytes = 128;
  budget.resultPageBytes = 256;
  budget.escapeWorstFactor = 6;
  budget.contentSlack = 64;
  budget.messageMetaSlack = 32;
  budget.bindingRowBytes = 256;
  budget.maxTitleChars = 32;
  budget.maxTitleCharsLoad = 4096;
  budget.maxMessages = 1000;
  budget.maxArchiveBytes =
    budget.turnReserve() + budget.titleReserve() + 2 * 1024;
  return budget;
}

ChatSession legacyArchive(const QString &id, int bodyChars) {
  ChatSession session;
  session.id = id;
  session.model = QStringLiteral("legacy-model");
  session.createdAt = QDateTime::currentDateTimeUtc();
  session.updatedAt = session.createdAt;
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QStringLiteral("早期的长问题？");
  user.timestamp = session.createdAt;
  session.messages.append(user);
  ChatMessage assistant;
  assistant.role = ChatRole::Assistant;
  assistant.content = QString(bodyChars, QLatin1Char('a'));
  assistant.timestamp = session.createdAt;
  session.messages.append(assistant);
  return session;
}
} // namespace

class TestH2CapacityGates : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();
  void sendRejectKeepsSessionUntouchedAndNewSessionWorks();
  void nearCapLegacyLoadRetryRefusedBeforeResize();
  void retryStillWorksWhenPrefixFits();
  void renameOverFieldOrBudgetRefused();
  void emptySessionRenameStillNoFile();
  void realTurnBytesStayWithinReserve();

private:
  QStringList m_sessions;
};

void TestH2CapacityGates::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
  QDir(ChatSessionStore::directory()).removeRecursively();
}

void TestH2CapacityGates::cleanup() {
  for (const QString &id : m_sessions)
    ChatSessionStore::remove(id, nullptr);
  m_sessions.clear();
  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2CapacityGates::sendRejectKeepsSessionUntouchedAndNewSessionWorks() {
  const auto budget = scaledBudget();
  paleo::chat::setChatArchiveBudgetForTesting(budget);
  // 载入一个「合法但接近上限」的旧档案（≤ cap 可载入；+ 一轮预留超 cap）。
  const ChatSession nearCap = legacyArchive(QStringLiteral("h2-gate-nearcap"), 7800);
  QString error;
  QVERIFY(ChatSessionStore::save(nearCap, &error));
  m_sessions.append(nearCap.id);
  QVERIFY(paleo::chat::chatArchiveSerializedBytes(nearCap) <= budget.maxArchiveBytes);

  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Plain));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  QVERIFY(controller.enabled());
  QVERIFY(controller.loadSession(nearCap.id, &error));
  const int messagesBefore = controller.messages().size();
  const QByteArray diskBefore =
    QFile(ChatSessionStore::pathFor(nearCap.id)).readAll();

  QSignalSpy errors(&controller, &AiChatController::errorOccurred),
      changed(&controller, &AiChatController::sessionChanged),
      appended(&controller, &AiChatController::messageAppended),
      listChanged(&controller, &AiChatController::sessionListChanged);
  controller.sendUserText(QStringLiteral("再来一轮"));
  QCOMPARE(server.requestCount(), 0);      // 零 HTTP
  QCOMPARE(controller.messages().size(), messagesBefore); // 零 append
  QCOMPARE(appended.count(), 0);
  QCOMPARE(errors.count(), 1);
  QVERIFY(errors.first().first().toString().contains(QStringLiteral("容量不足")));
  QVERIFY(errors.first().first().toString().contains(QStringLiteral("新会话")));
  // 不制造新欠账：后续保存语义未受损（statusText 无保存失败字样）。
  QVERIFY(!controller.statusText().contains(QStringLiteral("保存失败")));
  QCOMPARE(QFile(ChatSessionStore::pathFor(nearCap.id)).readAll(), diskBefore);

  // 原有出口照常：newSession 切换成功，新会话可正常发起新轮。
  controller.newSession();
  QVERIFY(controller.session().id != nearCap.id);
  controller.sendUserText(QStringLiteral("新话题"));
  QTRY_COMPARE(server.requestCount(), 1);
  QTRY_VERIFY(!controller.streaming());
  QCOMPARE(controller.messages().size(), 2); // user + 完成的 assistant
}

void TestH2CapacityGates::nearCapLegacyLoadRetryRefusedBeforeResize() {
  const auto budget = scaledBudget();
  paleo::chat::setChatArchiveBudgetForTesting(budget);
  // 合法近上限旧档案：最后一轮很短、较早历史很大。
  ChatSession nearCap = legacyArchive(QStringLiteral("h2-retry-nearcap"), 7800);
  QString error;
  QVERIFY(ChatSessionStore::save(nearCap, &error));
  m_sessions.append(nearCap.id);
  // 追加一个很短的尾部轮（仍保持在 cap 内可载入）。
  nearCap.updatedAt = QDateTime::currentDateTimeUtc();
  ChatMessage tail;
  tail.role = ChatRole::User;
  tail.content = QStringLiteral("短问题");
  tail.timestamp = nearCap.updatedAt;
  nearCap.messages.append(tail);
  ChatMessage tailAnswer;
  tailAnswer.role = ChatRole::Assistant;
  tailAnswer.content = QStringLiteral("短回答");
  tailAnswer.timestamp = nearCap.updatedAt;
  nearCap.messages.append(tailAnswer);
  QVERIFY(ChatSessionStore::save(nearCap, &error));
  QVERIFY(paleo::chat::chatArchiveSerializedBytes(nearCap) <= budget.maxArchiveBytes);

  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Plain));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  QVERIFY(controller.loadSession(nearCap.id, &error));
  const int messagesBefore = controller.messages().size();
  const auto bindingsBefore = controller.session().hostToolBindings;
  const QByteArray diskBefore =
    QFile(ChatSessionStore::pathFor(nearCap.id)).readAll();

  QSignalSpy errors(&controller, &AiChatController::errorOccurred),
      changed(&controller, &AiChatController::sessionChanged),
      listChanged(&controller, &AiChatController::sessionListChanged);
  QVERIFY(controller.canRetry());
  controller.retryLast();
  // resize 前拒绝：旧回答/工具事实/bindings 原样保留，零新模型请求，
  // 不发成功 sessionChanged。
  QCOMPARE(server.requestCount(), 0);
  QCOMPARE(controller.messages().size(), messagesBefore);
  QCOMPARE(controller.session().hostToolBindings, bindingsBefore);
  QCOMPARE(changed.count(), 0);
  QCOMPARE(errors.count(), 1);
  QVERIFY(errors.first().first().toString().contains(QStringLiteral("容量不足")));
  QCOMPARE(QFile(ChatSessionStore::pathFor(nearCap.id)).readAll(), diskBefore);
  QVERIFY(controller.canRetry()); // 拒绝不是终态：条件未变，入口仍在

  // newSession 仍可用（不被容量拒绝困住）。
  controller.newSession();
  QVERIFY(controller.session().id != nearCap.id);
}

void TestH2CapacityGates::retryStillWorksWhenPrefixFits() {
  // 回归：余量充足时重试照常截断重发（既有 G2 语义不变）。
  paleo::chat::setChatArchiveBudgetForTesting(scaledBudget());
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Plain));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  controller.sendUserText(QStringLiteral("第一问"));
  QTRY_VERIFY(!controller.streaming());
  QCOMPARE(server.requestCount(), 1);
  QSignalSpy changed(&controller, &AiChatController::sessionChanged);
  controller.retryLast();
  QTRY_COMPARE(server.requestCount(), 2);
  QCOMPARE(changed.count(), 1); // 截断后整页重画信号照发
  QCOMPARE(controller.messages().size(), 2);
  m_sessions.append(controller.session().id);
}

void TestH2CapacityGates::renameOverFieldOrBudgetRefused() {
  const auto budget = scaledBudget();
  paleo::chat::setChatArchiveBudgetForTesting(budget);
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Plain));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  controller.sendUserText(QStringLiteral("有内容的轮"));
  QTRY_VERIFY(!controller.streaming());
  m_sessions.append(controller.session().id);
  const QString currentId = controller.session().id;

  // 新标题超字段上限：赋值前拒绝；原标题/内存/文件/欠账不变。
  const QString tooLong(33, QLatin1Char('t'));
  QString error;
  QSignalSpy listChanged(&controller, &AiChatController::sessionListChanged);
  QVERIFY(!controller.renameSession(currentId, tooLong, &error));
  QVERIFY(error.contains(QStringLiteral("标题超限")));
  QVERIFY(controller.session().title != tooLong);
  const QString path = ChatSessionStore::pathFor(currentId);
  const QByteArray before = QFile(path).readAll();
  // 空会话规则不受影响：重命名失败不发成功列表更新。
  QCOMPARE(listChanged.count(), 0);

  // 非当前会话：缩到极小的 cap 后档案无法载入——赋值/保存前拒绝，磁盘不动
  //（超上限旧档案的重命名走诚实失败，不伪称成功）。
  const ChatSession other = legacyArchive(QStringLiteral("h2-rename-other"), 100);
  QVERIFY(ChatSessionStore::save(other, &error));
  m_sessions.append(other.id);
  const QByteArray otherBefore =
    QFile(ChatSessionStore::pathFor(other.id)).readAll();
  auto tiny = budget;
  tiny.maxArchiveBytes = 64;
  paleo::chat::setChatArchiveBudgetForTesting(tiny);
  QVERIFY(!controller.renameSession(other.id, QStringLiteral("新名"), &error));
  QCOMPARE(QFile(ChatSessionStore::pathFor(other.id)).readAll(), otherBefore);
  paleo::chat::setChatArchiveBudgetForTesting(budget);
  ChatSession onDisk;

  // 合法改名照常（当前会话）：落盘 + 列表更新。
  QVERIFY(controller.renameSession(currentId, QStringLiteral("新标题"), &error));
  QCOMPARE(controller.session().title, QStringLiteral("新标题"));
  QVERIFY(QFile(path).readAll() != before);
  QVERIFY(listChanged.count() >= 1);
  // 合法改名（非当前会话）：先校验候选再保存。
  QVERIFY(controller.renameSession(other.id, QStringLiteral("别的新名"), &error));
  QVERIFY(ChatSessionStore::load(other.id, &onDisk, &error));
  QCOMPARE(onDisk.title, QStringLiteral("别的新名"));

  // 候选档案成本超限（标题本身并不长）：档案离上限只差 5 字节，加一个标题
  // 就越线——报「会话容量不足」而不是「标题超限」，磁盘不动。
  ChatSession nearCap = legacyArchive(QStringLiteral("h2-rename-cost"), 1000);
  const qint64 target = budget.maxArchiveBytes - 5;
  nearCap.messages.last().content +=
    QString(target - paleo::chat::chatArchiveSerializedBytes(nearCap), QLatin1Char('a'));
  QCOMPARE(paleo::chat::chatArchiveSerializedBytes(nearCap), target);
  QVERIFY(ChatSessionStore::save(nearCap, &error));
  m_sessions.append(nearCap.id);
  const QByteArray nearCapBefore =
    QFile(ChatSessionStore::pathFor(nearCap.id)).readAll();
  QVERIFY(!controller.renameSession(nearCap.id, QStringLiteral("新名"), &error));
  QVERIFY2(error.contains(QStringLiteral("会话容量不足")), qPrintable(error));
  QVERIFY2(!error.contains(QStringLiteral("标题超限")), qPrintable(error));
  QCOMPARE(QFile(ChatSessionStore::pathFor(nearCap.id)).readAll(), nearCapBefore);
}

void TestH2CapacityGates::emptySessionRenameStillNoFile() {
  // 空会话仍保持「不落盘」；容量门不改变这条纪律。
  paleo::chat::setChatArchiveBudgetForTesting(scaledBudget());
  AiChatController controller;
  const QString id = controller.session().id;
  QString error;
  QVERIFY(controller.renameSession(id, QStringLiteral("空会话标题"), &error));
  QCOMPARE(controller.session().title, QStringLiteral("空会话标题"));
  QVERIFY(!QFileInfo::exists(ChatSessionStore::pathFor(id)));
}

void TestH2CapacityGates::realTurnBytesStayWithinReserve() {
  // 真实一轮（重转义内容）：落盘字节必须落在预留承诺范围内。
  const auto budget = scaledBudget();
  paleo::chat::setChatArchiveBudgetForTesting(budget);
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::HeavyEscape));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("问"));
  QTRY_VERIFY(!controller.streaming());
  QCOMPARE(errors.count(), 0);
  QCOMPARE(server.requestCount(), 1);
  m_sessions.append(controller.session().id);

  const ChatSession stored = controller.session();
  const qint64 storedBytes = paleo::chat::chatArchiveSerializedBytes(stored);
  // 预留承诺：user 消息本体 + 一轮最坏增量 + 标题余量（同一公式、同一缩放档）。
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QStringLiteral("问");
  const qint64 promised =
    paleo::chat::chatArchiveMessageBytes(user) + budget.turnReserve() +
    budget.titleReserve() + 512 /* 会话信封（id/model/时间戳等） */;
  QVERIFY2(storedBytes <= promised,
           qPrintable(QStringLiteral("stored=%1 promised=%2")
                        .arg(storedBytes)
                        .arg(promised)));
  // 磁盘上的字节与内存序列化一致（save 用同一度量）。
  QCOMPARE(QFile(ChatSessionStore::pathFor(stored.id)).size(), storedBytes);
  // 控制字符内容确实落盘（转义压力真实发生：1024 码元 × ≥6 字节）。
  QVERIFY(storedBytes > 6 * 1024);
}

QTEST_MAIN(TestH2CapacityGates)
#include "tst_h2_capacitygates.moc"
