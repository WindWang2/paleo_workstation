// 层：视图
#pragma once
#include "domain/wellsection.h"

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// ui/wellsection — 连井剖面显示模板（井道组成 + 参与连井的分层过滤）与
// 主题（纸面配色）值类型。图件是纸面文档：数据符号色不随 UI 主题翻转
// （DESIGN.md 2026-09-29 决策，同 wellcomposite 画布口径）。
namespace wellsection {

// 井道种类：层段名 | 曲线道 | 深度尺 | 岩性柱 | 相代码充填。
enum class TrackKind { Zone, Curve, Depth, Lithology, Facies };

// 一条曲线的显示式样：label 空 → 用 mnemonic；min/max 为道内刻度范围；
// logScale 时对数值取 log10 归一。
struct CurveStyle {
  QString mnemonic;
  QString label;
  double min = 0.0;
  double max = 150.0;
  bool logScale = false;
  QColor color;
  bool operator==(const CurveStyle &o) const;
  bool operator!=(const CurveStyle &o) const { return !(*this == o); }
};

struct TrackSpec {
  TrackKind kind = TrackKind::Curve;
  QString title;              // 空 → 自动（小层/深度/m/岩性/曲线名拼接）
  int width = 56;             // px，夹取 24..200
  QVector<CurveStyle> curves; // Curve 道 1..3 条叠加（GR/DT/AC 可配道序）
  bool sandFill = false;      // Curve：cutoff 以下自道左缘充填
  double cutoff = 75.0;       // sandFill 与 Lithology 的 GR 截断值
  QString sourceMnemonic = QStringLiteral("GR"); // Lithology 源曲线
  QString displayTitle() const;
  bool operator==(const TrackSpec &o) const;
  bool operator!=(const TrackSpec &o) const { return !(*this == o); }
};

// 参与连井的分层过滤：Mapping = 仅编图层位（domain/mappinghorizons.h）；
// 滤完任何井都无分层时退回全量。All = 全量。Custom = customTops 名单
// （空名单 = 全量）。
enum class TopFilter { Mapping, All, Custom };

struct SectionTemplate {
  QVector<TrackSpec> tracks;
  TopFilter topFilter = TopFilter::Mapping;
  QStringList customTops;

  static SectionTemplate defaults(); // 参考图件：小层|GR|深度|岩性|RD/RS
  QStringList mnemonics() const;     // 去重（大小写不敏感）曲线名 + 岩性源
  int columnWidth() const;           // Σ 道宽
  QJsonObject toJson() const;
  // 坏/缺 → defaults()，ok=false。
  static SectionTemplate fromJson(const QJsonObject &o, bool *ok = nullptr);
  bool operator==(const SectionTemplate &o) const;
  bool operator!=(const SectionTemplate &o) const { return !(*this == o); }
};

// 返回 tops 按模板过滤后的井集（其余字段原样拷贝）。
QVector<Well> filterTops(const QVector<Well> &wells, const SectionTemplate &t);

// 纸面主题：配色只对图件，不进 UI token。
struct SectionTheme {
  QString id, name;
  QColor paper, frame, text, link;
  qreal linkWidth = 1.2;
  bool curvedLinks = true;   // 连线走 S 形贝塞尔；false = 直线
  bool zoneFill = false;     // 层段底色（zoneColor 55% 透明）
  QColor sand, sandDots;     // 砂岩充填 + 点纹（sand 透明 = 只描轮廓）
  QColor lithoSand, lithoShale;
  QColor fault;              // 断层投绘线（缺省 classic #B33A3A）
  bool seismicGray = true;   // true = 灰阶，false = 红白蓝
  qreal seismicOpacity = 0.85;
  static QVector<SectionTheme> presets();         // classic/colored/print
  static SectionTheme byId(const QString &id);    // 未知 → classic
};

// 层段底色：柔和 pastel 色板循环（数据符号色）。
QColor zoneColor(int index);

} // namespace wellsection
