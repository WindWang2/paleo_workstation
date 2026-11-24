// 层：数据
// services/geostaticapabilityspec.h — I1 地统计原子的算法/能力契约工厂。
//
// 普通克里金与 SGS 的 v1 AlgorithmSpec 服务既有 UI 工作流（自动拟合变差、
// 屏障、MEMBERS 表描述），保持原语义不动。Agent 面需要显式变差参数与封闭
// 词表，按「新增 exact-version 描述」登记 v2；能力行只引用 v2，旧 v1 不被
// 偷换。参数默认值的单一来源仍是 AlgorithmSpec（capabilityspec.h 头注契约）。
#pragma once

#include "algorithmspec.h"
#include "capabilityspec.h"

namespace paleo::geostat_atom
{

// Agent 面精确版本：geostat:ordinary_kriging @ contractVersion 2 / geostat:sgs @ 2。
// 身份常量 inline（paleo_ai 的工具 TU 不链 paleo_services，不引入链接面）。
inline QString kKrigingAlgorithmId() { return QStringLiteral( "geostat:ordinary_kriging" ); }
inline QString kSgsAlgorithmId() { return QStringLiteral( "geostat:sgs" ); }
constexpr int kGeostatAtomAlgorithmContractVersion = 2;

// 能力行（capabilityspec.cpp table() 追加）。
CapabilitySpec krigingCapabilitySpec();
CapabilitySpec sgsCapabilitySpec();
inline QString kKrigingCapabilityId() { return QStringLiteral( "geostat.kriging" ); }
inline QString kSgsCapabilityId() { return QStringLiteral( "geostat.sgs" ); }
inline QString kKrigingToolAlias() { return QStringLiteral( "paleo.geostat_kriging" ); }
inline QString kSgsToolAlias() { return QStringLiteral( "paleo.geostat_sgs" ); }

// allSpecs() 追加的 v2 算法描述（ports + 参数默认值/域）。
void appendGeostatAtomAlgorithmSpecs( QVector<AlgorithmSpec> *specs );

// 变差模型封闭词表（与内核 VariogramModelType 一一对应）。
inline QStringList variogramModelVocabulary()
{
  return { QStringLiteral( "spherical" ), QStringLiteral( "exponential" ),
           QStringLiteral( "gaussian" ) };
}

// ---- 样本因子契约（W3：样本值单位来自已有已确认契约，不猜字段名）------
// 只接受单因素词表（SingleFactorRegistry）里「单位确定 + 井点可提取」的因子，
// 单位与字段取词表值。词表单位本身未确认的因子（poro：「小数或 %」）与不可
// 由井点提取的因子（welldist / confidence / 构造面口径的 strathick）首版不
// 开放——须先在既有词表确认单位，再登记进本表；不造新 Data_service 字段。
// 本表是准入清单而非第二份词表：resolveGeostatFactor 在 capture 时逐项对照
// 词表当前值，漂移即拒绝（geostat_sample_unit_unconfirmed）。
struct GeostatFactorContract
{
  QString id;
  QString unit;
  QString field;
};
inline QVector<GeostatFactorContract> geostatFactorContracts()
{
  return { { QStringLiteral( "sandthick" ), QStringLiteral( "m" ), QStringLiteral( "sand_thick" ) },
           { QStringLiteral( "sandratio" ), QStringLiteral( "无量纲" ), QStringLiteral( "sand_ratio" ) },
           { QStringLiteral( "strathick" ), QStringLiteral( "m" ),
             QStringLiteral( "log_layer_thickness_md" ) },
           { QStringLiteral( "perm" ), QStringLiteral( "mD" ), QStringLiteral( "perm" ) } };
}

// factor_id 解析：命中上表且与词表当前值逐项一致 → true，并给出单位/字段。
// code：geostat_factor_unsupported（不在上表）| geostat_sample_unit_unconfirmed。
bool resolveGeostatFactor( const QString &factorId, QString *unit, QString *field, QString *code );

} // namespace paleo::geostat_atom
