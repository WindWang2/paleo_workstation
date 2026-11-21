#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>

#include "wellfaciesagentfixture.h"
#include "wellfacieshttpfixture.h"
#include "app/aiwiring.h"
#include "app/capabilityhostwiring.h"
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityrecovery.h"
#include "workflow/documentsession.h"
#include "catalog/areawellprojection.h"
#include "metadata/projectlock.h"
#include "metadata/paleoprojectstore.h"
#include "../src/ai/chat/llmcredentialguard.h"

// G1（W8）凭据边界回归：
//   · llmcredentialguard 单元：任意非空长度的等值回显、≥8 包含式纵深、
//     JSON 转义（引号/反斜杠/非 ASCII）绕过字节包含的对抗形态、脱敏；
//   · runFaciesModels 两出口集成（真实 HTTP 夹具 + 全 runner 接线）：
//     模型目录回显短/转义 key → 诚实拒绝；干净目录（含非 ASCII 显示文本）
//     不受损；失败 reason 回显 key → 脱敏后仍可辨识。
// fixture key 一律假值；不读真实凭据。

namespace {
QByteArray encoded(const QJsonObject &json) {
  return QJsonDocument(json).toJson(QJsonDocument::Compact);
}
QJsonObject exactModel() {
  return {{"id", "model"},
          {"name", "Synthetic model"},
          {"version", "v1"},
          {"inputSchema", QJsonObject{{"curves", QJsonArray{"GR"}},
                                      {"curveUnits", QJsonObject{{"GR", "API"}}},
                                      {"window", 2},
                                      {"labels", QJsonArray{QStringLiteral("河口坝")}},
                                      {"categoricalFields", QJsonArray{QStringLiteral("段")}}}}};
}

// 与 tst_wellfaciesagent 同款宿主接线（只读复用 fixture 形态；本测试只跑
// well_facies_models 查询路径，不需要预测请求装配）。
struct AgentHost {
  Fixture data;
  DataCatalog catalog;
  PaleoProjectStore store;
  PaleoTaskService tasks{&store};
  paleo::document::DocumentSession session{&store, &tasks};
  paleo::capability::CapabilityHost host;
  AiChatController chat;
  std::unique_ptr<ProjectDirLock> lock;
  QString error;
  bool open(const WellFaciesConfig &config) {
    if (!data.init())
      return false;
    data.service.closeProject();
    const QString project = data.job.projectDir;
    store.setProjectPaths(project + "/project.qgz", project + "/project.gpkg",
                          project + "/project.sqlite");
    if (!catalog.open(project, &error) || !registerAreaWells(catalog, &error))
      return false;
    if (!session.bind(&catalog, project + "/project.sqlite", "doc", false, &error))
      return false;
    lock = std::make_unique<ProjectDirLock>(project);
    if (!lock->tryLock(&error))
      return false;
    host.setTaskService(&tasks);
    host.setCallerScope(paleo::host::Caller::Agent, "agent:g1-credential");
    host.setBindingProvider([this] {
      auto binding = captureCapabilityBinding(&session, &catalog, lock.get(), nullptr);
      binding.context.allowedEffects |= paleo::host::ExternalTransfer;
      return binding;
    });
    session.setHostContextProvider([this] { return host.context(); });
    bindChatToolRunner(&chat, nullptr, nullptr, &catalog, project,
                       session.documentId(), &host, &session);
    chat.toolRunner()->setTaskService(&tasks);
    chat.toolRunner()->setFaciesConfig(config);
    chat.toolRunner()->setResultSession("g1-credential-test");
    return true;
  }
  void runModelsQuery(const QString &modelId = {}) {
    QJsonObject args{{"topic", "well_facies_models"}};
    if (!modelId.isEmpty())
      args.insert("model_id", modelId);
    chat.toolRunner()->run(
        {"models", "paleo.query_project", QString::fromUtf8(encoded(args))});
  }
};
} // namespace

class TestG1CredentialBoundary : public QObject {
  Q_OBJECT
private slots:
  // ---- llmcredentialguard 单元 ------------------------------------------
  void valueDiscloseMatchesAnyNonEmptyLength();
  void valueDiscloseKeepsContainsDefenseForLongKeys();
  void jsonDiscloseCatchesEscapedKeyWhereBytesContainsMisses();
  void jsonDiscloseScansNestedStructuresAndKeys();
  void redactHandlesNonAsciiAndQuotes();
  // ---- runFaciesModels 集成（真实 HTTP 夹具） -----------------------------
  void modelCatalogEchoingShortKeyIsRejected();
  void modelCatalogEchoingEscapedKeyIsRejected();
  void cleanCatalogWithNonAsciiNamesIsUnaffected();
  void failureReasonEchoingKeyIsScrubbedButIdentifiable();
};

