// 层：数据
// services/algoparamschema.cpp — algo-atom P3：标量表单字段以 AlgorithmSpec.params
// 为单一事实源；本文件只做 UI 富化（中文标签、InList 选项、单位后缀）。
// 图层型端口不在此列（见 algoparamschema.h）。PredictPage 仍读 fieldsFor。
//
// 重要：Spec.complete ≠ PredictPage 可展示。许多 Processing Spec 的 params 仍用
// paramKey() 默认 Double 占位（structural/local_direction/surfer/cartographic…），
// 若直接 knows()=true 会把 STRING/BOOL 键渲成 spinbox。UI 面仅暴露下方 allowlist
// （与 master 五算法 + 已正确分型的 Spec 对齐；constraint_idw 含 ANISO_RATIO）。
#include "algoparamschema.h"

#include "algorithmspec.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

namespace
{

QString trLabel( const char *s )
{
  return QCoreApplication::translate( "AlgorithmParamSchema", s );
}

// PredictPage / AlgorithmParamSchema 动态表单 allowlist。
// Spec 登记可多于本表；未列入者 knows()=false → 空表单（与拆分前一致）。
QSet<QString> uiReadyAlgorithmIds()
{
  static const QSet<QString> k = {
    QStringLiteral( "paleo:paleo_geological_smoothing" ),
    QStringLiteral( "paleo:paleo_constraint_idw" ),
    QStringLiteral( "paleo:paleo_facies_fusion" ),
    QStringLiteral( "paleo:paleo_isopach" ),
    QStringLiteral( "paleo:paleo_facies_polygonize" ),
  };
  return k;
}

// Spec 已有 label 时保留；否则用本表补中文（polygonize 等 Spec 参数 label 为空）。
QHash<QString, QString> labelOverrides()
{
  static const QHash<QString, QString> k = {
    { QStringLiteral( "PASSES" ), trLabel( "滤波次数" ) },
    { QStringLiteral( "FIELD" ), trLabel( "Z 值字段名" ) },
    { QStringLiteral( "FACIES_CODE" ), trLabel( "相代码（元数据）" ) },
    { QStringLiteral( "CELL_SIZE" ), trLabel( "网格单元大小" ) },
    { QStringLiteral( "ANISO_RATIO" ), trLabel( "各向异性比（沿方向线）" ) },
    { QStringLiteral( "NEGATIVE_TO_NODATA" ), trLabel( "负厚度置为无数据" ) },
    { QStringLiteral( "SMOOTH" ), trLabel( "平滑次数（3×3 多数滤波）" ) },
    { QStringLiteral( "MIN_CELLS" ), trLabel( "最小图斑像元数" ) },
    { QStringLiteral( "MIN_FRACTION" ), trLabel( "最小图斑占有效区比例" ) },
    { QStringLiteral( "MIN_AREA" ), trLabel( "最小图斑面积" ) },
    { QStringLiteral( "SIMPLIFY" ), trLabel( "化简容差" ) },
    { QStringLiteral( "BOUNDARY_SMOOTH" ), trLabel( "边界圆润化迭代（Chaikin 切角）" ) },
    { QStringLiteral( "SNAP_TOLERANCE" ), trLabel( "约束吸附距离" ) },
    { QStringLiteral( "ANGLE_TOLERANCE" ), trLabel( "约束角度容差" ) },
    { QStringLiteral( "VALUE_RANGES" ), trLabel( "值域（可选）" ) },
    { QStringLiteral( "TENSION" ), trLabel( "张力" ) },
    { QStringLiteral( "MAX_SWEEPS" ), trLabel( "最大扫描次数" ) },
    { QStringLiteral( "POWER" ), trLabel( "幂" ) },
    { QStringLiteral( "MIN_POINTS" ), trLabel( "最少邻点数" ) },
    { QStringLiteral( "MAX_POINTS" ), trLabel( "最多邻点数" ) },
    { QStringLiteral( "SEARCH_RADIUS" ), trLabel( "搜索半径" ) },
  };
  return k;
}

QHash<QString, QString> suffixOverrides()
{
  static const QHash<QString, QString> k = {
    { QStringLiteral( "MIN_AREA" ), QStringLiteral( " m²" ) },
    { QStringLiteral( "ANGLE_TOLERANCE" ), QStringLiteral( "°" ) },
  };
  return k;
}

void enrichField( AlgorithmParamField *f )
{
  if ( !f )
    return;
  if ( f->label.isEmpty() || f->label == f->key )
  {
    const auto labels = labelOverrides();
    const auto it = labels.constFind( f->key );
    if ( it != labels.cend() )
      f->label = it.value();
  }
  if ( f->suffix.isEmpty() )
  {
    const auto suffixes = suffixOverrides();
    const auto it = suffixes.constFind( f->key );
    if ( it != suffixes.cend() )
      f->suffix = it.value();
  }
  // bool InList：Spec 只钉类型，选项在此补是/否。
  if ( f->type == AlgorithmParamField::InList && f->options.isEmpty()
       && f->key == QStringLiteral( "NEGATIVE_TO_NODATA" ) )
  {
    AlgorithmParamOption no, yes;
    no.label = trLabel( "否" );
    no.value = false;
    yes.label = trLabel( "是" );
    yes.value = true;
    f->options = { no, yes };
    if ( !f->defaultValue.isValid() )
      f->defaultValue = false;
  }
}

QVector<AlgorithmParamField> fieldsFromSpec( const AlgorithmSpec &spec )
{
  QVector<AlgorithmParamField> fields = spec.params;
  for ( AlgorithmParamField &f : fields )
    enrichField( &f );
  return fields;
}

} // namespace

bool AlgorithmParamSchema::knows( const QString &algorithmId )
{
  // UI allowlist only — Spec.complete alone is not enough (stub Double params).
  return uiReadyAlgorithmIds().contains( algorithmId );
}

QVector<AlgorithmParamField> AlgorithmParamSchema::fieldsFor( const QString &algorithmId )
{
  if ( !uiReadyAlgorithmIds().contains( algorithmId ) )
    return {};
  const AlgorithmSpec spec = AlgorithmSpecRegistry::spec( algorithmId );
  if ( spec.complete && spec.engine == QLatin1String( "processing" ) )
    return fieldsFromSpec( spec );
  // fusion：零标量（仅图层型）
  if ( algorithmId == QStringLiteral( "paleo:paleo_facies_fusion" ) )
    return {};
  return {};
}

QStringList AlgorithmParamSchema::registeredAlgorithms()
{
  QStringList ids = uiReadyAlgorithmIds().values();
  ids.sort();
  return ids;
}
