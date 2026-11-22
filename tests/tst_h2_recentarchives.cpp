#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/chatsession.h"
#include "../src/ai/chat/chatsessionarchive.h"
#include "../src/ai/chat/chatsessionindex.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aichatrecentsummaries.h"

// H2（agent-session-archive）W4/W5/W6：可重建索引、有界最近列表与冷修复
// 查询、写/删/缓存失效一致性。可靠性口径：chatArchiveFullLoadCount 计数
//（热列表不得增长）+ sidecar 状态断言，不依赖墙钟。
class TestH2RecentArchives : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();
  void coldRepairBuildsSidecarsAndPublishesComplete();
  void hotRefreshDoesNotParseBodies();
  void orderingTiesAndMissingTimes();
  void staleDetectionAndExternalReplace();
  void overLimitAndInvalidSummaries();
  void externalDeletePrunesOrphanSidecars();
  void noServiceFallsBackToTrustedCache();
  void repairPrimitiveHonoursCancellation();
  void repairCancellableAndRecoversViaTaskService();
  void controllerTeardownDuringRepairIsSafe();
  void saveFactIndependentFromIndexFailure();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 20000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  void seedSession(const QString &id, const QString &body, int offsetSeconds,
                   const QString &title = {});
  int sidecarCount() const;
  void waitForComplete(AiChatRecentSummaries &summaries);
  QStringList m_sessions;
  paleo::chat::ChatArchiveBudget m_budget;
};

void TestH2RecentArchives::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
  QDir(ChatSessionStore::directory()).removeRecursively();
  QDir().mkpath(ChatSessionStore::directory());
  // 缩放档：让「超限/边界」用例无需巨型夹具（结构同生产）。
  m_budget = paleo::chat::ChatArchiveBudget::production();
  m_budget.maxArchiveBytes = 8192;
  m_budget.maxTitleChars = 64;
  m_budget.maxTitleCharsLoad = 4096;
  m_budget.maxMessages = 2000;
  paleo::chat::setChatArchiveBudgetForTesting(m_budget);
}

void TestH2RecentArchives::cleanup() {
  for (const QString &id : ChatSessionStore::listIds())
    ChatSessionStore::remove(id, nullptr);
  m_sessions.clear();
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively();
  paleo::chat::setChatArchiveBudgetForTesting(m_budget);
}

void TestH2RecentArchives::seedSession(const QString &id, const QString &body,
                                       int offsetSeconds, const QString &title) {
  ChatSession session;
  session.id = id;
  session.title = title;
  session.model = QStringLiteral("test-model");
  session.createdAt = QDateTime::currentDateTimeUtc().addSecs(-3600 + offsetSeconds);
  session.updatedAt = QDateTime::currentDateTimeUtc().addSecs(-offsetSeconds);
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = body;
  user.timestamp = session.createdAt;
  session.messages.append(user);
  QString error;
  QVERIFY2(ChatSessionStore::save(session, &error), qPrintable(error));
  m_sessions.append(id);
}

int TestH2RecentArchives::sidecarCount() const {
  return QDir(paleo::chat::ChatSessionIndex::indexPath())
    .entryList({QStringLiteral("*.meta.json")}, QDir::Files)
    .size();
}

void TestH2RecentArchives::waitForComplete(AiChatRecentSummaries &summaries) {
  bool sawComplete = false;
  auto connection = connect(&summaries, &AiChatRecentSummaries::ready,
                            [&](const QVector<paleo::chat::ChatSessionSummary> &,
                                bool complete) { sawComplete = sawComplete || complete; });
  summaries.refresh(20);
  spinUntil([&] { return sawComplete && !summaries.repairInFlight(); });
  disconnect(connection);
  QVERIFY2(sawComplete, "冷修复后必须发布 complete 结果");
}

