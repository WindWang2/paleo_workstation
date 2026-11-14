#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <memory>
#include "services/seismicattributefusionclient.h"

using namespace paleo;
using Client = SeismicAttributeFusionClient;
namespace {
bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray codes(const QVector<int> &values) {
  QByteArray bytes(values.size() * 4, Qt::Uninitialized);
  for (qsizetype i = 0; i < values.size(); ++i) qToLittleEndian<qint32>(values[i], bytes.data() + i * 4);
  return bytes;
}
QJsonObject resultObject(const QString &output) {
  write(output + "/grid.bin", codes({1, 2, -9999, 3}));
  write(output + "/grid.tif", "fixture presence only");
  return {{"status", "succeeded"}, {"output_dir", output}, {"unit_name", "T70-T72/S2"}, {"nodata", -9999},
    {"grid", QJsonObject{{"columns", 2}, {"rows", 2}, {"dx", 50}, {"dy", 50},
      {"extent_left", 100}, {"extent_bottom", 200}, {"extent_right", 200}, {"extent_top", 300},
      {"width", 100}, {"height", 100}, {"gdal_geotransform", QJsonArray{100, 50, 0, 300, 0, -50}},
      {"binary_row_order", "north_to_south"}, {"axis_order", "east_north_xy"}, {"coordinate_unit", "m"},
      {"crs_auth", "EPSG:32650"}, {"crs_wkt", "fixture-wkt"}}},
    {"classification", QJsonObject{{"n_clusters", 3}, {"valid_cells", 3},
      {"selected_attributes", QJsonArray{"A4", "A1"}}}},
    {"output_files", QJsonObject{{"geotiff_north_up", "grid.tif"}, {"int32_north_up_bin", "grid.bin"}}}};
}
SeismicFusionRequest requestFor(const QString &dir) {
  write(dir + "/input.sgy", "seismic input fixture");
  write(dir + "/horizon.dat", "horizon input fixture");
  SeismicFusionRequest request;
  request.seismicPath = dir + "/input.sgy";
  request.horizonPath = dir + "/horizon.dat";
  request.serverUrl.clear();
  request.timeoutMs = 5000;
  request.outputDir = dir;
  return request;
}
struct Env {
  const char *name;
  QByteArray previous;
  bool existed;
  explicit Env(const char *key) : name(key), previous(qgetenv(key)), existed(qEnvironmentVariableIsSet(key)) { qunsetenv(key); }
  ~Env() { if (existed) qputenv(name, previous); else qunsetenv(name); }
};
struct Packet {
  int status = 200;
  QByteArray body;
  Packet(QJsonObject object) : body(QJsonDocument(object).toJson(QJsonDocument::Compact)) {}
  Packet(int code, QByteArray bytes) : status(code), body(std::move(bytes)) {}
};
class Server {
public:
  QTcpServer server;
  QList<QByteArray> paths;
  std::function<Packet(const QByteArray &, const QJsonObject &)> respond;
  Server() {
    server.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled] {
          *buffer += socket->readAll();
          const auto end = buffer->indexOf("\r\n\r\n");
          if (end < 0 || *handled) return;
          qsizetype length = 0;
          for (const auto &line : buffer->left(end).split('\n')) {
            if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
          }
          if (buffer->size() < end + 4 + length) return;
          *handled = true;
          const auto path = buffer->split(' ').value(1);
          paths.append(path);
          const auto body = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
          const auto reply = respond(path, body);
          if (!reply.status) return; // deliberate stalled connection
          socket->write("HTTP/1.1 " + QByteArray::number(reply.status) + " test\r\nContent-Type: application/json\r\nContent-Length: " +
                        QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body);
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
  static QJsonObject healthy() { return {{"status", "ok"}, {"service", "seismic-attribute-fusion"}, {"api_version", "1.0"}}; }
};
} // namespace

class TestSeismicFusion : public QObject {
  Q_OBJECT
private slots:
  void requestJson();
  void resultCoordinates();
  void rejectsResult_data();
  void rejectsResult();
  void http_data();
  void http();
  void healthAndConfiguration();
  void validation();
  void cli_data();
  void cli();
  void cliCancellation();
  void cliConfigurationErrors();
};

void TestSeismicFusion::requestJson()
{
  SeismicFusionRequest request;
  request.seismicPath = QStringLiteral("relative input.sgy");
  request.topHorizonPath = QStringLiteral("top.dat");
  request.bottomHorizonPath = QStringLiteral("bottom.dat");
  request.bbox = QRectF(100, 200, 300, 400);
  request.fractionTop = 0.2;
  request.fractionBottom = 0.7;
  request.targetCrs = QStringLiteral("EPSG:32650");
  const auto object = Client::buildRequestJson(request);
  QVERIFY(QFileInfo(object.value("seismic_path").toString()).isAbsolute());
  QCOMPARE(object.value("bbox").toArray(), QJsonArray({100, 200, 400, 600}));
  QCOMPARE(object.value("sub_interval_index").toInt(-1), 0);
  QCOMPARE(object.value("fraction_top").toDouble(), 0.2);
  QCOMPARE(object.value("fraction_bottom").toDouble(), 0.7);
  QCOMPARE(object.value("target_crs").toString(), request.targetCrs);
  QVERIFY(!object.contains("wells_path"));
}

void TestSeismicFusion::resultCoordinates()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto object = resultObject(dir.path());
  for (const auto &root : {object, QJsonObject{{"state", "succeeded"}, {"result", object}}}) {
    const auto parsed = Client::parseResultJson(root);
    QVERIFY2(parsed.ok, qPrintable(parsed.error));
    QCOMPARE(parsed.extent, QRectF(100, 200, 100, 100));
    QCOMPARE(parsed.gdalGeoTransform, QVector<double>({100, 50, 0, 300, 0, -50}));
    QCOMPARE(parsed.crsAuth, QStringLiteral("EPSG:32650"));
    QCOMPARE(parsed.crsWkt, QStringLiteral("fixture-wkt"));
    QCOMPARE(parsed.cellsNorthUp, QVector<int>({1, 2, -9999, 3}));
    QCOMPARE(parsed.validCells, 3);
    QCOMPARE(parsed.selectedAttributes, QStringList({"A4", "A1"}));
  }
  auto noDirectory = object;
  noDirectory.remove("output_dir");
  QVERIFY(Client::parseResultJson(noDirectory, dir.path()).ok);
}

