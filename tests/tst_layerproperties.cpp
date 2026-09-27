#include <QtTest>

#include <qgsapplication.h>

// wave/layer-platform 子任务 B：LayerPropertiesDialog（矢量/栅格/缺源三类
// 图层行为 + 业务页字段 + 样式预设生命周期）。骨架壳——子任务 B 用 TDD
// 逐步替换。
class TestLayerProperties : public QObject
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
  TestLayerProperties tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layerproperties.moc"
