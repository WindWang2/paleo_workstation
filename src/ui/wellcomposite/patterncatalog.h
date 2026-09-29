// 层：视图
#pragma once

#include <QBrush>
#include <QColor>
#include <QPixmap>
#include <QString>
#include <QStringList>

#include <QPair>

// ui/wellcomposite/patterncatalog — D4.2/D4.3 花纹库扩充与外置映射
//
// D4.2：岩性花纹 ≥30 种（砂/粉砂/泥/灰岩/白云岩/膏/煤/砾/凝灰岩/玄武岩/
//   油页岩/礁/滩…），程序化纹理为主；图例文档 docs/wellcomposite/PATTERNS.md。
// D4.3：相名→花纹映射外置 JSON 资源（resources/faciespatterns.json 内置 +
//   QStandardPaths 用户目录覆盖合并）；用户映射优先于内置。
//
// LithologyPatternFactory/FaciesPatternFactory 查不到内置键时先经本目录解析。

namespace WellComposite
{

struct PatternDef
{
  QString key;          // 英文键（patternType）
  QStringList nameHints; // 岩性名匹配关键词
  QColor bg;
  QColor fg;
  QString category;     // 图例分组：碎屑岩/碳酸盐岩/蒸发岩/火成岩/有机岩/其他
};

class PatternCatalog
{
public:
  // ≥30 种岩性花纹定义（含画刷生成）
  static const QList<PatternDef> &lithologyPatterns();
  static int lithologyPatternCount();

  // 花纹 pixmap（key 未登记返回空 pixmap）
  static QPixmap createLithoPattern(const QString &key, const QColor &bg, const QColor &fg);

  // 岩性名 → 花纹定义（关键词最长匹配；未命中返回 false）
  static bool lookupLithology(const QString &lithoName, PatternDef *out);

  // ---- D4.3 相名→花纹 JSON 映射 ----
  // 内置资源 + 用户覆盖（QStandardPaths::AppConfigLocation/wellcomposite/
  // faciespatterns.json），用户条目优先；返回 相名/键 → pattern key
  static QHash<QString, QString> faciesPatternMap();
  // 测试/刷新：清除缓存后重载
  static void reloadFaciesMap();
  // 相名 → 花纹 key（映射表命中；否则空）
  static QString faciesPatternKey(const QString &faciesName);
  // 用户映射文件路径（文档/测试）
  static QString userFaciesMapPath();
};

} // namespace WellComposite
