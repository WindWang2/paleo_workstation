#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <functional>

#include "../src/ai/chat/llmconfigcommit.h"
#include "../src/workflow/aichatcontroller.h"

// G1（W7）配置提交协议回归：LlmConfigCommitter 串行链 + 失败补偿 +
// applyConfig 接线。存储面全部注入（确定性失败注入、次序记录）；真实生产
// 存储走 defaultLlmConfigCommitHooks（LlmConfig::save + LlmKeyStore，本测试
// 不触真实钥匙串）。
//
// oracle：存储操作次序（ops 日志）+ 生效配置序列 + done 回调 + 「重启组合」
//（文件原文 × 钥匙串残留）——跨储一致性以事实组合断言，不以回调返回值断言。

namespace {
bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 5000) {
  QElapsedTimer clock;
  clock.start();
  while (!predicate() && clock.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return predicate();
}

// 注入式存储：记录操作次序；密钥操作可延迟完成（真异步形态）。
struct FakeStore {
  QByteArray fileBytes;      // 当前配置文件原文（空 = 无文件）
  QByteArray key;            // 钥匙串当前密钥（空 = 无）
  QStringList ops;           // 操作次序日志（断言实际存储次序）
  int failWriteConfig = 0;   // 受控失败注入（次数）
  int failWriteBytes = 0;
  int failWriteKey = 0;
  int failClearKey = 0;
  QList<std::function<void(bool, const QString &)>> pendingKeyOps;

  void settlePending() { // 立即完成所有挂起密钥操作（成功）
    const auto opsCopy = pendingKeyOps;
    pendingKeyOps.clear();
    for (auto &op : opsCopy)
      op(true, QString());
  }

  LlmConfigCommitHooks hooks() {
    LlmConfigCommitHooks hooks;
    hooks.readFile = [this] { return fileBytes; };
    hooks.writeConfig = [this](const LlmConfig &form, QString *error) {
      ops.append(QStringLiteral("writeConfig:%1").arg(form.model));
      if (failWriteConfig > 0) {
        --failWriteConfig;
        if (error)
          *error = QStringLiteral("注入：配置写失败");
        return false;
      }
      // 与生产同款形状（端点/模型等，无密钥）。
      QJsonObject object;
      object.insert(QStringLiteral("endpoint"), form.endpoint.toString());
      object.insert(QStringLiteral("model"), form.model);
      object.insert(QStringLiteral("stream"), form.stream);
      object.insert(QStringLiteral("timeout_ms"), form.timeoutMs);
      object.insert(QStringLiteral("total_timeout_ms"), form.totalTimeoutMs);
      object.insert(QStringLiteral("max_tokens"), form.maxTokens);
      object.insert(QStringLiteral("allow_insecure_http"), form.allowInsecureHttp);
      fileBytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
      return true;
    };
    hooks.writeFileBytes = [this](const QByteArray &bytes, QString *error) {
      ops.append(QStringLiteral("writeFileBytes:%1").arg(bytes.size()));
      if (failWriteBytes > 0) {
        --failWriteBytes;
        if (error)
          *error = QStringLiteral("注入：原文还原失败");
        return false;
      }
      fileBytes = bytes;
      return true;
    };
    hooks.writeKey = [this](QObject *, const QByteArray &newKey,
                            std::function<void(bool, const QString &)> done) {
      ops.append(QStringLiteral("writeKey:%1").arg(QString::fromUtf8(newKey)));
      if (failWriteKey > 0) {
        --failWriteKey;
        done(false, QStringLiteral("注入：密钥写失败"));
        return;
      }
      key = newKey;
      pendingKeyOps.append(std::move(done)); // 延迟完成（真异步形态）
    };
    hooks.clearKey = [this](QObject *,
                            std::function<void(bool, const QString &)> done) {
      ops.append(QStringLiteral("clearKey"));
      if (failClearKey > 0) {
        --failClearKey;
        done(false, QStringLiteral("注入：密钥清失败"));
        return;
      }
      key = QByteArray();
      pendingKeyOps.append(std::move(done));
    };
    return hooks;
  }
};

LlmConfig form(const QString &model, const QString &endpoint = "https://one.example/v1") {
  LlmConfig config;
  config.endpoint = QUrl(endpoint);
  config.model = model;
  config.stream = true;
  config.timeoutMs = 30000;
  config.totalTimeoutMs = 300000;
  config.maxTokens = 1024;
  return config;
}

struct CommitResult {
  int dones = 0;
  bool ok = false;
  QString error;
};

// 跑一次提交并等到 done（密钥操作 settle 完成后）。
CommitResult runCommit(LlmConfigCommitter &committer, FakeStore &store,
                       const LlmConfig &cfg, const QByteArray &newKey,
                       bool clearKey, const LlmConfig &current,
                       LlmConfig *applied = nullptr) {
  CommitResult result;
  QObject context;
  committer.submit(cfg, newKey, clearKey, current, &context,
                   [&](bool ok, const QString &error) {
                     result.dones += 1;
                     result.ok = ok;
                     result.error = error;
                   },
                   [applied](const LlmConfig &config) {
                     if (applied)
                       *applied = config;
                   });
  store.settlePending(); // 同步完成挂起的密钥操作
  return result;
}
} // namespace

