// 层：测试壳
#include <QtTest/QtTest>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgseditorwidgetregistry.h>
#include <qgsgeometry.h>
#include <qgsgui.h>
#include <qgsvectorlayer.h>

#include "../src/ai/chat/llmclient.h"
#include "../src/catalog/datacatalog.h"
#include "../src/domain/faciescatalog.h"
#include "../src/domain/interpretationevidence.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/faciespartitioneditor.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/wellattributestore.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/pages/mappingeditbasis.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/mappingexplanationservice.h"
#include "../src/workflow/mappingworkbench.h"

namespace {

// 轻量本地 HTTP 桩：在 127.0.0.1 响应 OpenAI 兼容 /chat/completions，可控制延迟/失败。
class LoopbackLlmServer : public QObject {
public:
  explicit LoopbackLlmServer(QObject *parent = nullptr) : QObject(parent) {
    connect(&m_server, &QTcpServer::newConnection, this, &LoopbackLlmServer::onConnection);
  }
  bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
  QUrl endpointUrl() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }
  void setReply(int statusCode, const QByteArray &jsonBody) {
    m_status = statusCode;
    m_body = jsonBody;
  }
  void setHoldResponses(bool hold) { m_hold = hold; }
  void releaseHeld() {
    m_hold = false;
    const auto held = std::move(m_heldSockets);
    m_heldSockets.clear();
    for (const auto &socket : held)
      if (socket)
        writeResponse(socket);
  }
  int requestCount() const { return m_requests.size(); }
  QByteArray lastRequestBody() const { return m_requests.isEmpty() ? QByteArray() : m_requests.last(); }

private:
  void onConnection() {
    while (auto *socket = m_server.nextPendingConnection()) {
      auto buffer = std::make_shared<QByteArray>();
      connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
        buffer->append(socket->readAll());
        const int sep = buffer->indexOf("\r\n\r\n");
        if (sep < 0)
          return;
        const QByteArray header = buffer->left(sep);
        int contentLength = 0;
        for (const QByteArray &line : header.split('\n')) {
          if (line.toLower().startsWith("content-length:"))
            contentLength = line.mid(15).trimmed().toInt();
        }
        const QByteArray body = buffer->mid(sep + 4);
        if (body.size() < contentLength)
          return;
        m_requests.append(body.left(contentLength));
        if (m_hold) {
          m_heldSockets.append(socket);
        } else {
          writeResponse(socket);
        }
      });
    }
  }

  void writeResponse(QTcpSocket *socket) {
    const QByteArray statusLine = m_status == 200
                                      ? QByteArrayLiteral("HTTP/1.1 200 OK\r\n")
                                      : QByteArrayLiteral("HTTP/1.1 500 Internal Server Error\r\n");
    const QByteArray payload = statusLine +
                               QByteArrayLiteral("Content-Type: application/json\r\n") +
                               QByteArrayLiteral("Content-Length: ") + QByteArray::number(m_body.size()) +
                               QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + m_body;
    socket->write(payload);
    socket->flush();
    socket->disconnectFromHost();
  }

  QTcpServer m_server;
  int m_status = 200;
  QByteArray m_body;
  bool m_hold = false;
  QList<QPointer<QTcpSocket>> m_heldSockets;
  QList<QByteArray> m_requests;
};

struct WorkbenchFixture {
  QTemporaryDir dir;
  DataCatalog catalog;
  PaleoProjectStore store;
  PaleoTaskService tasks{&store};
  QgisProjectService project;
  LayerManifest manifest{dir.filePath("project.sqlite")};
  QgisLayerService layers{&project, &manifest};
  QgisProcessingService processing{&store};
  ConstraintWorkflow constraints{&processing, &layers};
  MappingWorkbench work{&layers, &processing, &project, &constraints};

  bool init() {
    if (!dir.isValid())
      return false;
    if (!catalog.open(dir.path()))
      return false;
    if (!project.createProject(dir.filePath("project.qgz")))
      return false;
    if (!manifest.open())
      return false;
    store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"), dir.filePath("project.sqlite"));
    constraints.setStore(&store);
    constraints.setCatalog(&catalog, dir.path());
    project.setDeclarationProvider([this](QVector<LayerDeclaration> *out, QString *error) {
      return layers.tryDeclared(out, error);
    });
    processing.setProject(project.project());
    work.setTaskService(&tasks);
    work.bindCatalog(&catalog, dir.path());
    QString error;
    return work.saveFacies(QStringLiteral("D61"), FaciesCatalog::defaults().mid(0, 4), &error);
  }

