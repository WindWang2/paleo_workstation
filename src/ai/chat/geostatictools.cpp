// 层：功能
// ai/chat/geostatictools.cpp — 见 geostatictools.h。
#include "geostatictools.h"
#include "services/geostaticapabilityspec.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <cmath>
#include <limits>

namespace paleo::geostat_atom
{
namespace
{

constexpr double kUnbounded = std::numeric_limits<double>::quiet_NaN();

AiToolSpec capabilityEnvelope( const QString &name, const QString &title,
                               const QString &description )
{
  AiToolSpec spec;
  spec.name = name;
  spec.title = title;
  spec.description = description;
  spec.requiresLocalOrt = false;
  spec.parameters = {
      { QStringLiteral( "contract_version" ), QStringLiteral( "integer" ),
        QObject::tr( "能力契约版本；缺省 1" ), false },
      { QStringLiteral( "client_request_id" ), QStringLiteral( "string" ),
        QObject::tr( "调用方请求号" ), true },
      { QStringLiteral( "binding_mode" ), QStringLiteral( "string" ),
        QObject::tr( "pinned-value、follow-object-head 或 follow-selection" ), true,
        { QStringLiteral( "pinned-value" ), QStringLiteral( "follow-object-head" ),
          QStringLiteral( "follow-selection" ) } },
      { QStringLiteral( "binding_id" ), QStringLiteral( "string" ),
        QObject::tr( "follow-selection 的选择绑定" ), false },
      { QStringLiteral( "frozen_head" ), QStringLiteral( "string" ),
        QObject::tr( "冻结时的对象头" ), false },
      { QStringLiteral( "current_head" ), QStringLiteral( "string" ),
        QObject::tr( "忽略。提交头只来自宿主，模型填写无效" ), false },
      { QStringLiteral( "publish_intent" ), QStringLiteral( "string" ),
        QObject::tr( "prepare：物化样本快照并冻结计划；compute-only：按冻结计划计算；"
                     "publish：发布已计算结果（只给 source_operation_id）" ),
        false,
        { QStringLiteral( "prepare" ), QStringLiteral( "compute-only" ),
          QStringLiteral( "publish" ) } },
      { QStringLiteral( "parameters" ), QStringLiteral( "object" ),
        QObject::tr( "科学参数（形状见本 schema 的 parameters.properties）。默认值只来自 "
                     "AlgorithmSpec，不在工具描述里复制；compute-only 继承时应省略" ),
        true },
      { QStringLiteral( "inputs" ), QStringLiteral( "object" ),
        QObject::tr( "显式输入引用与冻结值（形状见本 schema 的 inputs.properties）" ), true },
  };
  spec.resultSchemaJson = QStringLiteral(
      "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},"
      "\"operationId\":{\"type\":\"string\"}}}" );
  return spec;
}

// 数值参数：NaN 表示无界；exclusive 用 JSON Schema 的 exclusiveMinimum/
// exclusiveMaximum，与宿主解码的严格不等式同义（sill/range/cell_size > 0、
// azimuth < 360）。
QJsonObject numberParam( const QString &description, double minimum, bool minimumExclusive,
                         double maximum, bool maximumExclusive )
{
  QJsonObject schema;
  schema.insert( "type", "number" );
  schema.insert( "description", description );
  if( std::isfinite( minimum ) )
    schema.insert( minimumExclusive ? "exclusiveMinimum" : "minimum", minimum );
  if( std::isfinite( maximum ) )
    schema.insert( maximumExclusive ? "exclusiveMaximum" : "maximum", maximum );
  return schema;
}

QJsonObject integerParam( const QString &description, int minimum, int maximum )
{
  return QJsonObject{ { "type", "integer" },
                      { "description", description },
                      { "minimum", minimum },
                      { "maximum", maximum } };
}

QJsonObject variogramProperties()
{
  QJsonArray models;
  for( const auto &name : variogramModelVocabulary() )
    models.append( name );
  return QJsonObject{
    { "VARIOGRAM_MODEL",
      QJsonObject{ { "type", "string" },
                   { "enum", models },
                   { "description", QObject::tr( "变差模型类型（缺省 spherical）" ) } } },
    { "NUGGET", numberParam( QObject::tr( "块金值（≥0，缺省 0）" ), 0, false, kUnbounded, false ) },
    { "SILL", numberParam( QObject::tr( "部分基台（>0，缺省 1；总基台=块金+部分基台）" ), 0, true,
                           kUnbounded, false ) },
    { "RANGE", numberParam( QObject::tr( "变程（>0 米，缺省 1；首版无自动拟合）" ), 0, true,
                            kUnbounded, false ) },
    { "ANISOTROPY_RATIO",
      numberParam( QObject::tr( "各向异性比（≥1，缺省 1=各向同性）" ), 1, false, kUnbounded, false ) },
    { "AZIMUTH_DEG",
      numberParam( QObject::tr( "长轴方位角（度 [0,360)，缺省 0）" ), 0, false, 360, true ) }
  };
}

QJsonObject gridProperty()
{
  // extent 是样本投影 CRS 下的平面坐标，可正可负（本地工程网格尤然）：
  // 只约束有限与次序（max > min），不设正负下限。
  return QJsonObject{
    { "type", "object" },
    { "description",
      QObject::tr( "显式计算网格（样本投影坐标系，米制）：extent + 像元尺寸；网格覆盖"
                   "整个 extent，跨度非整数倍时东/南边界最多外扩一个像元；不接受任意 "
                   "WKT 或绝对输出路径" ) },
    { "additionalProperties", false },
    { "required", QJsonArray{ "min_x", "max_x", "min_y", "max_y", "cell_size" } },
    { "properties",
      QJsonObject{
        { "min_x", numberParam( QObject::tr( "西边界（米，有限数）" ), kUnbounded, false, kUnbounded, false ) },
        { "max_x", numberParam( QObject::tr( "东边界（米，>min_x）" ), kUnbounded, false, kUnbounded, false ) },
        { "min_y", numberParam( QObject::tr( "南边界（米，有限数）" ), kUnbounded, false, kUnbounded, false ) },
        { "max_y", numberParam( QObject::tr( "北边界（米，>min_y）" ), kUnbounded, false, kUnbounded, false ) },
        { "cell_size", numberParam( QObject::tr( "像元尺寸（米，>0）" ), 0, true, kUnbounded, false ) } } }
  };
}

QJsonObject parameterProperties( bool sgs )
{
  QJsonObject properties = variogramProperties();
  properties.insert( QStringLiteral( "MAX_POINTS" ),
                     integerParam( QObject::tr( "邻域点数上限（缺省 16）" ), 1, 64 ) );
  properties.insert( QStringLiteral( "SEARCH_RADIUS" ),
                     numberParam( QObject::tr( "搜索半径（米，≥0，0=纯最近邻；缺省 0）" ), 0, false,
                                  kUnbounded, false ) );
  if( !sgs )
  {
    properties.insert( QStringLiteral( "MIN_POINTS" ),
                       integerParam( QObject::tr( "半径模式最少邻点（缺省 4；仅 SEARCH_RADIUS>0 "
                                                 "时生效，邻点不足的格点为 NoData）" ),
                                     1, 64 ) );
    return properties;
  }
  properties.insert( QStringLiteral( "REALIZATIONS" ),
                     integerParam( QObject::tr( "实现次数（缺省 4；仅开放 OUTPUT/STD，不交付个体"
                                               "实现成员）" ),
                                   1, 64 ) );
  properties.insert( QStringLiteral( "SEED" ),
                     QJsonObject{ { "type", "string" },
                                  { "description",
                                    QObject::tr( "随机种子（十进制字符串，缺省 \"42\"；同 seed "
                                                 "同输入逐字节复现）" ) } } );
  return properties;
}

QJsonObject inputsProperties()
{
  QJsonArray factors;
  QStringList factorNotes;
  for( const GeostatFactorContract &factor : geostatFactorContracts() )
  {
    factors.append( factor.id );
    factorNotes.append( QObject::tr( "%1（单位 %2，字段 %3）" ).arg( factor.id, factor.unit, factor.field ) );
  }
  return QJsonObject{
    { "sample_ref",
      QJsonObject{ { "type", "string" },
                   { "description",
                     QObject::tr( "受管点样本引用：\"catalog:<versionId>\" 或受管井点层键"
                                  "（如 wells.T70）。宿主解析并核验其与单元的关联，不信任路径"
                                  "自称" ) } } },
    { "factor_id",
      QJsonObject{ { "type", "string" },
                   { "enum", factors },
                   { "description",
                     QObject::tr( "样本因子的单因素词表 id：数值单位与字段取词表已确认值"
                                  "（%1）；词表单位未确认的因子须先在既有词表补齐，本工具不猜"
                                  "单位" ).arg( factorNotes.join( QObject::tr( "；" ) ) ) } } },
    { "field",
      QJsonObject{ { "type", "string" },
                   { "description", QObject::tr( "数值字段名（可省略=因子登记字段；给出时须与"
                                                 "登记字段相同）" ) } } },
    { "unit_selection",
      QJsonObject{ { "type", "object" },
                   { "description",
                     QObject::tr( "分析单元身份（S1–S4 子层段）；宿主做 Data Service 实体"
                                  "核验，Boundary/Interval/aggregate 拒绝。revision 省略=当前"
                                  "头修订，给出即冻结到该修订" ) },
                   { "additionalProperties", false },
                   { "required", QJsonArray{ "schemeId", "intervalId", "subIntervalId" } },
                   { "properties",
                     QJsonObject{ { "schemeId", QJsonObject{ { "type", "string" } } },
                                   { "revision", QJsonObject{ { "type", "string" } } },
                                   { "intervalId", QJsonObject{ { "type", "string" } } },
                                   { "subIntervalId", QJsonObject{ { "type", "string" } } } } } } },
    { "grid", gridProperty() },
    { "source_operation_id",
      QJsonObject{ { "type", "string" },
                   { "description",
                     QObject::tr( "compute-only：先前 prepare 操作的持久 operationId（此时"
                                  "不接受其他业务输入，parameters 应省略）；publish：已计算"
                                  "操作的 operationId（唯一输入）" ) } } }
  };
}

} // namespace

bool isGeostatAtomTool( const QString &name )
{
  return name == kKrigingToolAlias() || name == kSgsToolAlias();
}

QVector<AiToolSpec> geostatAtomToolSpecs()
{
  QVector<AiToolSpec> specs;
  specs.append( capabilityEnvelope(
      kKrigingToolAlias(), QObject::tr( "地统计普通克里金" ),
      QObject::tr( "对受管井点样本做普通克里金插值（OUTPUT 估值 + VARIANCE 克里金方差栅格），"
                   "成果为 catalog 受管版本，不自动显示。无约束地统计范围：不消费工程约束、"
                   "屏障或协变量。prepare 物化样本快照并回包冻结计划（样本数/单位/网格/资源"
                   "估算）；compute-only 按冻结计划计算；publish 仅发布已计算结果。" ) ) );
  specs.append( capabilityEnvelope(
      kSgsToolAlias(), QObject::tr( "序贯高斯模拟" ),
      QObject::tr( "对受管井点样本做固定种子的序贯高斯模拟（OUTPUT 均值 + STD 模拟离散度"
                   "栅格；STD 是实现间的离散程度，不是克里金方差、置信度或通过阈值），成果为 "
                   "catalog 受管版本，不自动显示。首版冻结 persistRealizations=false：不生成 "
                   "MEMBER_n 个体实现文件。无约束地统计范围同克里金工具。" ) ) );
  return specs;
}

QJsonObject geostatAtomToolSchema( const QString &name )
{
  const bool sgs = name == kSgsToolAlias();
  // 信封骨架就地构建（不经 AiToolSpec::toJsonSchema——那会按名委派回本函数）。
  const AiToolSpec envelope = capabilityEnvelope( name, QString(), QString() );
  QJsonObject schema;
  schema.insert( "type", QStringLiteral( "object" ) );
  QJsonObject properties;
  QJsonArray required;
  for( const AiToolParamSpec &parameter : envelope.parameters )
  {
    properties.insert( parameter.name, parameter.toJsonSchema() );
    if( parameter.required )
      required.append( parameter.name );
  }
  auto parameters = properties.value( "parameters" ).toObject();
  parameters.insert( "additionalProperties", false );
  parameters.insert( "properties", parameterProperties( sgs ) );
  properties.insert( "parameters", parameters );
  auto inputs = properties.value( "inputs" ).toObject();
  inputs.insert( "additionalProperties", false );
  inputs.insert( "properties", inputsProperties() );
  properties.insert( "inputs", inputs );
  schema.insert( "properties", properties );
  schema.insert( "required", required );
  schema.insert( "additionalProperties", false );
  return schema;
}

} // namespace paleo::geostat_atom
