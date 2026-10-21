// 层：数据
#include "lithologystyle.h"

#include <QRegularExpression>
#include <QtGlobal>

namespace WellComposite::LithologyStyle
{
namespace
{
QVector<WidthRule> &globalRules()
{
  static QVector<WidthRule> rules = defaultWidthRules();
  return rules;
}
} // namespace

QVector<WidthRule> defaultWidthRules()
{
  // 1/5 ~ 5/5 五级，粒度越粗越宽；化学岩/特殊岩性取中间档以示区分。
  return {
      {QStringLiteral("煤"), 0.2},
      {QStringLiteral("页岩"), 0.2},
      {QStringLiteral("泥岩"), 0.2},
      {QStringLiteral("膏岩"), 0.3},
      {QStringLiteral("石膏"), 0.3},
      {QStringLiteral("盐岩"), 0.3},
      {QStringLiteral("粉砂岩"), 0.4},
      {QStringLiteral("泥灰岩"), 0.4},
      {QStringLiteral("凝灰岩"), 0.4},
      {QStringLiteral("细砂岩"), 0.6},
      {QStringLiteral("砂岩"), 0.6},
      {QStringLiteral("灰岩"), 0.6},
      {QStringLiteral("白云岩"), 0.6},
      {QStringLiteral("中砂岩"), 0.7},
      {QStringLiteral("粗砂岩"), 0.8},
      {QStringLiteral("含砾砂岩"), 0.9},
      {QStringLiteral("砾岩"), 1.0},
  };
}

QVector<WidthRule> widthRules() { return globalRules(); }

void setWidthRules(const QVector<WidthRule> &rules)
{
  globalRules() = rules.isEmpty() ? defaultWidthRules() : rules;
}

double defaultWidthFraction() { return 0.6; }

namespace
{
const WidthRule *bestRule(const QString &name, const QVector<WidthRule> &rules)
{
  const WidthRule *best = nullptr;
  int bestLen = 0, bestPos = -1;
  for (const WidthRule &r : rules)
  {
    if (r.keyword.isEmpty()) continue;
    const int pos = name.lastIndexOf(r.keyword);
    if (pos < 0) continue;
    const int len = r.keyword.size();
    if (len > bestLen || (len == bestLen && pos > bestPos))
    {
      best = &r; bestLen = len; bestPos = pos;
    }
  }
  return best;
}
} // namespace

double widthFraction(const QString &lithoName, const QVector<WidthRule> &rules)
{
  const QVector<WidthRule> &table = rules.isEmpty() ? globalRules() : rules;
  const WidthRule *r = bestRule(lithoName.trimmed(), table);
  return r ? qBound(0.05, r->fraction, 1.0) : defaultWidthFraction();
}

QString matchedWidthKeyword(const QString &lithoName, const QVector<WidthRule> &rules)
{
  const QVector<WidthRule> &table = rules.isEmpty() ? globalRules() : rules;
  const WidthRule *r = bestRule(lithoName.trimmed(), table);
  return r ? r->keyword : QString();
}

QColor colorFromName(const QString &lithoName, QString *colorWord)
{
  // 复合色在前（最长优先）；必须以「色」收尾。
  static const QVector<QPair<QString, QColor>> words = {
      {QStringLiteral("灰黑"), QColor(QStringLiteral("#3C3C3C"))},
      {QStringLiteral("深灰"), QColor(QStringLiteral("#616161"))},
      {QStringLiteral("浅灰"), QColor(QStringLiteral("#CFCFCF"))},
      {QStringLiteral("灰白"), QColor(QStringLiteral("#E8E8E8"))},
      {QStringLiteral("灰绿"), QColor(QStringLiteral("#8FA98A"))},
      {QStringLiteral("黄绿"), QColor(QStringLiteral("#B5C46A"))},
      {QStringLiteral("紫红"), QColor(QStringLiteral("#8E3A59"))},
      {QStringLiteral("棕红"), QColor(QStringLiteral("#9C4A33"))},
      {QStringLiteral("褐红"), QColor(QStringLiteral("#8D3B2B"))},
      {QStringLiteral("灰褐"), QColor(QStringLiteral("#8A7865"))},
      {QStringLiteral("黄褐"), QColor(QStringLiteral("#A98448"))},
      {QStringLiteral("棕褐"), QColor(QStringLiteral("#7A5435"))},
      {QStringLiteral("土黄"), QColor(QStringLiteral("#D2B160"))},
      {QStringLiteral("浅黄"), QColor(QStringLiteral("#F3E3A0"))},
      {QStringLiteral("黑"), QColor(QStringLiteral("#1E1E1E"))},
      {QStringLiteral("灰"), QColor(QStringLiteral("#A0A0A0"))},
      {QStringLiteral("白"), QColor(QStringLiteral("#FAFAFA"))},
      {QStringLiteral("红"), QColor(QStringLiteral("#C0392B"))},
      {QStringLiteral("紫"), QColor(QStringLiteral("#7B4B94"))},
      {QStringLiteral("褐"), QColor(QStringLiteral("#795548"))},
      {QStringLiteral("棕"), QColor(QStringLiteral("#8D6E63"))},
      {QStringLiteral("黄"), QColor(QStringLiteral("#E6C84F"))},
      {QStringLiteral("绿"), QColor(QStringLiteral("#5B8C5A"))},
      {QStringLiteral("蓝"), QColor(QStringLiteral("#4F7CAC"))},
  };
  const QString name = lithoName.trimmed();
  int earliest = -1;
  QColor found;
  QString foundWord;
  for (const auto &w : words)
  {
    const int pos = name.indexOf(w.first + QStringLiteral("色"));
    if (pos < 0) continue;
    // 最先出现者优先；同位置按表序（复合色在前）
    if (earliest < 0 || pos < earliest)
    {
      earliest = pos; found = w.second; foundWord = w.first;
    }
  }
  if (colorWord) *colorWord = foundWord;
  return found;
}

QColor muteColor(const QColor &c)
{
  if (!c.isValid()) return c;
  float h = 0, s = 0, l = 0, a = 1;
  c.getHslF(&h, &s, &l, &a);
  if (s < kNeutralSaturation || h < 0) return c; // 黑/白/灰保持中性
  s = float(qMin<double>(s, kMutedMaxSaturation));
  l = float(qBound(kMutedMinLightness, double(l), kMutedMaxLightness));
  return QColor::fromHslF(h, s, l, a);
}

QColor resolveFillColor(const QString &lithoName, const QColor &fallback)
{
  const QColor c = colorFromName(lithoName);
  return muteColor(c.isValid() ? c : fallback);
}

double heightInQuanta(double thickness, double quantum)
{
  if (!(quantum > 0.0)) return 0.0;
  return thickness / quantum;
}
} // namespace WellComposite::LithologyStyle
