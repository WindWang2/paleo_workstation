// 层：数据
#pragma once
#include <QString>
#include <QVariantMap>

// 单因素请求的消费边界与参数指纹。视图和功能层都可包含本头，
// 不包含 algorithms，也不持有 QGIS 图层。
// 层：数据
namespace paleo::singlefactor
{

enum class LegacyConstraintRole
{
  HardBarrier,
  DirectionGuide,
  HullClip,
  NotInLegacyEngine
};

// 旧 IDW 只认识 break_line 与 direction_line。新语义不进入凸包，也不改成硬屏障。
// 没有 type 字段时调用方传入空字符串，结果是 HullClip。
inline LegacyConstraintRole legacyConstraintRole( QStringView type )
{
  if ( type == u"break_line" )
    return LegacyConstraintRole::HardBarrier;
  if ( type == u"direction_line" )
    return LegacyConstraintRole::DirectionGuide;
  if ( type == u"interpretive_boundary" || type == u"contour_stop" || type == u"cartographic_detour" )
    return LegacyConstraintRole::NotInLegacyEngine;
  return LegacyConstraintRole::HullClip;
}

// 制图工作场和制图等值线不能进入连续融合、相分类或厚度统计。
// 缺省 value_source 的 single_factor_raster 仍是分析场。
inline bool rejectsQuantitativeUse( QStringView kind, QStringView valueSource )
{
  if ( valueSource == u"cartographic_work" )
    return true;
  return kind == u"single_factor_cartographic_work" || kind == u"single_factor_cartographic_contour";
}

inline bool isAnalysisFactorRaster( QStringView kind, QStringView valueSource )
{
  return kind == u"single_factor_raster" && !rejectsQuantitativeUse( kind, valueSource );
}

// 制图成果使用独立图层：id 前缀 cartographic.，或组 04_SingleFactor/Cartographic。
// 分析场仍是 factor.<层位>.<因素>，组就是 04_SingleFactor。
inline bool isCartographicProductLayer( QStringView layerId, QStringView group )
{
  if ( layerId.startsWith( u"cartographic." ) )
    return true;
  return group == u"04_SingleFactor/Cartographic" ||
         group.startsWith( u"04_SingleFactor/Cartographic/" );
}

// 键按 UTF-8 字节序排序。数值用 general/17。NaN 与 Inf 拒绝。数组顺序保留。
// 成功时 canonical 是不含耗时、路径和创建时间的规范化 JSON；sha256 是其十六进制摘要。
struct ParameterHash
{
  bool ok = false;
  QString error;
  QByteArray canonical;
  QString sha256;
};

ParameterHash parameterHash( const QVariantMap &parameters );

} // namespace paleo::singlefactor