void TestH2RecentArchives::coldRepairBuildsSidecarsAndPublishesComplete() {
  // 冷启动：seed 后清空 sidecar（模拟 H2 之前的存量会话——save 会顺带写
  // sidecar，故 wipe 必须在 seed 之后）。
  const int total = 1000;
  for (int i = 0; i < total; ++i)
    seedSession(QStringLiteral("h2-cold-%1").arg(i, 4, 10, QLatin1Char('0')),
                QStringLiteral("会话正文 %1").arg(i), i);
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively();

  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  QVector<paleo::chat::ChatSessionSummary> firstPage;
  bool firstComplete = true;
  auto connection = connect(
    &summaries, &AiChatRecentSummaries::ready,
    [&](const QVector<paleo::chat::ChatSessionSummary> &result, bool complete) {
      if (firstComplete) { // 首发一定是 partial（全部待修复）
        firstPage = result;
        firstComplete = complete;
      }
    });
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] {
    return sidecarCount() == total && !summaries.repairInFlight();
  }), "冷修复应在有界时间内为全部候选重建 sidecar");
  disconnect(connection);

  QCOMPARE(sidecarCount(), total);
  QVERIFY(!firstComplete); // 有限扫描的第一页不冒称完整集合
  QCOMPARE(paleo::chat::chatArchiveFullLoadCount(), qint64(total)); // 每候选恰一次

  // 修复后的完整发布：1000 条按 updatedAt 倒序、limit 生效。
  QVector<paleo::chat::ChatSessionSummary> completed;
  bool completeFlag = false;
  connection = connect(&summaries, &AiChatRecentSummaries::ready,
                       [&](const QVector<paleo::chat::ChatSessionSummary> &result,
                           bool complete) {
                         if (complete) { completed = result; completeFlag = true; }
                       });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return completeFlag; }), "complete 结果必须发布");
  disconnect(connection);
  QCOMPARE(completed.size(), 20);
  for (int i = 1; i < completed.size(); ++i)
    QVERIFY2(completed.at(i - 1).updatedAt >= completed.at(i).updatedAt,
             "updatedAt 倒序");
  QCOMPARE(completed.first().id, QStringLiteral("h2-cold-0000"));
  QCOMPARE(completed.last().id, QStringLiteral("h2-cold-0019"));
}

void TestH2RecentArchives::hotRefreshDoesNotParseBodies() {
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively();
  for (int i = 0; i < 80; ++i)
    seedSession(QStringLiteral("h2-hot-%1").arg(i, 3, 10, QLatin1Char('0')),
                QStringLiteral("正文 %1").arg(i), i);
  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  waitForComplete(summaries);

  // 热路径：连续多次刷新（模拟每 checkpoint 的 sessionListChanged），
  // 权威正文解析计数零增长——列表不再随历史正文全量解析。
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  for (int i = 0; i < 5; ++i)
    summaries.refresh(20);
  QCOMPARE(paleo::chat::chatArchiveFullLoadCount(), qint64(0));

  // 少量已变文件：定向失效修复（只重新解析变化的那一个）——外部改写不走
  // Store::save（sidecar 不会顺带更新），逼出真正的修复路径。
  const QString changed = QStringLiteral("h2-hot-007");
  {
    ChatSession session;
    QString error;
    QVERIFY(ChatSessionStore::load(changed, &session, &error));
    session.title = QStringLiteral("外部改写标题");
    session.updatedAt = QDateTime::currentDateTimeUtc();
    session.messages.first().content = QStringLiteral("外部改写后的正文");
    QFile file(ChatSessionStore::pathFor(changed));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(session.toJson()).toJson(QJsonDocument::Compact));
  }
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  bool sawUpdate = false;
  auto connection = connect(
    &summaries, &AiChatRecentSummaries::ready,
    [&](const QVector<paleo::chat::ChatSessionSummary> &result, bool) {
      for (const auto &s : result)
        sawUpdate = sawUpdate || (s.id == changed &&
                                  s.displayTitle == QStringLiteral("外部改写标题"));
    });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return sawUpdate; }), "外部改写后定向修复必须完成");
  disconnect(connection);
  QCOMPARE(paleo::chat::chatArchiveFullLoadCount(), qint64(1)); // 定向修复恰好一次
  // 其余 79 个正文零解析。
}