class TestG1ConfigCommit : public QObject {
  Q_OBJECT
private slots:
  void structuralFailureTouchesNothing();
  void newKeySuccessWritesFileThenKeyAndApplies();
  void clearKeySuccessAppliesEmptyKey();
  void reuseKeyKeepsCurrentKeyWithoutKeyOp();
  void fileWriteFailureLeavesStoresConsistent();
  void keyWriteFailureRestoresPreviousFile();
  void keyWriteFailureWithoutPreviousFileClearsKeychain();
  void restoreFailureFallsBackToClearKeychain();
  void clearKeyFailureRestoresPreviousFile();
  void clearAndRestoreBothFailReportsHonestly();
  void serializedCommitsKeepActualStorageOrder();
  void olderCallbackCannotOverrideNewerConfig();
  void restartCombinationNeverAutoEnablesUnverifiedPair();
  void contextDeathDropsDoneButChainContinues();
  void applyConfigDelegatesThroughController();
  void applyHappensBeforeDoneCallback();
  void failedSubmitNeverApplies();

private:
  static bool fileHasModel(const QByteArray &bytes, const QString &model) {
    const auto object = QJsonDocument::fromJson(bytes).object();
    return object.value(QStringLiteral("model")).toString() == model;
  }
};

void TestG1ConfigCommit::structuralFailureTouchesNothing() {
  FakeStore store;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  LlmConfig bad = form("m1");
  bad.endpoint = QUrl(QStringLiteral("not-a-url"));
  const CommitResult result = runCommit(committer, store, bad, "k", false, form("m0"));
  QVERIFY(!result.ok);
  QVERIFY(!result.error.isEmpty());
  QCOMPARE(store.ops.size(), 0); // 不落任何存储
  QVERIFY(store.fileBytes.isEmpty());
  QVERIFY(store.key.isEmpty());
}

void TestG1ConfigCommit::newKeySuccessWritesFileThenKeyAndApplies() {
  FakeStore store;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  LlmConfig applied;
  const CommitResult result =
      runCommit(committer, store, form("m1"), "k1", true, form("m0"), &applied);
  QVERIFY2(result.ok, qPrintable(result.error));
  QCOMPARE(result.dones, 1);
  QCOMPARE(store.ops, (QStringList{"writeConfig:m1", "writeKey:k1"})); // 文件先写、密钥后写
  QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k1"));
  QVERIFY(fileHasModel(store.fileBytes, QStringLiteral("m1")));
  QCOMPARE(applied.model, QStringLiteral("m1"));
  QCOMPARE(applied.apiKey, QByteArray("k1"));
}

void TestG1ConfigCommit::clearKeySuccessAppliesEmptyKey() {
  FakeStore store;
  store.fileBytes = QByteArrayLiteral("{\"model\":\"m0\"}");
  store.key = QByteArrayLiteral("k0");
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  LlmConfig applied;
  const CommitResult result =
      runCommit(committer, store, form("m1"), "", true, form("m0"), &applied);
  QVERIFY2(result.ok, qPrintable(result.error));
  QCOMPARE(store.ops, (QStringList{"writeConfig:m1", "clearKey"}));
  QVERIFY(store.key.isEmpty());
  QVERIFY(applied.apiKey.isEmpty()); // 清除后禁用态（无密钥）
}

