// 层：数据
#include "singlefactordef.h"

#include <QStringList>

// PALEO_QGIS_PLAN.md §10 内置单因素词表（固定顺序）。processingAlgId 分级
//（mapping 主线6）：
//   · 已接入引擎——IDW 五项（sandthick/sandratio/strathick 之外的孔渗）
//     走 paleo:paleo_constraint_idw（C++ 嵌入注册表真实存在）；
//     strathick 走 paleo:paleo_isopach（INPUT_TOP/INPUT_BASE 双构造面栅格，
//     工作流侧参数整形见 ConstraintWorkflow::generateFactor）。
//   · 冻结契约的未接入引擎——welldist 需要 src/algorithms/ 侧的距离变换
//     算法（数据方向版图），confidence 需要 ONNX 真实置信度面（当前
//     onnxpredictionservice 只读首个输出张量，无置信度通道）。二者的
//     引擎 id + 参数键契约已冻结（SingleFactorContracts），生成链显式
//     拒绝（不静默降级成 IDW——那会产出语义错误的栅格）；实现侧按契约
//     接入即可点亮。契约全文见 docs/progress/mapping.md。

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

QString SingleFactorContracts::welldistEngineId()
{
  // 契约：INPUT=井点图层（距离源），OUTPUT=栅格目的地，CELL_SIZE=正数。
  // 语义：逐像元到最近井点的绕障距离（距离变换，障碍=约束线）。
  return QStringLiteral( "paleo:paleo_distance_transform" );
}

QString SingleFactorContracts::confidenceEngineId()
{
  // 契约：INPUT=预测结果栅格，OUTPUT=栅格目的地。
  // 语义：预测置信度面（直取模型置信度通道——当前 ONNX 会话只读首个输出
  // 张量，接入前置=onnxpredictionservice 暴露真实置信度输出）。
  return QStringLiteral( "paleo:paleo_confidence_surface" );
}

QVector<SingleFactorDefinition> SingleFactorRegistry::builtins()
{
  return {
    makeDef( QStringLiteral( "sandthick" ), QStringLiteral( "砂体厚度" ),
             QStringLiteral( "wells" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "sand_thick" ) ),
    makeDef( QStringLiteral( "sandratio" ), QStringLiteral( "砂地比" ),
             QStringLiteral( "wells" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "sand_ratio" ) ),
    // 主线6：strathick 接入真等厚引擎——顶/底构造面相减（paleo:paleo_isopach），
    // 参数键 topLayerId/baseLayerId（声明图层 id，页面选择、工作流整形）。
    [&]() {
      SingleFactorDefinition d = makeDef( QStringLiteral( "strathick" ),
                                          QStringLiteral( "地层厚度" ),
                                          QStringLiteral( "horizon" ),
                                          QStringLiteral( "顶−底构造面等厚" ),
                                          QStringLiteral( "thickness" ) );
      d.processingAlgId = QStringLiteral( "paleo:paleo_isopach" );
      d.defaultParams.insert( QStringLiteral( "topLayerId" ), QString() );
      d.defaultParams.insert( QStringLiteral( "baseLayerId" ), QString() );
      d.defaultParams.insert( QStringLiteral( "negativeToNodata" ), true );
      return d;
    }(),
    makeDef( QStringLiteral( "poro" ), QStringLiteral( "孔隙度" ),
             QStringLiteral( "well_log" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "poro" ) ),
    makeDef( QStringLiteral( "perm" ), QStringLiteral( "渗透率" ),
             QStringLiteral( "well_log" ), QStringLiteral( "IDW 井点插值" ),
             QStringLiteral( "perm" ) ),
    // blocked（主线6）：距离变换算法属数据方向（src/algorithms/）——契约已冻结
    //（SingleFactorContracts::welldistEngineId），生成链显式拒绝。
    [&]() {
      SingleFactorDefinition d = makeDef( QStringLiteral( "welldist" ),
                                          QStringLiteral( "距井距离" ),
                                          QStringLiteral( "wells" ),
                                          QStringLiteral( "距离变换（待接入）" ),
                                          QStringLiteral( "welldist" ) );
      d.processingAlgId = SingleFactorContracts::welldistEngineId();
      return d;
    }(),
    // blocked（主线6）：ONNX 只读首个输出张量，无真实置信度面——契约已冻结
    //（SingleFactorContracts::confidenceEngineId），生成链显式拒绝。
    [&]() {
      SingleFactorDefinition d = makeDef( QStringLiteral( "confidence" ),
                                          QStringLiteral( "预测置信度" ),
                                          QStringLiteral( "prediction" ),
                                          QStringLiteral( "置信度直取（待接入）" ),
                                          QStringLiteral( "confidence" ) );
      d.processingAlgId = SingleFactorContracts::confidenceEngineId();
      return d;
    }(),
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