void TestG1CredentialBoundary::valueDiscloseMatchesAnyNonEmptyLength() {
  const QByteArray shortKey = "s3cr3t"; // 6 字节：旧 ≥8 门槛漏检的形态
  QVERIFY(paleo::chat::valueDisclosesCredential(
      QStringLiteral("s3cr3t"), shortKey));
  QVERIFY(!paleo::chat::valueDisclosesCredential(
      QStringLiteral("other"), shortKey));
  QVERIFY(!paleo::chat::valueDisclosesCredential(
      QStringLiteral("anything"), QByteArray())); // 空 key 恒不命中
  // 短 key 不做包含匹配：正常文本里的单字符巧合不算泄露；等值才命中。
  QVERIFY(!paleo::chat::valueDisclosesCredential(QStringLiteral("alpha"),
                                                 QByteArray("a")));
}

void TestG1CredentialBoundary::valueDiscloseKeepsContainsDefenseForLongKeys() {
  const QByteArray key = "mock-key-0123456789"; // 18 字节
  QVERIFY(paleo::chat::valueDisclosesCredential(
      QStringLiteral("error: mock-key-0123456789 denied"), key)); // 嵌入回显
  QVERIFY(!paleo::chat::valueDisclosesCredential(
      QStringLiteral("mock-key-012345678"), key)); // 前缀不是泄露
}

void TestG1CredentialBoundary::jsonDiscloseCatchesEscapedKeyWhereBytesContainsMisses() {
  // key 含引号/反斜杠/非 ASCII：JSON 序列化后变成 \" 与 \\ 和 UTF-8，
  // 旧的「序列化字节 contains 原始 key」检查会漏——结构化等值必须命中。
  const QByteArray tricky = "sk-\"q\"\\键";
  QJsonObject payload{{"models", QJsonArray{QJsonObject{
      {"id", "model"},
      {"name", QString::fromUtf8(tricky)}, // 回显为字符串值（序列化即转义）
      {"version", "v1"}}}}};
  const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
  QVERIFY2(!bytes.contains(tricky),
           "场景自检：转义后原始 key 不再是字节子串（旧口径会漏检）");
  QVERIFY(paleo::chat::jsonDisclosesCredential(QJsonValue(payload), tricky));
}

void TestG1CredentialBoundary::jsonDiscloseScansNestedStructuresAndKeys() {
  const QByteArray key = "s3cr3t";
  QJsonObject nested{{"inner", QJsonObject{{"token", QStringLiteral("s3cr3t")}}}};
  QVERIFY(paleo::chat::jsonDisclosesCredential(QJsonValue(nested), key));
  QJsonArray array{QJsonObject{{"note", QStringLiteral("value s3cr3t embedded")}}};
  QVERIFY(!paleo::chat::jsonDisclosesCredential(QJsonValue(array), key)); // 短 key 不做包含
  QJsonObject objectKey{{QStringLiteral("s3cr3t"), QStringLiteral("value")}};
  QVERIFY(paleo::chat::jsonDisclosesCredential(QJsonValue(objectKey), key)); // 键名回显
}

void TestG1CredentialBoundary::redactHandlesNonAsciiAndQuotes() {
  QCOMPARE(paleo::chat::redactCredential(
               QStringLiteral("鉴权失败：sk-\"q\"\\键 无效"),
               QByteArray("sk-\"q\"\\键")),
           QStringLiteral("鉴权失败：[已隐藏] 无效"));
  QCOMPARE(paleo::chat::redactCredential(QStringLiteral("无密钥文本"), QByteArray()),
           QStringLiteral("无密钥文本"));
  const QString twice = QStringLiteral("a k1 b k1 c");
  QCOMPARE(paleo::chat::redactCredential(twice, QByteArray("k1")),
           QStringLiteral("a [已隐藏] b [已隐藏] c")); // 全部出现都替换
}

// ---- 集成 ----------------------------------------------------------------

