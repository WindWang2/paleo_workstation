// 层：QGIS 封装
#include "factorstylewriter.h"
#include "qgiserrors_internal.h"

#include <QColor>
#include <QDir>
#include <QFont>
#include <cmath>

#include <qgscolorrampimpl.h>
#include <qgsrasterbandstats.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterinterface.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgslinesymbol.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbollayer.h>
#include <qgspallabeling.h>
#include <qgslabelthinningsettings.h>
#include <qgstextformat.h>
#include <qgstextbuffersettings.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

// 色带语义体系（成对端点色，连续插值；地图域数据符号，非 UI token）：
//   sandthick   砂体厚度  黄→橙棕   （碎屑供给强 → 厚）
//   sandratio   砂地比    黄→棕红   （同族比值，端点更沉）
//   strathick   地层厚度  浅蓝→深蓝 （等厚图惯例：厚者深）
//   poro        孔隙度    蓝→红     （物性渐变）
//   perm        渗透率    紫→黄     （与孔隙度区分的高值亮端）
//   welldist    距井距离  绿→灰     （近井可信 → 绿，远井 → 灰）
//   confidence  预测置信度 红→绿     （低置信红、高置信绿，语义色承载状态）

namespace
{
using paleo::qgis_detail::setError;

  struct RampPreset
  {
    const char *color1;
    const char *color2;
  };

  bool presetFor( const QString &factorId, RampPreset *out )
  {
    if ( factorId == QLatin1String( "sandthick" ) )
      *out = { "#F7E7A1", "#C2570F" };
    else if ( factorId == QLatin1String( "sandratio" ) )
      *out = { "#EFD9A0", "#8C3B0E" };
    else if ( factorId == QLatin1String( "strathick" ) )
      *out = { "#CDE7F6", "#14507F" };
    else if ( factorId == QLatin1String( "poro" ) )
      *out = { "#2E86C1", "#C0392B" };
    else if ( factorId == QLatin1String( "perm" ) )
      *out = { "#5B2C6F", "#E9C46A" };
    else if ( factorId == QLatin1String( "welldist" ) )
      *out = { "#1E8449", "#7F8C8D" };
    else if ( factorId == QLatin1String( "confidence" ) )
      *out = { "#C0392B", "#27AE60" };
    else
      return false;
    return true;
  }

} // namespace

