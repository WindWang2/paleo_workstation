// 层：视图
// token 例外：DESIGN 数据符号例外：年代地层色标与标准年代配色，UI 主题不得改写。（tools/ui-token-exceptions.json 精确计数）。
#include "chronostratcolors.h"

#include <QMap>

namespace WellComposite
{

namespace {

// ICS 国际年代地层色标（规整化十六进制；按系收录 14 系 + 40 统/期 + 8 区域组）
struct SeriesDef
{
  const char *name;
  const char *color;
};

struct SystemDef
{
  const char *name;
  const char *color;
  const SeriesDef *series;
  int seriesCount;
};

// 每系的统表（中国区域地质通用译名；色值取 ICS 官方色系规整化）
static const SeriesDef kQuaternary[] = {
    {"全新统", "#F9F97F"}, {"更新统", "#F7F980"}};
static const SeriesDef kNeogene[] = {
    {"上新统", "#FCE97F"}, {"中新统", "#FFE27B"}, {"早中新统", "#FFE97B"}};
static const SeriesDef kPaleogene[] = {
    {"渐新统", "#FDC57A"}, {"始新统", "#FDB46C"}, {"古新统", "#FD9A52"}};
static const SeriesDef kCretaceous[] = {
    {"上白垩统", "#7FC64E"}, {"下白垩统", "#83BB68"}};
static const SeriesDef kJurassicSeries[] = {
    {"上侏罗统", "#33B679"}, {"中侏罗统", "#41C087"}, {"下侏罗统", "#53CA9C"}};
static const SeriesDef kTriassicSeries[] = {
    {"上三叠统", "#812B92"}, {"中三叠统", "#9A45A7"}, {"下三叠统", "#B360BC"}};
static const SeriesDef kPermianSeries[] = {
    {"乐平统", "#F04028"}, {"瓜德鲁普统", "#F45B41"}, {"乌拉尔统", "#F8765A"}};
static const SeriesDef kCarboniferousSeries[] = {
    {"宾夕法尼亚亚系", "#97CDE0"}, {"密西西比亚系", "#9DD0E3"}};
static const SeriesDef kDevonianSeries[] = {
    {"上泥盆统", "#CB8C37"}, {"中泥盆统", "#D0A04E"}, {"下泥盆统", "#D5B465"}};
static const SeriesDef kSilurianSeries[] = {
    {"普里多利统", "#B3E1B6"}, {"罗德洛统", "#AFE0B4"}, {"温洛克统", "#ACDFB2"}, {"兰多维列统", "#A9DEB0"}};
static const SeriesDef kOrdovicianSeries[] = {
    {"上奥陶统", "#009270"}, {"中奥陶统", "#25A37E"}, {"下奥陶统", "#4AB58C"}};
static const SeriesDef kCambrianSeries[] = {
    {"芙蓉统", "#7FA056"}, {"第十统", "#8BAA66"}, {"第九统", "#97B476"}, {"幸运统", "#A3BE86"}, {"第二统", "#AFC896"}};
static const SeriesDef kPrecambrianSeries[] = {
    {"埃迪卡拉系", "#FE95C9"}, {"成冰系", "#FEC9E4"}, {"拉伸系", "#FEDCF0"},
    {"延展系", "#FEE9F6"}, {"盖层系", "#FEF0FA"}, {"固结系", "#FEF6FD"},
    {"造山系", "#F9F1E8"}, {"层侵系", "#F9F6F1"}, {"成铁系", "#FAFBF7"}};

static const SystemDef kSystems[] = {
    {"第四系", "#F9F97F", kQuaternary, 2},
    {"新近系", "#FCE97F", kNeogene, 3},
    {"古近系", "#FDB46C", kPaleogene, 3},
    {"白垩系", "#7FC64E", kCretaceous, 2},
    {"侏罗系", "#33B679", kJurassicSeries, 3},
    {"三叠系", "#812B92", kTriassicSeries, 3},
    {"二叠系", "#F04028", kPermianSeries, 3},
    {"石炭系", "#97CDE0", kCarboniferousSeries, 2},
    {"泥盆系", "#CB8C37", kDevonianSeries, 3},
    {"志留系", "#B3E1B6", kSilurianSeries, 4},
    {"奥陶系", "#009270", kOrdovicianSeries, 3},
    {"寒武系", "#7FA056", kCambrianSeries, 5},
    {"新元古界", "#FE95C9", kPrecambrianSeries, 3},
    {"中元古界", "#FEC9E4", kPrecambrianSeries + 3, 3},
    {"古元古界", "#FEE9F6", kPrecambrianSeries + 6, 3},
};

// 区域组名 → (系, 统)：珠江口盆地标准序列（退役 H1 if-else 硬编码）
struct FormationDef
{
  const char *formation;
  const char *system;
  const char *series;
};
static const FormationDef kFormations[] = {
    {"粤海组", "新近系", "上新统"},
    {"万山组", "新近系", "上新统"},
    {"韩江组", "新近系", "中新统"},
    {"珠江组", "新近系", "早中新统"},
    {"珠海组", "古近系", "渐新统"},
    {"恩平组", "古近系", "始新统"},
    {"文昌组", "古近系", "始新统"},
    {"神狐组", "古近系", "古新统"},
    {"三水组", "古近系", "古新统"},
    {"大朗山组", "古近系", "始新统"},
    {"华涌组", "古近系", "始新统"},
    {"西布组", "白垩系", "上白垩统"},
    {"苏尼特组", "白垩系", "下白垩统"},
};

const SystemDef *findSystem(const QString &name)
{
  const QString n = name.trimmed();
  for (const auto &s : kSystems)
    if (n == QString::fromUtf8(s.name))
      return &s;
  return nullptr;
}

} // namespace

QColor ChronostratColors::systemColor(const QString &systemName)
{
  const SystemDef *s = findSystem(systemName);
  if (!s)
    return QColor(QStringLiteral("#ECEFF1")); // 未识别：中性灰，不臆造
  return QColor(QString::fromUtf8(s->color));
}

QColor ChronostratColors::seriesColor(const QString &seriesName)
{
  const QString n = seriesName.trimmed();
  for (const auto &s : kSystems)
  {
    for (int i = 0; i < s.seriesCount; ++i)
    {
      if (n == QString::fromUtf8(s.series[i].name))
        return QColor(QString::fromUtf8(s.series[i].color));
    }
  }
  // 回退：所属系色
  const QString sys = systemForSeries(seriesName);
  if (!sys.isEmpty())
    return systemColor(sys);
  return QColor(QStringLiteral("#ECEFF1"));
}

QStringList ChronostratColors::systems()
{
  QStringList list;
  for (const auto &s : kSystems)
    list << QString::fromUtf8(s.name);
  return list;
}

QStringList ChronostratColors::seriesOfSystem(const QString &systemName)
{
  QStringList list;
  const SystemDef *s = findSystem(systemName);
  if (!s)
    return list;
  for (int i = 0; i < s->seriesCount; ++i)
    list << QString::fromUtf8(s->series[i].name);
  return list;
}

QStringList ChronostratColors::allSeries()
{
  QStringList list;
  for (const auto &s : kSystems)
    for (int i = 0; i < s.seriesCount; ++i)
      list << QString::fromUtf8(s.series[i].name);
  return list;
}

QStringList ChronostratColors::formationVocabulary()
{
  QStringList list;
  for (const auto &f : kFormations)
    list << QString::fromUtf8(f.formation);
  return list;
}

void ChronostratColors::lookupByFormation(const QString &formationName, QString *system, QString *series)
{
  const QString n = formationName.trimmed();
  if (system)
    system->clear();
  if (series)
    series->clear();
  for (const auto &f : kFormations)
  {
    if (n == QString::fromUtf8(f.formation))
    {
      if (system)
        *system = QString::fromUtf8(f.system);
      if (series)
        *series = QString::fromUtf8(f.series);
      return;
    }
  }
  // 通用后缀回退：明确含「新近/古近/白垩」等系名的层名
  static const struct
  {
    const char *key;
    const char *system;
    const char *series;
  } kFallbacks[] = {
      {"新近", "新近系", "中新统"}, {"古近", "古近系", ""}, {"白垩", "白垩系", ""},
      {"侏罗", "侏罗系", ""}, {"三叠", "三叠系", ""}, {"二叠", "二叠系", ""},
      {"石炭", "石炭系", ""}, {"泥盆", "泥盆系", ""}, {"志留", "志留系", ""},
      {"奥陶", "奥陶系", ""}, {"寒武", "寒武系", ""}, {"第四", "第四系", ""},
  };
  for (const auto &fb : kFallbacks)
  {
    if (n.contains(QString::fromUtf8(fb.key)))
    {
      if (system)
        *system = QString::fromUtf8(fb.system);
      if (series && *fb.series)
        *series = QString::fromUtf8(fb.series);
      return;
    }
  }
}

QString ChronostratColors::systemForSeries(const QString &seriesName)
{
  const QString n = seriesName.trimmed();
  for (const auto &s : kSystems)
    for (int i = 0; i < s.seriesCount; ++i)
      if (n == QString::fromUtf8(s.series[i].name))
        return QString::fromUtf8(s.name);
  // 早/中/晚 + 系名前缀（如「早中新统」→「新近系」）
  for (const auto &s : kSystems)
  {
    const QString sysName = QString::fromUtf8(s.name);
    const QString sysKey = sysName.chopped(1); // 「系」→「新近」
    if (sysKey.size() >= 2 && n.contains(sysKey))
      return sysName;
  }
  return QString();
}

} // namespace WellComposite