void TestSeismicFusion::rejectsResult_data()
{
  QTest::addColumn<QString>("mode");
  for (const auto *mode : {"missing-bin", "short-bin", "long-bin", "bad-code", "bad-count", "bad-nodata",
       "bad-width", "bad-transform", "south-up", "wrong-axes", "wrong-unit", "overflow", "fractional-size",
       "budget", "failed", "null-result", "missing-tiff", "nan-spacing", "missing-grid", "bad-clusters"})
    QTest::newRow(mode) << QString::fromLatin1(mode);
}

void TestSeismicFusion::rejectsResult()
{
  QFETCH(QString, mode);
  QTemporaryDir dir;
  auto object = resultObject(dir.path());
  auto grid = object.value("grid").toObject();
  auto classification = object.value("classification").toObject();
  qint64 budget = 100;
  if (mode == "missing-bin") QFile::remove(dir.path() + "/grid.bin");
  if (mode == "missing-tiff") QFile::remove(dir.path() + "/grid.tif");
  if (mode == "short-bin") write(dir.path() + "/grid.bin", codes({1}));
  if (mode == "long-bin") write(dir.path() + "/grid.bin", codes({1, 2, -9999, 3, 1}));
  if (mode == "bad-code") write(dir.path() + "/grid.bin", codes({1, 0, -9999, 3}));
  if (mode == "bad-count") classification.insert("valid_cells", 4);
  if (mode == "bad-clusters") classification.insert("n_clusters", 1);
  if (mode == "bad-nodata") object.insert("nodata", 0);
  if (mode == "bad-width") grid.insert("width", 200);
  if (mode == "bad-transform") grid.insert("gdal_geotransform", QJsonArray{100, 50, 0, 300, 0, 50});
  if (mode == "south-up") grid.insert("binary_row_order", "south_to_north");
  if (mode == "wrong-axes") grid.insert("axis_order", "north_east_yx");
  if (mode == "wrong-unit") grid.insert("coordinate_unit", "ft");
  if (mode == "overflow") { grid.insert("columns", 2147483647); grid.insert("rows", 2147483647); }
  if (mode == "fractional-size") grid.insert("columns", 2.5);
  if (mode == "nan-spacing") grid.insert("dx", QStringLiteral("nan"));
  if (mode == "budget") budget = 3;
  object.insert("grid", grid);
  object.insert("classification", classification);
  if (mode == "failed") object.insert("state", "failed");
  if (mode == "null-result") object.insert("result", QJsonValue());
  if (mode == "missing-grid") object.remove("grid");
  const auto parsed = Client::parseResultJson(object, {}, budget);
  QVERIFY(!parsed.ok);
  QVERIFY(!parsed.error.isEmpty());
  QVERIFY(parsed.cellsNorthUp.isEmpty());
}

