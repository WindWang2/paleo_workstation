// 层：数据（测试壳位于 tests/，被测对象为数据层 io 统计面）
#include <QtTest/QtTest>

#include "io/outsourceworkbook.h"

#include <QDir>
#include <QFileInfo>
#include <cmath>

using namespace paleo::io;

namespace
{

QString fixturePath( const QString &name )
{
  const QString fromSource = QFileInfo( QString::fromUtf8( __FILE__ ) )
                                 .dir()
                                 .filePath( QStringLiteral( "fixtures/outsource/" ) + name );
  if ( QFileInfo::exists( fromSource ) )
    return fromSource;
  return QStringLiteral( "tests/fixtures/outsource/" ) + name;
}

// 12 行合成曲线表：深度 / GR（线性升）/ 孔隙度（与 GR 完全线性正相关）/
// 声波（强负相关）/ 无关列（固定伪随机）/ 常数列 / 短列（只有 2 个数值）。
WorkbookSheet syntheticCurveSheet()
{
  WorkbookSheet sheet;
  sheet.name = QStringLiteral( "测井曲线" );
  sheet.headers = { QStringLiteral( "深度" ),  QStringLiteral( "GR" ),
                    QStringLiteral( "孔隙度" ), QStringLiteral( "声波" ),
                    QStringLiteral( "无关列" ), QStringLiteral( "常数列" ),
                    QStringLiteral( "短列" ) };
  static const double unrelated[12] = { 1, 5, 2, 8, 3, 9, 1, 6, 2, 7, 4, 8 };
  for ( int i = 0; i < 12; ++i )
  {
    sheet.rows.append( QStringList{
        QString::number( 1000 + 10 * i ),
        QString::number( 45 + 5 * i ),
        QString::number( 0.10 + 0.005 * i, 'f', 3 ),
        QString::number( 240 - 2 * i + ( i % 2 ) * 4 ),
        QString::number( unrelated[i] ),
        QStringLiteral( "7" ),
        i < 2 ? QString::number( 3 + i ) : QString() } );
  }
  return sheet;
}

} // namespace

class OutsourceStatsTests : public QObject
{
  Q_OBJECT
  private slots:
    void curveStatisticsMatchDirectComputation();
    void curveStatisticsHonestAboutSparseColumns();
    void discoveryFindsCorrelatedPairs();
    void discoveryIsDeterministic();
    void discoveryThresholdsGate();
    void discoverySkipsDegenerateAndDepth();
    void fixtureSheetTooSmallForStatistics();
};

// Oracle 4：夹具统计值与直算一致（GR：整数等差，直算可得精确值）。
void OutsourceStatsTests::curveStatisticsMatchDirectComputation()
{
  const CurveSheetStats stats = curveSheetStatistics( syntheticCurveSheet() );
  QVERIFY2( stats.ok, qPrintable( stats.error ) );
  QCOMPARE( stats.depthColumn, QStringLiteral( "深度" ) );
  QCOMPARE( stats.columns.size(), 5 ); // GR/孔隙度/声波/无关列/常数列（短列不足 3 行）
  const CurveColumnStats gr = stats.columns.at( 0 );
  QCOMPARE( gr.name, QStringLiteral( "GR" ) );
  QCOMPARE( gr.rowCount, 12 );
  QCOMPARE( gr.badCells, 0 );
  QCOMPARE( gr.mean, 72.5 );
  QCOMPARE( gr.median, 72.5 );
  QCOMPARE( gr.min, 45.0 );
  QCOMPARE( gr.max, 100.0 );
  QVERIFY( gr.hasDepth );
  QCOMPARE( gr.depthMin, 1000.0 );
  QCOMPARE( gr.depthMax, 1110.0 );
  const CurveColumnStats porosity = stats.columns.at( 1 );
  QCOMPARE( porosity.median, 0.1275 );
  QVERIFY( porosity.min < 0.1001 && porosity.min > 0.0999 );
  QVERIFY( porosity.max < 0.1551 && porosity.max > 0.1549 );
}

// 数值行不足的列如实列 issue，不进统计、不冒充全读。
void OutsourceStatsTests::curveStatisticsHonestAboutSparseColumns()
{
  const CurveSheetStats stats = curveSheetStatistics( syntheticCurveSheet() );
  bool sawSparseIssue = false;
  for ( const QString &issue : stats.issues )
  {
    if ( issue.contains( QStringLiteral( "短列" ) ) && issue.contains( QStringLiteral( "不足" ) ) )
      sawSparseIssue = true;
  }
  QVERIFY( sawSparseIssue );

  WorkbookSheet headerless;
  headerless.name = QStringLiteral( "空表" );
  const CurveSheetStats empty = curveSheetStatistics( headerless );
  QVERIFY( !empty.ok );
}