void TestG1ConfigCommit::reuseKeyKeepsCurrentKeyWithoutKeyOp() {
  FakeStore store;
  store.key = QByteArrayLiteral("k0");
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  LlmConfig current = form("m0");
  current.apiKey = "k0";
  LlmConfig applied;
  const CommitResult result =
      runCommit(committer, store, form("m1"), "", false, current, &applied);
  QVERIFY2(result.ok, qPrintable(result.error));
  QCOMPARE(store.ops, (QStringList{"writeConfig:m1"})); // 不动钥匙串
  QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k0"));
  QCOMPARE(applied.apiKey, QByteArray("k0")); // 沿用当前密钥
}

void TestG1ConfigCommit::fileWriteFailureLeavesStoresConsistent() {
  FakeStore store;
  store.fileBytes = QByteArrayLiteral("{\"model\":\"m0\"}");
  store.key = QByteArrayLiteral("k0");
  store.failWriteConfig = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  LlmConfig applied = form("sentinel");
  const CommitResult result =
      runCommit(committer, store, form("m1"), "k1", false, form("m0"), &applied);
  QVERIFY(!result.ok);
  QVERIFY(result.error.contains(QStringLiteral("配置写入失败")));
  QCOMPARE(store.ops, (QStringList{"writeConfig:m1"})); // 密钥操作未发生
  QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k0")); // 旧组合完好
  QVERIFY(fileHasModel(store.fileBytes, QStringLiteral("m0")));
  QCOMPARE(applied.model, QStringLiteral("sentinel")); // 未生效
}

void TestG1ConfigCommit::keyWriteFailureRestoresPreviousFile() {
  FakeStore store;
  store.fileBytes = QByteArrayLiteral("{\"model\":\"m0\"}");
  store.key = QByteArrayLiteral("k0");
  store.failWriteKey = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  LlmConfig applied = form("sentinel");
  const CommitResult result =
      runCommit(committer, store, form("m1"), "k1", false, form("m0"), &applied);
  QVERIFY(!result.ok);
  QVERIFY(result.error.contains(QStringLiteral("密钥写入钥匙串失败")));
  QCOMPARE(store.ops, (QStringList{"writeConfig:m1", "writeKey:k1",
                                  "writeFileBytes:14"})); // 失败 → 还原旧原文
  QVERIFY(fileHasModel(store.fileBytes, QStringLiteral("m0"))); // 回到旧一致组合
  QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k0"));
  QCOMPARE(applied.model, QStringLiteral("sentinel")); // 新组合未生效
}

void TestG1ConfigCommit::keyWriteFailureWithoutPreviousFileClearsKeychain() {
  FakeStore store;
  store.key = QByteArrayLiteral("k0"); // 有旧密钥但此前无配置文件
  store.failWriteKey = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  const CommitResult result =
      runCommit(committer, store, form("m1"), "k1", false, form("m0"));
  QVERIFY(!result.ok);
  QVERIFY(store.ops.contains(QStringLiteral("clearKey"))); // 降级清钥匙串
  QVERIFY(store.key.isEmpty()); // 重启 = 新端点 + 无密钥 → 诚实禁用态
  QVERIFY(store.fileBytes.contains("m1")); // 文件保留（非敏感）
}

void TestG1ConfigCommit::restoreFailureFallsBackToClearKeychain() {
  FakeStore store;
  store.fileBytes = QByteArrayLiteral("{\"model\":\"m0\"}");
  store.key = QByteArrayLiteral("k0");
  store.failWriteKey = 1;
  store.failWriteBytes = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  const CommitResult result =
      runCommit(committer, store, form("m1"), "k1", false, form("m0"));
  QVERIFY(!result.ok);
  QVERIFY(result.error.contains(QStringLiteral("还原失败")));
  QVERIFY(store.ops.contains(QStringLiteral("clearKey")));
  QVERIFY(store.key.isEmpty()); // 未验证组合不得留给重启
}

void TestG1ConfigCommit::clearKeyFailureRestoresPreviousFile() {
  FakeStore store;
  store.fileBytes = QByteArrayLiteral("{\"model\":\"m0\"}");
  store.key = QByteArrayLiteral("k0");
  store.failClearKey = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  const CommitResult result =
      runCommit(committer, store, form("m1"), "", true, form("m0"));
  QVERIFY(!result.ok);
  QVERIFY(result.error.contains(QStringLiteral("清除密钥失败")));
  QVERIFY(store.ops.contains(QStringLiteral("writeFileBytes:14")));
  QVERIFY(fileHasModel(store.fileBytes, QStringLiteral("m0")));
  QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k0")); // 旧组合一致
}

