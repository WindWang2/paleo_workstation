// 层：视图
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

// ui/wellcomposite/chronostratcolors — D4.1 国际年代地层色标表
//
// 国际年代地层表（ICS Chart）色标的中国区域标准对照（GB/T）：
//   系（System）14 单元 + 统（Series）34+ 单元 + 区域组（Formation）映射，
//   合计 ≥40 分色单元。色值取 ICS 官方色系在设计色板约束内的规整化
//   （DESIGN.md：数据符号色由域规则管理，不属 UI token——本表即数据符号域）。
//
// 同时退役 wellcompositetrack.cpp 内「组名→系/统」if-else 硬编码（H1）：
// lookupByFormation() 提供同源查表。

namespace WellComposite
{

class ChronostratColors
{
public:
  // 系色
  static QColor systemColor(const QString &systemName);
  // 统色（缺省回退所属系色减淡）
  static QColor seriesColor(const QString &seriesName);
  // 词表
  static QStringList systems();
  static QStringList seriesOfSystem(const QString &systemName);
  static QStringList allSeries();
  // 区域组名词表（珠江口盆地标准序列，可被 sidecar 指派扩展）
  static QStringList formationVocabulary();
  // 组 → (系, 统)；未登记返回空串（程序不猜，D3.6 由用户显式指派补齐）
  static void lookupByFormation(const QString &formationName, QString *system, QString *series);
  // 系 → 统归属查表（指派对话框级联用）
  static QString systemForSeries(const QString &seriesName);
};

} // namespace WellComposite
