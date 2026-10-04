// 层：视图（测试壳位于 tests/，被测对象为 ui 层格架面板 + 柱状视图）
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "../src/catalog/datacatalog.h"
#include "../src/catalog/frameworkstore.h"
#include "../src/domain/arearules.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/services/frameworkservice.h"
#include "../src/ui/sequenceframeworkcolumn.h"
#include "../src/ui/sequenceframeworkpanel.h"

// 方向 28 的视图面断言（无头，offscreen）：
//   · 面板 CRUD/排序走程序化入口，树结构与模型一致；
//   · 柱状视图按格架树渲染，点击/设置当前单元双向联动；
//   · 建议页签默认不勾、未确认应用零写入；勾了才落新版本；
//   · 诊断页签把诊断项列出来，导出是 Markdown 表格。
namespace
{

using namespace SequenceFramework;

QTreeWidget *findTree( QWidget *root, const QString &name )
{
  return root->findChild<QTreeWidget *>( name );
}

QTableWidget *findTable( QWidget *root, const QString &name )
{
  return root->findChild<QTableWidget *>( name );
}

// 造一个 well_stratification 资产（受管 RAW 版本），供 loadWellTops 解析。
bool seedWellTops( DataCatalog &cat, const QString &projectDir, const QByteArray &text )
{
  CatalogAsset asset;
  asset.id = cat.nextAssetId();
  asset.type = QStringLiteral( "well_stratification" );
  asset.format = QStringLiteral( "dat" );
  asset.displayName = QStringLiteral( "DC" );
  if ( !cat.addAsset( asset ) )
    return false;
  const QString versionId = cat.nextVersionId();
  const QString rel = DataCatalog::managedPath( QStringLiteral( "RAW" ), asset.id, versionId,
                                                QStringLiteral( "DC.dat" ) );
  const QString abs = projectDir + QStringLiteral( "/" ) + rel;
  QFileInfo( abs ).dir().mkpath( QStringLiteral( "." ) );
  QFile f( abs );
  if ( !f.open( QIODevice::WriteOnly ) )
    return false;
  f.write( text );
  f.close();

  CatalogVersion ver;
  ver.id = versionId;
  ver.assetId = asset.id;
  ver.stage = QStringLiteral( "RAW" );
  ver.versionNumber = 1;
  ver.managed = true;
  ver.path = rel;
  ver.fileName = QStringLiteral( "DC.dat" );
  return cat.addVersion( ver );
}

Framework seedFramework()
{
  Framework fw;
  fw.name = QStringLiteral( "面板夹具" );
  FrameworkUnit sq1;
  sq1.id = QStringLiteral( "sfu-1" );
  sq1.name = QStringLiteral( "SQ1" );
  sq1.topBoundary = QStringLiteral( "C3" );
  sq1.baseBoundary = QStringLiteral( "D53" );
  sq1.thickness = 100.0;
  fw.units.append( sq1 );
  FrameworkUnit sq2;
  sq2.id = QStringLiteral( "sfu-2" );
  sq2.name = QStringLiteral( "SQ2" );
  sq2.ordinal = 1;
  sq2.topBoundary = QStringLiteral( "D53" );
  sq2.thickness = 100.0;
  fw.units.append( sq2 );
  MarkerBed seed;
  seed.id = QStringLiteral( "sfm-1" );
  seed.name = QStringLiteral( "M-C3" );
  seed.unitId = QStringLiteral( "sfu-1" );
  seed.layerNames.append( QStringLiteral( "C3" ) );
  fw.markers.append( seed );
  return fw;
}

} // namespace

class TestSequenceFrameworkUi : public QObject
{
  Q_OBJECT

private slots:
  void init()
  {
    AreaRules::reset();
  }

  void panelCrudAndSorting()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    DataCatalog cat;
    QVERIFY( cat.open( dir.path() ) );
    FrameworkService service( dir.path() );
    service.attachCatalog( &cat );
    QString err;
    QVERIFY2( service.save( seedFramework(), &err ), qPrintable( err ) );

    SequenceFrameworkPanel panel;
    QVERIFY( !panel.isEnabled() ); // 未接线前禁用
    panel.setService( &service );
    QVERIFY( panel.isEnabled() );

    auto *tree = findTree( &panel, QStringLiteral( "sfFrameworkTree" ) );
    QVERIFY( tree != nullptr );
    QCOMPARE( tree->topLevelItemCount(), 2 );