void TestG1ConfigCommit::clearAndRestoreBothFailReportsHonestly() {
  FakeStore store;
  store.fileBytes = QByteArrayLiteral("{\"model\":\"m0\"}");
  store.key = QByteArrayLiteral("k0");
  store.failClearKey = 2; // 清除失败 + 补偿清也失败
  store.failWriteBytes = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  const CommitResult result =
      runCommit(committer, store, form("m1"), "", true, form("m0"));
  QVERIFY(!result.ok);
  QVERIFY(result.error.contains(QStringLiteral("须手动重新配置"))); // 如实点名
}

void TestG1ConfigCommit::serializedCommitsKeepActualStorageOrder() {
  // 两次带密钥的提交：第二次的密钥操作必须等第一次完成后才开始——
  // 实际存储次序 == 提交次序（不是回调到达次序）。
  FakeStore store;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  CommitResult first, second;
  LlmConfig appliedFirst, appliedSecond;
  QObject context;
  committer.submit(form("m1"), "k1", false, form("m0"), &context,
                   [&](bool ok, const QString &error) {
                     first.dones++;
                     first.ok = ok;
                     first.error = error;
                   },
                   [&](const LlmConfig &config) { appliedFirst = config; });
  committer.submit(form("m2"), "k2", false, form("m0"), &context,
                   [&](bool ok, const QString &error) {
                     second.dones++;
                     second.ok = ok;
                     second.error = error;
                   },
                   [&](const LlmConfig &config) { appliedSecond = config; });
  // 第二个提交已入队，但密钥操作必须尚未开始（第一个在途）。
  QCOMPARE(store.ops, (QStringList{"writeConfig:m1", "writeKey:k1"}));
  store.settlePending(); // 完成第一个 → 链推进第二个
  store.settlePending(); // 完成第二个
  QVERIFY(first.ok);
  QVERIFY(second.ok);
  QCOMPARE(store.ops, (QStringList{"writeConfig:m1", "writeKey:k1", "writeConfig:m2",
                                  "writeKey:k2"}));
  QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k2")); // 末次写入胜出
  QVERIFY(fileHasModel(store.fileBytes, QStringLiteral("m2")));
  QCOMPARE(appliedSecond.model, QStringLiteral("m2"));
}

void TestG1ConfigCommit::olderCallbackCannotOverrideNewerConfig() {
  // 并发完成倒序的对抗形态：第一个密钥操作的完成被人为推迟到第二个之后
  // 也不可能——串行链下第二个操作根本不会提前开始；这里验证推迟完成也
  // 不会让旧提交覆盖新提交（生效序列末项恒为新提交）。
  FakeStore store;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  LlmConfig lastApplied;
  QObject context;
  committer.submit(form("m1"), "k1", false, form("m0"), &context, nullptr,
                   [&lastApplied](const LlmConfig &config) { lastApplied = config; });
  committer.submit(form("m2"), "k2", false, form("m0"), &context, nullptr,
                   [&lastApplied](const LlmConfig &config) { lastApplied = config; });
  store.settlePending();
  store.settlePending();
  QCOMPARE(lastApplied.model, QStringLiteral("m2")); // 新提交最终生效
  QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k2"));
}

void TestG1ConfigCommit::restartCombinationNeverAutoEnablesUnverifiedPair() {
  // 「重启组合」oracle：文件原文 × 钥匙串密钥。密钥失败路径结束后，组合
  // 必须要么回到旧一致状态、要么禁用（无密钥）——不得出现「新端点 + 旧密钥」。
  FakeStore store;
  store.fileBytes = QByteArrayLiteral("{\"model\":\"m0\"}");
  store.key = QByteArrayLiteral("k0");
  store.failWriteKey = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  runCommit(committer, store, form("m1"), "k1", false, form("m0"));
  const QString fileModel =
      QJsonDocument::fromJson(store.fileBytes).object().value("model").toString();
  const bool fileIsNew = fileModel == QStringLiteral("m1");
  const bool keyIsOld = store.key == QByteArray("k0");
  QVERIFY2(!(fileIsNew && keyIsOld),
           "重启组合不得是「新端点文件 + 旧密钥」（会被当成功启用并外泄）");
}

