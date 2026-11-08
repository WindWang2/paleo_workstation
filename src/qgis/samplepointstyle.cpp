// 层：QGIS 封装
#include "samplepointstyle.h"

#include "standardelements.h" // 图件字体单一事实源（figureTextFormat 回退链）

#include <QColor>
#include <QObject>
#include <QStringList>

#include <cmath>

#include <qgsexpression.h>
#include <qgsmarkersymbol.h>
#include <qgspallabeling.h>
#include <qgsproperty.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

namespace PaleoSamplePointStyle
{

namespace
{
  // 数值行常量：两位小数、整数去零；|v| ≥ 1e15 时两位小数已无意义且
  // double 尾数噪声主导，转科学计数保真（同一函数路径，屏幕/导出一致）。
  constexpr double kScaleLimit = 1e15;

  // 自定义表达式函数：数值行格式化的表达式侧入口（渲染线程逐 feature 调）。
  // 与 formatFactorValue 同一实现——标注表达式的求值结果即 C++ 契约结果，
  // 「屏幕与导出文本一致」由同源保证，不靠两处格式化代码对齐。
  class FormatSampleValueFunction : public QgsExpressionFunction
  {
    public:
      FormatSampleValueFunction()
        : QgsExpressionFunction( QStringLiteral( "paleo_format_sample_value" ),
                                 QgsExpressionFunction::ParameterList()
                                   << QgsExpressionFunction::Parameter( QStringLiteral( "value" ) ),
                                 QStringLiteral( "Paleo" ),
                                 QString(),      // helpText
                                 false,          // lazyEval
                                 /*handlesNull=*/true )
      {}

      // 纯函数：无副作用、不碰工程状态，渲染/导出线程可安全调用。
      bool isStatic( const QgsExpressionNodeFunction *, QgsExpression *, const QgsExpressionContext * ) const override { return true; }

      QVariant func( const QVariantList &values, const QgsExpressionContext *,
                     QgsExpression *, const QgsExpressionNodeFunction * ) override
      {
        return formatFactorValue( values.value( 0 ) );
      }
  };

  // 字段存在才生成「NULL 安全取列」表达式；缺列返回空串（调用方拼装时跳过）。
  QString columnOrNull( const QgsVectorLayer *layer, const QString &name )
  {
    if ( !layer || layer->fields().lookupField( name ) < 0 )
      return QString();
    return QStringLiteral( "nullif(trim(to_string(%1)), '')" )
      .arg( QgsExpression::quotedColumnRef( name ) );
  }
} // namespace

QString valueFunctionName() { return QStringLiteral( "paleo_format_sample_value" ); }

bool ensureFunctionsRegistered()
{
  static const bool registered = [] {
    auto *fn = new FormatSampleValueFunction();
    return QgsExpression::registerFunction( fn ); // 同名重复注册由 QGIS 拒绝
  }();
  Q_UNUSED( registered );
  return true;
}

QString formatFactorValue( const QVariant &value )
{
  bool ok = false;
  const double raw = value.toDouble( &ok );
  // NULL/字符串/非法 → toDouble 失败；NaN/±Inf → isfinite 拒绝，都给「无数据」。
  if ( !ok || !std::isfinite( raw ) )
    return QObject::tr( "无数据" );
  if ( std::abs( raw ) >= kScaleLimit )
    return QString::number( raw, 'g', 15 );
  // 四舍五入 half away from zero（std::round）；先放大再除回，规避
  // QString::number('f', 2) 在二进制近似值上的偶数舍入差异。
  const double rounded = std::round( raw * 100.0 ) / 100.0;
  if ( rounded == 0.0 )
    return QStringLiteral( "0" ); // 统一负零：-0.001 → 0，不显示 "-0"
  QString text = QString::number( rounded, 'f', 2 );
  if ( text.endsWith( QStringLiteral( ".00" ) ) )
    text.chop( 3 ); // 舍入后为整数去小数：12.00 → 12
  return text;
}

QString formatWellIdentity( const QVariant &wellName, const QVariant &wellId )
{
  const QString name = wellName.toString().trimmed();
  if ( !name.isEmpty() )
    return name;
  const QString id = wellId.toString().trimmed();
  if ( !id.isEmpty() )
    return id;
  return QObject::tr( "无数据" );
}

bool isWellFactorPoints( const QgsVectorLayer *layer )
{
  if ( !layer || layer->geometryType() != Qgis::GeometryType::Point )
    return false;
  if ( layer->fields().lookupField( QStringLiteral( "factor_value" ) ) < 0 )
    return false;
  return layer->fields().lookupField( QStringLiteral( "well_name" ) ) >= 0 ||
         layer->fields().lookupField( QStringLiteral( "well_id" ) ) >= 0;
}

QString labelExpression( const QgsVectorLayer *layer )
{
  if ( !isWellFactorPoints( layer ) )
    return QString();
  QStringList identityArms;
  if ( const QString nameExpr = columnOrNull( layer, QStringLiteral( "well_name" ) ); !nameExpr.isEmpty() )
    identityArms << nameExpr;
  if ( const QString idExpr = columnOrNull( layer, QStringLiteral( "well_id" ) ); !idExpr.isEmpty() )
    identityArms << idExpr;
  identityArms << QgsExpression::quotedString( QObject::tr( "无数据" ) );
  const QString valueExpr = QStringLiteral( "%1( %2 )" )
    .arg( valueFunctionName(),
          QgsExpression::quotedColumnRef( QStringLiteral( "factor_value" ) ) );
  return identityArms.join( QLatin1String( ", " ) ).prepend( QStringLiteral( "coalesce(" ) ) +
         QStringLiteral( ") || '\n' || " ) + valueExpr;
}

bool applyWellFactorLabels( QgsVectorLayer *layer, QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return false;
  };
  if ( !layer )
    return fail( QObject::tr( "没有可套用样点表达的图层" ) );
  if ( !isWellFactorPoints( layer ) )
    return fail( QObject::tr( "图层「%1」不是井点因子层（缺 factor_value 或井识别字段），"
                              "不套用样点表达——因子值不借其他字段冒充" )
                   .arg( layer->name() ) );
  ensureFunctionsRegistered();

