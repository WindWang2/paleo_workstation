// 层：功能
#pragma once
#include <QVariantMap>

#include "domain/stratigraphicunit.h"

class SelectionContext;

// services/ — 分析任务入口的 UnitSelection 桥接。
// 调用方禁止自行拆分「T70–T71」字符串；一律经本桥接从 SelectionContext
// 取 UnitSelection，再得到约束图层键（父单元上界面）与分析成果键（layerToken）。
namespace paleo::strat
{

struct AnalysisBinding
{
  UnitSelection unit;
  QString constraintHorizon; // constraints.<top> / 井因子等编辑态资产
  QString analysisKey;       // factor.<token>.* / 分析成果图层键
  bool hasUnit = false;

  // 把 UnitSelection 写入任务参数（identity 字段）；不覆盖调用方已有键以外的业务参数。
  QVariantMap enrich( QVariantMap params ) const;
};

// pageHorizonFallback = 页内 combo 当前文本（可能是旧界面标签）。
// 优先 activeUnit；无效时 analysisKey/constraintHorizon 都回落 pageHorizon 或 activeHorizon。
AnalysisBinding bindAnalysis( const SelectionContext *selection, const QString &pageHorizonFallback = {} );

// 证据/图层声明是否属于当前分析选择（稳定 ID 或父单元上界面或 activeHorizon）。
bool matchesActiveAnalysis( const SelectionContext *selection, const QString &horizonOrToken );

} // namespace paleo::strat
