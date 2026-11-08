// 层：QGIS 封装
#pragma once
#include <QString>
#include <QVariant>

class QgsVectorLayer;

// qgis/samplepointstyle — 井点因子样点表达组件（P5-W1）。
//
// 表达契约（单一事实源，屏幕与导出同一条路径）：
//   1. 井身份行：well_name（trim）优先，缺失回退 well_id，都缺给「无数据」
//      文案——不借 fid/id/行号冒充井名。
//   2. 数值行：只来自 factor_value。NULL / 非有限（NaN/±Inf）→「无数据」；
//      其余四舍五入到两位（half away from zero），舍入后为整数去小数
//      （12.00 → 12），负零统一为 0（-0.001 → 0）；比值保持原口径，
//      不自动乘 100；区域设置无关（C 风格小数点，不分组）。
//   3. 渲染只读：不写 feature、不落被格式化污染的数值副本。数值行经
//      自定义表达式函数 paleo_format_sample_value 进 QgsPalLayerSettings，
//      与 C++ formatFactorValue 同一实现（导出/屏幕一致性的可断言保证）。
//   4. 身份核对是语义的：点几何 + factor_value 字段 + well_name/well_id
//      之一。声明前缀（samples./wellfactor.）只是辅助，字段不齐就不套用
//      样点表达，也不让普通矢量走这套规则。
namespace PaleoSamplePointStyle
{

// 数值行契约（P5 验收矩阵的 oracle）。NULL/非法/非有限 → tr("无数据")。
QString formatFactorValue( const QVariant &value );
// 井身份行契约：well_name 优先、well_id 回退、都缺 → tr("无数据")。
QString formatWellIdentity( const QVariant &wellName, const QVariant &wellId );

// 表达式函数名（QgsPalLayerSettings 的数值行经它取值；注册幂等）。
QString valueFunctionName();
bool ensureFunctionsRegistered();

// 语义身份核对：点图层 + factor_value 字段 + well_name/well_id 之一。
bool isWellFactorPoints( const QgsVectorLayer *layer );

// 双行标注表达式（身份行 || '\n' || 数值行）；非井点因子层返回空串。
QString labelExpression( const QgsVectorLayer *layer );

// 套用样点表达（井位点标记 + 双行标注，QGIS 标签引擎避让）。
// 身份核对不过 → false + *error（不支持说明，不静默降级到通用矢量规则）。
// 幂等：重复调用即「恢复默认样点表达」入口（用户显式样式被覆盖后可再回来）。
bool applyWellFactorLabels( QgsVectorLayer *layer, QString *error = nullptr );

} // namespace PaleoSamplePointStyle