    // 新增体系域 → 树里出现子节点，且模型里能查到。
    QVERIFY( panel.addSystemsTract( QStringLiteral( "sfu-1" ), QStringLiteral( "TST" ),
                                    QStringLiteral( "C3" ), QStringLiteral( "C6" ), 40.0 ) );
    QCOMPARE( tree->topLevelItem( 0 )->childCount(), 1 );
    QCOMPARE( tree->topLevelItem( 0 )->child( 0 )->text( 0 ), QStringLiteral( "TST" ) );

    // 重命名 / 排序 / 删除都落到模型（面板只是渲染层）。
    QVERIFY( panel.renameUnit( QStringLiteral( "sfu-2" ), QStringLiteral( "SQ2b" ) ) );
    QCOMPARE( tree->topLevelItem( 1 )->text( 0 ), QStringLiteral( "SQ2b" ) );
    QVERIFY( panel.moveUnit( QStringLiteral( "sfu-2" ), 1 ) ); // 上移：与 sfu-1 换位
    const FrameworkUnit *moved = unitById( panel.columnView()->framework(),
                                           QStringLiteral( "sfu-2" ) );
    QVERIFY( moved != nullptr );
    QCOMPARE( moved->ordinal, 0 );
    QVERIFY( panel.removeUnit( QStringLiteral( "sfu-1" ) ) );
    QCOMPARE( tree->topLevelItemCount(), 1 ); // 子单元一并删除，不留悬空

