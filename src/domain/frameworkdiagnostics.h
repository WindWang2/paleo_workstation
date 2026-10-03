// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "sequenceframework.h"
#include "wellrecords.h"

// domain/ — 层序地层格架一致性诊断（方向 28 目标 2/4/6）。
// 纯函数面：给定格架 + 层位序 + 井分层 + 编图层位集合，产出可复现的诊断
// 报告。不碰 catalog、不落盘——报告的可导出形态由 toText()/toMarkdown()
// 给出，写入与展示分别是 catalog/frameworkstore 与 ui 的职责。
//
// 术语与判定口径（逐条钉死，避免「诊断器自己漂移」）：
//   · 单元覆盖层位 = horizonsCoveredBy()（顶界含、底界不含，按
//     mappingHorizons() 有序集合）。
//   · 悬空：标志层引用了 well_stratification 里不存在的分层名，或指向了
//     不存在的格架单元。
//   · 孤立井分层：出现在井分层里，既不属于任何标志层，也不落在任何格架
//     单元覆盖的层位集合内。
//   · 格架空洞：单元的覆盖层位集合在任何一口井的分层里都没有落点。
//   · 跨单元重名层：同一个分层名被两个及以上不同单元的标志层引用。

namespace SequenceFramework
{

enum class DiagnosticKind
{
  MarkerDanglingLayer,   // 标志层引用了不存在的井分层名
  MarkerDanglingUnit,    // 标志层指向了不存在的格架单元
  BoundaryOutsideSet,    // 单元顶/底界面不在 mappingHorizons() 集合内
  AmbiguousHorizonUnit,  // 一个层位被多个格架单元覆盖（歧义）
  HorizonWithoutUnit,    // 层位无任何格架单元归属
  UnitWithoutMappingData,// 格架单元无任何编图数据
  FrameworkGap,          // 格架单元全工区无井覆盖
  DuplicateLayerName,    // 同一分层名跨单元重复归属
  OrphanWellLayer        // 孤立井分层（无格架归属）
};

enum class Severity
{
  High,
  Medium,
  Low
};

struct Diagnostic
{
  DiagnosticKind kind = DiagnosticKind::BoundaryOutsideSet;
  Severity severity = Severity::Medium;
  QString code;          // 稳定短码（报告与测试断言都用它，不靠中文串）
  QString title;         // 中文标题
  QString detail;        // 说明（含判定口径要点）
  QStringList locations; // 可定位项（单元路径 / 井名 / 层名）
};

struct DiagnosticInput
{
  Framework framework;
  QStringList horizons;       // mappingHorizons()（浅→深，层位序唯一权威）
  QStringList mappedHorizons; // 有编图数据的层位（sequence_boundary 非未决）
  QVector<WellTopRecord> tops;// 井分层（domain/wellrecords.h）
  QStringList knownLayerNames;// well_stratification 分层名全集（引用完整性面）
};

struct DiagnosticReport
{
  QVector<Diagnostic> items;

  bool isEmpty() const { return items.isEmpty(); }
  int countOf( DiagnosticKind kind ) const;
  int countOf( Severity severity ) const;
  // 可复现文本报告（顺序稳定：kind → locations）。导出面与测试断言共用。
  QString toText() const;
  QString toMarkdown() const;
};

// 主入口。空输入（无格架）如实产出「层位无归属」类诊断——不静默变绿。
DiagnosticReport diagnose( const DiagnosticInput &input );

QString diagnosticCode( DiagnosticKind kind );
QString diagnosticTitle( DiagnosticKind kind );
QString severityToken( Severity severity );

} // namespace SequenceFramework