  QString addWell(const QString &name, double x, double y, int faciesCode) {
    CatalogEntity well;
    well.id = QStringLiteral("W-") + name;
    well.entityType = QStringLiteral("well");
    well.name = name;
    well.hasSurface = true;
    well.surfaceX = x;
    well.surfaceY = y;
    well.coordinateStatus = QStringLiteral("untransformed");
    QString err;
    if (!catalog.addEntity(well, &err))
      return {};
    const auto schema = work.facies(QStringLiteral("D61"));
    const auto hierarchy = FaciesCatalog::attributes(schema, faciesCode);
    const QVariantList updates{QVariantMap{
        {QStringLiteral("well_id"), well.id},
        {QStringLiteral("formation"), QStringLiteral("D61")},
        {QStringLiteral("top_md"), 1000.0},
        {QStringLiteral("base_md"), 1050.0},
        {QStringLiteral("facies_code"), faciesCode},
        {QStringLiteral("dominant_facies"), hierarchy.value(QStringLiteral("microfacies"))},
        {QStringLiteral("facies"), hierarchy.value(QStringLiteral("facies_name"))},
        {QStringLiteral("sub_facies"), hierarchy.value(QStringLiteral("subfacies"))},
        {QStringLiteral("micro_facies"), hierarchy.value(QStringLiteral("microfacies"))},
    }};
    WellAttributeStore::mergeFormations(&catalog, dir.path(), &layers, updates, &err);
    return well.id;
  }
};

} // namespace