void TestG1CredentialBoundary::modelCatalogEchoingShortKeyIsRejected() {
  const QByteArray key = "s3cr3t"; // 6 字节假 key：旧 ≥8 门槛放行的形态
  ScriptServer server;
  server.handler = [](const QByteArray &, const QJsonObject &) {
    auto model = exactModel();
    model.insert("name", QStringLiteral("s3cr3t")); // 服务端回显密钥
    return Reply{200, encoded({{"models", QJsonArray{model}}})};
  };
  AgentHost f;
  QVERIFY2(f.open(server.config(key)), qPrintable(f.error));
  QSignalSpy finished(f.chat.toolRunner(), &AiChatToolRunner::toolFinished);
  f.runModelsQuery();
  QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
  QVERIFY2(!finished.front()[1].toBool(), "回显密钥的目录必须失败，不得假称完整");
  const QString result = finished.front()[2].toString();
  const auto object = QJsonDocument::fromJson(result.toUtf8()).object();
  QCOMPARE(object.value("code").toString(),
           QStringLiteral("model_catalog_contains_credential"));
  QVERIFY2(!result.contains(QLatin1String(key)),
           "拒绝结果本身不得再回显密钥");
}

void TestG1CredentialBoundary::modelCatalogEchoingEscapedKeyIsRejected() {
  const QByteArray key = "k\"y\\z"; // 引号+反斜杠：序列化后字节包含漏检
  ScriptServer server;
  server.handler = [](const QByteArray &, const QJsonObject &) {
    auto model = exactModel();
    model.insert("name", QStringLiteral("k\"y\\z"));
    return Reply{200, encoded({{"models", QJsonArray{model}}})};
  };
  AgentHost f;
  QVERIFY2(f.open(server.config(key)), qPrintable(f.error));
  QSignalSpy finished(f.chat.toolRunner(), &AiChatToolRunner::toolFinished);
  f.runModelsQuery();
  QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
  QVERIFY(!finished.front()[1].toBool());
  const auto object =
      QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
  QCOMPARE(object.value("code").toString(),
           QStringLiteral("model_catalog_contains_credential"));
}

void TestG1CredentialBoundary::cleanCatalogWithNonAsciiNamesIsUnaffected() {
  const QByteArray key = "s3cr3t";
  ScriptServer server;
  server.handler = [](const QByteArray &, const QJsonObject &) {
    auto model = exactModel();
    model.insert("name", QStringLiteral("河口坝相模型·砂岩")); // 正常非 ASCII 文本
    return Reply{200, encoded({{"models", QJsonArray{model}}})};
  };
  AgentHost f;
  QVERIFY2(f.open(server.config(key)), qPrintable(f.error));
  QSignalSpy finished(f.chat.toolRunner(), &AiChatToolRunner::toolFinished);
  f.runModelsQuery(QStringLiteral("model"));
  QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
  QVERIFY2(finished.front()[1].toBool(),
           qPrintable(finished.front()[2].toString()));
  const auto object =
      QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
  const auto model = object.value("models").toArray().first().toObject();
  QCOMPARE(model.value("name").toString(), QStringLiteral("河口坝相模型·砂岩")); // 文本不受损
  QCOMPARE(object.value("coverage").toObject().value("scope").toString(),
           QStringLiteral("exact_model_id"));
}

void TestG1CredentialBoundary::failureReasonEchoingKeyIsScrubbedButIdentifiable() {
  const QByteArray key = "s3cr3t";
  ScriptServer server;
  server.handler = [key](const QByteArray &, const QJsonObject &) {
    return Reply{500, encoded({{"error", QStringLiteral("invalid key %1 denied")
                                        .arg(QString::fromUtf8(key))}})};
  };
  AgentHost f;
  QVERIFY2(f.open(server.config(key)), qPrintable(f.error));
  QSignalSpy finished(f.chat.toolRunner(), &AiChatToolRunner::toolFinished);
  f.runModelsQuery();
  QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
  QVERIFY(!finished.front()[1].toBool());
  const QString result = finished.front()[2].toString();
  const auto object = QJsonDocument::fromJson(result.toUtf8()).object();
  QCOMPARE(object.value("code").toString(), QStringLiteral("model_catalog_unavailable"));
  QVERIFY2(!result.contains(QLatin1String(key)), "失败 reason 不得携带真实凭据");
  QVERIFY(result.contains(QStringLiteral("invalid key"))); // 原因仍可辨识
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestG1CredentialBoundary tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_g1_credentialboundary.moc"