namespace FactorStyleWriter
{

bool applyTo(QgsRasterLayer *layer,const QString &factorId)
{
  if(!layer || !layer->isValid())return false;
  auto *ramp=rampFor(factorId);if(!ramp)return false;
  const auto stats=layer->dataProvider()->bandStatistics(1);
  double low=stats.minimumValue,high=stats.maximumValue;
  if(!std::isfinite(low) || !std::isfinite(high) || low>high){delete ramp;return false;}
  if(low==high)high=low+1;
  auto *renderer=new QgsSingleBandPseudoColorRenderer(layer->dataProvider(),1,nullptr);
  renderer->setClassificationMin(low);renderer->setClassificationMax(high);
  renderer->createShader(ramp,Qgis::ShaderInterpolationMethod::Linear,Qgis::ShaderClassificationMethod::Continuous,5);
  layer->setRenderer(renderer);layer->triggerRepaint();return true;
}

QgsColorRamp *rampFor( const QString &factorId )
{
  RampPreset p;
  if ( !presetFor( factorId, &p ) )
    return nullptr;
  return new QgsGradientColorRamp( QColor( p.color1 ), QColor( p.color2 ) );
}

QVariantMap presetDescription( const QString &factorId )
{
  RampPreset p;
  QVariantMap m;
  if ( !presetFor( factorId, &p ) )
    return m;
  m.insert( QStringLiteral( "color1" ), p.color1 );
  m.insert( QStringLiteral( "color2" ), p.color2 );
  m.insert( QStringLiteral( "interpolation" ), QStringLiteral( "linear" ) );
  return m;
}

QString writeStyleQml( const QString &factorId, const QString &rasterPath,
                       const QString &styleDir, QString *error )
{
  RampPreset preset;
  if ( !presetFor( factorId, &preset ) )
  {
    setError( error, QStringLiteral( "no color-ramp preset for factor '%1'" ).arg( factorId ) );
    return QString();
  }
  if ( styleDir.isEmpty() )
  {
    setError( error, QStringLiteral( "no style dir configured — style not written to disk" ) );
    return QString();
  }
  if ( rasterPath.isEmpty() )
  {
    setError( error, QStringLiteral( "no raster path to build the renderer against" ) );
    return QString();
  }

  QgsRasterLayer layer( rasterPath, QStringLiteral( "factor_style_tmp" ), QStringLiteral( "gdal" ) );
  if ( !layer.isValid() )
  {
    setError( error, QStringLiteral( "cannot open raster '%1' for styling: %2" )
                          .arg( rasterPath, layer.error().message() ) );
    return QString();
  }

  applyTo(&layer,factorId);

  if ( !QDir().mkpath( styleDir ) )
  {
    setError( error, QStringLiteral( "cannot create style dir '%1'" ).arg( styleDir ) );
    return QString();
  }
  const QString qmlPath = QDir( styleDir ).filePath(
      QStringLiteral( "factor_%1.qml" ).arg( factorId ) );
  bool ok = false;
  const QString status = layer.saveNamedStyle( qmlPath, ok );
  if ( !ok )
  {
    setError( error, QStringLiteral( "saveNamedStyle('%1') failed: %2" )
                          .arg( qmlPath, status.isEmpty() ? QStringLiteral( "unknown error" ) : status ) );
    return QString();
  }
  return qmlPath;
}

} // namespace FactorStyleWriter

bool FactorStyleWriter::applyContours(QgsVectorLayer *layer) {
  if (!layer || !layer->isValid() || layer->geometryType() != Qgis::GeometryType::Line) return false;
  const QColor ink(QStringLiteral("#24303E"));
  auto symbol = QgsLineSymbol::createSimple({{QStringLiteral("line_color"), QStringLiteral("#FFFFFF")},
      {QStringLiteral("line_width"), QString::number(contourCasingWidthMm)}});
  auto inner = QgsLineSymbol::createSimple({{QStringLiteral("line_color"), ink.name()},
      {QStringLiteral("line_width"), QString::number(contourWidthMm)}});
  symbol->appendSymbolLayer(inner->takeSymbolLayer(0));
  layer->setRenderer(new QgsSingleSymbolRenderer(symbol.release()));
  const QString field = layer->fields().lookupField(QStringLiteral("level")) >= 0 ? QStringLiteral("level") : QStringLiteral("ELEV");
  if (layer->fields().lookupField(field) < 0) { layer->triggerRepaint(); return true; }
  QgsPalLayerSettings labels;
  labels.fieldName = QStringLiteral("format_number(\"%1\", 2)").arg(field);
  labels.isExpression = true;
  labels.placement = Qgis::LabelPlacement::Curved;
  labels.repeatDistance = contourLabelRepeatMm;
  labels.repeatDistanceUnit = Qgis::RenderUnit::Millimeters;
  labels.thinningSettings().setMinimumFeatureSize(contourMinimumLengthMm);
  QgsTextFormat format; format.setFont(QFont(QStringLiteral("JetBrains Mono")));
  format.setSize(contourLabelSizePt); format.setSizeUnit(Qgis::RenderUnit::Points); format.setColor(ink);
  QgsTextBufferSettings buffer; buffer.setEnabled(true); buffer.setSize(contourLabelBufferMm);
  buffer.setSizeUnit(Qgis::RenderUnit::Millimeters); buffer.setColor(Qt::white); format.setBuffer(buffer);
  labels.setFormat(format); layer->setLabeling(new QgsVectorLayerSimpleLabeling(labels));
  layer->setLabelsEnabled(true); layer->triggerRepaint(); return true;
}