void TestG1ConfigCommit::contextDeathDropsDoneButChainContinues() {
  FakeStore store;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  int dones = 0;
  LlmConfig applied;
  {
    QObject context;
    committer.submit(form("m1"), "k1", false, form("m0"), &context,
                     [&](bool, const QString &) { dones++; },
                     [&](const LlmConfig &config) { applied = config; });
    // context 先亡（对话框关闭形态）：done 丢弃，协议链继续。
    store.settlePending();
    QCOMPARE(dones, 1); // 回调在 context 存活期内已同步 settle 完成
  }
  {
    QObject *context = new QObject;
    committer.submit(form("m2"), "k2", false, form("m1"), context,
                     [&](bool, const QString &) { dones++; },
                     [&](const LlmConfig &config) { applied = config; });
    delete context; // 完成前销毁
    store.settlePending();
    QCOMPARE(dones, 1); // done 不触发（context 已亡）
    QCOMPARE(applied.model, QStringLiteral("m2")); // apply 照常（链不断）
    QCOMPARE(QString::fromUtf8(store.key), QStringLiteral("k2"));
  }
}

void TestG1ConfigCommit::applyConfigDelegatesThroughController() {
  FakeStore store;
  AiChatController controller;
  controller.setLlmConfigCommitHooksForTesting(store.hooks());
  controller.setConfig(form("m0"));
  bool ok = false;
  QString error = QStringLiteral("unset");
  controller.applyConfig(form("m1"), "k1", false, &controller,
                         [&](bool result, const QString &message) {
                           ok = result;
                           error = message;
                         });
  store.settlePending();
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(controller.config().model, QStringLiteral("m1"));
  QCOMPARE(controller.config().apiKey, QByteArray("k1"));
  QVERIFY(store.ops.contains(QStringLiteral("writeKey:k1")));
  // 结构失败经同一入口同步拒绝。
  LlmConfig bad = form("bad");
  bad.endpoint = QUrl(QStringLiteral("http://nonloop.example/v1"));
  bool badOk = true;
  QString badError;
  controller.applyConfig(bad, "", false, &controller,
                         [&](bool result, const QString &message) {
                           badOk = result;
                           badError = message;
                         });
  QVERIFY(!badOk);
  QVERIFY(!badError.isEmpty());
  QCOMPARE(controller.config().model, QStringLiteral("m1")); // 未被污染
}

// 契约：done(ok) 触发时配置**已生效**——用共享 trace 直接观测 apply/done
// 的真实先后（不是「done 里看得到值」这种间接推断）。
void TestG1ConfigCommit::applyHappensBeforeDoneCallback() {
  FakeStore store;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  QStringList trace;
  QObject context;
  committer.submit(form("m1"), "k1", false, form("m0"), &context,
                   [&trace](bool ok, const QString &) {
                     trace.append(QStringLiteral("done:%1").arg(ok ? 1 : 0));
                   },
                   [&trace](const LlmConfig &applied) {
                     trace.append(QStringLiteral("apply:%1").arg(applied.model));
                   });
  store.settlePending();
  QCOMPARE(trace, (QStringList{"apply:m1", "done:1"}));
  QCOMPARE(store.ops.last(), QStringLiteral("writeKey:k1"));
}

// 失败提交不得 apply（生效面不被污染），done 仍恰好一次。
void TestG1ConfigCommit::failedSubmitNeverApplies() {
  FakeStore store;
  store.failWriteKey = 1;
  LlmConfigCommitter committer;
  committer.setHooks(store.hooks());
  QStringList trace;
  QObject context;
  committer.submit(form("m1"), "k1", false, form("m0"), &context,
                   [&trace](bool ok, const QString &) {
                     trace.append(QStringLiteral("done:%1").arg(ok ? 1 : 0));
                   },
                   [&trace](const LlmConfig &) { trace.append(QStringLiteral("apply")); });
  store.settlePending();
  QCOMPARE(trace, (QStringList{"done:0"}));
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestG1ConfigCommit tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_g1_configcommit.moc"
