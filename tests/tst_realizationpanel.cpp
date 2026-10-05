// 层：测试壳（方向 47 RealizationPanel：intent 信号 + 诚实态 + 动画帧序）
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QtTest>

#include "../src/catalog/datacatalog.h"
#include "../src/catalog/realizationset.h"
#include "../src/ui/realization/realizationpanel.h"

// 面板面只发 intent 信号不干活——断言点：
// - 成员切换/统计查看/派生/对比的 intent 参数正确且不重复发射；
// - 缺号集合如实标缺号位（禁用条目），单成员集合派生禁用、文案如实；
// - 动画帧序 = 成员 index 升序，每 tick 恰好一发 memberShowRequested；
// - 零成员集合不入枚举、无 catalog 时面板空态不崩。

using namespace paleo::realization;

namespace
{
// 与 tst_realizationset.cpp 同形：最小契约填充（catalog 行级直写，面板只读）。
QString makeSet( DataCatalog &catalog, const QString &title,
                 const QVector<int> &memberIndices, int declaredCount )
{
  CatalogAsset asset;
  asset.id = catalog.nextAssetId();
  asset.type = kAssetTypeSet;
  asset.format = QStringLiteral( "tif" );
  asset.displayName = title;
  if ( !catalog.addAsset( asset ) )
    return QString();
  for ( const int idx : memberIndices )
  {
    CatalogVersion v;
    v.id = catalog.nextVersionId();
    v.assetId = asset.id;
    v.stage = QStringLiteral( "DERIVED" );
    v.versionNumber = idx + 1;
    v.managed = true;
    v.fileName = QStringLiteral( "REALIZATION_r%1.tif" ).arg( idx, 3, 10, QLatin1Char( '0' ) );
    v.path = DataCatalog::managedPath( QStringLiteral( "derived" ), asset.id, v.id, v.fileName );
    v.extra.insert( kKeySetId, asset.id );
    v.extra.insert( kKeyIndex, idx );
    v.extra.insert( kKeyMemberCount, declaredCount );
    v.extra.insert( kKeySeed, static_cast<qulonglong>( 42 ) );
    if ( !catalog.addVersion( v ) )
      return QString();
  }
  return asset.id;
}

void addStatVersion( DataCatalog &catalog, const QString &setId,
                     const QString &token, int memberCount )
{
  CatalogAsset asset;
  asset.id = catalog.nextAssetId();
  asset.type = kAssetTypeStat;
  asset.format = QStringLiteral( "tif" );
  asset.displayName = QStringLiteral( "stat %1" ).arg( token );
  catalog.addAsset( asset );
  CatalogVersion v;
  v.id = catalog.nextVersionId();
  v.assetId = asset.id;
  v.stage = QStringLiteral( "DERIVED" );
  v.versionNumber = 1;
  v.managed = true;
  v.fileName = QStringLiteral( "STAT_%1.tif" ).arg( token );
  v.path = DataCatalog::managedPath( QStringLiteral( "derived" ), asset.id, v.id, v.fileName );
  v.extra.insert( kKeySetId, setId );
  v.extra.insert( kKeyStatistic, token );
  v.extra.insert( kKeyMemberCount, memberCount );
  catalog.addVersion( v );
}
} // namespace

class RealizationPanelTests : public QObject
{
  Q_OBJECT

  private slots:
    void emptyCatalogHonestState();
    void memberSwitchingEmitsIntent();
    void missingMemberRowsHonest();
    void statListEmitsIntent();
    void singleMemberDisablesDerive();
    void animationTicksInFrameOrder();
    void diffIntentRequiresTwoDistinctSets();
};

