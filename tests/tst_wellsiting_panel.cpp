// 层：测试壳
#include <QtTest>
#include <QApplication>

#include "helpers/workflowfixture.h"

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/wellsitingstore.h"
#include "../src/services/projectdata.h"
#include "../src/ui/pages/wellsitingpanel.h"
#include "../src/workflow/wellsitingworkflow.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>

// 方向34 布井辅助面板（视图层）测试：objectName 锚点齐全、地图布点回调
// 入 catalog 计划井并刷表、计划井变化触发实时评估、方案保存进对比表、
// 导出按钮走 exportRequested 意图信号（壳管文件对话框）。
// 面板与 workflow 之间只走公共 API/信号——视图不碰算法头。

using paleo::tests::WorkflowFixture;
using paleo::tests::initFixture;

class TestWellSitingPanel : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase();
    void anchorsAndIntents();
    void mapPlacementAddsPlannedWell();
    void scenarioSaveAndCompareTable();
    void cleanupTestCase();

  private:
    struct Stack
    {
        WorkflowFixture f;
        WellSitingStore store;
        WellSitingWorkflow wf;
        ProjectDataFacade facade;
        Stack()
          : store( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) ),
            wf( &f.layers, &f.store )
        {
          QVERIFY( initFixture( f ) );
          QString error;
          QVERIFY( store.open( &error ) );
          facade.setCatalog( &f.catalog, f.dir.path() );
          wf.setCatalog( &f.catalog, f.dir.path() );
          wf.setProjectData( &facade );
          wf.setSitingStore( &store );
          // 实井 3×3 缺东北（同核/编排测试夹具）。
          for ( int cx = 1000; cx <= 9000; cx += 4000 )
            for ( int cy = 1000; cy <= 9000; cy += 4000 )
            {
              if ( cx == 9000 && cy == 9000 )
                continue;
              CatalogEntity e;
              e.entityType = QStringLiteral( "well" );
              e.name = QStringLiteral( "W%1%2" ).arg( cx ).arg( cy );
              e.hasSurface = true;
              e.surfaceX = double( cx );
              e.surfaceY = double( cy );
              e.coordinateStatus = QStringLiteral( "untransformed" );
              e.id = f.catalog.nextEntityId( QStringLiteral( "well" ) );
              QVERIFY( f.catalog.addEntity( e, &error ) );
            }
          QList<SitingConstraintGeometry> constraints;
          SitingConstraintGeometry boundary;
          boundary.type = QStringLiteral( "polygon" );
          boundary.wkt = QStringLiteral( "POLYGON((0 0, 10000 0, 10000 10000, 0 10000))" );
          constraints.append( boundary );
          wf.setConstraintProvider( [constraints] { return constraints; } );
        }
    };
};

void TestWellSitingPanel::initTestCase()
{
  QVERIFY( qApp != nullptr );
}

void TestWellSitingPanel::anchorsAndIntents()
{
  Stack s;
  WellSitingPanel panel( &s.wf );
  panel.show();
  // 锚点齐全（a11y/测试定位面）。
  for ( const char *name :
        { "sitingRunButton", "sitingGenButton", "sitingPickButton", "sitingAddButton",
          "sitingPlannedTable", "sitingCandidateTable", "sitingScenarioTable",
          "sitingSummaryLabel", "sitingEvalLabel", "sitingNoteLabel",
          "sitingSaveScenarioButton", "sitingExportChartButton", "sitingControlRadius",
          "sitingCellSize" } )
  {
    QVERIFY2( panel.findChild<QWidget *>( QString::fromLatin1( name ) ) != nullptr,
              name );
  }

  // 地图布点/导出是意图信号，面板自己不装地图工具/文件对话框。
  QSignalSpy pickSpy( &panel, &WellSitingPanel::mapPlacementRequested );
  emit panel.findChild<QPushButton *>( QStringLiteral( "sitingPickButton" ) )->clicked();
  QCOMPARE( pickSpy.size(), 1 );

  QSignalSpy exportSpy( &panel, &WellSitingPanel::exportRequested );
  emit panel.findChild<QPushButton *>( QStringLiteral( "sitingExportChartButton" ) )->clicked();
  QCOMPARE( exportSpy.size(), 1 );
  QCOMPARE( exportSpy.first().at( 0 ).toString(), QStringLiteral( "chart" ) );
}