class TestMappingAsyncExplanation : public QObject {
  Q_OBJECT
private slots:
  void unblockedCompilationAndLocalBackgroundExplanation() {
    WorkbenchFixture f;
    QVERIFY(f.init());
    QVERIFY(!f.addWell(QStringLiteral("Well-A1"), 50.0, 50.0, 1).isEmpty());

    QString error;
    const auto draft = f.work.createBlankMap(QStringLiteral("D61"), &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    QVERIFY(!f.work.needsEditEvidence(draft));
    QVERIFY(f.work.autoExplainEnabled());
    QVERIFY(!f.work.aiExplanationAvailable());

    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY2(layer, qPrintable(error));
    QVERIFY(layer->startEditing());

    QSignalSpy readySpy(&f.work, &MappingWorkbench::autoExplanationReady);
    const auto outline = QgsGeometry::fromWkt(QStringLiteral("POLYGON((0 0,100 0,100 100,0 100,0 0))"));
    QVERIFY2(FaciesPartitionEditor::paint(layer, outline, 1, &error), qPrintable(error));
    QCOMPARE(layer->featureCount(), 1);
    const int undoIndexAfterPaint = layer->undoStack()->index();

    // 后台线程异步产出本地事实摘要（不能在主线程生成，也不能污染撤销栈）。
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);
    const auto generated = readySpy.last().at(1).toMap();
    QCOMPARE(readySpy.last().at(0).toString(), QStringLiteral("D61"));
    QCOMPARE(generated.value(QStringLiteral("generator")).toString(), QStringLiteral("local_factual"));
    QVERIFY(!generated.value(QStringLiteral("ai_generated")).toBool());
    QVERIFY(generated.value(QStringLiteral("worker_thread_id")).toULongLong() != 0);
    QVERIFY(generated.value(QStringLiteral("worker_thread_id")).toULongLong() !=
            static_cast<qulonglong>(reinterpret_cast<quintptr>(QThread::currentThreadId())));
    QVERIFY2(generated.value(QStringLiteral("text")).toString().contains(QStringLiteral("Well-A1")),
             qPrintable(generated.value(QStringLiteral("text")).toString()));
    QCOMPARE(layer->undoStack()->index(), undoIndexAfterPaint);

    // 详情快照立即展示后台生成的待复核草稿。
    const auto fids = layer->selectedFeatureIds().values();
    QCOMPARE(fids.size(), 1);
    const auto summary = f.work.featureEvidenceSummary(draft, fids);
    bool sawAutoSummary = false;
    for (const auto &v : summary.value(QStringLiteral("records")).toList()) {
      const auto row = v.toMap();
      if (row.value(QStringLiteral("origin")).toString() == QLatin1String("auto_explanation"))
        sawAutoSummary = true;
    }
    QVERIFY(sawAutoSummary);

    // 提交编辑时自动将已就绪草稿物化进 GPKG 要素证据字段。
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(QLatin1Char(';'))));
    QgsFeature committed;
    auto it = layer->getFeatures();
    QVERIFY(it.nextFeature(committed));
    it.close();
    QVariantList storedRecords;
    QVariantMap storedInterp;
    QVERIFY2(paleo::evidence::decodeRecords(committed.attribute(QStringLiteral("facies_evidence")).toString(),
                                            &storedRecords, &error), qPrintable(error));
    QVERIFY2(paleo::evidence::decodeInterpretation(committed.attribute(QStringLiteral("paleo_interpretation")).toString(),
                                                   &storedInterp, &error), qPrintable(error));
    QCOMPARE(storedRecords.size(), 1);
    QCOMPARE(storedRecords.first().toMap().value(QStringLiteral("origin")).toString(),
             QStringLiteral("auto_explanation"));
    QCOMPARE(storedInterp.value(QStringLiteral("status")).toString(), QStringLiteral("NeedsReview"));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    QVERIFY(!f.work.hasUnpublishedEdits(draft));
  }

  void aiBackgroundExplanationOverHttpOnWorkerThread() {
    LoopbackLlmServer server;
    QVERIFY(server.start());
    const QString innerJson = QString::fromUtf8(
        QJsonDocument(QJsonObject{
                          {QStringLiteral("text"),
                           QStringLiteral("依据 Well-B2 井相及当前相面边界，将该区域划定为河道微相。")},
                          {QStringLiteral("reason"), QStringLiteral("AI 后台说明：划定河道微相")},
                      })
            .toJson(QJsonDocument::Compact));
    const QByteArray aiJson =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("choices"),
                           QJsonArray{QJsonObject{
                               {QStringLiteral("index"), 0},
                               {QStringLiteral("message"),
                                QJsonObject{{QStringLiteral("content"), innerJson}}},
                               {QStringLiteral("finish_reason"), QStringLiteral("stop")},
                           }}},
                      })
            .toJson(QJsonDocument::Compact);
    server.setReply(200, aiJson);

    WorkbenchFixture f;
    QVERIFY(f.init());
    QVERIFY(!f.addWell(QStringLiteral("Well-B2"), 25.0, 25.0, 1).isEmpty());

    // 经 AiChatController::configChanged 同步配置到 MappingWorkbench。
    AiChatController chat;
    QObject::connect(&chat, &AiChatController::configChanged, &f.work, &MappingWorkbench::setLlmConfig);
    const auto cfg = LlmConfig::fromParts(server.endpointUrl(), QStringLiteral("test-paleo-llm"),
                                          QByteArrayLiteral("test-secret-key"), false);
    chat.setConfig(cfg);
    QVERIFY(f.work.aiExplanationAvailable());
    QVERIFY(f.work.explanationStatus().contains(QStringLiteral("test-paleo-llm")));

    QString error;
    const auto draft = f.work.createBlankMap(QStringLiteral("D61"), &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY(layer);
    QVERIFY(layer->startEditing());

    // 暂停服务端响应，验证用户先提交并保存图件版本完全不等待 AI。
    server.setHoldResponses(true);
    QSignalSpy readySpy(&f.work, &MappingWorkbench::autoExplanationReady);
    QVERIFY2(FaciesPartitionEditor::paint(
                 layer, QgsGeometry::fromWkt(QStringLiteral("POLYGON((0 0,50 0,50 50,0 50,0 0))")), 1, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() == 1, 5000);
    QVERIFY2(server.lastRequestBody().contains("Well-B2"), server.lastRequestBody().constData());

    // AI 仍在途：直接提交并保存图件新版本，零阻塞！
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(QLatin1Char(';'))));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto savedVersion = f.work.versionForLayer(draft);
    const auto savedExtraBefore = QJsonDocument::fromVariant(savedVersion.extra).toJson(QJsonDocument::Compact);
    QVERIFY(!f.work.hasUnpublishedEdits(draft));
    QVERIFY(!f.work.hasUnpublishedChanges(draft));

    // 放行 AI 响应：后台线程完成解析并投递，不弄脏已保存版本、不触发虚假未保存状态。
    server.releaseHeld();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1, 5000);
    const auto result = readySpy.last().at(1).toMap();
    QCOMPARE(result.value(QStringLiteral("generator")).toString(), QStringLiteral("ai"));
    QVERIFY(result.value(QStringLiteral("ai_generated")).toBool());
    QCOMPARE(result.value(QStringLiteral("model")).toString(), QStringLiteral("test-paleo-llm"));
    QVERIFY(result.value(QStringLiteral("text")).toString().contains(QStringLiteral("Well-B2")));
    QVERIFY(result.value(QStringLiteral("worker_thread_id")).toULongLong() !=
            static_cast<qulonglong>(reinterpret_cast<quintptr>(QThread::currentThreadId())));

    QVERIFY(!f.work.hasUnpublishedEdits(draft));
    QVERIFY(!f.work.hasUnpublishedChanges(draft));
    QCOMPARE(QJsonDocument::fromVariant(f.catalog.versionById(savedVersion.id).extra).toJson(QJsonDocument::Compact),
             savedExtraBefore);

    // 持久化到工作副本 sidecar：重绑目录后仍可读取，并在详情快照展示。
    const QString uid = result.value(QStringLiteral("feature_uid")).toString();
    QVERIFY(!uid.isEmpty());
    f.work.bindCatalog(&f.catalog, f.dir.path());
    const auto reloaded = f.work.generatedExplanationForFeature(draft, uid);
    QCOMPARE(reloaded.value(QStringLiteral("generator")).toString(), QStringLiteral("ai"));
    QVERIFY(reloaded.value(QStringLiteral("text")).toString().contains(QStringLiteral("Well-B2")));
    QVERIFY(reloaded.value("pending_adoption").toBool());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    QCOMPARE(f.work.versionForLayer(draft).id, savedVersion.id);
    QVERIFY(f.work.adoptExplanation(draft, uid, reloaded.value("request_id").toString()));
    QVERIFY(f.work.hasUnpublishedChanges(draft));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto next = f.work.versionForLayer(draft);
    QVERIFY(next.id != savedVersion.id);
    QCOMPARE(next.extra.value("auto_explanations").toMap().value(uid).toMap().value("request_id"), reloaded.value("request_id"));
    QVERIFY(!f.work.hasUnpublishedChanges(draft));
    QCOMPARE(QJsonDocument::fromVariant(f.catalog.versionById(savedVersion.id).extra).toJson(QJsonDocument::Compact),
             savedExtraBefore);
  }

  void aiFailureFallsBackHonestlyToLocalSummary() {
    LoopbackLlmServer server;
    QVERIFY(server.start());
    server.setReply(500, QByteArrayLiteral("{\"error\":{\"message\":\"upstream down\"}}"));

    WorkbenchFixture f;
    QVERIFY(f.init());
    f.work.setLlmConfig(LlmConfig::fromParts(server.endpointUrl(), QStringLiteral("broken-model"),
                                             QByteArrayLiteral("k"), false));
    QString error;
    const auto draft = f.work.createBlankMap(QStringLiteral("D61"), &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));

    QSignalSpy readySpy(&f.work, &MappingWorkbench::autoExplanationReady);
    QVERIFY2(f.work.requestBackgroundExplanation(QStringLiteral("D61"), draft, {}, {}, true, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1, 5000);
    const auto result = readySpy.last().at(1).toMap();
    QCOMPARE(result.value(QStringLiteral("generator")).toString(), QStringLiteral("local_factual"));
    QVERIFY(!result.value(QStringLiteral("ai_generated")).toBool());
    QVERIFY(!result.value(QStringLiteral("fallback_reason")).toString().isEmpty());
    QVERIFY(!result.value(QStringLiteral("text")).toString().isEmpty());
  }

  void undoAfterGenerationDropsStaleDraftFromTheNextVersion() {
    WorkbenchFixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = f.work.createBlankMap(QStringLiteral("D61"), &error);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY2(layer, qPrintable(error));
    const auto projectCrs = layer->crs();
    layer->setCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")));
    QVERIFY(layer->startEditing());
    QSignalSpy ready(&f.work, &MappingWorkbench::autoExplanationReady);
    const auto outline = QgsGeometry::fromWkt(QStringLiteral("POLYGON((0 0,1 0,1 1,0 1,0 0))"));
    QVERIFY2(FaciesPartitionEditor::paint(layer, outline, 1, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
    const auto generated = ready.first().at(1).toMap();
    QVERIFY(!generated.value("text").toString().contains(QStringLiteral("m²")));
    const auto uid = generated.value("feature_uid").toString();
    QVERIFY(!uid.isEmpty());
    layer->undoStack()->undo();
    QTRY_VERIFY(f.work.lastAutoExplanation(QStringLiteral("D61")).isEmpty());
    f.work.setAutoExplainEnabled(false);
    layer->setCrs(projectCrs);
    QVERIFY2(FaciesPartitionEditor::paint(layer, outline, 2, &error), qPrintable(error));
    QVERIFY(layer->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto saved = f.work.versionForLayer(draft).extra.value("auto_explanations").toMap();
    QVERIFY(!saved.contains(uid));
  }

  void undoWhileExplanationIsRunningDropsTheObsoleteFeature() {
    LoopbackLlmServer server;
    QVERIFY(server.start());
    server.setReply(500, QByteArrayLiteral("{}"));
    server.setHoldResponses(true);
    WorkbenchFixture f;
    QVERIFY(f.init());
    f.work.setLlmConfig(LlmConfig::fromParts(server.endpointUrl(), QStringLiteral("slow-model"),
                                             QByteArrayLiteral("k"), false));
    QString error;
    const auto draft = f.work.createBlankMap(QStringLiteral("D61"), &error);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY2(layer, qPrintable(error));
    QVERIFY(layer->startEditing());
    QSignalSpy ready(&f.work, &MappingWorkbench::autoExplanationReady);
    const auto outline = QgsGeometry::fromWkt(QStringLiteral("POLYGON((0 0,100 0,100 100,0 100,0 0))"));
    QVERIFY2(FaciesPartitionEditor::paint(layer, outline, 1, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 5000);
    const auto uid = f.work.featureEvidenceSummary(draft, layer->selectedFeatureIds().values())
                         .value(QStringLiteral("feature_uid")).toString();
    QVERIFY(!uid.isEmpty());
    layer->undoStack()->undo();
    QCOMPARE(layer->featureCount(), 0);
    server.releaseHeld();
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.explanationBusy(), 5000);
    QCOMPARE(ready.count(), 0);
    QVERIFY(f.work.lastAutoExplanation(QStringLiteral("D61")).isEmpty());
    QVERIFY(f.work.generatedExplanationForFeature(draft, uid).isEmpty());
    QVERIFY(layer->rollBack());
  }

  void cancellationDropsStaleBackgroundResult() {
    LoopbackLlmServer server;
    QVERIFY(server.start());
    const QString lateInner = QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("text"), QStringLiteral("迟到结果")},
                                  {QStringLiteral("reason"), QStringLiteral("迟到")}})
            .toJson(QJsonDocument::Compact));
    server.setReply(200, QJsonDocument(QJsonObject{
                                           {QStringLiteral("choices"),
                                            QJsonArray{QJsonObject{
                                                {QStringLiteral("index"), 0},
                                                {QStringLiteral("message"),
                                                 QJsonObject{{QStringLiteral("content"), lateInner}}},
                                                {QStringLiteral("finish_reason"), QStringLiteral("stop")},
                                            }}},
                                       })
                             .toJson(QJsonDocument::Compact));
    server.setHoldResponses(true);

    WorkbenchFixture f;
    QVERIFY(f.init());
    f.work.setLlmConfig(LlmConfig::fromParts(server.endpointUrl(), QStringLiteral("slow-model"),
                                             QByteArrayLiteral("k"), false));
    QString error;
    const auto draft = f.work.createBlankMap(QStringLiteral("D61"), &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));

    QSignalSpy readySpy(&f.work, &MappingWorkbench::autoExplanationReady);
    QVERIFY2(f.work.requestBackgroundExplanation(QStringLiteral("D61"), draft, {}, {}, true, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() == 1, 5000);
    QVERIFY(f.work.explanationBusy());

    f.work.cancelBackgroundExplanation();
    QVERIFY(!f.work.explanationBusy());
    server.releaseHeld();
    QTest::qWait(150);
    QCOMPARE(readySpy.count(), 0);
  }

  void coalescedRequestsClearBusyCleanly() {
    LoopbackLlmServer server;
    QVERIFY(server.start());
    const QString inner = QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("text"), QStringLiteral("合并队列后台说明")},
                                  {QStringLiteral("reason"), QStringLiteral("合并验证")}})
            .toJson(QJsonDocument::Compact));
    server.setReply(200, QJsonDocument(QJsonObject{
                                           {QStringLiteral("choices"),
                                            QJsonArray{QJsonObject{
                                                {QStringLiteral("index"), 0},
                                                {QStringLiteral("message"),
                                                 QJsonObject{{QStringLiteral("content"), inner}}},
                                                {QStringLiteral("finish_reason"), QStringLiteral("stop")},
                                            }}},
                                       })
                             .toJson(QJsonDocument::Compact));
    server.setHoldResponses(true);

    MappingExplanationService svc;
    svc.setLlmConfig(LlmConfig::fromParts(server.endpointUrl(), QStringLiteral("coalesce-model"),
                                          QByteArrayLiteral("k"), false));
    QSignalSpy readySpy(&svc, &MappingExplanationService::explanationReady);
    auto makeReq = [](const QString &uid) {
      MappingExplanationRequest r;
      r.horizon = QStringLiteral("D61");
      r.layerId = QStringLiteral("draft.1");
      r.featureUid = uid;
      r.operationKind = QStringLiteral("paint");
      return r;
    };
    svc.enqueue(makeReq(QStringLiteral("uid-A"))); // seq 1 (in-flight)
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() == 1, 5000);
    svc.enqueue(makeReq(QStringLiteral("uid-B"))); // seq 2 (queued slot 0)
    svc.enqueue(makeReq(QStringLiteral("uid-A"))); // seq 3 (queued slot 1)
    svc.enqueue(makeReq(QStringLiteral("uid-B"))); // seq 4 (coalesces into slot 0)
    QVERIFY(svc.busy());

    server.releaseHeld();
    QTRY_VERIFY_WITH_TIMEOUT(!svc.busy(), 5000);
    QCOMPARE(readySpy.count(), 3);
  }

  void formAndExplanationStayWithTheirDraft() {
    WorkbenchFixture f;
    QVERIFY(f.init());
    QString error;
    const auto first = f.work.createBlankMap("D61", &error);
    const auto second = f.work.createBlankMap("D61", &error);
    QVERIFY2(!first.isEmpty() && !second.isEmpty(), qPrintable(error));
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    page.selectLayer(first);
    auto *text = page.findChild<QPlainTextEdit *>("editBasisText");
    QVERIFY(text);
    text->setPlainText("first note");
    QVERIFY(f.work.setEditEvidence("D61", {{"layer", first}, {"text", text->toPlainText()}}, &error));
    page.selectLayer(second);
    QVERIFY(text->toPlainText().isEmpty());
    QVERIFY(f.work.editEvidenceForLayer(second).isEmpty());
    QSignalSpy ready(&f.work, &MappingWorkbench::autoExplanationReady);
    QVERIFY(f.work.requestBackgroundExplanation("D61", first, {}, {}, true, &error));
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
    QVERIFY(text->toPlainText().isEmpty());
    QVERIFY(f.work.explanationForTarget(second, {}).isEmpty());
    text->setPlainText("second note");
    page.selectLayer(first);
    QCOMPARE(text->toPlainText(), QStringLiteral("first note"));
    page.selectLayer(second);
    QCOMPARE(text->toPlainText(), QStringLiteral("second note"));
  }

  void uiStreamlinedFlowAndOptionalEditBasis() {
    WorkbenchFixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = f.work.createBlankMap(QStringLiteral("D61"), &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));

    MappingWorkbenchPage page(QStringLiteral("compose"), &f.work);
    page.setHorizon(QStringLiteral("D61"));
    page.selectLayer(draft);
    page.show();
    QCoreApplication::processEvents();

    // 编图向导精简为三步：基础图 -> 相面编辑 -> 保存输出，不再被解释依据阻断。
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("mappingTaskStep.basemap")));
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("mappingTaskStep.edit")));
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("mappingTaskStep.deliver")));
    QVERIFY(!page.findChild<QPushButton *>(QStringLiteral("mappingTaskStep.evidence")));
    QCOMPARE(page.findChild<QWidget *>(QStringLiteral("mappingTaskGuide"))->property("stage").toInt(), 1);

    // 可选解释面板默认折叠，showEditBasis 展开并聚焦。
    auto *basis = page.findChild<MappingEditBasis *>();
    QVERIFY(basis);
    QVERIFY(!basis->isExpanded());
    page.showEditBasis();
    QVERIFY(basis->isExpanded());

    // 手动触发后台生成说明：自动填充空白表单；用户手动输入后不被覆盖。
    auto *text = basis->findChild<QPlainTextEdit *>(QStringLiteral("editBasisText"));
    auto *reason = basis->findChild<QLineEdit *>(QStringLiteral("editBasisReason"));
    QVERIFY(text && reason);
    QVERIFY(text->toPlainText().isEmpty());

    QVERIFY2(f.work.requestBackgroundExplanation(QStringLiteral("D61"), draft, {}, {}, true, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!text->toPlainText().trimmed().isEmpty(), 5000);
    QVERIFY(!reason->text().trimmed().isEmpty());

    const QString userTyped = QStringLiteral("地质师手写说明：保留人工判断");
    text->setPlainText(userTyped);
    QSignalSpy readySpy(&f.work, &MappingWorkbench::autoExplanationReady);
    QVERIFY2(f.work.requestBackgroundExplanation(QStringLiteral("D61"), draft, {}, {}, true, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);
    QCOMPARE(text->toPlainText(), userTyped);
    text->clear();
    reason->clear();
    readySpy.clear();
    QVERIFY2(f.work.requestBackgroundExplanation(QStringLiteral("D61"), draft, {}, {}, true, &error),
             qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
    QVERIFY(text->toPlainText().isEmpty());
    QVERIFY(reason->text().isEmpty());
    auto *adopt = basis->findChild<QPushButton *>(QStringLiteral("editBasisAdoptDraft"));
    QVERIFY(adopt);
    adopt->click();
    QCOMPARE(text->toPlainText(), readySpy.first().at(1).toMap().value("text").toString());

    // 开关和按钮发命令信号（视图只发信号不直接改状态）。
    QSignalSpy cmdSpy(&page, &MappingWorkbenchPage::commandRequested);
    auto *autoToggle = basis->findChild<QCheckBox *>(QStringLiteral("editBasisAutoToggle"));
    QVERIFY(autoToggle);
    autoToggle->setChecked(false);
    QCOMPARE(cmdSpy.last().first().toString(), QStringLiteral("setAutoExplain"));
    QCOMPARE(cmdSpy.last().at(1).toMap().value(QStringLiteral("enabled")).toBool(), false);
  }
};

int main(int argc, char **argv) {
  QTemporaryDir profile;
  if (!profile.isValid())
    return 1;
  QgsApplication app(argc, argv, true, profile.path());
  const QString prefix = QgisRuntime::resolvePrefixPath({}, qEnvironmentVariable("QGIS_PREFIX_PATH"));
  app.setPrefixPath(prefix, true);
  QgsApplication::init(profile.path());
  QCoreApplication::setOrganizationName(QStringLiteral("paleo-tests"));
  QCoreApplication::setApplicationName(QStringLiteral("tst_mapping_async_explanation"));
  QString databaseError;
  if (!QgsApplication::createDatabase(&databaseError)) {
    qCritical().noquote() << databaseError;
    return 1;
  }
  app.initQgis();
  QgsCoordinateReferenceSystem::invalidateCache(true);
  QgsGui::editorWidgetRegistry()->initEditors();
  GDALAllRegister();
  TestMappingAsyncExplanation test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_mapping_async_explanation.moc"
