// 层：视图（测试壳位于 tests/，被测对象为演化动览面板）
#include "helpers/workflowfixture.h"

#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QSize>
#include <QToolButton>

#include <qgsapplication.h>

#include <cpl_conv.h>
#include <gdal.h>

#include "../src/domain/arearules.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/ui/evolution/evolutionplayerpanel.h"
#include "uipolish_capture.h"

// 方向35 动览面板：步进顺序 = mappingHorizons()（浅→深）、切换走
// SelectionContext + QgisLayerService 双服务同步、定格导出为 intent 信号、
// 播放到末帧自停（QTimer 功能性步进，非编排动画）。

using paleo::tests::WorkflowFixture;
using paleo::tests::initFixture;

class TestEvolutionPlayer : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase()
    {
      QVERIFY( QgsApplication::instance() != nullptr );
    }

    void init()
    {
      // 三期层位序（浅→深 H1/H2/H3）——工程级参数驱动，测试不 hardcode。
      m_dir = std::make_unique<QTemporaryDir>();
      QVERIFY( m_dir->isValid() );
      QFile config( m_dir->filePath( QStringLiteral( "project_area.json" ) ) );
      QVERIFY( config.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
      config.write( R"({
  "schema_version": 1,
  "sequence_boundaries": ["H1", "H2", "H3"],
  "target_horizon": "H1",
  "classifier": {"dat_path_rules": [], "reference_dir_names": [], "fixed_auxiliary_name_stem": "OTHER-1"},
  "segy_indexing": {"inline_word_offset": 189, "crossline_word_offset": 193, "field_record_offset": 9, "cdp_xline_offset": 24},
  "onnx_grid": {"rows": 97, "cols": 129}
})" );
      config.close();
      AreaRules::setProjectDir( m_dir->path() );
      QVERIFY( AreaRules::lastError().isEmpty() );
      QCOMPARE( mappingHorizons(),
                QStringList( { QStringLiteral( "H1" ), QStringLiteral( "H2" ), QStringLiteral( "H3" ) } ) );
    }

    void cleanup()
    {
      AreaRules::reset();
      m_dir.reset();
    }

    // Oracle 4：动览步进顺序与 mappingHorizons() 一致。
    void frameOrderMatchesMappingHorizons();

    void stepAdvancesInMappingOrder();
    void exportEmitsCurrentFrame();
    void playStopsAtLastFrame();
    void followsExternalActiveHorizon();
    void outOfSetHorizonKeepsFrame();

  private:
    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestEvolutionPlayer::frameOrderMatchesMappingHorizons()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  SelectionContext selection;
  EvolutionPlayerPanel panel( &selection, &f.layers );
  // 修前/修后视觉证据（PALEO_UI_CAPTURE 未设时零开销直通）。
  selection.setActiveHorizon( QStringLiteral( "H2" ) );
  uipolish::capturePanel( &panel, QStringLiteral( "evolution_player" ), QSize( 480, 48 ) );
  QCOMPARE( panel.frameOrder(), mappingHorizons() );
  QCOMPARE( panel.frameOrder(),
            QStringList( { QStringLiteral( "H1" ), QStringLiteral( "H2" ), QStringLiteral( "H3" ) } ) );
}

void TestEvolutionPlayer::stepAdvancesInMappingOrder()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  SelectionContext selection;
  EvolutionPlayerPanel panel( &selection, &f.layers );
  panel.show();
  QVERIFY( QTest::qWaitForWindowExposed( &panel ) );

  // 起点 = 首帧（浅）H1；「下一期」沿 mappingHorizons() 向深步进。
  selection.setActiveHorizon( QStringLiteral( "H1" ) );
  QCOMPARE( panel.currentHorizon(), QStringLiteral( "H1" ) );

  auto *next = panel.findChild<QToolButton *>( QStringLiteral( "evolutionNextButton" ) );
  auto *prev = panel.findChild<QToolButton *>( QStringLiteral( "evolutionPrevButton" ) );
  QVERIFY( next && prev );

  QTest::mouseClick( next, Qt::LeftButton );
  QCOMPARE( selection.activeHorizon(), QStringLiteral( "H2" ) );
  QCOMPARE( panel.currentHorizon(), QStringLiteral( "H2" ) );
  QTest::mouseClick( next, Qt::LeftButton );
  QCOMPARE( selection.activeHorizon(), QStringLiteral( "H3" ) );
  // 末帧再进不动（演化是有向序列，不循环）。
  QTest::mouseClick( next, Qt::LeftButton );
  QCOMPARE( selection.activeHorizon(), QStringLiteral( "H3" ) );
  QTest::mouseClick( prev, Qt::LeftButton );
  QCOMPARE( selection.activeHorizon(), QStringLiteral( "H2" ) );
}

void TestEvolutionPlayer::exportEmitsCurrentFrame()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  SelectionContext selection;
  EvolutionPlayerPanel panel( &selection, &f.layers );
  selection.setActiveHorizon( QStringLiteral( "H2" ) );
  QSignalSpy exportSpy( &panel, &EvolutionPlayerPanel::frameExportRequested );

  auto *exportButton = panel.findChild<QToolButton *>( QStringLiteral( "evolutionExportButton" ) );
  QVERIFY( exportButton != nullptr );
  QTest::mouseClick( exportButton, Qt::LeftButton );
  QCOMPARE( exportSpy.count(), 1 );
  QCOMPARE( exportSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "H2" ) );
}

void TestEvolutionPlayer::playStopsAtLastFrame()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  SelectionContext selection;
  EvolutionPlayerPanel panel( &selection, &f.layers );
  panel.setPlayIntervalMs( 30 );
  selection.setActiveHorizon( QStringLiteral( "H1" ) );

  auto *play = panel.findChild<QToolButton *>( QStringLiteral( "evolutionPlayButton" ) );
  QVERIFY( play != nullptr );
  QTest::mouseClick( play, Qt::LeftButton );
  QVERIFY( panel.isPlaying() );
  // 三帧序列：两步到末帧后自动停播（不等循环）。
  QTRY_VERIFY_WITH_TIMEOUT( !panel.isPlaying(), 5000 );
  QCOMPARE( selection.activeHorizon(), QStringLiteral( "H3" ) );
}

void TestEvolutionPlayer::followsExternalActiveHorizon()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  SelectionContext selection;
  EvolutionPlayerPanel panel( &selection, &f.layers );
  selection.setActiveHorizon( QStringLiteral( "H1" ) );
  // 外部（如层位 chip）切换 → 动览帧跟随。
  selection.setActiveHorizon( QStringLiteral( "H3" ) );
  QCOMPARE( panel.currentHorizon(), QStringLiteral( "H3" ) );
}

void TestEvolutionPlayer::outOfSetHorizonKeepsFrame()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  SelectionContext selection;
  EvolutionPlayerPanel panel( &selection, &f.layers );
  selection.setActiveHorizon( QStringLiteral( "H2" ) );
  // 集合外层位不挪帧（与 chip「不点亮任何 chip」同口径）。
  selection.setActiveHorizon( QStringLiteral( "ZZZ" ) );
  QCOMPARE( panel.currentHorizon(), QStringLiteral( "H2" ) );
}

#include "tst_evolutionplayer.moc"

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true ); // distro install
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestEvolutionPlayer tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}