void TestWellSitingPanel::mapPlacementAddsPlannedWell()
{
  Stack s;
  WellSitingPanel panel( &s.wf );
  panel.show();
  QVERIFY( panel.findChild<QTableWidget *>( QStringLiteral( "sitingPlannedTable" ) )
               ->rowCount() == 0 );

  // 壳回调路径：拾取点 → 面板 → workflow → catalog 计划井 → 信号刷表
  // + 实时评估（目标形态 4）。
  panel.placePlannedAt( 9000, 9000 );
  auto *plannedTable = panel.findChild<QTableWidget *>( QStringLiteral( "sitingPlannedTable" ) );
  QCOMPARE( plannedTable->rowCount(), 1 );
  QCOMPARE( s.wf.plannedWells().size(), 1 );
  QVERIFY( plannedTable->item( 0, 0 )->text().contains( QStringLiteral( "计划井" ) ) );

  // 评估标签实时刷新（基线 vs 方案两行）。
  auto *evalLabel = panel.findChild<QLabel *>( QStringLiteral( "sitingEvalLabel" ) );
  QVERIFY( evalLabel->text().contains( QStringLiteral( "基线" ) ) );
  QVERIFY( evalLabel->text().contains( QStringLiteral( "方案" ) ) );

  // 计划井表列 0 带 id（删除/改名按 id 走）。
  const QString id = plannedTable->item( 0, 0 )->data( Qt::UserRole ).toString();
  QVERIFY( id.startsWith( QStringLiteral( "planned-" ) ) );
}

void TestWellSitingPanel::scenarioSaveAndCompareTable()
{
  Stack s;
  WellSitingPanel panel( &s.wf );
  panel.show();
  // 夹具口径：井控半径拨到 3500（3×3 缺东北 + R=3500 → 只有东北角空洞）。
  panel.findChild<QDoubleSpinBox *>( QStringLiteral( "sitingControlRadius" ) )->setValue( 3500 );
  panel.placePlannedAt( 9000, 9000 );

  // 填方案名 → 保存按钮 → 对比表出现一行（指标快照列非空）。
  auto *nameEdit = panel.findChild<QLineEdit *>( QStringLiteral( "sitingScenarioNameEdit" ) );
  nameEdit->setText( QStringLiteral( "加密东北" ) );
  emit panel.findChild<QPushButton *>( QStringLiteral( "sitingSaveScenarioButton" ) )->clicked();
  auto *scenarioTable =
      panel.findChild<QTableWidget *>( QStringLiteral( "sitingScenarioTable" ) );
  QCOMPARE( scenarioTable->rowCount(), 1 );
  QCOMPARE( scenarioTable->item( 0, 0 )->text(), QStringLiteral( "加密东北" ) );
  QCOMPARE( scenarioTable->item( 0, 1 )->text(), QStringLiteral( "1" ) );
  // 覆盖率列是百分数（方案把东北空洞清零 → 100%）。
  QCOMPARE( scenarioTable->item( 0, 3 )->text(), QStringLiteral( "100.0%" ) );

  // 诊断按钮 → 摘要/口径注记上屏（诚实面）。
  emit panel.findChild<QPushButton *>( QStringLiteral( "sitingRunButton" ) )->clicked();
  auto *summary = panel.findChild<QLabel *>( QStringLiteral( "sitingSummaryLabel" ) );
  QVERIFY( summary->text().contains( QStringLiteral( "空洞" ) ) );
  auto *note = panel.findChild<QLabel *>( QStringLiteral( "sitingNoteLabel" ) );
  QVERIFY( note->text().contains( QStringLiteral( "欧氏" ) ) );
}

void TestWellSitingPanel::cleanupTestCase() {}

QTEST_MAIN( TestWellSitingPanel )
#include "tst_wellsiting_panel.moc"
