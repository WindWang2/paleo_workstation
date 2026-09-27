#include <QtTest>

#include <qgsapplication.h>

// wave/layer-platform 子任务 A：LayerTreePanel（筛选/组级显隐/复制图层/
// indicator/objectName 兼容）。骨架壳——子任务 A 用 TDD 逐步替换。
class TestLayerTreePanel : public QObject
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
  TestLayerTreePanel tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layertreepanel.moc"
