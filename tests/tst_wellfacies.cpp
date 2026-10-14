#include "ai/wellfaciesservice.h"
#include "domain/welllogfacies.h"
#include "io/wellcompositexml.h"
#include "ui/paleotheme.h"
#include "services/paleotaskservice.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "workflow/wellfaciesworkflow.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
using namespace WellComposite;
namespace {
WellFaciesModel model() {
  return {"model-1", QStringLiteral("测试微相"), "v1", QStringLiteral("恩平组"),
          {"GR"},    {QStringLiteral("河口坝")}, 128};
}
ComprehensiveWellData well() {
  ComprehensiveWellData d;
  d.wellName = "W-1";
  d.minDepth = 1000;
  d.maxDepth = 1016;
  CurveData c;
  c.name = "GR";
  c.unit = "API";
  for (int i = 0; i < 128; ++i) {
    c.depths.append(1000 + i * .125f);
    c.values.append(80 + i * .1f);
  }
  d.continuousCurves = {c};
  FormationInterval group;
  group.name = QStringLiteral("恩平组");
  group.unitType = QStringLiteral("组");
  group.topDepth = 1000;
  group.bottomDepth = 1016;
  auto member = group;
  member.name = QStringLiteral("恩平一段");
  member.unitType = QStringLiteral("段");
  d.formationIntervals = {group, member};
  LithologyInterval lith;
  lith.topDepth = 1000;
  lith.bottomDepth = 1016;
  lith.lithoName = QStringLiteral("细砂岩");
  d.lithologyIntervals = {lith};
  FaciesInterval observed;
  observed.topDepth = 1000;
  observed.bottomDepth = 1016;
  observed.microFacies = QStringLiteral("原解释");
  d.faciesIntervals = {observed};
  return d;
}
WellFaciesModel zhuhaiModel() {
  return {"model-2", QStringLiteral("测试微相二"), "v1", QStringLiteral("珠海组"),
          {"GR"},    {QStringLiteral("分流河道")}, 128};
}
// 两个地层组（珠海组 1000–1016、恩平组 1016–1032）各 128 点，另有无模型的文昌组。
ComprehensiveWellData twoGroupWell(bool withUnmodelledGroup = false) {
  ComprehensiveWellData d;
  d.wellName = "W-2";
  d.minDepth = 1000;
  d.maxDepth = withUnmodelledGroup ? 1048 : 1032;
  CurveData c;
  c.name = "GR";
  c.unit = "API";
  for (int i = 0; i < (withUnmodelledGroup ? 384 : 256); ++i) {
    c.depths.append(1000 + i * .125f);
    c.values.append(80 + i * .1f);
  }
  d.continuousCurves = {c};
  const auto interval = [](const QString &name, const QString &type, float top,
                           float bottom) {
    FormationInterval f;
    f.name = name;
    f.unitType = type;
    f.topDepth = top;
    f.bottomDepth = bottom;
    return f;
  };
  d.formationIntervals = {interval(QStringLiteral("珠海组"), QStringLiteral("组"), 1000, 1016),
                          interval(QStringLiteral("珠海一段"), QStringLiteral("段"), 1000, 1016),
                          interval(QStringLiteral("恩平组"), QStringLiteral("组"), 1016, 1032),
                          interval(QStringLiteral("恩平一段"), QStringLiteral("段"), 1016, 1032)};
  if (withUnmodelledGroup) {
    d.formationIntervals.append(interval(QStringLiteral("文昌组"), QStringLiteral("组"), 1032, 1048));
    d.formationIntervals.append(interval(QStringLiteral("文昌一段"), QStringLiteral("段"), 1032, 1048));
  }
  LithologyInterval lith;
  lith.topDepth = 1000;
  lith.bottomDepth = d.maxDepth;
  lith.lithoName = QStringLiteral("细砂岩");
  d.lithologyIntervals = {lith};
  return d;
}
QJsonObject response(const QJsonArray &rows, const QString &name,
                     const QString &label = QStringLiteral("河口坝")) {
  QJsonArray p;
  for (const auto &r : rows)
    p.append(QJsonObject{{"wellName", name},
                         {"depth", r.toObject().value(QStringLiteral("深度"))},
                         {"label", label},
                         {"confidence", .91}});
  return {{"jobId", "job-1"},
          {"status", "completed"},
          {"model", QJsonObject{{"name", QStringLiteral("测试微相")},
                                {"version", "v1"}}},
          {"predictions", p}};
}
class Server {
public:
  QTcpServer server;
  QList<QByteArray> requests;
  QJsonObject result;
  QList<WellFaciesModel> models{model()};
  QHash<QString, QString> labels; // modelVersionId → 预测词面（缺省「河口坝」）
  QString failModel;              // 该模型的 /predict 返回 500
  QString holdModel;              // 该模型的 /predict 不回应
  QStringList predictModels;      // 每次 /predict 的 modelVersionId（按到达序）
  QList<int> predictRows;         // 每次 /predict 提交的行数
  int predictCount = 0, pollCount = 0, httpError = 0;
  bool queued = false, malformed = false, foreignPoll = false,
       terminalFailure = false, hold = false;
  Server() {
    server.listen(QHostAddress::LocalHost);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
      while (server.hasPendingConnections()) {
        auto *s = server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(s, &QTcpSocket::disconnected, s,
                         &QObject::deleteLater);
        QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buffer] {
          *buffer += s->readAll();
          const auto end = buffer->indexOf("\r\n\r\n");
          if (end < 0)
            return;
          qsizetype length = 0;
          for (const auto &h : buffer->left(end).split('\n'))
            if (h.toLower().startsWith("content-length:"))
              length = h.mid(15).trimmed().toLongLong();
          if (buffer->size() < end + 4 + length)
            return;
          const QByteArray request = *buffer;
          buffer->clear();
          requests.append(request);
          const auto path = request.split(' ').value(1);
          QJsonObject json;
          int status = 200;
          if (httpError) {
            status = httpError;
            json = {{"code", "INVALID_INPUT"},
                    {"error", QStringLiteral("缺少岩性")}};
          } else if (path.endsWith("/models")) {
            QJsonArray list;
            for (const auto &m : models)
              list.append(QJsonObject{
                  {"id", m.id},
                  {"name", m.name},
                  {"version", m.version},
                  {"inputSchema",
                   QJsonObject{{"formationGroup", m.formationGroup},
                               {"curves", QJsonArray{"GR"}},
                               {"categoricalFields",
                                QJsonArray{QStringLiteral("段"),
                                           QStringLiteral("岩性")}},
                               {"window", 128}}}});
            json = {{"models", list}};
          } else if (path.endsWith("/predict")) {
            ++predictCount;
            const auto body =
                QJsonDocument::fromJson(request.mid(end + 4)).object();
            const auto w = body.value("wells").toArray().first().toObject();
            const QString modelId = body.value("modelVersionId").toString();
            predictModels << modelId;
            predictRows << w.value("rows").toArray().size();
            result = response(w.value("rows").toArray(),
                              w.value("wellName").toString(),
                              labels.value(modelId, QStringLiteral("河口坝")));
            if (hold || (!holdModel.isEmpty() && holdModel == modelId))
              return;
            if (!failModel.isEmpty() && failModel == modelId) {
              status = 500;
              json = {{"code", "INTERNAL"},
                      {"error", QStringLiteral("模型服务内部错误")}};
            } else if (queued) {
              status = 202;
              json = {{"jobId", "job-1"},
                      {"status", "predicting"},
                      {"pollAfterMs", 100},
                      {"progress", 45},
                      {"pollUrl",
                       foreignPoll
                           ? "http://other.invalid/api/v1/predictions/job-1"
                           : "/api/v1/predictions/job-1"}};
            } else
              json = result;
          } else {
            ++pollCount;
            json = terminalFailure
                       ? QJsonObject{{"jobId", "job-1"},
                                     {"status", "failed"},
                                     {"error", QStringLiteral("模型执行失败")}}
                       : result;
          }
          QByteArray payload =
              malformed ? QByteArray("not-json")
                        : QJsonDocument(json).toJson(QJsonDocument::Compact);
          s->write("HTTP/1.1 " + QByteArray::number(status) +
                   " OK\r\nContent-Type: application/json\r\nConnection: "
                   "close\r\nContent-Length: " +
                   QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
          s->disconnectFromHost();
        });
      }
    });
  }
  WellFaciesConfig config() {
    WellFaciesConfig c;
    c.baseUrl = QUrl(
        QStringLiteral("http://127.0.0.1:%1/api/v1").arg(server.serverPort()));
    c.apiKey = "test-key";
    return c;
  }
};
} // namespace
class TestWellFacies : public QObject {
  Q_OBJECT
  QTemporaryDir m_cacheHome;
  QJsonArray m_referenceReport;
private slots:
  void initTestCase() { qputenv("XDG_DATA_HOME", m_cacheHome.path().toUtf8()); }
  void cleanupTestCase() {
    const QString report =
        qEnvironmentVariable("PALEO_WELL_FACIES_TEST_REPORT");
    if (report.isEmpty())
      return;
    QFile file(report);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes = QJsonDocument(m_referenceReport).toJson();
    QCOMPARE(file.write(bytes), bytes.size());
  }
  // 冻结相 schema 查码：name/microfacies 命中；schema 外标签必须返回 0，
  // 调用方拒收（旧行为自增 code=max+1——编码随到达顺序漂移，已废止）。
  void frozenSchemaCodeForLabel() {
    QVariantList schema;
    schema << QVariantMap{{"code", 1}, {"name", "辫状河"}, {"microfacies", "BR"},
                          {"color", "#123456"}}
           << QVariantMap{{"code", 7}, {"name", "三角洲前缘"}, {"microfacies", "DF"},
                          {"color", "#654321"}};
    QCOMPARE(faciesCodeForLabel(schema, QStringLiteral("辫状河")), 1);
    QCOMPARE(faciesCodeForLabel(schema, QStringLiteral("DF")), 7);
    QCOMPARE(faciesCodeForLabel(schema, QStringLiteral("辫状 河")), 0);
    QCOMPARE(faciesCodeForLabel(schema, QStringLiteral("未知微相")), 0);
    QCOMPARE(faciesCodeForLabel(schema, QString()), 0);
    QCOMPARE(faciesCodeForLabel(QVariantList(), QStringLiteral("辫状河")), 0);
    // 查询不改变 schema（冻结性）
    QCOMPARE(schema.size(), 2);
  }
  void inputContract() {
    auto d = well();
    auto input = prepareWellFaciesInput(d, model());
    QVERIFY2(input.ready(), qPrintable(input.reason));
    QCOMPARE(input.rows.size(), 128);
    QCOMPARE(
        input.rows.first().toObject().value(QStringLiteral("段")).toString(),
        QStringLiteral("恩平一段"));
    d.continuousCurves[0].name = "RT";
    QVERIFY(prepareWellFaciesInput(d, model()).reason.contains("GR"));
    d = well();
    d.lithologyIntervals.clear();
    QVERIFY(prepareWellFaciesInput(d, model())
                .reason.contains(QStringLiteral("岩性")));
    d = well();
    d.formationIntervals.removeLast();
    QVERIFY(prepareWellFaciesInput(d, model())
                .reason.contains(QStringLiteral("段")));
    d = well();
    auto m = model();
    m.formationGroup = QStringLiteral("沙一组");
    QVERIFY(!prepareWellFaciesInput(d, m).ready());
    d = well();
    d.continuousCurves[0].values[20] = std::numeric_limits<float>::quiet_NaN();
    QVERIFY(!prepareWellFaciesInput(d, model()).ready());
    d = well();
    d.continuousCurves[0].values[20] = -9999;
    QVERIFY(!prepareWellFaciesInput(d, model()).ready());
    d = well();
    d.continuousCurves[0].depths[20] = d.continuousCurves[0].depths[19];
    QVERIFY(!prepareWellFaciesInput(d, model()).ready());
    d = well();
    d.continuousCurves[0].depths.removeAt(20);
    d.continuousCurves[0].values.removeAt(20);
    QVERIFY(prepareWellFaciesInput(d, model())
                .reason.contains(QStringLiteral("缺口")));
    d = well();
    d.formationIntervals.clear();
    FormationInterval memberCode;
    memberCode.name = QStringLiteral("D61");
    memberCode.unitType = QStringLiteral("段");
    memberCode.topDepth = 1000;
    memberCode.bottomDepth = 1016;
    d.formationIntervals = {memberCode};
    d.continuousCurves[0].name = QStringLiteral("GR(API)");
    input = prepareWellFaciesInput(d, model());
    QVERIFY2(input.ready(), qPrintable(input.reason));
    QCOMPARE(input.rows.first().toObject().value(QStringLiteral("段")).toString(),
             QStringLiteral("D61"));
    d.lithologyIntervals[0].topDepth = 1008;
    QVERIFY(prepareWellFaciesInput(d, model()).reason.contains(QStringLiteral("128")));
    d = well();
    d.continuousCurves[0].depths.removeLast();
    d.continuousCurves[0].values.removeLast();
    QVERIFY(prepareWellFaciesInput(d, model()).reason.contains("128"));
    QTemporaryDir configDir;
    QVERIFY(configDir.isValid());
    const QString configPath = configDir.filePath(QStringLiteral("well-facies.json"));
    QFile configFile(configPath);
    QVERIFY(configFile.open(QIODevice::WriteOnly));
    configFile.write(
        "{\"baseUrl\":\"http://203.0.113.10:3100/api/v1\",\"apiKey\":\"test-key\"}");
    configFile.close();
    const QByteArray previous = qgetenv("PALEO_WELL_FACIES_CONFIG");
    qputenv("PALEO_WELL_FACIES_CONFIG", configPath.toUtf8());
    qunsetenv("PALEO_WELL_FACIES_URL");
    qunsetenv("PALEO_WELL_FACIES_API_KEY");
    qunsetenv("PALEO_WELL_FACIES_ALLOW_INSECURE_HTTP");
    const WellFaciesConfig loaded = WellFaciesConfig::load();
    if (previous.isEmpty())
      qunsetenv("PALEO_WELL_FACIES_CONFIG");
    else
      qputenv("PALEO_WELL_FACIES_CONFIG", previous);
    QVERIFY(loaded.allowInsecureHttp);
    QVERIFY(WellFaciesConfig::validateUrl(loaded.baseUrl, loaded.allowInsecureHttp).isEmpty());
  }
  void labelRemainsVisibleInsideLongInterval() {
    TextTrack track(QStringLiteral("预测相"), 100);
    TextInterval interval;
    interval.topDepth = 1000;
    interval.bottomDepth = 1100;
    // 用拉丁字母标签：本用例测「文字收进可见段」的几何，不测 CJK 字形；
    // CI runner 无 CJK 字体，「河口坝」的缺字形墨点数随字体环境漂移（实测 4～73）。
    interval.text = QStringLiteral("MouthBar");
    track.setIntervals({interval});
    // 视口 [1010,1020] m 完全落在区间 [1000,1100] 内：区间矩形中心在视口外，
    // 不 keepTextVisible 时文字画到视口外 → 中部无墨；keep 时文字收进可见段。
    // #131/#151：旧阈值 ink > 20 依赖 CJK 字体——CI runner 无 CJK 字体时
    // 「河口坝」渲染成缺字框（DejaVu-only 本机 20 点、CI runner 实测仅 4 点
    // lightness<120）。改为相对判别 + 「非白即墨」：keep 必须比不 keep 多出
    // 明显墨迹，抗锯齿的细缺字框也计入，与字体/字形无关。
    const auto inkFor = [&](bool keep) {
      track.setKeepTextVisible(keep);
      QImage image(100, 200, QImage::Format_RGB32);
      image.fill(Qt::white);
      QPainter painter(&image);
      track.paintBody(painter, QRectF(0, 0, 100, 200), 1010, 1020, 20);
      painter.end();
      int ink = 0;
      for (int y = 60; y < 140; ++y)
        for (int x = 10; x < 90; ++x)
          if (image.pixelColor(x, y).lightness() < 245) // 任何非白（含抗锯齿缺字框）都算墨
            ++ink;
      return ink;
    };
    const int hidden = inkFor(false);
    const int visible = inkFor(true);
    QVERIFY2(visible >= hidden + 8,
             qPrintable(QStringLiteral("keep=%1 nokeep=%2").arg(visible).arg(hidden)));
  }
  void resultContract() {
    auto input = prepareWellFaciesInput(well(), model());
    auto json = response(input.rows, "W-1");
    WellFaciesResult result;
    QString error;
    QVERIFY(parseWellFaciesResult(json, "W-1", input, &result, &error));
    QCOMPARE(result.intervals.size(), 1);
    QCOMPARE(result.confidence.values.size(), 128);
    QVERIFY(!parseWellFaciesResult(json, "different", input, &result, &error));
    auto rows = json["predictions"].toArray();
    auto p = rows[10].toObject();
    p["depth"] = 999.;
    rows[10] = p;
    json["predictions"] = rows;
    QVERIFY(!parseWellFaciesResult(json, "W-1", input, &result, &error));
  }
  void transport_data() {
    QTest::addColumn<bool>("queued");
    QTest::newRow("sync-200") << false;
    QTest::newRow("async-202") << true;
  }
  void transport() {
    QFETCH(bool, queued);
    Server server;
    server.queued = queued;
    server.foreignPoll = true;
    WellFaciesService service;
    service.configure(server.config());
    QSignalSpy models(&service, &WellFaciesService::modelsReady),
        done(&service, &WellFaciesService::completed),
        errors(&service, &WellFaciesService::failed);
    service.fetchModels();
    QTRY_COMPARE(models.size(), 1);
    service.predict(model(), "W-1", prepareWellFaciesInput(well(), model()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(errors.size(), 0);
    QCOMPARE(server.predictCount, 1);
    QCOMPARE(server.pollCount, queued ? 1 : 0);
    for (const auto &r : server.requests)
      QVERIFY(r.toLower().contains("x-api-key: test-key"));
    QVERIFY(server.requests[1].startsWith("POST /api/v1/predict"));
    QVERIFY(server.requests[1].contains("model-1"));
    if (queued)
      QVERIFY(
          server.requests.last().startsWith("GET /api/v1/predictions/job-1"));
  }
  void serviceErrors_data() {
    QTest::addColumn<int>("http");
    for (int status : {400, 401, 403, 404, 429, 500})
      QTest::newRow(qPrintable(QString::number(status))) << status;
  }
  void serviceErrors() {
    QFETCH(int, http);
    Server server;
    server.httpError = http;
    WellFaciesService service;
    service.configure(server.config());
    QSignalSpy failed(&service, &WellFaciesService::failed);
    service.fetchModels();
    QTRY_COMPARE(failed.size(), 1);
    QVERIFY(failed.first()[0].toString().contains(QString::number(http)));
    QVERIFY(failed.first()[0].toString().contains(QStringLiteral("缺少岩性")));
  }
  void malformedAndTerminalFailure() {
    Server server;
    server.malformed = true;
    WellFaciesService service;
    service.configure(server.config());
    QSignalSpy errors(&service, &WellFaciesService::failed);
    service.fetchModels();
    QTRY_COMPARE(errors.size(), 1);
    server.malformed = false;
    server.queued = true;
    server.terminalFailure = true;
    service.predict(model(), "W-1", prepareWellFaciesInput(well(), model()));
    QTRY_COMPARE(errors.size(), 2);
    QVERIFY(
        errors.last()[0].toString().contains(QStringLiteral("模型执行失败")));
    QCOMPARE(server.predictCount, 1);
  }
  void resumeAcceptedJob() {
    Server server;
    server.queued = true;
    WellFaciesService service;
    service.configure(server.config());
    QSignalSpy progress(&service, &WellFaciesService::progress),
        done(&service, &WellFaciesService::completed);
    const auto input = prepareWellFaciesInput(well(), model());
    service.predict(model(), "W-1", input);
    QTRY_COMPARE(progress.size(), 1);
    service.cancel();
    service.predict(model(), "W-1", input);
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(server.predictCount, 1);
  }
  void panelGatePredictionAndRestore() {
    Server server;
    QTemporaryDir dir;
    const QString path = dir.filePath("well.xml");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QByteArray xml = writeComprehensiveWellXml(well());
    QByteArray sheet =
        "<Worksheet "
        "ss:Name=\"测井曲线-W-1\"><Table><Row><Cell><Data>井号</Data></"
        "Cell><Cell><Data>深度</Data></Cell><Cell><Data>GR</Data></Cell></Row>";
    for (int i = 0; i < 128; ++i)
      sheet += "<Row><Cell><Data>W-1</Data></Cell><Cell><Data>" +
               QByteArray::number(1000 + i * .125, 'f', 3) +
               "</Data></Cell><Cell><Data>" +
               QByteArray::number(80 + i * .1, 'f', 3) + "</Data></Cell></Row>";
    sheet += "</Table></Worksheet>";
    xml.replace("</Workbook>", sheet + "</Workbook>");
    f.write(xml);
    f.close();
    WellCompositePanel panel;
    auto *workflow = new WellFaciesWorkflow(&panel);
    panel.bindFaciesWorkflow(workflow);
    workflow->configure(server.config(), false);
    QVERIFY(panel.loadComprehensiveXml(path));
    auto *run = panel.findChild<QToolButton *>("btnPredictFacies");
    QVERIFY(run);
    QTRY_VERIFY2(run->toolTip().startsWith(QStringLiteral("提交当前井")),
                 qPrintable(run->toolTip()));
    QVERIFY(panel.isReferenceWell());
    QVERIFY(run->toolTip().contains(QStringLiteral("恩平组")));
    auto *faciesStatus = panel.findChild<QLabel *>("lblFaciesPredictionStatus");
    QVERIFY(faciesStatus);
    QVERIFY2(faciesStatus->text().contains(QStringLiteral("恩平组")),
             qPrintable(faciesStatus->text()));
    const auto observed = panel.currentData().faciesIntervals;
    run->click();
    QTRY_VERIFY(
        panel.findChild<QToolButton *>("btnShowPredictedFacies")->isEnabled());
    QCOMPARE(panel.currentData().faciesIntervals.first().microFacies,
             observed.first().microFacies);
    auto count = [&panel] {
      int n = 0;
      for (const auto &t : panel.canvas()->tracks())
        if (t->title() == QStringLiteral("预测相"))
          ++n;
      return n;
    };
    QCOMPARE(count(), 1);
    // 优势相道：well() 的地层道有 恩平组（组）和 恩平一段（段），各一道。
    const auto dominantTracks = [&panel] {
      QStringList titles;
      for (const auto &t : panel.canvas()->tracks())
        if (t->title().startsWith(QStringLiteral("优势相")))
          titles << t->title();
      return titles;
    };
    QCOMPARE(dominantTracks(),
             (QStringList{QStringLiteral("优势相（组）"), QStringLiteral("优势相（段）")}));
    for (const auto &t : panel.canvas()->tracks())
      if (t->title() == QStringLiteral("优势相（组）")) {
        const auto text = std::static_pointer_cast<TextTrack>(t)->intervals();
        QCOMPARE(text.size(), 1);
        QCOMPARE(text.first().text, QStringLiteral("河口坝 100%"));
        QCOMPARE(text.first().category, QStringLiteral("恩平组"));
      }
    workflow->setData(well());
    QTRY_COMPARE(count(), 1);
    QCOMPARE(dominantTracks().size(), 2);
    QCOMPARE(server.predictCount, 1); // 精确输入命中缓存
    QVERIFY(panel.loadWellData(well()));
    QVERIFY(!panel.isReferenceWell());
    workflow->setData(well());
    QTRY_VERIFY(run->isEnabled());
    auto *show = panel.findChild<QToolButton *>("btnShowPredictedFacies");
    show->click();
    for (const auto &t : panel.canvas()->tracks())
      if (t->title() == QStringLiteral("预测相") ||
          t->title().startsWith(QStringLiteral("优势相")))
        QVERIFY(!t->isVisible());
    QVERIFY(panel.loadLasCurves("another", well().continuousCurves,
                                well().formationIntervals));
    QVERIFY(run->isEnabled());
    QVERIFY(run->toolTip().contains(QStringLiteral("岩性")));
    QCOMPARE(count(), 0);
    QVERIFY(dominantTracks().isEmpty());
    QVERIFY(panel.currentData().lithologyIntervals.isEmpty());
    QVERIFY(panel.currentData().faciesIntervals.isEmpty());
  }
  void closeDuringPrediction() {
    Server server;
    server.hold = true;
    auto service = std::make_unique<WellFaciesService>();
    service->configure(server.config());
    service->predict(model(), "W-1", prepareWellFaciesInput(well(), model()));
    QTRY_COMPARE(server.predictCount, 1);
    service.reset();
    QTest::qWait(100);
    QCOMPARE(server.predictCount, 1);
  }
  void cancelAndSwitchWell() {
    Server server;
    server.hold = true;
    WellFaciesWorkflow workflow;
    workflow.configure(server.config(), false);
    workflow.setData(well());
    QSignalSpy busy(&workflow, &WellFaciesWorkflow::busyChanged),
        done(&workflow, &WellFaciesWorkflow::resultReady);
    QTest::qWait(100);
    workflow.run();
    QTRY_COMPARE(server.predictCount, 1);
    auto other = well();
    other.wellName = "other";
    other.lithologyIntervals.clear();
    workflow.setData(other);
    QCOMPARE(busy.last()[0].toBool(), false);
    QCOMPARE(done.size(), 0);
    QTest::qWait(150);
    QCOMPARE(server.predictCount, 1);
    QCOMPARE(done.size(), 0);
  }
  void planPredictsByFormationGroup() {
    const QVector<WellFaciesModel> both{model(), zhuhaiModel()};
    auto plan = planWellFaciesRuns(twoGroupWell(), both, "model-1");
    QVERIFY2(plan.ready(), qPrintable(plan.reason));
    QCOMPARE(plan.runs.size(), 2);
    // 按井深自上而下，每组用 formationGroup 一致的模型。
    QCOMPARE(plan.runs[0].group, QStringLiteral("珠海组"));
    QCOMPARE(plan.runs[0].model.id, QStringLiteral("model-2"));
    QCOMPARE(plan.runs[0].input.rows.size(), 128);
    QCOMPARE(plan.runs[1].group, QStringLiteral("恩平组"));
    QCOMPARE(plan.runs[1].model.id, QStringLiteral("model-1"));
    QCOMPARE(plan.runs[1].input.rows.size(), 128);
    QVERIFY(plan.skipped.isEmpty());
    QCOMPARE(plan.runs[0].input.rows.first().toObject().value(QStringLiteral("段")).toString(),
             QStringLiteral("珠海一段"));
    QCOMPARE(plan.runs[1].input.rows.first().toObject().value(QStringLiteral("段")).toString(),
             QStringLiteral("恩平一段"));

    // 只有一个模型：另一个组如实标「无对应模型」，不拿这个模型去预测它。
    plan = planWellFaciesRuns(twoGroupWell(), {model()}, "model-1");
    QCOMPARE(plan.runs.size(), 1);
    QCOMPARE(plan.runs[0].group, QStringLiteral("恩平组"));
    QCOMPARE(plan.skipped, QStringList{QStringLiteral("珠海组（无对应模型）")});

    // 井上有第三个组但没有模型：照跑有模型的两组，并在 skipped 里说明。
    plan = planWellFaciesRuns(twoGroupWell(true), both, "model-1");
    QCOMPARE(plan.runs.size(), 2);
    QCOMPARE(plan.skipped, QStringList{QStringLiteral("文昌组（无对应模型）")});

    // 同组多个版本：所选版本优先。
    auto v2 = model();
    v2.id = "model-1b";
    v2.version = "v2";
    plan = planWellFaciesRuns(twoGroupWell(), {model(), v2}, "model-1b");
    QCOMPARE(plan.runs.size(), 1);
    QCOMPARE(plan.runs[0].model.id, QStringLiteral("model-1b"));
    plan = planWellFaciesRuns(twoGroupWell(), {model(), v2}, "");
    QCOMPARE(plan.runs[0].model.id, QStringLiteral("model-1"));

    // 某组输入不可用（点数不足）：不拖累别的组，原因写进 skipped。
    auto shortWell = twoGroupWell();
    shortWell.continuousCurves[0].depths.resize(128 + 64);
    shortWell.continuousCurves[0].values.resize(128 + 64);
    plan = planWellFaciesRuns(shortWell, both, "model-1");
    QCOMPARE(plan.runs.size(), 1);
    QCOMPARE(plan.runs[0].group, QStringLiteral("珠海组"));
    QCOMPARE(plan.skipped.size(), 1);
    QVERIFY(plan.skipped[0].startsWith(QStringLiteral("恩平组（")));

    // 没有任何组命中模型：沿用所选模型的原因，不静默。
    auto none = model();
    none.formationGroup = QStringLiteral("沙一组");
    plan = planWellFaciesRuns(twoGroupWell(), {none}, none.id);
    QVERIFY(!plan.ready());
    QVERIFY(plan.reason.contains(QStringLiteral("沙一组")));
    QVERIFY(!planWellFaciesRuns(twoGroupWell(), {}, "").ready());
  }
  // #410：编图口径——当前地层裁剪 + 单模型单 run（planWellFaciesFormationRun
  // 此前零覆盖）。
  void planFormationRunScopesToCurrentHorizon() {
    const QVector<WellFaciesModel> both{model(), zhuhaiModel()};
    // 当前地层 = 恩平组（1016–1032）：输入裁到该窗，只匹配恩平组模型、只跑一次。
    auto plan = planWellFaciesFormationRun(twoGroupWell(), both, QStringLiteral("恩平组"));
    QVERIFY2(plan.ready(), qPrintable(plan.reason));
    QCOMPARE(plan.runs.size(), 1);
    QCOMPARE(plan.runs[0].group, QStringLiteral("恩平组"));
    QCOMPARE(plan.runs[0].model.id, QStringLiteral("model-1"));
    QCOMPARE(plan.runs[0].input.rows.size(), 128);
    QCOMPARE(plan.runs[0].input.rows.first().toObject().value(QStringLiteral("段")).toString(),
             QStringLiteral("恩平一段"));

    // 换当前地层到珠海组：另一组模型对应另一组窗，不串组。
    plan = planWellFaciesFormationRun(twoGroupWell(), both, QStringLiteral("珠海组"));
    QVERIFY2(plan.ready(), qPrintable(plan.reason));
    QCOMPARE(plan.runs.size(), 1);
    QCOMPARE(plan.runs[0].group, QStringLiteral("珠海组"));
    QCOMPARE(plan.runs[0].model.id, QStringLiteral("model-2"));
    QCOMPARE(plan.runs[0].input.rows.size(), 128);

    // 当前地层给段名：仍按窗内组匹配模型（编图段名不限组模型沿用）。
    plan = planWellFaciesFormationRun(twoGroupWell(), both, QStringLiteral("恩平一段"));
    QVERIFY2(plan.ready(), qPrintable(plan.reason));
    QCOMPARE(plan.runs[0].model.id, QStringLiteral("model-1"));

    // 不限组模型兜底：当前地层没有组模型时用它。
    auto generic = model();
    generic.id = "model-g";
    generic.formationGroup.clear();
    plan = planWellFaciesFormationRun(twoGroupWell(), {generic}, QStringLiteral("恩平组"));
    QVERIFY2(plan.ready(), qPrintable(plan.reason));
    QCOMPARE(plan.runs[0].model.id, QStringLiteral("model-g"));

    // 只有别的组的模型：如实拒绝，不拿错模型预测当前地层。
    plan = planWellFaciesFormationRun(twoGroupWell(), {zhuhaiModel()}, QStringLiteral("恩平组"));
    QVERIFY(!plan.ready());
    QVERIFY(plan.reason.contains(QStringLiteral("恩平组")));

    // 不存在的地层：如实报缺有效顶底深。
    plan = planWellFaciesFormationRun(twoGroupWell(), both, QStringLiteral("不存在的组"));
    QVERIFY(!plan.ready());
    QVERIFY(!plan.reason.isEmpty());
  }
  void mergeResultsKeepsDepthOrder() {
    const auto make = [](const QString &group, double top, const QString &label) {
      WellFaciesResult r;
      r.group = group;
      r.jobId = "job-" + group;
      r.modelName = QStringLiteral("M");
      r.modelVersion = "v1";
      TextInterval i;
      i.topDepth = top;
      i.bottomDepth = top + 2;
      i.text = label;
      r.intervals = {i};
      r.confidence.name = QStringLiteral("置信度");
      r.confidence.depths = {float(top), float(top + 1)};
      r.confidence.values = {50, 60};
      r.response = {{"jobId", r.jobId}};
      return r;
    };
    const auto lower = make(QStringLiteral("恩平组"), 1016, QStringLiteral("河口坝"));
    const auto upper = make(QStringLiteral("珠海组"), 1000, QStringLiteral("分流河道"));
    QCOMPARE(mergeWellFaciesResults({lower}).intervals.size(), 1);
    const auto merged = mergeWellFaciesResults({lower, upper});
    QCOMPARE(merged.intervals.size(), 2);
    QCOMPARE(merged.intervals[0].text, QStringLiteral("分流河道"));
    QCOMPARE(merged.intervals[1].text, QStringLiteral("河口坝"));
    QCOMPARE(merged.confidence.depths, (QVector<float>{1000, 1001, 1016, 1017}));
    QCOMPARE(merged.group, QStringLiteral("恩平组、珠海组"));
    QCOMPARE(merged.modelName, QStringLiteral("M"));
    QVERIFY(merged.jobId.contains(QStringLiteral("job-恩平组")));
    QVERIFY(merged.jobId.contains(QStringLiteral("job-珠海组")));
    QCOMPARE(merged.response.value("groups").toArray().size(), 2);
  }
  // 逐组预测：每组一次 /predict、逐组返回合并显示，每组一条任务中心任务。
  void workflowPredictsFormationByFormation() {
    Server server;
    server.models = {model(), zhuhaiModel()};
    server.labels = {{"model-1", QStringLiteral("河口坝")},
                     {"model-2", QStringLiteral("分流河道")}};
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy results(&workflow, &WellFaciesWorkflow::resultReady),
        busy(&workflow, &WellFaciesWorkflow::busyChanged),
        status(&workflow, &WellFaciesWorkflow::statusChanged),
        plans(&workflow, &WellFaciesWorkflow::planChanged),
        ready(&workflow, &WellFaciesWorkflow::availabilityChanged);
    workflow.configure(server.config(), false);
    workflow.setData(twoGroupWell());
    QTRY_VERIFY2(!ready.isEmpty() && ready.last()[0].toBool(),
                 ready.isEmpty() ? "no availability"
                                 : qPrintable(ready.last()[1].toString()));
    QVERIFY(plans.last()[0].toString().contains(QStringLiteral("珠海组")));
    QVERIFY(plans.last()[0].toString().contains(QStringLiteral("恩平组")));
    results.clear();

    workflow.run();
    // 每个地层一条任务，按井深排队，进场即在任务列表里。
    QCOMPARE(tasks.tasks().size(), 2);
    QVERIFY(tasks.tasks()[0]->title().contains(QStringLiteral("珠海组")));
    QVERIFY(tasks.tasks()[1]->title().contains(QStringLiteral("恩平组")));
    QVERIFY(!tasks.tasks()[0]->quiet());
    // #407：不钉 percent()==0——任务派发即进线程池，run() 返回瞬间 worker
    // 可能已上报进度（负载机上间歇假红）；初始进度不是可同步断言的契约。

    QTRY_COMPARE(results.size(), 2);
    QTRY_VERIFY(!busy.last()[0].toBool());
    QCOMPARE(server.predictCount, 2);
    QCOMPARE(server.predictModels, (QStringList{"model-2", "model-1"}));
    QCOMPARE(server.predictRows, (QList<int>{128, 128}));
    // 第一组一回来就显示，第二组回来后并入。
    const auto first = qvariant_cast<WellFaciesResult>(results[0][0]);
    QCOMPARE(first.group, QStringLiteral("珠海组"));
    QCOMPARE(first.intervals.size(), 1);
    QCOMPARE(first.intervals[0].text, QStringLiteral("分流河道"));
    const auto merged = qvariant_cast<WellFaciesResult>(results[1][0]);
    QCOMPARE(merged.intervals.size(), 2);
    QCOMPARE(merged.intervals[0].text, QStringLiteral("分流河道"));
    QCOMPARE(merged.intervals[1].text, QStringLiteral("河口坝"));
    QCOMPARE(merged.intervals[1].topDepth, 1016.f);
    QCOMPARE(merged.confidence.depths.size(), 256);
    // 优势相：第一组回来时只有珠海组/段，合并后四个地层单位各一行。
    const auto firstDominant = first.dominant;
    QCOMPARE(firstDominant.size(), 2);
    QCOMPARE(firstDominant[0].formation, QStringLiteral("珠海组"));
    QCOMPARE(firstDominant[0].dominant, QStringLiteral("分流河道"));
    QCOMPARE(merged.dominant.size(), 4);
    QHash<QString, FormationDominance> byName;
    for (const auto &d : merged.dominant)
      byName.insert(d.formation, d);
    QCOMPARE(byName.value(QStringLiteral("恩平组")).dominant, QStringLiteral("河口坝"));
    QCOMPARE(byName.value(QStringLiteral("恩平组")).unitType, QStringLiteral("组"));
    QCOMPARE(byName.value(QStringLiteral("珠海一段")).dominant, QStringLiteral("分流河道"));
    QCOMPARE(byName.value(QStringLiteral("珠海一段")).unitType, QStringLiteral("段"));
    QVERIFY(byName.value(QStringLiteral("恩平组")).coverage > 0.99);
    QTRY_VERIFY(tasks.tasks()[0]->state() == PaleoTask::State::Succeeded);
    QTRY_VERIFY(tasks.tasks()[1]->state() == PaleoTask::State::Succeeded);
    QCOMPARE(tasks.tasks()[1]->percent(), 100);
    QVERIFY2(status.last()[0].toString().contains(QStringLiteral("预测完成")),
             qPrintable(status.last()[0].toString()));
    QVERIFY(status.last()[0].toString().contains(QStringLiteral("2 个地层")));

    // 重新打开同一口井：逐组命中缓存，合并后恢复，不再请求。
    results.clear();
    workflow.setData(twoGroupWell());
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(qvariant_cast<WellFaciesResult>(results[0][0]).intervals.size(), 2);
    QCOMPARE(server.predictCount, 2);
  }
  // 缓存键含模型版本/标签：服务端同 id 换版后，旧键的缓存不得回放——
  // 预测结果必须与产它的模型对得上（旧键缺版本会静默回放上一版结果）。
  void cacheKeyIncludesModelVersion() {
    Server server;
    server.models = {model()};
    server.labels = {{"model-1", QStringLiteral("河口坝")}};
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy results(&workflow, &WellFaciesWorkflow::resultReady),
        ready(&workflow, &WellFaciesWorkflow::availabilityChanged);
    workflow.configure(server.config(), false);
    workflow.setData(well());
    QTRY_VERIFY2(!ready.isEmpty() && ready.last()[0].toBool(),
                 ready.isEmpty() ? "no availability"
                                 : qPrintable(ready.last()[1].toString()));
    workflow.run();
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(server.predictCount, 1);
    QCOMPARE(qvariant_cast<WellFaciesResult>(results[0][0]).intervals.size(), 1);
    QCOMPARE(qvariant_cast<WellFaciesResult>(results[0][0]).intervals[0].text,
             QStringLiteral("河口坝"));

    // 同一井重开：v1 缓存命中（既有行为），不重新请求。
    results.clear();
    workflow.setData(well());
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(server.predictCount, 1);

    // 服务端同 id 换版（v1→v2，标签也换）：刷新模型后旧缓存必须失效——
    // 换版后的 updateInput 不得回放 v1 结果（results 保持空），run() 重新
    // 请求并拿到 v2 标签。
    WellFaciesModel upgraded = model();
    upgraded.version = QStringLiteral("v2");
    upgraded.labels = {QStringLiteral("三角洲前缘")};
    server.models = {upgraded};
    server.labels = {{"model-1", QStringLiteral("三角洲前缘")}};
    results.clear();
    ready.clear();
    workflow.refreshModels();
    // 模型清单就绪且计划可用后，缓存回放只应命中 v2 键——v1 文件不再匹配。
    QTRY_VERIFY2(!ready.isEmpty() && ready.last()[0].toBool(),
                 ready.isEmpty() ? "no availability"
                                 : qPrintable(ready.last()[1].toString()));
    QVERIFY2(results.isEmpty(), "换版后旧缓存被回放（缓存键缺模型版本）");
    workflow.run();
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(server.predictCount, 2);
    QCOMPARE(qvariant_cast<WellFaciesResult>(results[0][0]).intervals[0].text,
             QStringLiteral("三角洲前缘"));
  }
  void taskProgressFollowsServerProgress() {
    Server server;
    server.queued = true;
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy ready(&workflow, &WellFaciesWorkflow::availabilityChanged),
        results(&workflow, &WellFaciesWorkflow::resultReady);
    workflow.configure(server.config(), false);
    workflow.setData(well());
    QTRY_VERIFY(!ready.isEmpty() && ready.last()[0].toBool());
    workflow.run();
    QCOMPARE(tasks.tasks().size(), 1);
    PaleoTask *task = tasks.tasks()[0];
    int runningPercent = -1;
    connect(task, &PaleoTask::changed, &workflow, [&] {
      if (task->running())
        runningPercent = qMax(runningPercent, task->percent());
    });
    QTRY_COMPARE(results.size(), 1);
    QTRY_VERIFY(task->state() == PaleoTask::State::Succeeded);
    QCOMPARE(runningPercent, 45); // 服务端报的进度原样进任务行
    QCOMPARE(task->percent(), 100);
  }
  // 一个地层失败不拖累其余：失败任务标失败，其余照常返回并显示。
  void failedFormationDoesNotStopOthers() {
    Server server;
    server.models = {model(), zhuhaiModel()};
    server.failModel = "model-2";
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy ready(&workflow, &WellFaciesWorkflow::availabilityChanged),
        results(&workflow, &WellFaciesWorkflow::resultReady),
        busy(&workflow, &WellFaciesWorkflow::busyChanged),
        status(&workflow, &WellFaciesWorkflow::statusChanged);
    workflow.configure(server.config(), false);
    workflow.setData(twoGroupWell());
    QTRY_VERIFY(!ready.isEmpty() && ready.last()[0].toBool());
    workflow.run();
    QTRY_VERIFY(!busy.isEmpty() && !busy.last()[0].toBool());
    QCOMPARE(server.predictCount, 2);
    QCOMPARE(results.size(), 1);
    const auto result = qvariant_cast<WellFaciesResult>(results[0][0]);
    QCOMPARE(result.group, QStringLiteral("恩平组"));
    QTRY_VERIFY(tasks.tasks()[0]->state() == PaleoTask::State::Failed);
    QVERIFY(tasks.tasks()[0]->errorText().contains(QStringLiteral("模型服务内部错误")));
    QTRY_VERIFY(tasks.tasks()[1]->state() == PaleoTask::State::Succeeded);
    const QString last = status.last()[0].toString();
    QVERIFY2(last.contains(QStringLiteral("预测完成")) &&
                 last.contains(QStringLiteral("失败：珠海组")),
             qPrintable(last));
  }
  // 任务中心里取消某一个地层：只放弃它，后面的地层照常提交。
  void cancelOneFormationTaskContinuesWithNext() {
    Server server;
    server.models = {model(), zhuhaiModel()};
    server.hold = true;
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy ready(&workflow, &WellFaciesWorkflow::availabilityChanged),
        busy(&workflow, &WellFaciesWorkflow::busyChanged);
    workflow.configure(server.config(), false);
    workflow.setData(twoGroupWell());
    QTRY_VERIFY(!ready.isEmpty() && ready.last()[0].toBool());
    workflow.run();
    QTRY_COMPARE(server.predictCount, 1);
    QCOMPARE(tasks.tasks().size(), 2);
    tasks.tasks()[0]->requestCancel();
    QTRY_COMPARE(server.predictCount, 2);
    QTRY_VERIFY(tasks.tasks()[0]->state() == PaleoTask::State::Cancelled);
    QVERIFY(tasks.tasks()[1]->running());
    QVERIFY(busy.last()[0].toBool());
    tasks.tasks()[1]->requestCancel();
    QTRY_VERIFY(!busy.last()[0].toBool());
    QTRY_VERIFY(tasks.tasks()[1]->state() == PaleoTask::State::Cancelled);
    QCOMPARE(server.predictCount, 2);
  }
  // 排队中的地层被取消：轮到它时跳过，不发请求。
  void cancelQueuedFormationTaskIsSkipped() {
    Server server;
    server.models = {model(), zhuhaiModel()};
    server.hold = true;
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy ready(&workflow, &WellFaciesWorkflow::availabilityChanged),
        busy(&workflow, &WellFaciesWorkflow::busyChanged);
    workflow.configure(server.config(), false);
    workflow.setData(twoGroupWell());
    QTRY_VERIFY(!ready.isEmpty() && ready.last()[0].toBool());
    workflow.run();
    QTRY_COMPARE(server.predictCount, 1);
    tasks.tasks()[1]->requestCancel(); // 恩平组还在排队：任务行立即结束
    QTRY_VERIFY(tasks.tasks()[1]->state() == PaleoTask::State::Cancelled);
    QVERIFY(tasks.tasks()[0]->running());
    QVERIFY(busy.last()[0].toBool());
    tasks.tasks()[0]->requestCancel();
    QTRY_VERIFY(!busy.last()[0].toBool());
    QTest::qWait(100);
    QCOMPARE(server.predictCount, 1);
    QTRY_VERIFY(tasks.tasks()[0]->state() == PaleoTask::State::Cancelled);
    QTRY_VERIFY(tasks.tasks()[1]->state() == PaleoTask::State::Cancelled);
  }
  // 工程切换（任务会话号变化）：已返回的地层照常显示，但不写进新工程的属性。
  void sessionChangeDoesNotPublishResults() {
    Server server;
    server.models = {model(), zhuhaiModel()};
    server.holdModel = "model-1"; // 恩平组一直不回应
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy ready(&workflow, &WellFaciesWorkflow::availabilityChanged),
        results(&workflow, &WellFaciesWorkflow::resultReady),
        busy(&workflow, &WellFaciesWorkflow::busyChanged),
        status(&workflow, &WellFaciesWorkflow::statusChanged);
    workflow.configure(server.config(), false);
    workflow.setData(twoGroupWell());
    QTRY_VERIFY(!ready.isEmpty() && ready.last()[0].toBool());
    workflow.run();
    QTRY_COMPARE(results.size(), 1); // 珠海组已返回
    QTRY_COMPARE(server.predictCount, 2);
    tasks.beginNewSession();
    QTRY_VERIFY(!busy.last()[0].toBool());
    QCOMPARE(results.size(), 1);
    const QString last = status.last()[0].toString();
    QVERIFY2(last.contains(QStringLiteral("工程已切换")), qPrintable(last));
    QVERIFY2(last.contains(QStringLiteral("已取消：恩平组")), qPrintable(last));
    QTRY_VERIFY(tasks.tasks()[0]->state() == PaleoTask::State::Succeeded);
    QTRY_VERIFY(tasks.tasks()[1]->state() == PaleoTask::State::Cancelled);
  }
  // 「停止等待」：全部地层任务一并结束为已取消。
  void stopWaitingCancelsEveryFormationTask() {
    Server server;
    server.models = {model(), zhuhaiModel()};
    server.hold = true;
    PaleoTaskService tasks;
    WellFaciesWorkflow workflow;
    workflow.setTaskService(&tasks);
    QSignalSpy ready(&workflow, &WellFaciesWorkflow::availabilityChanged),
        busy(&workflow, &WellFaciesWorkflow::busyChanged);
    workflow.configure(server.config(), false);
    workflow.setData(twoGroupWell());
    QTRY_VERIFY(!ready.isEmpty() && ready.last()[0].toBool());
    workflow.run();
    QTRY_COMPARE(server.predictCount, 1);
    workflow.cancel();
    QVERIFY(!busy.last()[0].toBool());
    QTRY_VERIFY(tasks.tasks()[0]->state() == PaleoTask::State::Cancelled);
    QTRY_VERIFY(tasks.tasks()[1]->state() == PaleoTask::State::Cancelled);
    QTest::qWait(100);
    QCOMPARE(server.predictCount, 1);
  }
  // 停掉后再点预测：在途地层续查已受理任务，不重复提交该地层。
  void resumeInFlightFormationAfterStop() {
    Server server;
    server.models = {model(), zhuhaiModel()};
    server.queued = true;
    WellFaciesWorkflow workflow;
    QSignalSpy ready(&workflow, &WellFaciesWorkflow::availabilityChanged),
        results(&workflow, &WellFaciesWorkflow::resultReady),
        status(&workflow, &WellFaciesWorkflow::statusChanged);
    workflow.configure(server.config(), false);
    workflow.setData(twoGroupWell());
    QTRY_VERIFY(!ready.isEmpty() && ready.last()[0].toBool());
    workflow.run();
    // 等到 202 受理并回报进度（此时任务 ID 已记下），再停。
    QTRY_VERIFY(std::any_of(status.cbegin(), status.cend(), [](const auto &s) {
      return s[0].toString().contains(QStringLiteral("45%"));
    }));
    QCOMPARE(server.predictCount, 1);
    workflow.cancel();
    workflow.run();
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 10000);
    // 珠海组的 job 已受理，续查而不是重新提交：两组共 2 次 /predict。
    QCOMPARE(server.predictCount, 2);
  }
  void realReferenceDirectory_data() {
    QTest::addColumn<QString>("path");
    const QString directory =
        qEnvironmentVariable("PALEO_WELL_FACIES_TEST_DIR");
    if (directory.isEmpty()) {
      QTest::newRow("not-configured") << QString();
      return;
    }
    const QDir dir(directory);
    const auto files = dir.entryInfoList({"*.xml"}, QDir::Files, QDir::Name);
    QVERIFY2(!files.isEmpty(),
             "No XML reference wells found in test directory");
    for (const auto &file : files)
      QTest::newRow(qPrintable(file.fileName())) << file.absoluteFilePath();
  }
  void realReferenceDirectory() {
    QFETCH(QString, path);
    if (path.isEmpty())
      QSKIP("Set PALEO_WELL_FACIES_TEST_DIR for batch reference-well QA.");
    ComprehensiveWellData data;
    QString error;
    QVERIFY2(parseComprehensiveWellXml(path, data, &error), qPrintable(error));
    const auto input = prepareWellFaciesInput(data, model());
    QJsonObject entry{{"file", QFileInfo(path).fileName()},
                      {"wellName", data.wellName},
                      {"eligible", input.ready()},
                      {"rows", input.rows.size()},
                      {"reason", input.reason}};
    if (input.ready()) {
      entry.insert("top",
                   input.rows.first().toObject().value(QStringLiteral("深度")));
      entry.insert("bottom",
                   input.rows.last().toObject().value(QStringLiteral("深度")));
    }
    m_referenceReport.append(entry);
    qInfo().noquote() << QJsonDocument(entry).toJson(QJsonDocument::Compact);

    // 模型要求与线上当前模型一致；批量传输只调用本地测试服务器。
    Server server;
    server.queued = m_referenceReport.size() % 2 == 0;
    WellCompositePanel panel;
    auto *workflow = new WellFaciesWorkflow(&panel);
    panel.bindFaciesWorkflow(workflow);
    QSignalSpy models(workflow, &WellFaciesWorkflow::modelsChanged),
        done(workflow, &WellFaciesWorkflow::resultReady);
    workflow->configure(server.config(), false);
    QTRY_VERIFY(models.size() >= 2);
    auto *run = panel.findChild<QToolButton *>("btnPredictFacies");
    QVERIFY(run);
    for (bool reference : {true, false}) {
      QVERIFY(panel.loadWellData(data, path, reference));
      QCOMPARE(panel.isReferenceWell(), reference);
      QVERIFY(run->isEnabled());
      if (!input.ready()) {
        QCOMPARE(run->toolTip(), input.reason);
        continue;
      }
      done.clear();
      run->click();
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
      const auto result = qvariant_cast<WellFaciesResult>(done.first()[0]);
      QCOMPARE(result.confidence.depths.size(), input.rows.size());
      QVERIFY(!result.intervals.isEmpty());
      QCOMPARE(panel.currentData().faciesIntervals.size(),
               data.faciesIntervals.size());
      QVERIFY(panel.findChild<QToolButton *>("btnShowPredictedFacies")
                  ->isEnabled());
      int tracks = 0;
      for (const auto &track : panel.canvas()->tracks())
        if (track->title() == QStringLiteral("预测相") ||
            track->title() == QStringLiteral("预测置信度"))
          ++tracks;
      QCOMPARE(tracks, 2);
    }
  }
  void realReferenceWell() {
    const QString path = qEnvironmentVariable("PALEO_WELL_FACIES_TEST_XML");
    if (path.isEmpty())
      QSKIP("Set PALEO_WELL_FACIES_TEST_XML to verify a real reference well; "
            "PALEO_WELL_FACIES_LIVE=1 enables its authorized API call.");
    ComprehensiveWellData d;
    QString error;
    QVERIFY2(parseComprehensiveWellXml(path, d, &error), qPrintable(error));
    const auto input = prepareWellFaciesInput(
        d, WellFaciesModel{"8140daf4-664d-440b-8aa6-f68773f57dfc",
                           {},
                           {},
                           QStringLiteral("恩平组"),
                           {"GR"},
                           {},
                           128});
    QVERIFY2(input.ready(), qPrintable(input.reason));
    qInfo() << "Reference well" << d.wellName << "eligible rows"
            << input.rows.size();
    const bool live = qEnvironmentVariable("PALEO_WELL_FACIES_LIVE") == "1";
    const QString screenshot =
        qEnvironmentVariable("PALEO_WELL_FACIES_SCREENSHOT");
    if (!live && screenshot.isEmpty())
      return;
    Server mockServer;
    PaleoTheme::applyLightTheme();
    WellCompositePanel panel;
    auto *workflow = new WellFaciesWorkflow(&panel);
    if (!live) {
      workflow->configure(mockServer.config(), false);
      qInfo() << "Rendering uses local test-server predictions, not live "
                 "inference.";
    }
    QSignalSpy done(workflow, &WellFaciesWorkflow::resultReady),
        status(workflow, &WellFaciesWorkflow::statusChanged),
        busy(workflow, &WellFaciesWorkflow::busyChanged);
    connect(workflow, &WellFaciesWorkflow::statusChanged, workflow,
            [](const QString &message) { qInfo().noquote() << message; });
    bool ready = false;
    QObject::connect(workflow, &WellFaciesWorkflow::availabilityChanged,
                     workflow,
                     [&ready](bool ok, const QString &) { ready = ok; });
    panel.bindFaciesWorkflow(workflow);
    QVERIFY(panel.loadComprehensiveXml(path));
    QTRY_VERIFY_WITH_TIMEOUT(ready, 30000);
    busy.clear();
    panel.findChild<QToolButton *>("btnPredictFacies")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!busy.isEmpty() && !busy.last()[0].toBool(),
                             180000);
    QVERIFY2(!done.isEmpty(), status.isEmpty()
                                  ? "No result"
                                  : qPrintable(status.last()[0].toString()));
    qInfo() << status.last()[0].toString();
    QVERIFY(
        panel.findChild<QToolButton *>("btnShowPredictedFacies")->isEnabled());
    for (const auto &t : panel.canvas()->tracks()) {
      if (t->type() == TrackType::Curve) {
        const auto c = std::static_pointer_cast<CurveTrack>(t);
        bool keep = t->title() == tr("预测置信度");
        for (const auto &curve : c->curves())
          if (curve.name == "GR")
            keep = true;
        t->setVisible(keep);
      } else if (t->type() != TrackType::DepthScale &&
                 t->type() != TrackType::Lithology &&
                 t->type() != TrackType::Formation &&
                 t->type() != TrackType::FaciesCompound &&
                 t->title() != tr("预测相"))
        t->setVisible(false);
    }
    panel.resize(1600, 900);
    panel.show();
    panel.canvas()->setZoomFactor(4);
    panel.canvas()->setScrollDepth(
        input.rows.first().toObject().value(QStringLiteral("深度")).toDouble());
    panel.canvas()->updateAll();
    QTest::qWait(100);
    if (!screenshot.isEmpty())
      QVERIFY(panel.grab().save(screenshot));
  }
};
QTEST_MAIN(TestWellFacies)
#include "tst_wellfacies.moc"
