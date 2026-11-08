#include <QtTest>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QTemporaryDir>

#include <cmath>
#include <limits>

#include "../src/qgis/mapbooklayout.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/samplepointstyle.h"

#include <qgscoordinatereferencesystem.h>
#include <qgsexpression.h>
#include <qgsexpressioncontext.h>
#include <qgsexpressioncontextutils.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsvectorlayer.h>

// P5-W1/W2 契约测试：井点因子样点表达——数值/身份格式化 oracle、语义身份
// 核对、标注表达式与 C++ 契约同源（屏幕与导出一致性的可断言保证）、真实
// 点层的样式套用与只读性（不改源 feature 数值）、以及真实版面 PNG 导出
// （表达组件在导出链路上可渲染）。
//
// 不覆盖：mappingworkbench/datapreview 两处 3 行薄接线（编译期约束 + 评审
// 阅读；组件本身即两处的全部语义）；样式视觉观感（DESIGN.md 评审项）。
class TestSamplePointPresentation : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
      QVERIFY( PaleoSamplePointStyle::ensureFunctionsRegistered() );
    }

    // ---- 数值行契约（验收矩阵）----
    void factorValueFormatting()
    {
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 0.23 ), QStringLiteral( "0.23" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 12.00 ), QStringLiteral( "12" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 12.126 ), QStringLiteral( "12.13" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( QVariant() ), QStringLiteral( "无数据" ) );
      // 负数与舍入边界：-12.126 → -12.13；0.005 → 0.01（half away from zero）。
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( -12.126 ), QStringLiteral( "-12.13" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 0.005 ), QStringLiteral( "0.01" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( -0.005 ), QStringLiteral( "-0.01" ) );
      // 负零统一：-0.001 舍入到两位是 -0.00 → 显示 0，不是 "-0"。
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( -0.001 ), QStringLiteral( "0" ) );
      // 很小/很大数值：小于半个分位的进 0；≥1e15 转科学计数保真（不丢位）。
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 1e-9 ), QStringLiteral( "0" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 2e15 ), QStringLiteral( "2e+15" ) );
      // 非有限值：NaN/±Inf 都是无数据，不印 "nan"/"inf"。
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue(
                    std::numeric_limits<double>::quiet_NaN() ), QStringLiteral( "无数据" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue(
                    std::numeric_limits<double>::infinity() ), QStringLiteral( "无数据" ) );
      // 比值保持原口径：0.23 不乘 100（不是 23%）。
      QVERIFY( !PaleoSamplePointStyle::formatFactorValue( 0.23 ).contains( QLatin1Char( '%' ) ) );
      // 非数值输入不装懂。
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( QStringLiteral( "abc" ) ),
                QStringLiteral( "无数据" ) );
    }

    // 区域设置无关：德语 locale（逗号小数点）下仍是 C 风格输出。
    void factorValueFormattingIsLocaleIndependent()
    {
      const QLocale prior = QLocale();
      QLocale::setDefault( QLocale::German );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 0.23 ), QStringLiteral( "0.23" ) );
      QCOMPARE( PaleoSamplePointStyle::formatFactorValue( 12.126 ), QStringLiteral( "12.13" ) );
      QLocale::setDefault( prior );
    }

    // ---- 井身份行契约 ----
    void wellIdentityFallback()
    {
      QCOMPARE( PaleoSamplePointStyle::formatWellIdentity( QStringLiteral( " W1井 " ),
                                                            QStringLiteral( "w1" ) ),
                QStringLiteral( "W1井" ) );
      // 缺 well_name → well_id 合法回退。
      QCOMPARE( PaleoSamplePointStyle::formatWellIdentity( QVariant(), QStringLiteral( "w2" ) ),
                QStringLiteral( "w2" ) );
      QCOMPARE( PaleoSamplePointStyle::formatWellIdentity( QStringLiteral( "  " ),
                                                            QStringLiteral( "w3" ) ),
                QStringLiteral( "w3" ) );
      // 都缺 → 无数据（不借 fid/行号冒充）。
      QCOMPARE( PaleoSamplePointStyle::formatWellIdentity( QVariant(), QVariant() ),
                QStringLiteral( "无数据" ) );
    }

    // ---- 语义身份核对 ----
    void identityCheckIsFieldBased()
    {
      QgsVectorLayer points( QStringLiteral( "Point" ), QStringLiteral( "wf" ),
                             QStringLiteral( "memory" ) );
      points.dataProvider()->addAttributes(
        { QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "well_name" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) } );
      points.updateFields();
      QVERIFY( PaleoSamplePointStyle::isWellFactorPoints( &points ) );

      // 缺 factor_value：不是井点因子层——不允许套用（防借其他字段冒充因子值）。
      QgsVectorLayer noValue( QStringLiteral( "Point" ), QStringLiteral( "nv" ),
                              QStringLiteral( "memory" ) );
      noValue.dataProvider()->addAttributes(
        { QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "name" ), QMetaType::Type::QString ) } );
      noValue.updateFields();
      QVERIFY( !PaleoSamplePointStyle::isWellFactorPoints( &noValue ) );

      // 只有 well_name（无 well_id）也成立——两个识别字段之一即可。
      QgsVectorLayer nameOnly( QStringLiteral( "Point" ), QStringLiteral( "no" ),
                               QStringLiteral( "memory" ) );
      nameOnly.dataProvider()->addAttributes(
        { QgsField( QStringLiteral( "well_name" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) } );
      nameOnly.updateFields();
      QVERIFY( PaleoSamplePointStyle::isWellFactorPoints( &nameOnly ) );

      // 非点层（面）即使字段齐全也不套用。
      QgsVectorLayer polygons( QStringLiteral( "Polygon" ), QStringLiteral( "pg" ),
                               QStringLiteral( "memory" ) );
      polygons.dataProvider()->addAttributes(
        { QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) } );
      polygons.updateFields();
      QVERIFY( !PaleoSamplePointStyle::isWellFactorPoints( &polygons ) );
    }

    // ---- 标注表达式 = C++ 契约（屏幕/导出同源的证明）----
    void labelExpressionEvaluatesToContract()
    {
      QgsVectorLayer points( QStringLiteral( "Point" ), QStringLiteral( "wf" ),
                             QStringLiteral( "memory" ) );
      points.dataProvider()->addAttributes(
        { QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "well_name" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) } );
      points.updateFields();

      const QString expression = PaleoSamplePointStyle::labelExpression( &points );
      QVERIFY2( !expression.isEmpty(), qPrintable( expression ) );
      // 数值行只来自 factor_value：表达式引用 paleo_format_sample_value 与
      // factor_value 列，不出现 fid/首列冒充。
      QVERIFY( expression.contains( QStringLiteral( "paleo_format_sample_value" ) ) );
      QVERIFY( expression.contains( QStringLiteral( "\"factor_value\"" ) ) );
      QVERIFY( !expression.contains( QStringLiteral( "fid" ) ) );

      struct Row
      {
          QString wellId, wellName;
          QVariant value;
          QString expected;
      };
      const Row rows[] = {
        { QStringLiteral( "w1" ), QStringLiteral( "W1井" ), 0.23, QStringLiteral( "W1井\n0.23" ) },
        { QStringLiteral( "w2" ), QString(), 12.00, QStringLiteral( "w2\n12" ) },
        { QStringLiteral( "w3" ), QStringLiteral( "W3" ), 12.126, QStringLiteral( "W3\n12.13" ) },
        { QStringLiteral( "w4" ), QStringLiteral( "W4" ), QVariant(), QStringLiteral( "W4\n无数据" ) },
        { QStringLiteral( "w5" ), QString(), QVariant(), QStringLiteral( "w5\n无数据" ) },
      };
      QgsFeatureList features;
      for ( const Row &row : rows )
      {
        QgsFeature feature( points.fields() );
        feature.setAttribute( QStringLiteral( "well_id" ), row.wellId );
        feature.setAttribute( QStringLiteral( "well_name" ), row.wellName );
        feature.setAttribute( QStringLiteral( "factor_value" ), row.value );
        feature.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 1.0, 2.0 ) ) );
        features << feature;
      }
      QVERIFY( points.dataProvider()->addFeatures( features ) );

      int row = 0;
      QgsFeature feature;
      QgsFeatureIterator it = points.getFeatures();
      while ( it.nextFeature( feature ) )
      {
        QgsExpressionContext context;
        context.appendScopes(
          QgsExpressionContextUtils::globalProjectLayerScopes( &points ) );
        context.setFeature( feature );
        // 标签引擎口径：每 feature 一个已 prepare 的求值上下文（表达式
        // 实例不跨 context 复用求值缓存）。
        QgsExpression parser( expression );
        QVERIFY2( parser.isValid(), qPrintable( parser.parserErrorString() ) );
        QVERIFY2( parser.prepare( &context ), qPrintable( parser.parserErrorString() ) );
        const QString actual = parser.evaluate( &context ).toString();
        QCOMPARE( actual, rows[row].expected );
        ++row;
      }
      QCOMPARE( row, 5 );
    }

    // ---- 样式套用：只改渲染，不改源数值 ----
    void applyLabelsIsRenderOnlyAndIdempotent()
    {
      QgsVectorLayer points( QStringLiteral( "Point" ), QStringLiteral( "wf" ),
                             QStringLiteral( "memory" ) );
      points.dataProvider()->addAttributes(
        { QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "well_name" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) } );
      points.updateFields();
      QgsFeature feature( points.fields() );
      feature.setAttribute( QStringLiteral( "well_id" ), QStringLiteral( "w1" ) );
      feature.setAttribute( QStringLiteral( "well_name" ), QStringLiteral( "W1" ) );
      feature.setAttribute( QStringLiteral( "factor_value" ), 0.23 );
      feature.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 0.0, 0.0 ) ) );
      QVERIFY( points.dataProvider()->addFeature( feature ) );

      QString error;
      QVERIFY( PaleoSamplePointStyle::applyWellFactorLabels( &points, &error ) );
      QVERIFY( points.labelsEnabled() );
      QVERIFY( points.labeling() != nullptr );
      // 幂等：重复套用即「恢复默认样点表达」。
      QVERIFY( PaleoSamplePointStyle::applyWellFactorLabels( &points, &error ) );

      // 原属性不变：没有被格式化污染的数值副本。
      const QgsFeature reloaded = points.getFeature( 1 );
      QCOMPARE( reloaded.attribute( QStringLiteral( "factor_value" ) ).toDouble(), 0.23 );
      QCOMPARE( reloaded.attribute( QStringLiteral( "well_name" ) ).toString(),
                QStringLiteral( "W1" ) );

      // 身份核对不过 → false + 不支持说明（不静默降级）。
      QgsVectorLayer plain( QStringLiteral( "Point" ), QStringLiteral( "plain" ),
                            QStringLiteral( "memory" ) );
      QString mismatchError;
      QVERIFY( !PaleoSamplePointStyle::applyWellFactorLabels( &plain, &mismatchError ) );
      QVERIFY2( mismatchError.contains( QStringLiteral( "不套用" ) ),
                qPrintable( mismatchError ) );
    }

    // ---- 表达组件在真实导出链路上可渲染（P5-W8 图件检查的自动化面）----
    void styledLayerRendersIntoTilePng()
    {
      QTemporaryDir dir;
      QVERIFY2( dir.isValid(), qPrintable( dir.errorString() ) );

      QgsVectorLayer points( QStringLiteral( "Point" ), QStringLiteral( "wf" ),
                             QStringLiteral( "memory" ) );
      points.setCrs( QgsCoordinateReferenceSystem::fromEpsgId( 32650 ) );
      points.dataProvider()->addAttributes(
        { QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "well_name" ), QMetaType::Type::QString ),
          QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) } );
      points.updateFields();
      QgsFeatureList features;
      for ( int i = 0; i < 4; ++i )
      {
        QgsFeature feature( points.fields() );
        feature.setAttribute( QStringLiteral( "well_id" ),
                              QStringLiteral( "w%1" ).arg( i ) );
        feature.setAttribute( QStringLiteral( "well_name" ),
                              QStringLiteral( "井%1" ).arg( i ) );
        feature.setAttribute( QStringLiteral( "factor_value" ), 0.23 + i );
        feature.setGeometry( QgsGeometry::fromPointXY(
          QgsPointXY( 1000.0 + i * 100.0, 1000.0 + ( i % 2 ) * 50.0 ) ) );
        features << feature;
      }
      QVERIFY( points.dataProvider()->addFeatures( features ) );
      QString styleError;
      QVERIFY( PaleoSamplePointStyle::applyWellFactorLabels( &points, &styleError ) );

      QgsProject project;
      project.setCrs( points.crs() );
      // 版面地图项按 spec.layers 渲染——图层不入 QgsProject（其会接管栈对象
      // 所有权导致析构双删），与 buildTileLayout 的图层集口径一致。

      PaleoMapBookLayout::TileSpec spec;
      spec.title = QStringLiteral( "样点表达导出核验" );
      spec.footer = QStringLiteral( "CRS EPSG:32650" );
      spec.extent = QgsRectangle( 900.0, 900.0, 1400.0, 1150.0 );
      spec.layers = { &points };
      QString buildError;
      QgsPrintLayout *layout = PaleoMapBookLayout::buildTileLayout( &project, spec, &buildError );
      QVERIFY2( layout != nullptr, qPrintable( buildError ) );

      const QString outPath = QDir( dir.path() ).filePath( QStringLiteral( "tile.png" ) );
      const auto outcome = PaleoMapBookLayout::renderLayout(
        layout, outPath, PaleoLayoutExport::Format::Png, 96.0 );
      delete layout;
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QVERIFY( outcome.bytes > 0 );
      QVERIFY2( outcome.width > 0 && outcome.height > 0,
                "PNG 应带回像素尺寸（标签已随图层渲染）" );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  // QgisRuntime 自建 QgsApplication（GUI 档 = QApplication 级实例）——调用方
  // 不得先构造 QCoreApplication（二次构造在 Qt 内 abort）。
  if ( !QgisRuntime::initialize() )
    qFatal( "QgisRuntime::initialize failed" );
  TestSamplePointPresentation tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_samplepoint_presentation.moc"
