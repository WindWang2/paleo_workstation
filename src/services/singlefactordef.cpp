// 层：功能
#include "singlefactordef.h"

#include <QStringList>

// 层：数据
// PALEO_QGIS_PLAN.md §10 内置单因素词表（固定顺序）。processingAlgId 全部是
// C++ 嵌入运行时真实注册的算法（QgisProcessingService::algorithmIds() 核实，
// 见 tst_factorworkflow）：gdal:grid* 属 Python provider，不进嵌入注册表，
// 因此 v1 生成链统一走 paleo:paleo_constraint_idw（约束 IDW，§11）。
// 非 IDW 引擎（距井距离的距离变换、预测置信度的结果直取）本机未注册，
// algorithm 标签如实标注「待接入」，见任务报告。

namespace
{
  SingleFactorDefinition makeDef( const QString &id, const QString &title,
                                  const QString &input, const QString &algorithm,
                                  const QString &field )
  {
    SingleFactorDefinition d;
    d.factorId = id;
    d.title = title;
    d.inputAssetType = input;
    d.algorithm = algorithm;
    d.processingAlgId = QStringLiteral( "paleo:paleo_constraint_idw" );
    d.styleRef = QStringLiteral( "factor_%1" ).arg( id );
    d.defaultParams.insert( QStringLiteral( "field" ), field );
    d.defaultParams.insert( QStringLiteral( "cellSize" ), 1.0 );
    return d;
  }
} // namespace

QVector<SingleFactorDefinition> SingleFactorRegistry::builtins()
{
  return {
    makeDef( QStringLiteral( "sandthick" ), QStringLiteral( "砂体厚度" ),
             QStringLiteral( "wells" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "sand_thick" ) ),
    makeDef( QStringLiteral( "sandratio" ), QStringLiteral( "砂地比" ),
             QStringLiteral( "wells" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "sand_ratio" ) ),
    makeDef( QStringLiteral( "strathick" ), QStringLiteral( "地层厚度" ),
             QStringLiteral( "wells" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "thickness" ) ),
    makeDef( QStringLiteral( "poro" ), QStringLiteral( "孔隙度" ),
             QStringLiteral( "well_log" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "poro" ) ),
    makeDef( QStringLiteral( "perm" ), QStringLiteral( "渗透率" ),
             QStringLiteral( "well_log" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "perm" ) ),
    makeDef( QStringLiteral( "welldist" ), QStringLiteral( "距井距离" ),
             QStringLiteral( "wells" ), QStringLiteral( "IDW 井点插值（距离变换待接入）" ),
             QStringLiteral( "welldist" ) ),
    makeDef( QStringLiteral( "confidence" ), QStringLiteral( "预测置信度" ),
             QStringLiteral( "prediction" ), QStringLiteral( "IDW 井点插值（预测结果直取待接入）" ),
             QStringLiteral( "confidence" ) ),
  };
}

SingleFactorDefinition SingleFactorRegistry::byId( const QString &factorId, bool *ok )
{
  const QVector<SingleFactorDefinition> all = builtins();
  for ( const SingleFactorDefinition &d : all )
  {
    if ( d.factorId == factorId )
    {
      if ( ok )
        *ok = true;
      return d;
    }
  }
  if ( ok )
    *ok = false;
  return SingleFactorDefinition();
}

QString SingleFactorRegistry::titleFor( const QString &factorId )
{
  bool ok = false;
  const SingleFactorDefinition d = byId( factorId, &ok );
  return ok ? d.title : factorId;
}