void TestSeismicFusion::http_data()
{
  QTest::addColumn<QString>("mode");
  for (const auto *mode : {"success", "cancel", "timeout", "stalled-poll", "stalled-submit", "stalled-health",
       "cancel-health", "cancel-submit", "cancel-poll", "failed", "cancelled", "unknown-state",
       "malformed-json", "http-error", "unsafe-id", "oversize"})
    QTest::newRow(mode) << QString::fromLatin1(mode);
}

void TestSeismicFusion::http()
{
  QFETCH(QString, mode);
  QTemporaryDir dir;
  const auto result = resultObject(dir.path());
  auto request = requestFor(dir.path());
  Server server;
  QVERIFY(server.server.isListening());
  request.serverUrl = server.url();
  request.scriptPath = QStringLiteral("must-not-fallback.py");
  if (mode.startsWith("stalled") || mode == "timeout") request.timeoutMs = 350;
  int polls = 0;
  bool cancelled = false;
  QJsonObject submitted;
  server.respond = [&](const QByteArray &path, const QJsonObject &body) -> Packet {
    if (path == "/health") {
      if (mode == "cancel-health") QTimer::singleShot(30, &server.server, [&] { cancelled = true; });
      return mode == "stalled-health" || mode == "cancel-health" ? Packet(0, {}) : Packet(Server::healthy());
    }
    if (path == "/api/v1/jobs") {
      submitted = body;
      if (mode == "cancel-submit") QTimer::singleShot(30, &server.server, [&] { cancelled = true; });
      if (mode == "stalled-submit" || mode == "cancel-submit") return {0, {}};
      return QJsonObject{{"job_id", mode == "unsafe-id" ? "../../oops" : "test-job"}};
    }
    if (path.endsWith("/cancel")) return QJsonObject{{"state", "cancelled"}};
    ++polls;
    if (mode == "cancel-poll") QTimer::singleShot(30, &server.server, [&] { cancelled = true; });
    if (mode == "stalled-poll" || mode == "cancel-poll") return {0, {}};
    if (mode == "malformed-json") return {200, "[]"};
    if (mode == "oversize") return {200, QByteArray(8 * 1024 * 1024 + 1, 'x')};
    if (mode == "http-error") return {422, "{\"detail\":\"invalid input\"}"};
    if (mode == "failed" || mode == "cancelled")
      return QJsonObject{{"state", mode}, {"percent", 25}, {"error", "fixture failure"}};
    if (mode == "unknown-state") return QJsonObject{{"state", "surprise"}, {"percent", 25}};
    if (mode != "success" || polls < 3)
      return QJsonObject{{"state", "running"}, {"percent", 20}, {"detail", QString::number(polls)}};
    return QJsonObject{{"state", "succeeded"}, {"percent", 100}, {"result", result}};
  };
  QList<QString> details;
  QElapsedTimer clock;
  clock.start();
  const auto parsed = Client::execute(request, [&](int, const QString &detail) {
    details.append(detail);
    if (mode == "cancel") cancelled = true;
  }, [&] { return cancelled; });
  QVERIFY(clock.elapsed() < 4000);
  QCOMPARE(server.paths.count("/api/v1/jobs"), mode == "stalled-health" || mode == "cancel-health" ? 0 : 1);
  if (mode == "success") {
    QVERIFY2(parsed.ok, qPrintable(parsed.error));
    QCOMPARE(parsed.cellsNorthUp, QVector<int>({1, 2, -9999, 3}));
    QVERIFY(details.contains(QStringLiteral("1")) && details.contains(QStringLiteral("2")));
    QCOMPARE(submitted.value("wait").toBool(true), false);
  } else {
    QVERIFY(!parsed.ok);
    QVERIFY2(!parsed.error.isEmpty(), qPrintable(mode));
    QVERIFY(!parsed.error.contains("must-not-fallback"));
  }
  const bool cancelSent = mode == "cancel" || mode == "cancel-poll" || mode == "timeout" || mode == "stalled-poll" ||
      mode == "unknown-state" || mode == "malformed-json" || mode == "http-error" || mode == "oversize";
  QCOMPARE(server.paths.count("/api/v1/jobs/test-job/cancel"), cancelSent ? 1 : 0);
  if (mode == "http-error") QVERIFY(parsed.error.contains("invalid input"));
}

