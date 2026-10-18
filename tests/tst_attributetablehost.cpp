// 层：视图（测试壳位于 tests/，被测对象为 QGIS 封装层的属性表宿主）
#include "../src/qgis/nativeattributetablehost.h"

#include <QtTest>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgspointxy.h>
#include <qgsvectorlayer.h>

#include "../src/qgis/qgisruntime.h"

// FD-004/R2-SF-04：PaleoAttributeTableHost 的破坏性确认 ask seam——注入后
// 三分支（保存/放弃/取消）真实驱动编辑结局；未注入时 offscreen 保持直存
// 语义（降级契约不变）。独立成文件：tst_mappingworkbench/tst_edittools 的
// 既有属性表用例所在文件均被开放 PR 占用（避免文本冲突）。
class TestAttributeTableHost : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase() { QVERIFY( QgisRuntime::isInitialized() ); }

    void askSeamDrivesSaveDiscardCancel();

  private:
    QgsVectorLayer *makeDirtyLayer();
};

QgsVectorLayer *TestAttributeTableHost::makeDirtyLayer()
{
  auto *layer = new QgsVectorLayer( QStringLiteral( "Point?field=name:string" ),
                                    QStringLiteral( "seam_pts" ), QStringLiteral( "memory" ) );
  layer->startEditing();
  QgsFeature f( layer->fields() );
  f.setAttribute( QStringLiteral( "name" ), QStringLiteral( "井A" ) );
  f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 1, 2 ) ) );
  layer->addFeature( f );
  return layer;
}

void TestAttributeTableHost::askSeamDrivesSaveDiscardCancel()
{
  QgsMapCanvas canvas;
  PaleoAttributeTableHost::SaveChoice choice = PaleoAttributeTableHost::SaveChoice::Cancel;
  int asked = 0;
  {
    auto *layer = makeDirtyLayer(); // 测试内手动清理（无父子挂靠）
    PaleoAttributeTableHost host( &canvas, nullptr );
    host.setSaveDiscardAsk( [&]( const QString & ) {
      ++asked;
      return choice;
    } );
    QVERIFY( layer->isModified() );
    choice = PaleoAttributeTableHost::SaveChoice::Cancel;
    QVERIFY( !host.stopEditing( layer, true ) ); // 取消：编辑态保持
    QVERIFY( layer->isEditable() );
    QVERIFY( layer->isModified() );
    choice = PaleoAttributeTableHost::SaveChoice::Discard;
    QVERIFY( host.stopEditing( layer, true ) ); // 放弃：回滚
    QVERIFY( !layer->isEditable() );
    QCOMPARE( layer->featureCount(), 0 );
    delete layer;
  }
  {
    auto *layer = makeDirtyLayer();
    PaleoAttributeTableHost host( &canvas, nullptr );
    host.setSaveDiscardAsk( [&]( const QString & ) {
      ++asked;
      return choice;
    } );
    choice = PaleoAttributeTableHost::SaveChoice::Save;
    QVERIFY( host.stopEditing( layer, true ) ); // 保存：提交
    QVERIFY( !layer->isEditable() );
    QCOMPARE( layer->featureCount(), 1 );
    delete layer;
  }
  QCOMPARE( asked, 3 );
  // 未注入 seam：offscreen 平台保持直存语义（旧降级契约不变）。
  {
    auto *layer = makeDirtyLayer();
    PaleoAttributeTableHost host( &canvas, nullptr );
    QVERIFY( host.stopEditing( layer, true ) );
    QVERIFY( !layer->isEditable() );
    QCOMPARE( layer->featureCount(), 1 );
    delete layer;
  }
}

int main( int argc, char *argv[] )
{
  QGuiApplication::setAttribute( Qt::AA_ShareOpenGLContexts );
  QgsApplication app( argc, argv, true );
  QgsApplication::initQgis();
  TestAttributeTableHost tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_attributetablehost.moc"