    // 保存后重开仍在（走 catalog 版本，不是内存）。
    QVERIFY2( panel.saveFramework(), qPrintable( err ) );
    DataCatalog reopened;
    QVERIFY( reopened.open( dir.path() ) );
    FrameworkStore store( &reopened, dir.path() );
    Framework loaded;
    QVERIFY2( store.load( &loaded, &err ), qPrintable( err ) );
    QCOMPARE( loaded.units.size(), 1 );
    QCOMPARE( loaded.units.first().name, QStringLiteral( "SQ2b" ) );
  }

  void columnViewTracksFrameworkAndSelection()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    DataCatalog cat;
    QVERIFY( cat.open( dir.path() ) );
    FrameworkService service( dir.path() );
    service.attachCatalog( &cat );
    QString err;
    QVERIFY2( service.save( seedFramework(), &err ), qPrintable( err ) );

    SequenceFrameworkPanel panel;
    panel.setService( &service );
    panel.resize( 900, 600 );
    panel.show();
    QTest::qWaitForWindowExposed( &panel );

    auto *column = panel.columnView();
    QVERIFY( column != nullptr );
    QCOMPARE( column->framework().units.size(), 2 );

    QSignalSpy spy( &panel, &SequenceFrameworkPanel::currentUnitChanged );
    panel.setCurrentUnit( QStringLiteral( "sfu-2" ) );
    QCOMPARE( column->currentUnit(), QStringLiteral( "sfu-2" ) );
    QCOMPARE( spy.count(), 1 );
    QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "sfu-2" ) );

    // 命中测试：柱状图上半部落在 SQ1 带内（格架树序 SQ1 在上）。
    QVERIFY( column->size().height() > 40 );
    const QString upper = column->unitAt( QPoint( column->width() / 2, 20 ) );
    QVERIFY2( !upper.isEmpty(), qPrintable( QString::number( column->height() ) ) );
    QCOMPARE( upper, QStringLiteral( "sfu-1" ) );
  }

  void suggestionsRequireConfirmation()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    DataCatalog cat;
    QVERIFY( cat.open( dir.path() ) );
    QVERIFY( seedWellTops( cat, dir.path(),
                           "A1 C3 1000\nA1 C6 1100\nA1 D53 1200\n"
                           "A2 C3 1010\nA2 C6 1110\nA2 D53 1210\n" ) );
    FrameworkService service( dir.path() );
    service.attachCatalog( &cat );
    QString err;
    QVERIFY2( service.save( seedFramework(), &err ), qPrintable( err ) );

    SequenceFrameworkPanel panel;
    panel.setService( &service );
    panel.refreshSuggestions();
    const QVector<SuggestionCandidate> candidates = panel.suggestions();
    // 种子只确认了 C3 → 未归属的是 C6 与 D53，两口井各一条 = 4 条候选。
    QVERIFY2( candidates.size() == 4, qPrintable( QString::number( candidates.size() ) ) );

    auto *table = findTable( &panel, QStringLiteral( "sfSuggestionTable" ) );
    QVERIFY( table != nullptr );
    QCOMPARE( table->rowCount(), 4 );
    // 默认不勾——未经人工确认的候选不得进库。
    QCOMPARE( table->item( 0, 0 )->checkState(), Qt::Unchecked );
    QCOMPARE( table->item( 1, 0 )->checkState(), Qt::Unchecked );
    // 稳定序：距离 → 层名 → 井名，首条是 A1 井的 C6。
    QCOMPARE( candidates.first().layerName, QStringLiteral( "C6" ) );
    QCOMPARE( candidates.first().wellName, QStringLiteral( "A1" ) );

    auto versionCount = [&cat]() -> int {
      for ( const CatalogAsset &a : cat.assets() )
        if ( a.type == FrameworkStore::assetType() )
          return int( cat.versionsForAsset( a.id ).size() );
      return 0;
    };
    const quint64 seqBefore = cat.mutationSeq();
    const int versionsBefore = versionCount();
    // 全未确认 → 不落盘。
    QVERIFY( panel.applySuggestions() );
    QCOMPARE( cat.mutationSeq(), seqBefore );
    QCOMPARE( versionCount(), versionsBefore );

    // 勾一条 → 只写这一条。
    panel.refreshSuggestions();
    auto *table2 = findTable( &panel, QStringLiteral( "sfSuggestionTable" ) );
    table2->item( 0, 0 )->setCheckState( Qt::Checked );
    QVERIFY( panel.applySuggestions() );
    QCOMPARE( versionCount(), versionsBefore + 1 );
    const QString acceptedLayer = panel.suggestions().isEmpty()
                                      ? QString()
                                      : QString(); // 应用后清空候选表
    Q_UNUSED( acceptedLayer )
    const Framework &after = service.framework();
    int withC6 = 0;
    for ( const MarkerBed &m : after.markers )
      if ( m.layerNames.contains( QStringLiteral( "C6" ) ) )
        ++withC6;
    QCOMPARE( withC6, 1 ); // 只勾了第一条（C6）
  }

  void diagnosticsSurfaceAndExport()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    DataCatalog cat;
    QVERIFY( cat.open( dir.path() ) );
    QVERIFY( seedWellTops( cat, dir.path(), "A1 C3 1000\nA1 XZ 1500\n" ) );
    FrameworkService service( dir.path() );
    service.attachCatalog( &cat );
    QString err;
    QVERIFY2( service.save( seedFramework(), &err ), qPrintable( err ) );

    SequenceFrameworkPanel panel;
    panel.setService( &service );
    panel.refreshDiagnostics();
    const DiagnosticReport report = panel.diagnosticReport();
    QVERIFY( !report.isEmpty() );
    // 夹具里 XZ 没有格架归属 → 至少一条孤立井分层。
    QVERIFY( report.countOf( DiagnosticKind::OrphanWellLayer ) >= 1 );

    auto *diagTree = findTree( &panel, QStringLiteral( "sfDiagnosticTree" ) );
    QVERIFY( diagTree != nullptr );
    QCOMPARE( diagTree->topLevelItemCount(), report.items.size() );

    const QString out = dir.path() + QStringLiteral( "/report.md" );
    QVERIFY2( panel.exportReport( out, &err ), qPrintable( err ) );
    QFile f( out );
    QVERIFY( f.open( QIODevice::ReadOnly ) );
    const QString md = QString::fromUtf8( f.readAll() );
    QVERIFY( md.contains( QStringLiteral( "|" ) ) );
    QVERIFY( md.contains( diagnosticCode( DiagnosticKind::OrphanWellLayer ) ) );
  }
};

int main( int argc, char *argv[] )
{
  QApplication app( argc, argv );
  TestSequenceFrameworkUi tc;
  QList<QByteArray> forwarded;
  forwarded << QByteArray( argv[ 0 ] );
  for ( int i = 1; i < argc; ++i )
    forwarded << QByteArray( argv[ i ] );
  forwarded << QByteArray( "-o" )
            << QByteArray( QT_TESTCASE_BUILDDIR ) + "/tst_sequenceframework_ui-result.txt,txt";
  QList<char *> cargv;
  cargv.reserve( forwarded.size() );
  for ( QByteArray &a : forwarded )
    cargv << a.data();
  return QTest::qExec( &tc, cargv.size(), cargv.data() );
}

#include "tst_sequenceframework_ui.moc"