void TestSeismicFusion::healthAndConfiguration()
{
  Env script("PALEO_FUSION_SCRIPT"), python("PALEO_FUSION_PYTHON");
  QVERIFY(Client::defaultServiceScript().isEmpty());
  qputenv("PALEO_FUSION_SCRIPT", "explicit-missing-script.py");
  QCOMPARE(Client::defaultServiceScript(), QStringLiteral("explicit-missing-script.py"));
  qputenv("PALEO_FUSION_PYTHON", "explicit-missing-python");
  QCOMPARE(Client::defaultPythonExecutable(), QStringLiteral("explicit-missing-python"));
  Server server;
  server.respond = [](const QByteArray &, const QJsonObject &) -> Packet { return Server::healthy(); };
  QVERIFY(Client::isServerAlive(server.url(), 500));
  server.respond = [](const QByteArray &, const QJsonObject &) -> Packet { return QJsonObject{{"status", "ok"}}; };
  QVERIFY(!Client::isServerAlive(server.url(), 500));
  QVERIFY(!Client::isServerAlive("http://example.com", 10));
  QVERIFY(!Client::isServerAlive(server.url(), 0));
}

void TestSeismicFusion::validation()
{
  Env env("PALEO_FUSION_SCRIPT");
  QTemporaryDir dir;
  auto request = requestFor(dir.path());
  Server server;
  server.respond = [](const QByteArray &, const QJsonObject &) -> Packet { return Server::healthy(); };
  request.serverUrl = server.url();
  const auto invalid = [&](const SeismicFusionRequest &r) {
    const auto result = Client::execute(r);
    return !result.ok && !result.error.isEmpty() && server.paths.isEmpty();
  };
  auto r = request;
  r.serverUrl = "https://example.com";
  QVERIFY(invalid(r));
  r = request; r.clusters = 33; QVERIFY(invalid(r));
  r = request; r.fractionTop = 0.2; QVERIFY(invalid(r));
  r = request; r.gridSpacingX = std::nan(""); QVERIFY(invalid(r));
  r = request; r.bbox = QRectF(0, 0, -1, 2); QVERIFY(invalid(r));
  r = request; r.timeoutMs = 0; QVERIFY(invalid(r));
  r = request; r.seismicPath += "missing"; QVERIFY(invalid(r));
  r = request; r.horizonPath.clear(); QVERIFY(invalid(r));
  request.serverUrl.clear();
  const auto unconfigured = Client::execute(request);
  QVERIFY(!unconfigured.ok);
  QVERIFY(unconfigured.error.contains("PALEO_FUSION_SCRIPT"));
  bool ranProgress = false;
  const auto cancelled = Client::execute(request, [&](int, const QString &) { ranProgress = true; }, [] { return true; });
  QVERIFY(!cancelled.ok);
  QVERIFY(cancelled.error.contains("cancel", Qt::CaseInsensitive));
  QVERIFY(!ranProgress);
}

void TestSeismicFusion::cli_data()
{
  QTest::addColumn<QString>("mode");
  for (const auto *mode : {"success", "noise", "fallback", "auto-output", "failure", "bad-json", "missing-result", "nonzero-success", "sleep"})
    QTest::newRow(mode) << QString::fromLatin1(mode);
}