void RealizationPanelTests::emptyCatalogHonestState()
{
  RealizationPanel panel;
  auto *setCombo = panel.findChild<QComboBox *>( QStringLiteral( "realizationSetCombo" ) );
  auto *state = panel.findChild<QLabel *>( QStringLiteral( "realizationMemberState" ) );
  auto *derive = panel.findChild<QPushButton *>( QStringLiteral( "realizationDeriveButton" ) );
  auto *play = panel.findChild<QPushButton *>( QStringLiteral( "realizationPlayButton" ) );
  QVERIFY( setCombo && state && derive && play );
  QCOMPARE( setCombo->count(), 0 );
  QVERIFY( !state->text().isEmpty() ); // 空态有文案，不空白
  QVERIFY( !derive->isEnabled() );
  QVERIFY( !play->isEnabled() );
}

void RealizationPanelTests::memberSwitchingEmitsIntent()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString setId = makeSet( catalog, QStringLiteral( "集合A" ), { 0, 1, 2 }, 3 );
  QVERIFY( !setId.isEmpty() );

  RealizationPanel panel;
  panel.bindCatalog( &catalog );
  auto *setCombo = panel.findChild<QComboBox *>( QStringLiteral( "realizationSetCombo" ) );
  auto *slider = panel.findChild<QSlider *>( QStringLiteral( "realizationMemberSlider" ) );
  auto *combo = panel.findChild<QComboBox *>( QStringLiteral( "realizationMemberCombo" ) );
  QVERIFY( setCombo && slider && combo );
  QCOMPARE( setCombo->count(), 1 );
  QCOMPARE( panel.currentSetId(), setId );
  QCOMPARE( combo->count(), 3 );
  QVERIFY( slider->isEnabled() );

  QSignalSpy spy( &panel, &RealizationPanel::memberShowRequested );
  slider->setValue( 1 );
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.takeFirst().at( 1 ).toInt(), 1 ); // index

  emit combo->activated( 2 );
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.takeFirst().at( 1 ).toInt(), 2 );
}

void RealizationPanelTests::missingMemberRowsHonest()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString setId = makeSet( catalog, QStringLiteral( "缺号集合" ), { 0, 2 }, 3 );
  QVERIFY( !setId.isEmpty() );

  RealizationPanel panel;
  panel.bindCatalog( &catalog );
  auto *setCombo = panel.findChild<QComboBox *>( QStringLiteral( "realizationSetCombo" ) );
  auto *combo = panel.findChild<QComboBox *>( QStringLiteral( "realizationMemberCombo" ) );
  auto *state = panel.findChild<QLabel *>( QStringLiteral( "realizationMemberState" ) );
  QVERIFY( setCombo && combo && state );
  QVERIFY( setCombo->itemText( 0 ).contains( QStringLiteral( "缺" ) ) );
  QCOMPARE( combo->count(), 3 ); // 缺号位也列出（禁用占位）
  // 缺号行不可选（Qt item 禁用），在场行可选。
  QVERIFY( !( combo->model()->index( 1, 0 ).flags() & Qt::ItemIsEnabled ) );
  QVERIFY( state->text().contains( QStringLiteral( "缺" ) ) );
  QCOMPARE( panel.frameOrder(), QList<int>( { 0, 2 } ) ); // 缺号帧跳过
}

void RealizationPanelTests::statListEmitsIntent()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString setId = makeSet( catalog, QStringLiteral( "集合S" ), { 0, 1 }, 2 );
  QVERIFY( !setId.isEmpty() );
  addStatVersion( catalog, setId, kStatMean, 2 );
  addStatVersion( catalog, setId, kStatStdDev, 2 );

  RealizationPanel panel;
  panel.bindCatalog( &catalog );
  auto *statList = panel.findChild<QListWidget *>( QStringLiteral( "realizationStatList" ) );
  QVERIFY( statList );
  QCOMPARE( statList->count(), 2 );

  QSignalSpy spy( &panel, &RealizationPanel::statShowRequested );
  emit statList->itemActivated( statList->item( 0 ) );
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.takeFirst().at( 1 ).toString(), kStatMean );
}