void TestH2RecentArchives::orderingTiesAndMissingTimes() {
  // 同 updatedAt → id 升序；缺合法时间排最后；updatedAt 优先于 createdAt。
  const QDateTime same = QDateTime::currentDateTimeUtc();
  for (const QString &id : {QStringLiteral("h2-tie-b"), QStringLiteral("h2-tie-a"),
                            QStringLiteral("h2-tie-c")}) {
    ChatSession session;
    session.id = id;
    session.model = QStringLiteral("m");
    session.createdAt = same;
    session.updatedAt = same;
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = QStringLiteral("同秒写入");
    user.timestamp = same;
    session.messages.append(user);
    QString error;
    QVERIFY(ChatSessionStore::save(session, &error));
    m_sessions.append(id);
  }
  // 只有 createdAt（旧档容错）：按 createdAt 参与排序。
  {
    ChatSession session;
    session.id = QStringLiteral("h2-only-created");
    session.createdAt = same.addSecs(-5);
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = QStringLiteral("旧档案");
    user.timestamp = session.createdAt;
    session.messages.append(user);
    QString error;
    QVERIFY(ChatSessionStore::save(session, &error));
    m_sessions.append(session.id);
  }
  // 无任何合法时间：排最后（不悄悄改用文件 mtime 冒充等价）。
  {
    ChatSession session;
    session.id = QStringLiteral("h2-no-time");
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = QStringLiteral("无时间档案");
    session.messages.append(user);
    QString error;
    QVERIFY(ChatSessionStore::save(session, &error));
    m_sessions.append(session.id);
  }
  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  QVector<paleo::chat::ChatSessionSummary> result;
  waitForComplete(summaries);
  auto connection = connect(&summaries, &AiChatRecentSummaries::ready,
                            [&](const QVector<paleo::chat::ChatSessionSummary> &r,
                                bool complete) {
                              if (complete)
                                result = r;
                            });
  summaries.refresh(20);
  spinUntil([&] { return !result.isEmpty(); });
  disconnect(connection);
  QCOMPARE(result.size(), 5);
  // 三条同秒在最新区（同值按 id 升序）；h2-only-created（createdAt 早 5 秒）
  // 其后；无合法时间的档案最后（不改用文件 mtime 冒充等价）。
  QCOMPARE(result.at(0).id, QStringLiteral("h2-tie-a"));
  QCOMPARE(result.at(1).id, QStringLiteral("h2-tie-b"));
  QCOMPARE(result.at(2).id, QStringLiteral("h2-tie-c"));
  QCOMPARE(result.at(3).id, QStringLiteral("h2-only-created"));
  QCOMPARE(result.at(4).id, QStringLiteral("h2-no-time"));
}

void TestH2RecentArchives::staleDetectionAndExternalReplace() {
  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  seedSession(QStringLiteral("h2-stale"), QStringLiteral("旧标题正文"), 10);
  waitForComplete(summaries);

  // 外部改写（替换）：size/mtime 弱指纹失配 → 定向修复 → 新标题生效。
  ChatSession session;
  QString error;
  QVERIFY(ChatSessionStore::load(QStringLiteral("h2-stale"), &session, &error));
  session.title = QStringLiteral("外部重命名");
  session.updatedAt = QDateTime::currentDateTimeUtc();
  session.messages.first().content = QStringLiteral("外部改写后的正文内容更长");
  QVERIFY(ChatSessionStore::save(session, &error));

  bool sawNewTitle = false;
  auto connection = connect(
    &summaries, &AiChatRecentSummaries::ready,
    [&](const QVector<paleo::chat::ChatSessionSummary> &result, bool) {
      for (const auto &s : result)
        sawNewTitle = sawNewTitle || s.displayTitle == QStringLiteral("外部重命名");
    });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return sawNewTitle; }), "外部改写标题必须进入列表");
  disconnect(connection);

  // 损坏 sidecar（截断）：读取失败 → 待修复 → 从权威文件重建（不反向覆盖正文）。
  const QString sidecar =
    paleo::chat::ChatSessionIndex::sidecarPathFor(QStringLiteral("h2-stale"));
  {
    QFile file(sidecar);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"schema\":1"); // 截断 JSON
  }
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] {
    paleo::chat::ChatSessionIndex::Entry entry;
    return paleo::chat::ChatSessionIndex::read(QStringLiteral("h2-stale"), &entry) &&
           entry.displayTitle == QStringLiteral("外部重命名");
  }), "坏缓存必须从权威文件重建");
}

