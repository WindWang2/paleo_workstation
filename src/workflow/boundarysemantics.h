// 层：功能
#pragma once
#include <QString>
#include <QStringList>

// workflow/boundarysemantics.h — 相界地质语义类型词表（TODOS P2「相界地质
// 语义类型」，wave/deepen-perf C2）。相界不只是 polygon 边：整合接触/尖灭/
// 相变/断层切割影响编辑行为与图面表达。存储值进相多边形图层 boundary_kind
// 字段（saveFaciesAttributes 词表），显示名供页面/图例使用。
//
// 落地节奏 = 「先单类型跑通」：词表全集在此冻结，激活子集（activeKinds）
// 当前只有断层切割——编辑钩子（页面下拉 + 属性回写）与符号映射
//（QgisStyleService::applyFaciesBoundaryStyle 的 fault_cut 类目）按激活
// 子集开放；其余类型词面保留、UI 不放开（地质专家参与的类型差异化编辑
// 行为属后续轮次，见 TODOS 该条 Depends on）。
// 层：功能
namespace BoundarySemantics
{

// 存储值（稳定 id，落 boundary_kind 字段）。
inline constexpr auto kConformable = "conformable";   // 整合接触
inline constexpr auto kPinchout = "pinchout";         // 尖灭
inline constexpr auto kFaciesChange = "facies_change"; // 相变
inline constexpr auto kFaultCut = "fault_cut";        // 断层切割（首发单类型）

// 全词表（固定顺序 = 规范图式目录顺序）。
inline QStringList kinds()
{
  return { QString::fromLatin1( kConformable ), QString::fromLatin1( kPinchout ),
           QString::fromLatin1( kFaciesChange ), QString::fromLatin1( kFaultCut ) };
}

// 当前激活的编辑/符号子集（单类型跑通；扩类型时在此登记）。
inline QStringList activeKinds()
{
  return { QString::fromLatin1( kFaultCut ) };
}

// 显示名（未知/空 → 原样返回）。
inline QString titleFor( const QString &kind )
{
  if ( kind == QLatin1String( kConformable ) )
    return QStringLiteral( "整合接触" );
  if ( kind == QLatin1String( kPinchout ) )
    return QStringLiteral( "尖灭" );
  if ( kind == QLatin1String( kFaciesChange ) )
    return QStringLiteral( "相变" );
  if ( kind == QLatin1String( kFaultCut ) )
    return QStringLiteral( "断层切割" );
  return kind;
}

} // namespace BoundarySemantics
