// 层：功能
#include "boundaryeditrules.h"

#include "boundarysemantics.h"

#include <QObject>

// workflow/boundaryeditrules.cpp — 规则实现（词面/类型 id 均出自冻结词表；
// 拒绝原因给证据值，不静默修正）。

namespace BoundarySemantics
{

namespace
{
QString codeLabel( int code )
{
  return code < 0 ? QObject::tr( "未知" ) : QString::number( code );
}
} // namespace

BoundaryEditVerdict checkKindAssignment( const BoundaryEditFacts &facts )
{
  BoundaryEditVerdict verdict;
  const QString kind = facts.kind;
  if ( kind.isEmpty() )
    return verdict; // 未分类是合法态（中性放行）

  if ( !isKnownKind( kind ) )
  {
    verdict.accepted = false;
    verdict.reason = QObject::tr( "未知相界类型「%1」——词表仅有 整合接触/尖灭/相变/断层切割" )
                         .arg( kind );
    return verdict;
  }

  if ( kind == QLatin1String( kConformable ) && facts.adjacentFaciesDiffers )
  {
    verdict.accepted = false;
    verdict.reason = QObject::tr( "整合接触边界不切割两侧相——该要素（相代码 %1）与相邻相带"
                                  "（相代码 %2）直接接触，两侧相不同；请改用相变或断层切割" )
                         .arg( codeLabel( facts.selfFaciesCode ),
                               codeLabel( facts.adjacentFaciesCode ) );
    return verdict;
  }
  return verdict;
}

BoundaryEditVerdict checkTransitionBand( const BoundaryEditFacts &facts )
{
  BoundaryEditVerdict verdict;
  if ( facts.transitionWidth < 0 )
  {
    verdict.accepted = false;
    verdict.reason = QObject::tr( "渐变带宽度不能为负：%1" ).arg( facts.transitionWidth );
    return verdict;
  }
  if ( facts.transitionWidth > 0 && facts.kind != QLatin1String( kFaciesChange ) )
  {
    verdict.accepted = false;
    verdict.reason = facts.kind.isEmpty()
                         ? QObject::tr( "渐变带仅相变边界可携带——请先选择相界类型「相变」" )
                         : QObject::tr( "渐变带仅相变边界可携带（当前类型「%1」）" )
                               .arg( titleFor( facts.kind ) );
    return verdict;
  }
  return verdict;
}

BoundaryEditVerdict checkRingClosure( const BoundaryEditFacts &facts )
{
  BoundaryEditVerdict verdict;
  if ( facts.ringClosed )
    return verdict;
  if ( facts.kind == QLatin1String( kPinchout ) )
    return verdict; // 尖灭：允许单边相带终止（开放端）
  if ( facts.kind.isEmpty() )
    return verdict; // 未分类不判（环缺口归 QA UnclosedRing 报告）
  verdict.accepted = false;
  verdict.reason = QObject::tr( "「%1」边界不允许开放端（尖灭允许单侧终止）——"
                                "请闭合边界环或把类型改为尖灭" )
                       .arg( titleFor( facts.kind ) );
  return verdict;
}

} // namespace BoundarySemantics