void TestH2RecentArchives::overLimitAndInvalidSummaries() {
  // 超限旧档案：元信息状态 OverLimit，不读正文、不伪称完整载入。
  {
    paleo::chat::setChatArchiveBudgetForTesting(
      paleo::chat::ChatArchiveBudget::production()); // 用生产档写出大档案
    ChatSession big;
    big.id = QStringLiteral("h2-over");
    big.model = QStringLiteral("m");
    big.createdAt = big.updatedAt = QDateTime::currentDateTimeUtc();
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = QString(20000, QLatin1Char('x'));
    big.messages.append(user);
    QString error;
    QVERIFY(ChatSessionStore::save(big, &error));
    m_sessions.append(big.id);
    paleo::chat::setChatArchiveBudgetForTesting(m_budget); // 回到缩放档
  }
  // 损坏档案：结构错误 → Invalid + 稳定诊断码。
  {
    QFile file(ChatSessionStore::pathFor(QStringLiteral("h2-invalid")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"schema\":1,\"id\":\"h2-invalid\",\"messages\":\"not-an-array\"}");
    m_sessions.append(QStringLiteral("h2-invalid"));
  }
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  QVector<paleo::chat::ChatSessionSummary> result;
  bool complete = false;
  auto connection = connect(
    &summaries, &AiChatRecentSummaries::ready,
    [&](const QVector<paleo::chat::ChatSessionSummary> &r, bool c) {
      result = r;
      complete = c;
    });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return complete; }), "超限/损坏状态必须发布 complete");
  disconnect(connection);

  bool sawOver = false, sawInvalid = false;
  for (const auto &s : result) {
    if (s.id == QStringLiteral("h2-over")) {
      sawOver = true;
      QCOMPARE(s.state, paleo::chat::ChatSessionSummary::State::OverLimit);
      QCOMPARE(s.diagnostic, QStringLiteral("archive_size_exceeded"));
    }
    if (s.id == QStringLiteral("h2-invalid")) {
      sawInvalid = true;
      QCOMPARE(s.state, paleo::chat::ChatSessionSummary::State::Invalid);
      QCOMPARE(s.diagnostic, QStringLiteral("structure_invalid"));
    }
  }
  QVERIFY(sawOver);
  QVERIFY(sawInvalid);
  // 超限档案不读正文（只 stat）；损坏档案读一次后判 Invalid。
  QCOMPARE(paleo::chat::chatArchiveFullLoadCount(), qint64(1));
  // 权威入口照旧拒绝完整载入。
  ChatSession out;
  QString error;
  paleo::chat::ChatArchiveReject reject = paleo::chat::ChatArchiveReject::None;
  QVERIFY(!ChatSessionStore::load(QStringLiteral("h2-over"), &out, &error, &reject));
  QCOMPARE(reject, paleo::chat::ChatArchiveReject::SizeExceeded);
}

void TestH2RecentArchives::externalDeletePrunesOrphanSidecars() {
  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  seedSession(QStringLiteral("h2-del"), QStringLiteral("将被外部删除"), 5);
  waitForComplete(summaries);
  QVERIFY(QFileInfo::exists(
    paleo::chat::ChatSessionIndex::sidecarPathFor(QStringLiteral("h2-del"))));

  // 外部删除正文（不经 Store::remove）：孤儿 sidecar 在下一次修复时清理。
  QVERIFY(QFile::remove(ChatSessionStore::pathFor(QStringLiteral("h2-del"))));
  bool sawGone = false;
  auto connection = connect(
    &summaries, &AiChatRecentSummaries::ready,
    [&](const QVector<paleo::chat::ChatSessionSummary> &result, bool complete) {
      if (!complete)
        return;
      sawGone = true;
      for (const auto &s : result)
        QVERIFY2(s.id != QStringLiteral("h2-del"), "已删除会话不得复活");
    });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return sawGone; }), "外部删除后列表必须收敛");
  disconnect(connection);
  QVERIFY2(!QFileInfo::exists(
             paleo::chat::ChatSessionIndex::sidecarPathFor(QStringLiteral("h2-del"))),
           "孤儿 sidecar 必须被清理");
}

