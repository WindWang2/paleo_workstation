// 任务 A（数据/资产/图谱深化 2026-10-13）：dataops sidecar 读写加固回归。
// recycle_bin/asset_tags 等四个 .paleo/*.json 此前是非原子直写 + 损坏静默
// 回空——断电半截文件 = 重开静默全丢。本套件钉死原子写与损坏告警契约。
//
// #534 追加：三类 sidecar 的目录推导与 load/save 收敛到数据层门面
// （services/jsonsidecar.h），视图不再自持路径与 QSaveFile。以下两条用例在
// 无头路径（不建任何 QWidget、不经 ui/ 值类型）上钉死门面本身——壳/无头
// 场景复用这三类数据不再需要连 UI 一起拖。
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
#include "../src/services/jsonsidecar.h"
#include "../src/ui/pages/dataops/dataopsmodel.h"

using namespace paleo::dataops;

class TestDataOpsSidecar : public QObject {
  Q_OBJECT
private slots:
  void missingSidecarIsNormalEmptyState();
  void corruptSidecarWarnsAndLoadsEmpty();
  void saveRoundTripsWithoutTempLeftovers();
  void failedSaveKeepsPreviousContentIntact();
  void dataLayerFacadeOwnsPathsAndRoundTrips();
  void undoVaultFacadeRoundTripsHeadlessly();
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

// #534：门面自带目录推导（<projectDir>/.paleo/<name>.json）与读写；视图侧
// sidecarPath/loadSidecarObject/saveSidecarObject 只是同名转发，两边必须同源。
void TestDataOpsSidecar::dataLayerFacadeOwnsPathsAndRoundTrips() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString path =
      paleo::dataopsSidecarPath(&cat, QStringLiteral("asset_tags.json"));
  QVERIFY(path.endsWith(QStringLiteral(".paleo/asset_tags.json")));
  QCOMPARE(sidecarPath(&cat, QStringLiteral("asset_tags.json")), path);
  QVERIFY(paleo::dataopsSidecarLoad(&cat, QStringLiteral("asset_tags.json"))
              .isEmpty()); // 无文件 = 初始态，不是错误
  QJsonObject root;
  root.insert(QStringLiteral("tags"),
              QJsonObject{{QStringLiteral("a1"),
                           QJsonArray{QStringLiteral("相图")}}});
  QVERIFY(paleo::dataopsSidecarSave(&cat, QStringLiteral("asset_tags.json"),
                                    root));
  const auto back =
      paleo::dataopsSidecarLoad(&cat, QStringLiteral("asset_tags.json"));
  QCOMPARE(back.value(QStringLiteral("tags"))
               .toObject()
               .value(QStringLiteral("a1"))
               .toArray()
               .size(),
           1);
  // catalog 未开 → 空转：不猜路径、不写盘、不当失败炸调用方。
  DataCatalog closed;
  QVERIFY(paleo::dataopsSidecarPath(&closed, QStringLiteral("asset_tags.json"))
              .isEmpty());
  QVERIFY(paleo::dataopsSidecarLoad(&closed, QStringLiteral("asset_tags.json"))
              .isEmpty());
  QVERIFY(!paleo::dataopsSidecarSave(&closed, QStringLiteral("asset_tags.json"),
                                     root));
}

// #534：undo 库（.paleo/undo_stack.json）同样只经数据层门面读写——记录/记忆
// 的字段映射留在视图层（UndoRecord 是 ui/ 类型），持久化本身无头可完成。
void TestDataOpsSidecar::undoVaultFacadeRoundTripsHeadlessly() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString path = paleo::undoVaultPath(&cat);
  QVERIFY(path.endsWith(QStringLiteral(".paleo/undo_stack.json")));
  QVERIFY(paleo::undoVaultLoad(&cat).isEmpty()); // 不存在 = 合法空库
  QJsonObject root;
  root.insert(QStringLiteral("undo"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("asset_id"), QStringLiteral("a1")},
                  {QStringLiteral("entity_id"), QStringLiteral("e1")},
                  {QStringLiteral("role"), QStringLiteral("well_log")},
                  {QStringLiteral("entity_type"), QStringLiteral("well")},
                  {QStringLiteral("note"), QStringLiteral("挂接备注")}}});
  root.insert(QStringLiteral("notes"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("asset_id"), QStringLiteral("a1")},
                  {QStringLiteral("role"), QStringLiteral("well_log")},
                  {QStringLiteral("note"), QStringLiteral("未决链接记忆")}}});
  QVERIFY(paleo::undoVaultSave(&cat, root));
  bool ok = false;
  const auto back = paleo::undoVaultLoad(&cat, &ok);
  QVERIFY(ok);
  QCOMPARE(back.value(QStringLiteral("undo")).toArray().size(), 1);
  QCOMPARE(back.value(QStringLiteral("notes"))
               .toArray()
               .first()
               .toObject()
               .value(QStringLiteral("note"))
               .toString(),
           QStringLiteral("未决链接记忆"));
  // 半截 undo 库：如实失败并留痕——静默按空库成功会让全部撤销入口无声消失。
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write("{\"undo\": [ {");
  f.close();
  QTest::ignoreMessage(QtWarningMsg,
                       QRegularExpression("DataPage undo vault corrupt.*"));
  QVERIFY(paleo::undoVaultLoad(&cat, &ok).isEmpty()); // 损坏按空处理
  QVERIFY(!ok);                                       // 但如实标失败（已留痕）
  // catalog 未开 → 空库而非失败（无工程可读写）。
  DataCatalog closed;
  QVERIFY(paleo::undoVaultPath(&closed).isEmpty());
  QVERIFY(paleo::undoVaultLoad(&closed, &ok).isEmpty());
  QVERIFY(ok);
}

QTEST_MAIN(TestDataOpsSidecar)
#include "tst_dataopssidecar.moc"
