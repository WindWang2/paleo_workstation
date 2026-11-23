#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/chatsession.h"
#include "../src/ai/chat/chatsessionarchive.h"
#include "../src/ai/chat/chatsessionindex.h"

// H2（agent-session-archive）W1/W2：档案身份与结构合同、私有文件边界、
// 有界读取。全部本地磁盘操作（会话根重定向 .qttest 沙箱），无网络。
class TestH2SessionArchive : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();
  void roundTripKeepsLegacyContract();
  void identityMismatchRefusesWithoutTouchingFiles();
  void futureAndMissingSchemaRefused();
  void structureShapesRefusedInsteadOfMasked();
  void directoryAndSymlinkMasqueradeRefused();
  void overLimitArchivePreservedBytes();
  void boundedReadEnforcedDuringGrowth();
  void exactBoundaryAcceptedOneByteOverRefused();
  void messageBudgetRefused();
  void admissionMathBoundaries();
  void saveRefusesOverCapKeepsPreviousFile();
  void sidecarWrittenAfterSaveRemovedWithSession();

private:
  ChatSession makeSession(const QString &id, const QString &body,
                          int messages = 2);
  QString writeRawArchive(const QString &id, const QJsonObject &object);
  QByteArrayList m_written;
};

class EndlessDevice : public QIODevice {
public:
  using QIODevice::QIODevice;
  bool openReadOnly() { return QIODevice::open(ReadOnly); }

protected:
  qint64 readData(char *data, qint64 maxSize) override
  {
    memset(data, 'a', size_t(maxSize));
    return maxSize; // 永不 EOF：模拟「打开后持续增长」的设备
  }
  qint64 writeData(const char *, qint64) override { return -1; }
};

void TestH2SessionArchive::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
  QDir(ChatSessionStore::directory()).removeRecursively();
  QDir().mkpath(ChatSessionStore::directory());
}

void TestH2SessionArchive::cleanup() {
  for (const QString &id : ChatSessionStore::listIds()) {
    QString error;
    ChatSessionStore::remove(id, &error);
  }
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively();
  // 恢复生产预算，避免影响后续用例/测试进程。
  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

ChatSession TestH2SessionArchive::makeSession(const QString &id,
                                              const QString &body, int messages) {
  ChatSession session;
  session.id = id;
  session.title = QStringLiteral("tst-") + id;
  session.model = QStringLiteral("test-model");
  session.createdAt = session.updatedAt = QDateTime::currentDateTimeUtc();
  for (int i = 0; i < messages; ++i) {
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = body;
    user.timestamp = QDateTime::currentDateTimeUtc();
    session.messages.append(user);
    ChatMessage assistant;
    assistant.role = ChatRole::Assistant;
    assistant.content = body + QStringLiteral("-answer");
    assistant.timestamp = QDateTime::currentDateTimeUtc();
    session.messages.append(assistant);
  }
  return session;
}

QString TestH2SessionArchive::writeRawArchive(const QString &id,
                                              const QJsonObject &object) {
  const QString path = ChatSessionStore::pathFor(id);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return {};
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  file.write(bytes);
  file.close();
  m_written.append(id.toUtf8());
  return path;
}

void TestH2SessionArchive::roundTripKeepsLegacyContract() {
  // 旧 schema=1（无 bindings、无标题）继续可读；hostTurn/call 等身份字段
  // 原样保留，不因校验被改写。
  ChatSession session = makeSession(QStringLiteral("h2-legacy-roundtrip"),
                                    QStringLiteral("层位怎么定？"));
  session.title.clear();
  session.hostToolBindings = QJsonArray{
    QJsonObject{{"admittedSha256", QString(64, QLatin1Char('a'))},
                {"callId", QStringLiteral("c1")},
                {"clientRequestId", QStringLiteral("pi-x")},
                {"documentId", QStringLiteral("doc")},
                {"toolName", QStringLiteral("paleo.query_project")},
                {"turnId", QStringLiteral("t1")}}};
  QString error;
  QVERIFY2(ChatSessionStore::save(session, &error), qPrintable(error));
  ChatSession restored;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY2(ChatSessionStore::load(session.id, &restored, &error, &reject),
           qPrintable(error));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::None);
  QCOMPARE(restored.id, session.id);
  QCOMPARE(restored.messages.size(), session.messages.size());
  QCOMPARE(restored.hostToolBindings, session.hostToolBindings);
  QVERIFY(restored.displayTitle().contains(QStringLiteral("层位")));
}