void TestSeismicFusion::cli()
{
  QFETCH(QString, mode);
  QTemporaryDir dir(QDir::tempPath() + "/fusion test 中文 XXXXXX");
  QVERIFY(dir.isValid());
  auto request = requestFor(dir.path());
  request.unitName = mode;
  request.scriptPath = QFINDTESTDATA("fixtures/scripts/seismicfusion.py");
  QVERIFY(!request.scriptPath.isEmpty());
  request.pythonExe = QString::fromUtf8(PALEO_FUSION_TEST_PYTHON);
  Server server;
  server.respond = [](const QByteArray &, const QJsonObject &) -> Packet { return QJsonObject{{"status", "wrong-service"}}; };
  if (mode == "fallback") request.serverUrl = server.url();
  if (mode == "auto-output") request.outputDir.clear();
  if (mode == "sleep") request.timeoutMs = 500;
  QList<int> progress;
  const auto parsed = Client::execute(request, [&](int percent, const QString &) { progress.append(percent); });
  if (mode == "success" || mode == "noise" || mode == "fallback" || mode == "auto-output") {
    QVERIFY2(parsed.ok, qPrintable(parsed.error));
    QCOMPARE(parsed.cellsNorthUp, QVector<int>({1, 2, -9999, 3}));
    QVERIFY(progress.contains(20));
    QCOMPARE(progress.last(), 100);
    QVERIFY(!progress.contains(999));
    if (mode == "auto-output") {
      QVERIFY(parsed.outputDir.startsWith(QDir::tempPath()));
      QVERIFY(parsed.outputDir != dir.path());
      QVERIFY(QDir(parsed.outputDir).removeRecursively());
    }
  } else {
    QVERIFY2(!parsed.ok, qPrintable(mode));
    QVERIFY(!parsed.error.isEmpty());
    if (mode == "failure") QCOMPARE(parsed.error, QStringLiteral("fixture failure"));
    if (mode == "sleep") QVERIFY(parsed.error.contains("timed out"));
  }
  QVERIFY(QDir(dir.path()).entryList({"request*.json", "result*.json"}, QDir::Files).isEmpty());
  QCOMPARE(server.paths.count("/api/v1/jobs"), 0);
}

void TestSeismicFusion::cliCancellation()
{
  QTemporaryDir dir;
  auto request = requestFor(dir.path());
  request.scriptPath = QFINDTESTDATA("fixtures/scripts/seismicfusion.py");
  request.pythonExe = QString::fromUtf8(PALEO_FUSION_TEST_PYTHON);
  request.unitName = "sleep";
  bool cancel = false;
  QElapsedTimer clock;
  clock.start();
  const auto result = Client::execute(request, [&](int, const QString &) { cancel = true; }, [&] { return cancel; });
  QVERIFY(!result.ok);
  QVERIFY(result.error.contains("cancel", Qt::CaseInsensitive));
  QVERIFY(clock.elapsed() < 3000);
  QVERIFY(!QFileInfo::exists(dir.path() + "/grid.bin"));
}

void TestSeismicFusion::cliConfigurationErrors()
{
  QTemporaryDir dir;
  auto request = requestFor(dir.path());
  request.scriptPath = dir.path() + "/missing.py";
  request.pythonExe = QString::fromUtf8(PALEO_FUSION_TEST_PYTHON);
  QVERIFY(Client::execute(request).error.contains("script"));
  request.scriptPath = QFINDTESTDATA("fixtures/scripts/seismicfusion.py");
  request.pythonExe = dir.path() + "/missing-python";
  QVERIFY(Client::execute(request).error.contains("Python"));
  request.pythonExe = QString::fromUtf8(PALEO_FUSION_TEST_PYTHON);
  request.outputDir = request.seismicPath; // existing input file cannot become an output directory
  QVERIFY(Client::execute(request).error.contains("output directory"));
  QFile file(request.seismicPath);
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(file.readAll(), QByteArray("seismic input fixture"));
}

QTEST_GUILESS_MAIN(TestSeismicFusion)
#include "tst_seismicattributefusion.moc"
