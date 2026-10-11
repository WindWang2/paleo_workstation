// 层：数据
#pragma once

#include "../../domain/formationdominance.h"
#include "../../domain/wellcompositemodel.h"

#include <QVector>

// faciesmapping/formationdominance — 一口井的相观测按地层单位统计优势相。
// 复用 computeDominantFacies（同一套「最上优先、重叠不双算、并列如实标记」
// 口径），文字相标签按首次出现（自上而下）编成局部码；只给有相观测的地层出行。
namespace paleo::faciesmapping
{

// formations 为井的地层单位（组、段都可以，各出一行）；spans 为该井的相观测。
// 地层名为空或顶底倒置的跳过；返回顺序与 formations 一致。
QVector<FormationDominance> dominantFaciesByFormation(
  const QVector<WellComposite::FormationInterval> &formations,
  const QVector<FaciesSpan> &spans );

} // namespace paleo::faciesmapping