void TestH2RecentArchives::noServiceFallsBackToTrustedCache() {
  // 无任务服务：冷缓存 → 只发布 partial（不在 GUI 线程做全库正文解析）；
  // 缓存可信（手动修复后）→ 直接发布 complete。
  seedSession(QStringLiteral("h2-nosvc-a"), QStringLiteral("a"), 1);
  seedSession(QStringLiteral("h2-nosvc-b"), QStringLiteral("b"), 2);
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively(); // seed 后清缓存
  AiChatRecentSummaries summaries; // 无 setTaskService
  QVector<paleo::chat::ChatSessionSummary> result;
  bool complete = true;
  auto connection = connect(&summaries, &AiChatRecentSummaries::ready,
                            [&](const QVector<paleo::chat::ChatSessionSummary> &r,
                                bool c) {
                              result = r;
                              complete = c;
                            });
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  summaries.refresh(20);
  QCOMPARE(paleo::chat::chatArchiveFullLoadCount(), qint64(0)); // 零正文解析
  QVERIFY(!complete);

  // 手动修复（等价于另一进程/上一次运行留下的可信缓存）后：complete。
  paleo::chat::ChatArchiveScan scan =
    paleo::chat::scanChatArchiveFiles(ChatSessionStore::directory(), 4096);
  paleo::chat::repairChatArchiveSummaries(scan.files, [] { return false; });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return complete; }), "无服务+可信缓存必须发布 complete");
  disconnect(connection);
  QVERIFY(complete);
  QCOMPARE(result.size(), 2);
  QCOMPARE(result.first().id, QStringLiteral("h2-nosvc-b")); // updatedAt 新者优先
}

void TestH2RecentArchives::repairPrimitiveHonoursCancellation() {
  for (int i = 0; i < 12; ++i)
    seedSession(QStringLiteral("h2-cancel-%1").arg(i, 2, 10, QLatin1Char('0')),
                QStringLiteral("c%1").arg(i), i);
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively(); // seed 后清缓存
  paleo::chat::ChatArchiveScan scan =
    paleo::chat::scanChatArchiveFiles(ChatSessionStore::directory(), 4096);
  QCOMPARE(scan.files.size(), 12);
  int checks = 0;
  const auto outcome = paleo::chat::repairChatArchiveSummaries(
    scan.files, [&checks] { return ++checks > 3; }); // 第 4 次检查起取消
  QVERIFY(outcome.items.size() < 12);
  QVERIFY(outcome.items.size() >= 3);
  QCOMPARE(sidecarCount(), outcome.items.size());
}

void TestH2RecentArchives::repairCancellableAndRecoversViaTaskService() {
  for (int i = 0; i < 60; ++i)
    seedSession(QStringLiteral("h2-taskcancel-%1").arg(i, 2, 10, QLatin1Char('0')),
                QStringLiteral("t%1").arg(i), i);
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively(); // seed 后清缓存
  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  summaries.refresh(20);
  // 立刻请求取消（任务页同款入口）：修复停在有界状态，不悬挂、不崩溃。
  for (PaleoTask *task : service.tasks())
    if (task->running())
      task->requestCancel();
  QVERIFY2(spinUntil([&] { return !summaries.repairInFlight(); }),
           "取消后修复必须有界停止（0..60 由取消点决定，不悬挂即达标）");

  // 取消不是终态：再次刷新（无取消）恢复到 complete。
  bool complete = false;
  auto connection = connect(
    &summaries, &AiChatRecentSummaries::ready,
    [&](const QVector<paleo::chat::ChatSessionSummary> &, bool c) { complete = c; });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] {
    return complete && sidecarCount() == 60 && !summaries.repairInFlight();
  }), "取消不是终态：再刷新必须恢复 complete");
  disconnect(connection);
  QCOMPARE(sidecarCount(), 60);
}