// Oracle 4：相关对候选（GR↔孔隙度 r=1；GR↔声波强负相关；无关对被阈值滤掉）。
void OutsourceStatsTests::discoveryFindsCorrelatedPairs()
{
  const FactorDiscovery discovery = discoverFactorCandidates( syntheticCurveSheet() );
  QVERIFY2( discovery.ok, qPrintable( discovery.error ) );
  // 曲线列 5 个 → C(5,2)=10 对；常数列占 4 对（degenerate），评估 10 对。
  QCOMPARE( discovery.pairsConsidered, 10 );
  QCOMPARE( discovery.pairsSkippedDegenerate, 4 );
  QVERIFY( discovery.candidates.size() >= 2 );
  const FactorCandidate top = discovery.candidates.at( 0 );
  QCOMPARE( top.columnA, QStringLiteral( "GR" ) );
  QCOMPARE( top.columnB, QStringLiteral( "孔隙度" ) );
  QCOMPARE( top.correlation, 1.0 ); // 两列同为行号的一次函数 → 完全正相关
  QCOMPARE( top.pairedRows, 12 );
  bool sawAcoustic = false;
  for ( const FactorCandidate &candidate : discovery.candidates )
  {
    const bool acoustic = ( candidate.columnA == QStringLiteral( "GR" ) &&
                            candidate.columnB == QStringLiteral( "声波" ) ) ||
                          ( candidate.columnA == QStringLiteral( "声波" ) &&
                            candidate.columnB == QStringLiteral( "GR" ) );
    if ( acoustic )
    {
      sawAcoustic = true;
      QVERIFY( candidate.correlation < -0.9 );
      QVERIFY( candidate.correlation >= -1.0 );
    }
    QVERIFY2( candidate.columnA != QStringLiteral( "无关列" ),
              "无关列不得进候选（阈值 0.7）" );
  }
  QVERIFY( sawAcoustic );
}

// Oracle 4：同输入同输出（两次调用逐条一致）。
void OutsourceStatsTests::discoveryIsDeterministic()
{
  const FactorDiscovery first = discoverFactorCandidates( syntheticCurveSheet() );
  const FactorDiscovery second = discoverFactorCandidates( syntheticCurveSheet() );
  QCOMPARE( first.candidates.size(), second.candidates.size() );
  for ( int i = 0; i < first.candidates.size(); ++i )
  {
    QCOMPARE( first.candidates.at( i ).columnA, second.candidates.at( i ).columnA );
    QCOMPARE( first.candidates.at( i ).columnB, second.candidates.at( i ).columnB );
    QCOMPARE( first.candidates.at( i ).correlation, second.candidates.at( i ).correlation );
  }
}

// 阈值口径：配对样本下沿 / 相关强度下沿各自生效。
void OutsourceStatsTests::discoveryThresholdsGate()
{
  const WorkbookSheet sheet = syntheticCurveSheet();
  const FactorDiscovery sparse = discoverFactorCandidates( sheet, 0.7, 13 );
  QVERIFY( sparse.ok );
  QVERIFY( sparse.candidates.isEmpty() );
  QCOMPARE( sparse.pairsSkippedSparse, 6 ); // 非常数对的 6 对全被配对数滤掉

  const FactorDiscovery strict = discoverFactorCandidates( sheet, 0.999 );
  QVERIFY( strict.ok );
  QCOMPARE( strict.candidates.size(), 1 ); // 只剩 r=1 的完全相关对
  QCOMPARE( strict.candidates.at( 0 ).columnB, QStringLiteral( "孔隙度" ) );
}

// 深度列不进候选；常数列相关无定义（degenerate 计数，不虚报 0 相关）。
void OutsourceStatsTests::discoverySkipsDegenerateAndDepth()
{
  const FactorDiscovery discovery = discoverFactorCandidates( syntheticCurveSheet() );
  for ( const FactorCandidate &candidate : discovery.candidates )
  {
    QVERIFY( candidate.columnA != QStringLiteral( "深度" ) );
    QVERIFY( candidate.columnB != QStringLiteral( "深度" ) );
  }
}

// 真实外委夹具（测井曲线表只有 2 行）：统计如实报「没有可统计的曲线列」，
// 发现面给「曲线列不足两列」注记——不虚构候选。
void OutsourceStatsTests::fixtureSheetTooSmallForStatistics()
{
  const WorkbookReadResult workbook = readWorkbook( fixturePath( QStringLiteral( "wg1_well.xml" ) ) );
  QVERIFY2( workbook.ok, qPrintable( workbook.error ) );
  QCOMPARE( workbook.sheets.size(), 2 );
  const CurveSheetStats stats = curveSheetStatistics( workbook.sheets.at( 0 ) );
  QVERIFY( stats.ok );
  QVERIFY( stats.columns.isEmpty() );
  bool sawNote = false;
  for ( const QString &issue : stats.issues )
  {
    if ( issue.contains( QStringLiteral( "没有可统计的曲线列" ) ) )
      sawNote = true;
  }
  QVERIFY( sawNote );
  const FactorDiscovery discovery = discoverFactorCandidates( workbook.sheets.at( 0 ) );
  QVERIFY( discovery.ok );
  QVERIFY( discovery.candidates.isEmpty() );
  QVERIFY( !discovery.notes.isEmpty() );
}

QTEST_MAIN( OutsourceStatsTests )
#include "tst_io_outsource_stats.moc"