void RealizationPanelTests::singleMemberDisablesDerive()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString setId = makeSet( catalog, QStringLiteral( "单成员" ), { 0 }, 1 );
  QVERIFY( !setId.isEmpty() );

  RealizationPanel panel;
  panel.bindCatalog( &catalog );
  auto *derive = panel.findChild<QPushButton *>( QStringLiteral( "realizationDeriveButton" ) );
  auto *state = panel.findChild<QLabel *>( QStringLiteral( "realizationMemberState" ) );
  auto *play = panel.findChild<QPushButton *>( QStringLiteral( "realizationPlayButton" ) );
  QVERIFY( derive && state && play );
  QVERIFY( !derive->isEnabled() );   // 单成员无不确定性可派生
  QVERIFY( !play->isEnabled() );     // 一帧不成动画
  QVERIFY( state->text().contains( QStringLiteral( "单成员" ) ) );
}

void RealizationPanelTests::animationTicksInFrameOrder()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  // 缺号集合 {0,2,4}：帧序必须按 index 升序跳过缺号。
  const QString setId = makeSet( catalog, QStringLiteral( "缺号动画" ), { 4, 0, 2 }, 5 );
  QVERIFY( !setId.isEmpty() );

  RealizationPanel panel;
  panel.setPlayIntervalMs( 10 );
  panel.bindCatalog( &catalog );
  QCOMPARE( panel.frameOrder(), QList<int>( { 0, 2, 4 } ) );

  auto *play = panel.findChild<QPushButton *>( QStringLiteral( "realizationPlayButton" ) );
  QVERIFY( play && play->isEnabled() );
  QSignalSpy spy( &panel, &RealizationPanel::memberShowRequested );
  play->click();
  QVERIFY( panel.isPlaying() );
  QVERIFY( QTest::qWaitFor( [ & ] { return spy.count() >= 4; }, 3000 ) );
  play->click(); // 停止
  QVERIFY( !panel.isPlaying() );

  // 每 tick 恰好一发，index 按升序轮转（0→2→4→0…）。
  QList<int> emitted;
  for ( const auto &args : spy )
    emitted << args.at( 1 ).toInt();
  QVERIFY( emitted.size() >= 4 );
  QCOMPARE( emitted.at( 0 ), 0 );
  QCOMPARE( emitted.at( 1 ), 2 );
  QCOMPARE( emitted.at( 2 ), 4 );
  QCOMPARE( emitted.at( 3 ), 0 ); // 轮转回绕
}

void RealizationPanelTests::diffIntentRequiresTwoDistinctSets()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString a = makeSet( catalog, QStringLiteral( "A" ), { 0, 1 }, 2 );
  const QString b = makeSet( catalog, QStringLiteral( "B" ), { 0, 1 }, 2 );
  QVERIFY( !a.isEmpty() && !b.isEmpty() );

  RealizationPanel panel;
  panel.bindCatalog( &catalog );
  auto *diffA = panel.findChild<QComboBox *>( QStringLiteral( "realizationDiffA" ) );
  auto *diffB = panel.findChild<QComboBox *>( QStringLiteral( "realizationDiffB" ) );
  auto *diffBtn = panel.findChild<QPushButton *>( QStringLiteral( "realizationDiffButton" ) );
  QVERIFY( diffA && diffB && diffBtn );
  QCOMPARE( diffA->count(), 2 );

  // 同集合对比 → statusMessage 拒绝，不发 diffRequested。
  QSignalSpy diffSpy( &panel, &RealizationPanel::diffRequested );
  QSignalSpy statusSpy( &panel, &RealizationPanel::statusMessage );
  diffA->setCurrentIndex( diffA->findData( a ) );
  diffB->setCurrentIndex( diffB->findData( a ) );
  diffBtn->click();
  QCOMPARE( diffSpy.count(), 0 );
  QCOMPARE( statusSpy.count(), 1 );

  diffB->setCurrentIndex( diffB->findData( b ) );
  diffBtn->click();
  QCOMPARE( diffSpy.count(), 1 );
  QCOMPARE( diffSpy.takeFirst().at( 0 ).toString(), a );
}

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  QApplication app( argc, argv );
  RealizationPanelTests tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_realizationpanel.moc"