void TestH2RecentArchives::controllerTeardownDuringRepairIsSafe() {
  for (int i = 0; i < 40; ++i)
    seedSession(QStringLiteral("h2-teardown-%1").arg(i, 2, 10, QLatin1Char('0')),
                QStringLiteral("d%1").arg(i), i);
  QDir(paleo::chat::ChatSessionIndex::indexPath()).removeRecursively(); // seed 后清缓存
  PaleoTaskService service;
  {
    AiChatController controller;
    controller.toolRunner()->setTaskService(&service);
    controller.requestRecentSessionSummaries(20); // 启动冷修复
    // 修复在池线程进行时销毁控制器（协调器随之销毁）：连接自断、worker
    // 由共享护栏安全收尾，无悬空回调。
    spinUntil([&] { return service.runningCount() > 0 || sidecarCount() > 0; });
  }
  QVERIFY2(spinUntil([&] { return service.runningCount() == 0; }),
           "控制器销毁后 worker 必须安全排空");
  // 服务池排空后进程健康；再次查询照常工作。
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  bool complete = false;
  auto connection = connect(&summaries, &AiChatRecentSummaries::ready,
                            [&](const QVector<paleo::chat::ChatSessionSummary> &,
                                bool c) { complete = c; });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return complete; }), "销毁后的新查询必须照常 complete");
  disconnect(connection);
}

void TestH2RecentArchives::saveFactIndependentFromIndexFailure() {
  seedSession(QStringLiteral("h2-idxfail"), QStringLiteral("正文"), 3);
  PaleoTaskService service;
  AiChatRecentSummaries summaries;
  summaries.setTaskService(&service);
  waitForComplete(summaries);

  // 索引目录只读：权威 save 仍成功（sidecar 失败不产生假欠账），列表保持
  // partial 且不无限重试；权限恢复 + 状态变化后自动收敛。
#ifndef Q_OS_WINDOWS
  const QString indexDir = paleo::chat::ChatSessionIndex::indexPath();
  QFile indexAsFile(indexDir);
  QVERIFY(indexAsFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadGroup |
                                     QFileDevice::ReadOther));
  ChatSession session;
  QString error;
  QVERIFY(ChatSessionStore::load(QStringLiteral("h2-idxfail"), &session, &error));
  session.title = QStringLiteral("索引失败期间的改名");
  session.updatedAt = QDateTime::currentDateTimeUtc();
  paleo::chat::resetChatArchiveFullLoadCountForTesting();
  QVERIFY2(ChatSessionStore::save(session, &error),
           qPrintable(error)); // save=true：索引失败不影响权威保存事实

  int repairs = 0;
  auto connection = connect(&summaries, &AiChatRecentSummaries::ready,
                            [&](const QVector<paleo::chat::ChatSessionSummary> &,
                                bool) { ++repairs; });
  for (int i = 0; i < 4; ++i)
    summaries.refresh(20);
  spinUntil([&] { return repairs >= 4; });
  disconnect(connection);
  const int bounded = sidecarCount(); // 写失败的 sidecar 不落盘

  // 恢复权限并触发状态变化（保存改动）：修复资格恢复，收敛到磁盘事实。
  QVERIFY(indexAsFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                     QFileDevice::ReadOther));
  ChatSession changed;
  QVERIFY(ChatSessionStore::load(QStringLiteral("h2-idxfail"), &changed, &error));
  changed.title = QStringLiteral("恢复后的新标题");
  changed.updatedAt = QDateTime::currentDateTimeUtc();
  QVERIFY(ChatSessionStore::save(changed, &error));
  bool sawTitle = false;
  connection = connect(
    &summaries, &AiChatRecentSummaries::ready,
    [&](const QVector<paleo::chat::ChatSessionSummary> &result, bool) {
      for (const auto &s : result)
        sawTitle = sawTitle || s.displayTitle == QStringLiteral("恢复后的新标题");
    });
  summaries.refresh(20);
  QVERIFY2(spinUntil([&] { return sawTitle; }), "权限恢复后必须收敛到磁盘事实");
  disconnect(connection);
  QVERIFY(sidecarCount() >= bounded);
#endif
}

QTEST_MAIN(TestH2RecentArchives)
#include "tst_h2_recentarchives.moc"