void TestH2SessionArchive::identityMismatchRefusesWithoutTouchingFiles() {
  // 文件名 A / payload id B：拒绝完整载入；out 不变；不写 B；不清洗 A。
  QJsonObject object = makeSession(QStringLiteral("h2-payload-b"),
                                   QStringLiteral("x"), 1).toJson();
  object.insert(QStringLiteral("id"), QStringLiteral("h2-payload-b"));
  const QString path = writeRawArchive(QStringLiteral("h2-file-a"), object);
  QVERIFY(!path.isEmpty());
  const QByteArray before = QFile(path).readAll();

  ChatSession out;
  out.id = QStringLiteral("sentinel");
  QString error;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-file-a"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::IdentityMismatch);
  QCOMPARE(out.id, QStringLiteral("sentinel")); // 失败不部分覆盖 out
  QCOMPARE(QFile(path).readAll(), before);      // 原文件字节保留
  QVERIFY(!QFileInfo::exists(ChatSessionStore::pathFor(QStringLiteral("h2-payload-b"))));

  // 尺寸相近/大小写差异不做「修复」：id 不同就是不同。
  QJsonObject nearMiss = object;
  nearMiss.insert(QStringLiteral("id"), QStringLiteral("H2-FILE-A"));
  const QString nearPath = writeRawArchive(QStringLiteral("h2-near-miss"), nearMiss);
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-near-miss"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::IdentityMismatch);
}

void TestH2SessionArchive::futureAndMissingSchemaRefused() {
  ChatSession session = makeSession(QStringLiteral("h2-schema"), QStringLiteral("s"), 1);
  QJsonObject future = session.toJson();
  future.insert(QStringLiteral("schema"), 2);
  writeRawArchive(QStringLiteral("h2-schema-future"), future);
  QJsonObject missing = session.toJson();
  missing.remove(QStringLiteral("schema"));
  writeRawArchive(QStringLiteral("h2-schema-missing"), missing);

  ChatSession out;
  QString error;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-schema-future"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::SchemaUnsupported);
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-schema-missing"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::SchemaMissing);
  QVERIFY(error.contains(QStringLiteral("schema"), Qt::CaseInsensitive));
}