  // 井位点标记：与井位层同一套圆点（数据符号色，不入 UI token）。
  QVariantMap props;
  props.insert( QStringLiteral( "name" ), QStringLiteral( "circle" ) );
  props.insert( QStringLiteral( "color" ), QStringLiteral( "#24303E" ) );
  props.insert( QStringLiteral( "outline_color" ), QStringLiteral( "#FFFFFF" ) );
  props.insert( QStringLiteral( "outline_width" ), QStringLiteral( "0.4" ) );
  props.insert( QStringLiteral( "size" ), QStringLiteral( "3" ) );
  layer->setRenderer(
    new QgsSingleSymbolRenderer( QgsMarkerSymbol::createSimple( props ).release() ) );

  // 双行标注：身份行 + 数值行；数值行经 paleo_format_sample_value 取值。
  const QString expression = labelExpression( layer );
  if ( expression.isEmpty() )
    return fail( QObject::tr( "井点因子标注表达式构建失败" ) );
  QgsPalLayerSettings settings;
  settings.isExpression = true;
  settings.fieldName = expression;
  settings.placement = Qgis::LabelPlacement::OrderedPositionsAroundPoint;
  // 字体族走 standardelements 图件字体单一事实源（IBM Plex → Noto Sans SC →
  // CJK 回退链）——硬编码族名在 vendored 字体缺失环境下 PDF 文本无保证
  //（mapbooklayout 同口径，复审 M4）。
  QgsTextFormat format = PaleoStandardElements::figureTextFormat( 9.0 );
  format.setSize( 9 );
  format.setSizeUnit( Qgis::RenderUnit::Points );
  format.setColor( QColor( QStringLiteral( "#24303E" ) ) );
  QgsTextBufferSettings buffer;
  buffer.setEnabled( true );
  buffer.setSize( .8 );
  buffer.setColor( Qt::white );
  format.setBuffer( buffer );
  settings.setFormat( format );
  // 字号随画布比例尺自适应（与 qgisstyleservice 井位标注同一写法）。
  settings.dataDefinedProperties().setProperty(
    QgsPalLayerSettings::Property::Size,
    QgsProperty::fromExpression( QStringLiteral(
      "clamp(6.0, scale_linear(@map_scale, 25000, 500000, 15.0, 6.0), 15.0)" ) ) );
  layer->setLabeling( new QgsVectorLayerSimpleLabeling( settings ) );
  layer->setLabelsEnabled( true );
  layer->triggerRepaint();
  return true;
}

} // namespace PaleoSamplePointStyle
