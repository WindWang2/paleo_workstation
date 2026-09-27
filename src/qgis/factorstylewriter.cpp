#include "factorstylewriter.h"

#include <QColor>
#include <QDir>

#include <qgscolorrampimpl.h>
#include <qgsrasterbandstats.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterinterface.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgssinglebandpseudocolorrenderer.h>

// 层：QGIS 封装
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

  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }
} // namespace

namespace FactorStyleWriter
{

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

  // Renderer 分类区间取真实像元 min/max（小栅格精确统计）；不可用时退 0..1，
  // 色带本身仍完整落盘，用户可在样式面板里重分类。
  double min = 0.0;
  double max = 1.0;
  if ( QgsRasterDataProvider *provider = layer.dataProvider() )
  {
    const QgsRasterBandStats stats = provider->bandStatistics( 1 );
    if ( stats.minimumValue <= stats.maximumValue &&
         stats.maximumValue > -std::numeric_limits<double>::max() &&
         stats.minimumValue < std::numeric_limits<double>::max() )
    {
      min = stats.minimumValue;
      max = stats.maximumValue;
    }
  }
  if ( !( max > min ) )
  {
    min = 0.0;
    max = 1.0;
  }

  auto *shader = new QgsRasterShader();
  auto *rampShader = new QgsColorRampShader( min, max, rampFor( factorId ),
                                             Qgis::ShaderInterpolationMethod::Linear,
                                             Qgis::ShaderClassificationMethod::Continuous );
  shader->setRasterShaderFunction( rampShader );
  auto *renderer = new QgsSingleBandPseudoColorRenderer( layer.dataProvider(), 1, shader );
  layer.setRenderer( renderer );

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