void TestH2SessionArchive::structureShapesRefusedInsteadOfMasked() {
  const QString base = QStringLiteral("h2-shape-%1");
  int index = 0;
  // 身份校验先于结构校验：夹具的 id 必须与文件名一致，只留被测的结构缺陷。
  const auto write = [&](QJsonObject object) -> QString {
    const QString id = base.arg(++index);
    object.insert(QStringLiteral("id"), id);
    writeRawArchive(id, object);
    return id;
  };
  ChatSession good = makeSession(QStringLiteral("shape"), QStringLiteral("s"), 1);

  QJsonObject messagesNotArray = good.toJson();
  messagesNotArray.insert(QStringLiteral("messages"), 5);
  const QString a = write(messagesNotArray);

  QJsonObject messageNotObject = good.toJson();
  {
    QJsonArray array = messageNotObject.value(QStringLiteral("messages")).toArray();
    array.append(7);
    messageNotObject.insert(QStringLiteral("messages"), array);
  }
  const QString b = write(messageNotObject);

  QJsonObject unknownRole = good.toJson();
  {
    QJsonArray array = unknownRole.value(QStringLiteral("messages")).toArray();
    QJsonObject first = array.at(0).toObject();
    first.insert(QStringLiteral("role"), QStringLiteral("model"));
    array.replace(0, first);
    unknownRole.insert(QStringLiteral("messages"), array);
  }
  const QString c = write(unknownRole);

  QJsonObject contentNotString = good.toJson();
  {
    QJsonArray array = contentNotString.value(QStringLiteral("messages")).toArray();
    QJsonObject first = array.at(0).toObject();
    first.insert(QStringLiteral("content"), 42);
    array.replace(0, first);
    contentNotString.insert(QStringLiteral("messages"), array);
  }
  const QString d = write(contentNotString);

  QJsonObject toolCallsNotArray = good.toJson();
  {
    QJsonArray array = toolCallsNotArray.value(QStringLiteral("messages")).toArray();
    QJsonObject second = array.at(1).toObject();
    second.insert(QStringLiteral("tool_calls"), QStringLiteral("nope"));
    array.replace(1, second);
    toolCallsNotArray.insert(QStringLiteral("messages"), array);
  }
  const QString e = write(toolCallsNotArray);

  QJsonObject bindingsNotArray = good.toJson();
  bindingsNotArray.insert(QStringLiteral("host_tool_bindings"), 3);
  const QString f = write(bindingsNotArray);

  QJsonObject bindingsRowNotObject = good.toJson();
  bindingsRowNotObject.insert(QStringLiteral("host_tool_bindings"),
                              QJsonArray{QJsonValue(9)});
  const QString g = write(bindingsRowNotObject);

  // 缺 messages 整体：结构错误（不再默认成「合法空历史」）。
  QJsonObject noMessages = good.toJson();
  noMessages.remove(QStringLiteral("messages"));
  const QString h = write(noMessages);

  for (const QString &id : {a, b, c, d, e, f, g, h}) {
    ChatSession out;
    out.id = QStringLiteral("sentinel");
    QString error;
    paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
    QVERIFY2(!ChatSessionStore::load(id, &out, &error, &reject),
             qPrintable(id + QStringLiteral(" 应被拒绝")));
    QCOMPARE(reject, paleo::chat::ChatArchiveReject::StructureInvalid);
    QCOMPARE(out.id, QStringLiteral("sentinel"));
  }

  // 合法兼容形态照读：对象形态 arguments（读侧历史容错）、空无参调用、
  // 旧时间格式缺省。
  QJsonObject legacyArguments = good.toJson();
  {
    QJsonArray array = legacyArguments.value(QStringLiteral("messages")).toArray();
    QJsonObject second = array.at(1).toObject();
    second.insert(QStringLiteral("tool_calls"),
                  QJsonArray{QJsonObject{{"id", QStringLiteral("c9")},
                                         {"type", QStringLiteral("function")},
                                         {"function", QJsonObject{
                                           {"name", QStringLiteral("paleo.query_project")},
                                           {"arguments", QJsonObject{{"topic", QStringLiteral("wells")}}}}}}});
    array.replace(1, second);
    legacyArguments.insert(QStringLiteral("messages"), array);
  }
  const QString legacy = write(legacyArguments);
  ChatSession restored;
  QString error;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY2(ChatSessionStore::load(legacy, &restored, &error, &reject),
           qPrintable(error));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::None);
  QCOMPARE(restored.messages.last().toolCalls.size(), 1);
}

void TestH2SessionArchive::directoryAndSymlinkMasqueradeRefused() {
  // 目录伪装成会话文件。
  const QString dirPath = ChatSessionStore::pathFor(QStringLiteral("h2-dir-fake"));
  QDir().mkdir(dirPath);
  ChatSession out;
  QString error;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-dir-fake"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::NotRegularFile);
  QVERIFY(QDir(dirPath).removeRecursively()); // 原样保留：load 没动它

  // 叶子符号链接指向别的会话文件：别名被拒（不读目标、不缓存）。
  const QString real = ChatSessionStore::pathFor(QStringLiteral("h2-sym-real"));
  {
    QFile file(real);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(makeSession(QStringLiteral("h2-sym-real"),
                                         QStringLiteral("s"), 1).toJson())
                 .toJson(QJsonDocument::Compact));
  }
  const QString link = ChatSessionStore::pathFor(QStringLiteral("h2-sym-alias"));
  QVERIFY(QFile::link(real, link));
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-sym-alias"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::NotRegularFile);
  // 穿过链接到达 root 外的文件同样被拒：叶子解析后的真实路径必须在根内。
  QTemporaryDir outside;
  const QString outsideFile = outside.path() + QStringLiteral("/evil.json");
  {
    QFile file(outsideFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(makeSession(QStringLiteral("h2-outside"),
                                         QStringLiteral("s"), 1).toJson())
                 .toJson(QJsonDocument::Compact));
  }
  const QString linkOutside = ChatSessionStore::pathFor(QStringLiteral("h2-sym-outside"));
  QVERIFY(QFile::link(outsideFile, linkOutside));
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-sym-outside"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::NotRegularFile);
}

