// 层：数据
// services/algoparamschema.cpp — algo-atom P3：标量表单字段以 AlgorithmSpec.params
// 为单一事实源；本文件只做 UI 富化（中文标签、InList 选项、单位后缀）。
// 图层型端口不在此列（见 algoparamschema.h）。PredictPage 仍读 fieldsFor。
#include "algoparamschema.h"

#include "algorithmspec.h"

#include <QCoreApplication>
#include <QHash>

namespace
{

QString trLabel( const char *s )
{
  return QCoreApplication::translate( "AlgorithmParamSchema", s );
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
  const AlgorithmSpec spec = AlgorithmSpecRegistry::spec( algorithmId );
  if ( spec.complete && spec.engine == QLatin1String( "processing" ) )
    return true;
  // 兼容：历史登记的零标量 fusion
  return algorithmId == QStringLiteral( "paleo:paleo_facies_fusion" );
}

QVector<AlgorithmParamField> AlgorithmParamSchema::fieldsFor( const QString &algorithmId )
{
  const AlgorithmSpec spec = AlgorithmSpecRegistry::spec( algorithmId );
  if ( spec.complete && spec.engine == QLatin1String( "processing" ) )
    return fieldsFromSpec( spec );
  if ( algorithmId == QStringLiteral( "paleo:paleo_facies_fusion" ) )
    return {}; // 仅图层型
  return {};
}

QStringList AlgorithmParamSchema::registeredAlgorithms()
{
  QStringList ids;
  for ( const QString &id : AlgorithmSpecRegistry::completeIds() )
  {
    const AlgorithmSpec s = AlgorithmSpecRegistry::spec( id );
    if ( s.engine == QLatin1String( "processing" ) )
      ids.append( id );
  }
  ids.sort();
  return ids;
}
