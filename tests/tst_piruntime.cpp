// 层：测试壳
#include "services/piruntime.h"
#include "services/piprocess.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <future>
#include <type_traits>

using namespace paleo::pi;
namespace {
RuntimeInspection worker(RuntimeConfig config,std::function<bool()> cancelled={}) {
  return std::async(std::launch::async,[config,cancelled]{return inspectRuntime(config,cancelled);}).get();
}
}
class TestPiRuntime:public QObject {
  Q_OBJECT
private slots:
  void ordinaryBuildShipsItsPinnedRuntime() {
    const auto config = bundledRuntime();
    QVERIFY(config.enabled);
    QCOMPARE(QFileInfo(config.root).fileName(), config.manifestSha256);
    const auto verified = worker(config);
    QVERIFY2(verified.ok(), qPrintable(verified.code + QLatin1Char(' ') + config.root));
  }
  void disabledDoesNotRequireNode() {
    static_assert(!std::is_default_constructible_v<VerifiedRuntime>);
    const auto inspected=inspectRuntime({});QCOMPARE(inspected.code,QString("pi_disabled"));QVERIFY(!inspected.runtime);
    QCOMPARE(inspected.status.value("executionReady").toBool(),false);QCOMPARE(inspected.status.value("isSandbox").toBool(),false);
    const auto handshake=runHandshake({});QCOMPARE(handshake.code,QString("pi_disabled"));QVERIFY(handshake.peer.isEmpty());
  }
  void enabledInspectionCannotBlockOwnerThread() {
    RuntimeConfig config;config.enabled=true;QCOMPARE(inspectRuntime(config).code,QString("pi_worker_required"));
  }
  void hostAnchorIsRequiredBeforeReadingFiles() {
    QTemporaryDir directory;QVERIFY(directory.isValid());RuntimeConfig config{true,directory.path(),{}};
    const auto inspected=worker(config);QCOMPARE(inspected.code,QString("pi_manifest_anchor_required"));QVERIFY(!inspected.runtime);
    config.manifestSha256=QString(64,'0');QCOMPARE(worker(config).code,QString("pi_manifest_anchor_required"));
  }
  void selfReportedMetadataDoesNotAuthorizeRuntime() {
    QTemporaryDir directory;QVERIFY(directory.isValid());QFile manifest(directory.filePath("pi-manifest.json"));
    const QByteArray raw="{}\n";QVERIFY(manifest.open(QIODevice::WriteOnly));QCOMPARE(manifest.write(raw),raw.size());manifest.close();
    RuntimeConfig config{true,directory.path(),QString(64,'a')};
    const auto wrong=worker(config);QCOMPARE(wrong.code,QString("pi_manifest_hash_mismatch"));QVERIFY(!wrong.runtime);
    config.manifestSha256=QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex());
    const auto incompatible=worker(config);QCOMPARE(incompatible.code,QString("pi_manifest_incompatible"));QVERIFY(!incompatible.runtime);
    QVERIFY(!incompatible.status.value("integrityVerified").toBool());QVERIFY(!incompatible.status.value("executionReady").toBool());
  }
  void cancellationNeverReturnsAnIdentity() {
    RuntimeConfig config;config.enabled=true;const auto result=worker(config,[]{return true;});
    QCOMPARE(result.code,QString("cancelled"));QVERIFY(!result.runtime);
  }
};
QTEST_GUILESS_MAIN(TestPiRuntime)
#include "tst_piruntime.moc"