void TestH2SessionArchive::overLimitArchivePreservedBytes() {
  // 缩放档：上限 4 KiB；先用生产档写出大档案（> 4 KiB），再切缩放档验证
  // 「超限旧档案」路径——字节保留、明确拒绝、可删除。
  paleo::chat::ChatArchiveBudget budget;
  budget.maxArchiveBytes = 4096;
  budget.maxTitleChars = 64;
  budget.maxTitleCharsLoad = 4096;
  budget.maxMessages = 100;

  const QString big = QString(9000, QLatin1Char('x'));
  ChatSession session = makeSession(QStringLiteral("h2-overlimit"), big, 2);
  QString error;
  paleo::chat::setChatArchiveBudgetForTesting(
    paleo::chat::ChatArchiveBudget::production());
  QVERIFY2(ChatSessionStore::save(session, &error), qPrintable(error));
  paleo::chat::setChatArchiveBudgetForTesting(budget); // 已写文件 > 4 KiB

  const QString path = ChatSessionStore::pathFor(session.id);
  const QByteArray before = QFile(path).readAll();
  QVERIFY(before.size() > 4096);
  ChatSession out;
  out.id = QStringLiteral("sentinel");
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY(!ChatSessionStore::load(session.id, &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::SizeExceeded);
  QCOMPARE(QFile(path).readAll(), before); // 不截断、不删除、不改名、不重存
  QCOMPARE(out.id, QStringLiteral("sentinel"));
  QVERIFY(error.contains(QStringLiteral("MiB")));
  // 超限档案仍可经原入口删除（用户可恢复）。
  QVERIFY(ChatSessionStore::remove(session.id, &error));

  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2SessionArchive::boundedReadEnforcedDuringGrowth() {
  // 无限设备：累计上限在读中途生效（不依赖打开前 size）。
  {
    EndlessDevice device;
    QVERIFY(device.openReadOnly());
    paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
    const QByteArray bytes = paleo::chat::readChatArchiveBytes(&device, 100, &reject);
    QVERIFY(bytes.isEmpty());
    QCOMPARE(reject, paleo::chat::ChatArchiveReject::ReadLimitExceeded);
    device.close();
  }
  {
    // 块边界（64 KiB 读取块）上同样拦住：跨块累计超限整体拒绝（部分
    // 读取不外泄——load 侧把任何超限都当作完整失败处理）。
    EndlessDevice device;
    QVERIFY(device.openReadOnly());
    paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
    const QByteArray bytes =
      paleo::chat::readChatArchiveBytes(&device, 65536, &reject);
    QVERIFY(bytes.isEmpty());
    QCOMPARE(reject, paleo::chat::ChatArchiveReject::ReadLimitExceeded);
    device.close();
  }
  {
    // 正常有限设备：完整读回，无拒绝。
    QTemporaryDir dir;
    QFile file(dir.path() + QStringLiteral("/finite.json"));
    QVERIFY(file.open(QIODevice::ReadWrite));
    file.write("{\"schema\":1}");
    file.seek(0);
    paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
    const QByteArray bytes = paleo::chat::readChatArchiveBytes(&file, 4096, &reject);
    QCOMPARE(bytes, QByteArray("{\"schema\":1}"));
    QCOMPARE(reject, paleo::chat::ChatArchiveReject::None);
  }
}

void TestH2SessionArchive::exactBoundaryAcceptedOneByteOverRefused() {
  // 构造恰好等于上限字节的合法档案：可载入；+1 字节：SizeExceeded。
  paleo::chat::ChatArchiveBudget budget;
  budget.maxArchiveBytes = 4096;
  budget.maxTitleChars = 64;
  budget.maxTitleCharsLoad = 4096;
  budget.maxMessages = 100;
  paleo::chat::setChatArchiveBudgetForTesting(budget);

  ChatSession session = makeSession(QStringLiteral("h2-exact"), QStringLiteral("m"), 1);
  QJsonObject object = session.toJson();
  QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  // 用 title 填充到恰好 4096（ASCII 每字符恰 1 字节，一步收敛）。
  QString title = QStringLiteral("pad");
  object.insert(QStringLiteral("title"), title);
  bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  title += QString(4096 - bytes.size(), QLatin1Char('x'));
  object.insert(QStringLiteral("title"), title);
  bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QCOMPARE(bytes.size(), 4096);

  const QString id = QStringLiteral("h2-exact");
  writeRawArchive(id, object);
  ChatSession out;
  QString error;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY2(ChatSessionStore::load(id, &out, &error, &reject), qPrintable(error));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::None);
  QCOMPARE(out.title, title);

  // +1 字节即超限。
  QJsonObject oneMore = object;
  oneMore.insert(QStringLiteral("title"), title + QLatin1Char('x'));
  writeRawArchive(QStringLiteral("h2-exact-plus1"), oneMore);
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-exact-plus1"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::SizeExceeded);

  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2SessionArchive::messageBudgetRefused() {
  paleo::chat::ChatArchiveBudget budget;
  budget.maxArchiveBytes = 1 << 20;
  budget.maxTitleChars = 64;
  budget.maxTitleCharsLoad = 4096;
  budget.maxMessages = 4;
  paleo::chat::setChatArchiveBudgetForTesting(budget);
  // 直接写盘构造「消息数超限」的旧档案（save 侧同尺拒绝，不经 save）。
  ChatSession session = makeSession(QStringLiteral("h2-count"), QStringLiteral("m"), 3);
  writeRawArchive(session.id, session.toJson());
  ChatSession out;
  QString error;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY(!ChatSessionStore::load(session.id, &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::MessageBudget);
  // 对称面：save 拒绝写超条数档案（不产生 load 永远拒绝的档案）。
  QVERIFY(!ChatSessionStore::save(session, &error));
  QVERIFY(error.contains(QStringLiteral("消息数")));
  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2SessionArchive::admissionMathBoundaries() {
  // 纯准入数学（无 IO）：构造差 1 字节即拒绝/放行的边界。
  paleo::chat::ChatArchiveBudget budget;
  budget.toolRounds = 1;
  budget.maxResponseBytes = 256;
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
  budget.maxArchiveBytes = budget.turnReserve() + budget.titleReserve() +
                           2 * 1024; // fresh 会话余量 2 KiB

  ChatSession empty;
  empty.id = QStringLiteral("h2-admit");
  QString reason;
  QVERIFY(paleo::chat::chatArchiveAcceptsTurn(empty, QStringLiteral("hi"), budget, &reason));

  // 用户输入恰好把余量用完：放行；再加 1 字符：拒绝。探针与生产同形
  //（timestamp + 32 码元 host_turn_id——见 chatArchiveAcceptsTurn）。
  ChatMessage probe;
  probe.role = ChatRole::User;
  probe.timestamp = QDateTime::currentDateTimeUtc();
  probe.hostTurnId = QString(32, QLatin1Char('0'));
  // ASCII 内容每多一个字符，序列化恰好多 1 字节：先量出消息信封的固定开销，
  // 再把内容长度反解到「恰好等于上限」（期望值由度量得出，不抄实现公式）。
  probe.content = QStringLiteral("u");
  const qint64 envelope = paleo::chat::chatArchiveSerializedBytesWithMessage(empty, probe) - 1;
  const qint64 contentLength = budget.maxArchiveBytes - budget.turnReserve() -
                               budget.titleReserve() - envelope;
  QVERIFY2(contentLength > 0 && contentLength < 100000,
           qPrintable(QStringLiteral("contentLength=%1").arg(contentLength)));
  probe.content = QString(contentLength, QLatin1Char('u'));
  const qint64 projected = paleo::chat::chatArchiveSerializedBytesWithMessage(empty, probe) +
                           budget.turnReserve() + budget.titleReserve();
  QCOMPARE(projected, budget.maxArchiveBytes);
  QVERIFY(paleo::chat::chatArchiveAcceptsTurn(empty, probe.content, budget, &reason));
  QVERIFY(!paleo::chat::chatArchiveAcceptsTurn(empty, probe.content + QLatin1Char('!'),
                                               budget, &reason));
  QCOMPARE(reason, QStringLiteral("archive_budget_exceeded"));

  // 消息数边界：护栏按「现有 + 一轮可能追加的最大条数」评估。
  paleo::chat::ChatArchiveBudget tiny = budget;
  tiny.maxMessages = budget.messagesPerTurn() + 2;
  ChatSession three;
  three.id = QStringLiteral("h2-admit-3");
  for (int i = 0; i < 3; ++i) {
    ChatMessage m;
    m.role = ChatRole::User;
    m.content = QStringLiteral("m");
    three.messages.append(m);
  }
  QVERIFY(!paleo::chat::chatArchiveAcceptsTurn(three, QStringLiteral("x"), tiny, &reason));
  QCOMPARE(reason, QStringLiteral("message_budget_exceeded"));
  QVERIFY(paleo::chat::chatArchiveAcceptsTurn(three, QStringLiteral("x"), budget, &reason));

  // retry 前缀边界：大内容放在**最后一条 user 之前**（前缀=整体）——
  // 「合法近上限旧档案（最后一轮很短、较早历史很大）的重试必须在 resize
  // 前拒绝」。夹具形状与 capacitygates 的 nearCap 同款。
  ChatSession retrySession = empty;
  ChatMessage assistant;
  assistant.role = ChatRole::Assistant;
  assistant.content = QStringLiteral("早期大回答");
  retrySession.messages.append(assistant);
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QStringLiteral("q");
  retrySession.messages.append(user);
  QVERIFY2(paleo::chat::chatArchiveAcceptsRetryPrefix(retrySession, 1, budget, &reason),
           qPrintable(reason)); // 小前缀 + 预留仍在限内：放行
  // 加大早期回答：整体仍可 load（< cap），但前缀（=整体）+ 一轮预留超限。
  qint64 whole = paleo::chat::chatArchiveSerializedBytes(retrySession);
  const qint64 target = budget.maxArchiveBytes - 64; // cap 内、贴近上限
  QVERIFY(target > whole);
  assistant.content += QString(target - whole, QLatin1Char('a'));
  retrySession.messages.first() = assistant;
  whole = paleo::chat::chatArchiveSerializedBytes(retrySession);
  QVERIFY(whole < budget.maxArchiveBytes);            // 可载入
  QVERIFY(whole + budget.turnReserve() + budget.titleReserve() >
          budget.maxArchiveBytes);                    // 不足一轮预留
  QVERIFY(!paleo::chat::chatArchiveAcceptsRetryPrefix(retrySession, 1, budget, &reason));
  QCOMPARE(reason, QStringLiteral("archive_budget_exceeded"));
  QVERIFY(!paleo::chat::chatArchiveAcceptsTurn(retrySession, QStringLiteral("x"),
                                               budget, &reason)); // 发送同门
  // lastUser 越界/指向非 user：拒绝（与 validateInvocationRetry 同界）。
  QVERIFY(!paleo::chat::chatArchiveAcceptsRetryPrefix(retrySession, -1, budget, &reason));
  QVERIFY(!paleo::chat::chatArchiveAcceptsRetryPrefix(retrySession, 0, budget, &reason));
  QVERIFY(!paleo::chat::chatArchiveAcceptsRetryPrefix(retrySession, 5, budget, &reason));

  // 标题：超字段/候选档案成本拒绝；合法标题放行。
  QVERIFY(!paleo::chat::chatArchiveAcceptsTitle(empty, QString(33, QLatin1Char('t')),
                                                budget, &reason));
  QCOMPARE(reason, QStringLiteral("title_field_budget_exceeded"));
  QVERIFY(paleo::chat::chatArchiveAcceptsTitle(empty, QStringLiteral("正常标题"), budget, &reason));
  paleo::chat::ChatArchiveBudget small = budget;
  small.maxArchiveBytes = 128; // 候选档案本身放不下
  QVERIFY(!paleo::chat::chatArchiveAcceptsTitle(retrySession, QStringLiteral("t"),
                                                small, &reason));
  QCOMPARE(reason, QStringLiteral("archive_budget_exceeded"));

  // 生产预算交叉核对：fresh 空会话 + 最大一轮预留恒可容纳（W3 门的不变量）。
  const paleo::chat::ChatArchiveBudget production = paleo::chat::ChatArchiveBudget::production();
  QVERIFY(production.maxArchiveBytes >=
          production.turnReserve() + production.titleReserve() +
          paleo::chat::chatArchiveSerializedBytes(empty));
  QCOMPARE(production.escapeWorstFactor, 6);
  QCOMPARE(production.maxResponseBytes, paleo::chat::kArchiveMaxModelResponseBytes);
  QCOMPARE(production.toolRounds, paleo::chat::kArchiveMaxToolRounds);
}

void TestH2SessionArchive::saveRefusesOverCapKeepsPreviousFile() {
  // 写侧对称容量：超限 save 拒绝，上一份完整文件保留。
  paleo::chat::ChatArchiveBudget budget;
  budget.maxArchiveBytes = 4096;
  budget.maxTitleChars = 64;
  budget.maxTitleCharsLoad = 4096;
  budget.maxMessages = 100;
  paleo::chat::setChatArchiveBudgetForTesting(budget);
  ChatSession session = makeSession(QStringLiteral("h2-save-cap"), QStringLiteral("small"), 1);
  QString error;
  QVERIFY(ChatSessionStore::save(session, &error));
  const QString path = ChatSessionStore::pathFor(session.id);
  const QByteArray good = QFile(path).readAll();

  session.messages.append(ChatMessage()); // 明显超限增长
  session.messages.last().role = ChatRole::Assistant;
  session.messages.last().content = QString(9000, QLatin1Char('z'));
  QVERIFY(!ChatSessionStore::save(session, &error));
  QVERIFY(error.contains(QStringLiteral("MiB")));
  QCOMPARE(QFile(path).readAll(), good); // 上一份完整文件保留
  paleo::chat::setChatArchiveBudgetForTesting(paleo::chat::ChatArchiveBudget::production());
}

void TestH2SessionArchive::sidecarWrittenAfterSaveRemovedWithSession() {
  ChatSession session = makeSession(QStringLiteral("h2-sidecar"), QStringLiteral("正文标题来源"), 1);
  QString error;
  QVERIFY(ChatSessionStore::save(session, &error));
  paleo::chat::ChatSessionIndex::Entry entry;
  QVERIFY(paleo::chat::ChatSessionIndex::read(session.id, &entry));
  QCOMPARE(entry.id, session.id);
  QCOMPARE(entry.state, paleo::chat::ChatSessionSummary::State::Ok);
  QVERIFY(entry.displayTitle.contains(QStringLiteral("tst-")));
  QVERIFY(entry.sourceBytes > 0);
  const QFileInfo info(ChatSessionStore::pathFor(session.id));
  QCOMPARE(entry.sourceBytes, info.size());
  QCOMPARE(entry.sourceMtimeMs, info.lastModified().toMSecsSinceEpoch());

  // 坏 sidecar（截断/换 id）：读取失败（待修复），正文不受影响。
  const QString sidecar = paleo::chat::ChatSessionIndex::sidecarPathFor(session.id);
  QVERIFY(!sidecar.isEmpty());
  {
    QFile file(sidecar);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"schema\":1,\"id\":\"someone-else\"}");
  }
  paleo::chat::ChatSessionIndex::Entry bad;
  QVERIFY(!paleo::chat::ChatSessionIndex::read(session.id, &bad));
  ChatSession restored;
  QVERIFY(ChatSessionStore::load(session.id, &restored, &error)); // 正文权威不受缓存影响

  // 删除会话：sidecar 一并清理。
  QVERIFY(ChatSessionStore::remove(session.id, &error));
  QVERIFY(!QFileInfo::exists(sidecar));
}

QTEST_MAIN(TestH2SessionArchive)
#include "tst_h2_sessionarchive.moc"
