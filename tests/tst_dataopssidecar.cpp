// 任务 A（数据/资产/图谱深化 2026-10-13）：dataops sidecar 读写加固回归。
// recycle_bin/asset_tags 等四个 .paleo/*.json 此前是非原子直写 + 损坏静默
// 回空——断电半截文件 = 重开静默全丢。本套件钉死原子写与损坏告警契约。
#include <QtTest>
#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include "../src/catalog/datacatalog.h"
#include "../src/ui/pages/dataops/dataopsmodel.h"

using namespace paleo::dataops;

class TestDataOpsSidecar : public QObject {
  Q_OBJECT
private slots:
  void missingSidecarIsNormalEmptyState();
  void corruptSidecarWarnsAndLoadsEmpty();
  void saveRoundTripsWithoutTempLeftovers();
  void failedSaveKeepsPreviousContentIntact();
};

void TestDataOpsSidecar::missingSidecarIsNormalEmptyState() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const auto loaded = loadSidecarObject(&cat, QStringLiteral("asset_tags.json"));
  QVERIFY(loaded.isEmpty()); // 无文件 = 初始态，不是错误
}

void TestDataOpsSidecar::corruptSidecarWarnsAndLoadsEmpty() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString path = sidecarPath(&cat, QStringLiteral("recycle_bin.json"));
  QVERIFY(!path.isEmpty());
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile broken(path);
  QVERIFY(broken.open(QIODevice::WriteOnly));
  broken.write("{\"entries\": [ { \"id\": \"a1\", \"truncat"); // 半截 JSON
  broken.close();
  QTest::ignoreMessage(QtWarningMsg, QRegularExpression("dataops sidecar corrupt.*"));
  const auto loaded = loadSidecarObject(&cat, QStringLiteral("recycle_bin.json"));
  QVERIFY(loaded.isEmpty()); // 损坏按空处理，但必须留痕
  // 损坏文件不挡后续正常保存与读取。
  QJsonObject root;
  root.insert(QStringLiteral("entries"), QJsonArray{});
  QVERIFY(saveSidecarObject(&cat, QStringLiteral("recycle_bin.json"), root));
  QVERIFY(!loadSidecarObject(&cat, QStringLiteral("recycle_bin.json")).isEmpty());
}

void TestDataOpsSidecar::saveRoundTripsWithoutTempLeftovers() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QJsonObject root;
  root.insert(QStringLiteral("a"), QStringLiteral("标签值"));
  root.insert(QStringLiteral("n"), 42);
  QVERIFY(saveSidecarObject(&cat, QStringLiteral("asset_tags.json"), root));
  const auto back = loadSidecarObject(&cat, QStringLiteral("asset_tags.json"));
  QCOMPARE(back.value(QStringLiteral("a")).toString(), QStringLiteral("标签值"));
  QCOMPARE(back.value(QStringLiteral("n")).toInt(), 42);
  // 原子写不遗留临时文件。
  const QStringList leftovers =
      QDir(QFileInfo(sidecarPath(&cat, QStringLiteral("asset_tags.json")))
               .absolutePath())
          .entryList(QDir::Files);
  QCOMPARE(leftovers, QStringList{QStringLiteral("asset_tags.json")});
}

void TestDataOpsSidecar::failedSaveKeepsPreviousContentIntact() {
  bool elevated = false;
#ifndef Q_OS_WIN
  elevated = (::geteuid() == 0);
#endif
  if (elevated)
    QSKIP("root 不受目录权限位约束，无法注入写失败");
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString name = QStringLiteral("entity_overrides.json");
  QJsonObject root;
  root.insert(QStringLiteral("w1"), QStringLiteral("Well-Alpha"));
  QVERIFY(saveSidecarObject(&cat, name, root));
  const QString path = sidecarPath(&cat, name);
  // 只读目录注入提交失败：原内容必须保持完好。
  QFile::Permissions original = QFile::permissions(path);
  QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner));
  QJsonObject next;
  next.insert(QStringLiteral("w1"), QStringLiteral("Well-Beta"));
  QTest::ignoreMessage(QtWarningMsg, QRegularExpression("dataops sidecar.*failed.*"));
  QVERIFY(!saveSidecarObject(&cat, name, next));
  QFile::setPermissions(path, original);
  QFile file(path);
  QVERIFY(file.open(QIODevice::ReadOnly));
  const auto doc = QJsonDocument::fromJson(file.readAll());
  QCOMPARE(doc.object().value(QStringLiteral("w1")).toString(),
           QStringLiteral("Well-Alpha"));
}

QTEST_MAIN(TestDataOpsSidecar)
#include "tst_dataopssidecar.moc"
