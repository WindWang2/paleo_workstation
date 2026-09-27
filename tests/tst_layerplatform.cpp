#include <QtTest>

#include <qgsapplication.h>

// wave/layer-platform 子任务 C：QgisLayerProfileService（页面档案/主题生命周期/
// 层位时序/布局钉主题/.qgz 往返）。骨架壳——子任务 C 用 TDD 逐步替换。
class TestLayerPlatform : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase()
    {
      QVERIFY(QgsApplication::instance() != nullptr);
    }

    void skeletonPlaceholder()
    {
      QVERIFY(true);
    }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(QStringLiteral("/usr"), true);
  app.initQgis();
  TestLayerPlatform tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layerplatform.moc"
