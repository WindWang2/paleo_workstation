// 层：数据
#include "singlefactordef.h"

#include <QStringList>
#include <cmath>

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
    d.defaultParams.insert(QStringLiteral("valueRange"),
                          id == QLatin1String("sandratio") ? QStringLiteral("ratio_0_1") : QStringLiteral("data"));
    return d;
  }
} // namespace

FactorMapDefaults SingleFactorRegistry::mapDefaults(const QString &factorId, double observedMin,
                                                     double observedMax)
{
  FactorMapDefaults result;
  result.unit = geologicalProfile(factorId).unit;
  if (factorId == QLatin1String("sandratio") || factorId == QLatin1String("confidence")) {
    result.maximum = 1.0;
    result.contourInterval = 0.1;
    result.hasRange = true;
    return result;
  }
  if (!std::isfinite(observedMin) || !std::isfinite(observedMax) || observedMin > observedMax)
    return result;
  result.minimum = observedMin;
  result.maximum = observedMax;
  if (factorId == QLatin1String("sandthick") || factorId == QLatin1String("strathick")) {
    if (observedMin < 0.0) return result;
    result.minimum = 0.0;
    const double scale = observedMax > 0.0 ? std::pow(10.0, std::floor(std::log10(observedMax))) : 1.0;
    const double value = observedMax / scale;
    result.maximum = (value <= 1.0 ? 1.0 : value <= 2.0 ? 2.0 : value <= 5.0 ? 5.0 : 10.0) * scale;
  }
  if (result.minimum == result.maximum)
    result.maximum = result.minimum + 1.0;
  const double raw = (result.maximum - result.minimum) / 6.0;
  const double scale = std::pow(10.0, std::floor(std::log10(raw)));
  const double value = raw / scale;
  result.contourInterval = (value < 1.5 ? 1.0 : value < 3.5 ? 2.0 : value < 7.5 ? 5.0 : 10.0) * scale;
  result.hasRange = std::isfinite(result.contourInterval) && result.contourInterval > 0.0;
  return result;
}

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
    // welldist：绕障距离引擎已落地（paleo:paleo_distance_transform，
    // wave/deepen-perf C5）——契约 SingleFactorContracts::welldistEngineId
    // 即引擎注册 id，生成链分派见 workflows::generateDistanceFactor。
    [&]() {
      SingleFactorDefinition d = makeDef( QStringLiteral( "welldist" ),
                                          QStringLiteral( "距井距离" ),
                                          QStringLiteral( "wells" ),
                                          QStringLiteral( "绕障距离变换" ),
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

SingleFactorDefinition SingleFactorRegistry::byId( const QString &factorId, bool *ok, const QString &source )
{
  const QVector<SingleFactorDefinition> all = builtins();
  for ( const SingleFactorDefinition &d : all )
  {
    if ( d.factorId == factorId )
    {
      if ( ok )
        *ok = true;
      if (factorId == QLatin1String("strathick") && source == QLatin1String("wells"))
        return makeDef(factorId, d.title, QStringLiteral("wells"),
                       QStringLiteral("井分层厚度（MD）插值"), QStringLiteral("log_layer_thickness_md"));
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

FactorGeologicalProfile SingleFactorRegistry::geologicalProfile( const QString &factorId, bool *ok, const QString &source )
{
  const auto setOk = [&]( bool v ) {
    if ( ok )
      *ok = v;
  };
  FactorGeologicalProfile p;
  p.factorId = factorId;
  if ( factorId == QLatin1String( "sandthick" ) )
  {
    p.unit = QStringLiteral( "m" );
    p.validRange = QStringLiteral( "[0, +∞) m" );
    p.ruleSentence = QStringLiteral( "采用解释砂厚（MD，同一层段）" );
    p.note = QStringLiteral( "只认解释岩性段；GR 截断等推断岩性不得当作已确认砂厚。" );
    p.supportsAutoMode = true;
    setOk( true );
    return p;
  }
  if ( factorId == QLatin1String( "sandratio" ) )
  {
    p.unit = QStringLiteral( "无量纲" );
    p.validRange = QStringLiteral( "[0, 1]" );
    p.ruleSentence = QStringLiteral( "采用解释砂厚 ÷ 地层厚度（MD，同一层段）" );
    p.note = QStringLiteral( "砂厚与层厚须同一层位与深度基准；分母为零、缺值或比值越出 [0,1] 的井排除。"
                             "优先直读已维护的砂地比字段，否则按厚度相除；不用 GR 切砂冒充解释。" );
    p.supportsAutoMode = true;
    setOk( true );
    return p;
  }
  if ( factorId == QLatin1String( "strathick" ) )
  {
    if (source == QLatin1String("wells")) {
      p.unit = QStringLiteral("m");
      p.validRange = QStringLiteral("(0, +∞) m（MD）");
      p.ruleSentence = QStringLiteral("优先读取已维护层厚，否则按井分层底深减顶深（MD，同层段）");
      p.note = QStringLiteral("仅使用有效、正向的同井分层厚度；不与构造面高程差混用。");
      p.supportsAutoMode = true;
      setOk(true);
      return p;
    }
    p.unit = QStringLiteral( "m" );
    p.validRange = QStringLiteral( "(0, +∞) m（负值→NoData）" );
    p.ruleSentence = QStringLiteral( "采用顶−底构造面等厚（不走井点插值）" );
    p.note = QStringLiteral( "与井点砂厚/砂地比流程分开；负厚度按契约置为 NoData。" );
    p.wellPointExtraction = false;
    setOk( true );
    return p;
  }
  if ( factorId == QLatin1String( "poro" ) )
  {
    p.unit = QStringLiteral( "小数或 %" );
    p.validRange = QStringLiteral( "按工程字段口径" );
    p.ruleSentence = QStringLiteral( "采用井点孔隙度属性直读（登记字段）" );
    p.note = QStringLiteral( "只读工程已维护字段（如 poro/porosity/PHIE）；不静默把无关助记符当成孔隙度。" );
    p.supportsAutoMode = true;
    setOk( true );
    return p;
  }
  if ( factorId == QLatin1String( "perm" ) )
  {
    p.unit = QStringLiteral( "mD" );
    p.validRange = QStringLiteral( "[0, +∞) mD" );
    p.ruleSentence = QStringLiteral( "采用井点渗透率属性直读（登记字段）" );
    p.note = QStringLiteral( "只读工程已维护字段（如 perm/permeability）；负值不参与。" );
    p.supportsAutoMode = true;
    setOk( true );
    return p;
  }
  if ( factorId == QLatin1String( "welldist" ) )
  {
    p.unit = QStringLiteral( "m" );
    p.validRange = QStringLiteral( "[0, +∞) m" );
    p.ruleSentence = QStringLiteral( "采用绕障距离变换（不走井点插值）" );
    p.note = QStringLiteral( "与井点因素提取流程分开；障碍来自硬屏障约束线。" );
    p.wellPointExtraction = false;
    setOk( true );
    return p;
  }
  if ( factorId == QLatin1String( "confidence" ) )
  {
    p.unit = QStringLiteral( "[0, 1]" );
    p.validRange = QStringLiteral( "[0, 1]" );
    p.ruleSentence = QStringLiteral( "采用预测置信度面（待接入真实通道）" );
    p.wellPointExtraction = false;
    setOk( true );
    return p;
  }
  setOk( false );
  return p;
}

bool SingleFactorRegistry::isImplemented(const QString &factorId) {
  bool known = false; byId(factorId, &known);
  return known && factorId != QLatin1String("confidence");
}
