// 层：测试壳
#include "services/pythondelivery.h"
#include "services/pythondelivery_files.h"
#include "services/pythonruntime.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

using namespace paleo::python;
namespace {
QString sha(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'; }
QByteArray fileBytes(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
struct Package {
  QTemporaryDir temporary;
  QByteArray manifest, payload;
  QJsonObject header;
  QString executable;
  Package(const QByteArray &code = "not-an-executable; installer must never run it\n") {
#ifdef Q_OS_WIN
    executable = "python.exe";
#else
    executable = "bin/python";
#endif
    const QJsonObject row{{"path", executable}, {"sha256", sha(code)}, {"size", code.size()}, {"executable", true}};
    manifest = json({{"schema", QLatin1String(RuntimeSchema)}, {"profile", QLatin1String(ProfileId)},
      {"runtimeVersion", "cpython-3.14.10-numpy-2.5.4-1"}, {"pythonVersion", QLatin1String(PythonVersion)},
      {"numpyVersion", QLatin1String(NumpyVersion)}, {"sourceLockSha256", runtimeSourceLockSha256()},
      {"platform", runtimePlatform()}, {"interpreter", executable}, {"treeSize", code.size()}, {"files", QJsonArray{row}}});
    const QJsonObject manifestRow{{"path", "runtime-manifest.json"}, {"sha256", sha(manifest)},
                                  {"size", manifest.size()}, {"executable", false}};
    const bool first = executable.toUtf8() < QByteArray("runtime-manifest.json");
    header = {{"schema", "paleo.python-delivery.v1"}, {"kind", "runtime"}, {"platform", runtimePlatform()},
      {"manifestSha256", sha(manifest)}, {"licenseStatus", "runtime-licenses-included"},
      {"entries", first ? QJsonArray{row, manifestRow} : QJsonArray{manifestRow, row}}};
    payload = first ? code + manifest : manifest + code;
  }
  QByteArray bytes() const {
    const auto raw = json(header); QByteArray output("PALEOPY1\n");
    char length[4]; qToLittleEndian<quint32>(quint32(raw.size()), reinterpret_cast<uchar *>(length));
    output.append(length, 4); output += raw; output += payload; return output;
  }
  DeliveryRequest request() const {
    const auto raw = bytes(); const auto path = temporary.path() + "/package.paleopy";
    QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(raw) != raw.size()) return {};
    return {path, sha(raw), temporary.path() + "/store"};
  }
  QString selection(const DeliveryRequest &request) const {
    return request.storeRoot + "/current-" + runtimePlatform() + ".json";
  }
  void scripts(const QByteArray &noticeText = "Internal test notice; no redistribution grant.\n") {
    QMap<QString, QByteArray> files;
    QJsonArray scripts;
    const QString source = QString::fromUtf8(PALEO_DELIVERY_SOURCE_ROOT);
    for (const auto &id : {QString("attribute_correlation"), QString("summary"), QString("well_quality")}) {
      const auto path = id + ".py";
      const auto raw = fileBytes(source + "/vendor/agent-python/templates/" + path);
      files.insert(path, raw);
      scripts.append(QJsonObject{{"id", id}, {"path", path}, {"source", "vendor/agent-python/templates/" + path},
                                  {"size", raw.size()}, {"sha256", sha(raw)}});
    }
    files.insert("NOTICE.md", noticeText);
    const auto notice = files.value("NOTICE.md");
    qint64 total = 0; for (const auto &file : files) total += file.size();
    manifest = json({{"schema", "paleo.python-scripts.v1"}, {"version", 1}, {"profile", QLatin1String(ProfileId)},
      {"pythonVersion", QLatin1String(PythonVersion)}, {"numpyVersion", QLatin1String(NumpyVersion)},
      {"runtimeVersion", "cpython-3.14.10-numpy-2.5.4-1"}, {"sourceRuntimeLockSha256", runtimeSourceLockSha256()},
      {"scripts", scripts}, {"licenses", QJsonArray{QJsonObject{{"path", "NOTICE.md"}, {"source", "NOTICE.md"},
        {"kind", "notice"}, {"size", notice.size()}, {"sha256", sha(notice)}}}},
      {"licenseStatus", "notice-only"}, {"treeSize", total}});
    files.insert("scripts-manifest.json", manifest);
    QJsonArray entries; payload.clear();
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
      entries.append(QJsonObject{{"path", it.key()}, {"sha256", sha(it.value())}, {"size", it.value().size()}, {"executable", false}});
      payload += it.value();
    }
    header = {{"schema", "paleo.python-delivery.v1"}, {"kind", "scripts"}, {"platform", "all"},
              {"manifestSha256", sha(manifest)}, {"licenseStatus", "notice-only"}, {"entries", entries}};
  }
};
}
class TestPythonDelivery : public QObject {
  Q_OBJECT
private slots:
  void installsWithoutStartingInterpreterAndRetainsGeneration() {
    Package package; const auto request = package.request();
    const auto first = installDelivery(request); QVERIFY2(first.ready(), qPrintable(first.code));
    QCOMPARE(first.manifestSha256, sha(package.manifest)); QVERIFY(!first.alreadyInstalled);
    QCOMPARE(fileBytes(first.root + "/runtime-manifest.json"), package.manifest);
    QVERIFY(verifyRuntime({first.root, first.manifestSha256, true}).ready());
    const auto second = installDelivery(request); QVERIFY2(second.ready(), qPrintable(second.code));
    QVERIFY(second.alreadyInstalled); QCOMPARE(second.root, first.root);
    QCOMPARE(QDir(request.storeRoot + "/generations").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
    const auto selection = QJsonDocument::fromJson(fileBytes(package.selection(request))).object();
    QCOMPARE(selection.value("generation").toString(), "generations/" + first.manifestSha256);
    QVERIFY(!selection.contains("enabled"));
  }
  void changedPackageCannotReplaceSelection() {
    Package package; auto request = package.request(); QVERIFY(installDelivery(request).ready());
    const auto original = fileBytes(package.selection(request));
    request.packageSha256 = QString(64, 'a');
    QCOMPARE(installDelivery(request).code, QString("python_delivery_anchor_mismatch"));
    QCOMPARE(fileBytes(package.selection(request)), original);
    QVERIFY(!QDir(request.storeRoot + "/generations").entryList(QDir::Dirs | QDir::Hidden).filter(".stage-").size());
  }
  void rejectsUnsafeAndDuplicateEntries_data() {
    QTest::addColumn<QString>("name");
    for (const auto &name : {QString("../outside"), QString("/absolute"), QString("CON.txt"),
                            QString("bad:stream"), QString("tail."), QString("a\\b"), QString("runtime-manifest.json")})
      QTest::newRow(qPrintable(name)) << name;
  }
  void rejectsUnsafeAndDuplicateEntries() {
    QFETCH(QString, name); Package package;
    auto entries = package.header.value("entries").toArray(); auto row = entries[0].toObject();
    row.insert("path", name); entries[0] = row; package.header.insert("entries", entries);
    const auto request = package.request(); QVERIFY(!installDelivery(request).ready());
    QVERIFY(!QFileInfo::exists(package.selection(request)));
  }
  void rejectsTrailingTruncatedAndOversizedPackages() {
    Package package; auto request = package.request();
    QFile file(request.packagePath); QVERIFY(file.open(QIODevice::Append)); file.write("trailing"); file.close();
    request.packageSha256 = sha(fileBytes(request.packagePath)); QVERIFY(!installDelivery(request).ready());
    request = package.request(); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.resize(file.size() - 1)); file.close();
    request.packageSha256 = sha(fileBytes(request.packagePath)); QVERIFY(!installDelivery(request).ready());
    auto entries = package.header.value("entries").toArray(); auto row = entries[0].toObject();
    row.insert("size", double(2LL * 1024 * 1024 * 1024)); entries[0] = row;
    package.header.insert("entries", entries); QVERIFY(!installDelivery(package.request()).ready());
  }
  void cancellationAndConcurrentPreparationPreserveSelection() {
    Package package; const auto request = package.request(); QVERIFY(installDelivery(request).ready());
    const auto original = fileBytes(package.selection(request));
    QCOMPARE(installDelivery(request, [] { return true; }).code, QString("cancelled"));
    { delivery::StoreLock lock(request.storeRoot + "/.prepare.lock"); QVERIFY(lock.locked());
      QCOMPARE(installDelivery(request).code, QString("python_delivery_store_busy")); }
    QVERIFY(installDelivery(request).ready()); QCOMPARE(fileBytes(package.selection(request)), original);
  }
  void cancellationDuringPayloadCleansPrivateStage() {
    Package package; const auto request = package.request(); QVERIFY(installDelivery(request).ready());
    const auto original = fileBytes(package.selection(request));
    int checks = 0;
    QCOMPARE(installDelivery(request, [&] { return ++checks >= 3; }).code, QString("cancelled"));
    QVERIFY(checks >= 3); QCOMPARE(fileBytes(package.selection(request)), original);
    const auto directories = QDir(request.storeRoot + "/generations").entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(directories.filter(".stage-").isEmpty());
  }
  void newGenerationDoesNotReplaceInUseContent() {
    Package oldPackage, nextPackage("a separately approved interpreter generation\n");
    const auto oldRequest = oldPackage.request(); const auto old = installDelivery(oldRequest);
    QVERIFY2(old.ready(), qPrintable(old.code));
    RuntimeProvider provider({old.root, old.manifestSha256, true});
    const auto held = provider.inspectIntegrity(); QVERIFY(held.ready());
    auto request = nextPackage.request(); request.storeRoot = oldRequest.storeRoot;
    const auto next = installDelivery(request); QVERIFY2(next.ready(), qPrintable(next.code));
    QVERIFY(next.root != old.root); QCOMPARE(fileBytes(old.root + "/runtime-manifest.json"), oldPackage.manifest);
    QCOMPARE(held.runtime->manifestSha256, old.manifestSha256);
    QCOMPARE(QJsonDocument::fromJson(fileBytes(oldPackage.selection(oldRequest))).object().value("manifestSha256").toString(),
             next.manifestSha256);
  }
  void scriptsRemainInternalAndDoNotEnableExecution() {
    Package package; package.scripts(); const auto request = package.request();
    const auto result = installDelivery(request); QVERIFY2(result.ready(), qPrintable(result.code));
    QCOMPARE(result.kind, QString("scripts")); QCOMPARE(result.licenseStatus, QString("notice-only"));
    const auto selection = QJsonDocument::fromJson(fileBytes(request.storeRoot + "/current.json")).object();
    QCOMPARE(selection.value("schema").toString(), QString("paleo.python-scripts.selection.v1"));
    QVERIFY(!selection.contains("enabled"));
    QCOMPARE(fileBytes(result.root + "/scripts-manifest.json"), package.manifest);
    QVERIFY(installDelivery(request).ready());
  }
  void unknownSelectionIsNotOverwritten() {
    Package package; const auto request = package.request(); QVERIFY(installDelivery(request).ready());
    QFile selection(package.selection(request)); QVERIFY(selection.open(QIODevice::WriteOnly));
    const QByteArray unknown("{\"schema\":\"unknown\"}\n"); selection.write(unknown); selection.close();
    QCOMPARE(installDelivery(request).code, QString("python_delivery_store_invalid"));
    QCOMPARE(fileBytes(package.selection(request)), unknown);
  }
  void malformedNoticeDoesNotBecomeAnInstalledGeneration_data() {
    QTest::addColumn<QByteArray>("notice");
    QTest::newRow("invalid-utf8") << QByteArray("bad\xff\n");
    QTest::newRow("nul") << QByteArray("bad\0notice\n", 11);
    QTest::newRow("crlf") << QByteArray("bad\r\n");
  }
  void malformedNoticeDoesNotBecomeAnInstalledGeneration() {
    QFETCH(QByteArray, notice); Package package; package.scripts(notice);
    const auto request = package.request();
    QCOMPARE(installDelivery(request).code, QString("python_scripts_notice_invalid"));
    QVERIFY(!QFileInfo::exists(request.storeRoot + "/current.json"));
  }
  void refusesLinkedPackage() {
    Package package; auto request = package.request();
#ifdef Q_OS_WIN
    const auto link = package.temporary.path() + "/linked-package.lnk";
#else
    const auto link = package.temporary.path() + "/linked-package";
#endif
    if (!QFile::link(request.packagePath, link)) QSKIP("Filesystem cannot create a link");
    QVERIFY(QFileInfo::exists(link));
    request.packagePath = link; QVERIFY(!installDelivery(request).ready());
  }
  void unrelatedDirectoryRemainsUntouched() {
    Package package; const auto request = package.request(); QVERIFY(QDir().mkpath(request.storeRoot));
    QFile unrelated(request.storeRoot + "/user-document.txt"); QVERIFY(unrelated.open(QIODevice::WriteOnly));
    unrelated.write("preserve existing user data\n"); unrelated.close();
    QCOMPARE(installDelivery(request).code, QString("python_delivery_store_invalid"));
    QCOMPARE(fileBytes(unrelated.fileName()), QByteArray("preserve existing user data\n"));
    QCOMPARE(QDir(request.storeRoot).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList{"user-document.txt"});
  }
  void runtimeAndScriptsStoresCannotBeMixed() {
    Package runtime, scripts; const auto request = runtime.request(); QVERIFY(installDelivery(request).ready());
    const auto selection = fileBytes(runtime.selection(request));
    scripts.scripts(); auto scriptRequest = scripts.request(); scriptRequest.storeRoot = request.storeRoot;
    QCOMPARE(installDelivery(scriptRequest).code, QString("python_delivery_store_invalid"));
    QCOMPARE(fileBytes(runtime.selection(request)), selection); QVERIFY(!QFileInfo::exists(request.storeRoot + "/.scripts.lock"));
  }
};
QTEST_GUILESS_MAIN(TestPythonDelivery)
#include "tst_pythondelivery.moc"
