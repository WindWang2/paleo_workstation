// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "app/portableenvironment.h"

class TestPortableEnvironment : public QObject
{
  Q_OBJECT
  static void touch(const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
  }
private slots:
  void developmentEnvironmentIsUnchanged() {
    QTemporaryDir root;
    QProcessEnvironment env;
    env.insert("QGIS_PREFIX_PATH", "/configured/vendor");
    env.insert("PROJ_DATA", "/configured/proj");
    const auto result = inspectPortableEnvironment(root.path(), env);
    QVERIFY(!result.detected); QCOMPARE(result.values, env);
  }
  void relocatedPackageOverridesForeignGis() {
    QTemporaryDir parent;
    // 中文目录名是有意为之：inspect 层保持路径原样；真实初始化
    //（initializePortableEnvironment）对非 ASCII 安装根改用纯 ASCII 锚点。
    const auto root = parent.filePath(QStringLiteral("新电脑/地质软件 Portable"));
    touch(root + "/plugins/platforms/qwindows.dll");
    touch(root + "/share/proj/proj.db"); touch(root + "/qgis/resources/srs.db");
    QProcessEnvironment foreign;
    foreign.insert("PROJ_DATA", "C:/old-gis/proj");
    foreign.insert("PROJ_LIB", "C:/another-gis/proj");
    foreign.insert("QGIS_PREFIX_PATH", "C:/build-machine/qgis");
    foreign.insert("GDAL_DATA", "C:/other-gdal");
    foreign.insert("QT_PLUGIN_PATH", "C:/other-qt/plugins");
    const auto result = inspectPortableEnvironment(root, foreign);
    QVERIFY(result.detected); QVERIFY(result.missing.isEmpty());
    QCOMPARE(result.values.value("PROJ_DATA"), root + "/share/proj");
    QCOMPARE(result.values.value("PROJ_LIB"), result.values.value("PROJ_DATA"));
    QCOMPARE(result.values.value("QGIS_PREFIX_PATH"), root + "/qgis");
    QCOMPARE(result.values.value("GDAL_DATA"), root + "/share/gdal");
    QCOMPARE(result.values.value("QT_PLUGIN_PATH"), root + "/plugins");
  }
  void anchoredBindingsUseAnchorRoot() {
    QTemporaryDir parent;
    const auto root = parent.filePath(QStringLiteral("地质软件 Portable"));
    touch(root + "/plugins/platforms/qwindows.dll");
    touch(root + "/share/proj/proj.db"); touch(root + "/qgis/resources/srs.db");
    QTemporaryDir anchorRoot;
    const auto anchor = anchorRoot.path();
    const auto result = inspectPortableEnvironment(root, {}, anchor);
    QVERIFY(result.detected); QVERIFY(result.missing.isEmpty());
    // 锚点路径按纯 ASCII 设计（短路径或 ProgramData 联接），三编码域无损。
    for (const auto &ch : anchor) QVERIFY2(ch.unicode() < 128, qPrintable(anchor));
    QCOMPARE(result.values.value("PROJ_DATA"), anchor + "/share/proj");
    QCOMPARE(result.values.value("QGIS_PREFIX_PATH"), anchor + "/qgis");
    QCOMPARE(result.values.value("QT_PLUGIN_PATH"), anchor + "/plugins");
    // 缺件诊断仍按真实安装根报告，用户拿到的是可定位的长路径。
    QVERIFY(result.missing.isEmpty());
  }
  void missingDatabasesAreReported() {
    QTemporaryDir root;
    touch(root.filePath("plugins/platforms/qwindows.dll"));
    const auto result = inspectPortableEnvironment(root.path(), {});
    QVERIFY(result.detected); QCOMPARE(result.missing.size(), 2);
    QVERIFY(result.missing.contains(root.filePath("share/proj/proj.db")));
    QVERIFY(result.missing.contains(root.filePath("qgis/resources/srs.db")));
  }
};
QTEST_GUILESS_MAIN(TestPortableEnvironment)
#include "tst_portableenvironment.moc"
