#include "ai/wellfaciesservice.h"
#include "domain/welllogfacies.h"
#include "io/wellcompositexml.h"
#include "ui/paleotheme.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "workflow/wellfaciesworkflow.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
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
QJsonObject response(const QJsonArray &rows, const QString &name) {
  QJsonArray p;
  for (const auto &r : rows)
    p.append(QJsonObject{{"wellName", name},
                         {"depth", r.toObject().value(QStringLiteral("深度"))},
                         {"label", QStringLiteral("河口坝")},
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
            const auto m = model();
            json = {{"models",
                     QJsonArray{QJsonObject{
                         {"id", m.id},
                         {"name", m.name},
                         {"version", m.version},
                         {"inputSchema",
                          QJsonObject{{"formationGroup", m.formationGroup},
                                      {"curves", QJsonArray{"GR"}},
                                      {"categoricalFields",
                                       QJsonArray{QStringLiteral("段"),
                                                  QStringLiteral("岩性")}},
                                      {"window", 128}}}}}}};
          } else if (path.endsWith("/predict")) {
            ++predictCount;
            const auto body =
                QJsonDocument::fromJson(request.mid(end + 4)).object();
            const auto w = body.value("wells").toArray().first().toObject();
            result = response(w.value("rows").toArray(),
                              w.value("wellName").toString());
            if (hold)
              return;
            if (queued) {
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
    d.continuousCurves[0].depths.removeLast();
    d.continuousCurves[0].values.removeLast();
    QVERIFY(prepareWellFaciesInput(d, model()).reason.contains("128"));
  }
  void labelRemainsVisibleInsideLongInterval() {
    TextTrack track(QStringLiteral("预测相"), 100);
    TextInterval interval;
    interval.topDepth = 1000;
    interval.bottomDepth = 1100;
    interval.text = QStringLiteral("河口坝");
    track.setIntervals({interval});
    // 视口 [1010,1020] m 完全落在区间 [1000,1100] 内：区间矩形中心在视口外，
    // 不 keepTextVisible 时文字画到视口外 → 中部无墨；keep 时文字收进可见段。
    // #131/#151：旧阈值 ink > 20 依赖 CJK 字体——CI runner 无 CJK 字体时
    // 「河口坝」渲染成缺字框，墨点恰好 = 20（本机 DejaVu-only fontconfig 复现），
    // 断言随字体环境翻转。改为相对判别：keep 必须比不 keep 多出明显墨迹，
    // 与字体/字形无关。
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
          if (image.pixelColor(x, y).lightness() < 120)
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
    QTRY_VERIFY2(run->isEnabled(), qPrintable(run->toolTip()));
    QVERIFY(panel.isReferenceWell());
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
    workflow->setData(well());
    QTRY_COMPARE(count(), 1);
    QCOMPARE(server.predictCount, 1); // 精确输入命中缓存
    QVERIFY(panel.loadWellData(well()));
    QVERIFY(!panel.isReferenceWell());
    workflow->setData(well());
    QTRY_VERIFY(run->isEnabled());
    auto *show = panel.findChild<QToolButton *>("btnShowPredictedFacies");
    show->click();
    for (const auto &t : panel.canvas()->tracks())
      if (t->title() == QStringLiteral("预测相"))
        QVERIFY(!t->isVisible());
    QVERIFY(panel.loadLasCurves("another", well().continuousCurves,
                                well().formationIntervals));
    QVERIFY(!run->isEnabled());
    QVERIFY(run->toolTip().contains(QStringLiteral("岩性")));
    QCOMPARE(count(), 0);
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
      QCOMPARE(run->isEnabled(), input.ready());
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
