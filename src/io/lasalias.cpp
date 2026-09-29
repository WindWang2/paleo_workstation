// 层：数据
#include "lasalias.h"

namespace
{
  QString normalizeCurveKey(const QString &raw)
  {
    // 大写 + 去内部空格/连字符/下划线：DT-4P → DT4P，GR _EDTC → GREdtc…保持
    // 简单：只去空白与分隔符，字母统一大写。
    QString out;
    out.reserve(raw.size());
    for (const QChar c : raw.trimmed())
    {
      if (c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('_'))
        continue;
      out.append(c.toUpper());
    }
    return out;
  }

  QString normalizeUnitKey(const QString &raw)
  {
    QString out;
    out.reserve(raw.size());
    for (const QChar c : raw.trimmed())
    {
      if (c.isSpace() || c == QLatin1Char('/') || c == QLatin1Char('-'))
        continue;
      out.append(c.toLower());
    }
    return out;
  }

  // 去尾部数字修饰（DT35 → DT、GR2 → GR）：仅当去掉后是已知名时才采用。
  QString stripTrailingDigits(const QString &key)
  {
    int end = key.size();
    while (end > 0 && key.at(end - 1).isDigit())
      --end;
    return end > 0 ? key.left(end) : key;
  }
} // namespace

LasAliasMap &LasAliasMap::shared()
{
  static LasAliasMap inst;
  return inst;
}

LasAliasMap::LasAliasMap()
{
  buildTables();
}

void LasAliasMap::buildTables()
{
  QMutexLocker lock(&m_mutex);
  m_curveTable.clear();
  m_unitTable.clear();
  m_familyTable.clear();

  // ---- 曲线族（规范名 → 族）----
  auto fam = [this](const char *canon, const QString &family,
                    std::initializer_list<const char *> aliases) {
    const QString c = QString::fromLatin1(canon);
    m_familyTable.insert(c, family);
    m_curveTable.insert(normalizeCurveKey(c), c);
    for (const char *a : aliases)
      m_curveTable.insert(normalizeCurveKey(QString::fromLatin1(a)), c);
  };
  fam("DEPT", QStringLiteral("Depth"), {"DEPTH", "MD", "M_DEPTH", "DEPTMEAS", "DEPTH_MD", "TVD", "TVDSS"});
  fam("DT", QStringLiteral("Sonic"), {"AC", "DT4P", "DT35", "DTC", "DTCO", "SONIC", "DT_R"});
  fam("GR", QStringLiteral("Gamma"), {"NGR", "SGR", "CGR", "NGT", "GR_EDTC", "GRD", "GRS", "GAMMA"});
  fam("RHOB", QStringLiteral("Density"), {"DEN", "ZDEN", "RHOZ", "RHO8", "DENC", "RHOBC"});
  fam("NPHI", QStringLiteral("Neutron"), {"PHIN", "NEUTRON", "NPHI_LS", "NPOR", "TNPH"});
  fam("SP", QStringLiteral("SP"), {"SSP", "SSP1"});
  fam("CALI", QStringLiteral("Caliper"), {"CAL", "CALX", "CALS", "CAL1", "C1"});
  fam("RT", QStringLiteral("Resistivity"), {"LLD", "ILD", "RESD", "RD", "LL9", "AT90"});
  fam("RS", QStringLiteral("Resistivity"), {"LLS", "ILS", "RESS", "RSN", "LL8", "AT30"});
  fam("RXO", QStringLiteral("Resistivity"), {"RXOZ", "RXOT", "MSFL", "RX0"});
  fam("PE", QStringLiteral("Litho"), {"PEF", "PDPE"});
  fam("POR", QStringLiteral("Porosity"), {"PHIT", "PHIE", "PHIA", "PORO"});
  fam("VS", QStringLiteral("Velocity"), {"Vp", "VP", "VSONIC"});

  // ---- 单位 ----
  auto unit = [this](const QString &canon, std::initializer_list<const char *> aliases) {
    m_unitTable.insert(normalizeUnitKey(canon), canon);
    for (const char *a : aliases)
      m_unitTable.insert(normalizeUnitKey(QString::fromLatin1(a)), canon);
  };
  unit(QStringLiteral("m"), {"M", "METER", "METERS", "METRE", "METRES", "MM"});
  unit(QStringLiteral("ft"), {"FT", "FEET", "F", "FOOT"});
  unit(QStringLiteral("us"), {"US", "USEC", "MICROSEC", "MICROSECONDS", "USM", "USEC/M", "US/M", "USFT", "US/FT"});
  unit(QStringLiteral("ms"), {"MS", "MSEC", "MILLISEC"});
  unit(QStringLiteral("gapi"), {"GAPI", "API", "G"});
  unit(QStringLiteral("ohmm"), {"OHMM", "OHM/M", "OHMM", "OHM", "OHMS"});
  unit(QStringLiteral("v"), {"V", "VOL", "VOLTS"});
  unit(QStringLiteral("%"), {"PERCENT", "PCT", "PCT."});
  unit(QStringLiteral("g/cm3"), {"G/CM3", "GCM3", "GM/CC", "G_CC", "GCC"});
  unit(QStringLiteral("kg/m3"), {"KG/M3", "KGM3"});
  unit(QStringLiteral("mv"), {"MV", "MILLIVOLT", "MVOLT"});
  unit(QStringLiteral("cm"), {"CM", "CENTIMETER", "CENTIMETERS"});
  unit(QStringLiteral("in"), {"IN", "INCH", "INCHES"});
}

QString LasAliasMap::canonicalCurve(const QString &raw)
{
  return lookupNormalized(raw, m_curveTable, m_familyTable, true);
}

QString LasAliasMap::canonicalUnit(const QString &raw)
{
  static const QHash<QString, QString> noFamilies;
  return lookupNormalized(raw, m_unitTable, noFamilies, false);
}

QString LasAliasMap::lookupNormalized(const QString &raw, const QHash<QString, QString> &table,
                                      const QHash<QString, QString> &familyTable, bool isCurve)
{
  QMutexLocker lock(&m_mutex);
  ++m_stats.lookups;
  QHash<QString, QString> &memo = isCurve ? m_curveMemo : m_unitMemo;
  const auto hit = memo.constFind(raw);
  if (hit != memo.constEnd())
  {
    ++m_stats.memoHits;
    return hit.value();
  }
  const QString key = isCurve ? normalizeCurveKey(raw) : normalizeUnitKey(raw);
  QString result = table.value(key);
  if (result.isEmpty() && isCurve)
  {
    // 尾部数字修饰：DT35 → DT（仅当去后是已知名）。
    const QString stripped = stripTrailingDigits(key);
    if (stripped != key)
      result = table.value(stripped);
  }
  if (result.isEmpty())
    result = key; // 未知：归一形态原样返回（曲线大写、单位小写）
  else if (result != raw)
    ++m_stats.normalized;
  // 家族表只对已知曲线名扩展族信息；canonical 输出不变。
  Q_UNUSED(familyTable);
  memo.insert(raw, result);
  while (memo.size() > 4096)
    memo.erase(memo.begin());
  return result;
}

bool LasAliasMap::knowsCurve(const QString &raw) const
{
  QMutexLocker lock(&m_mutex);
  const QString key = normalizeCurveKey(raw);
  if (m_curveTable.contains(key))
    return true;
  const QString stripped = stripTrailingDigits(key);
  return stripped != key && m_curveTable.contains(stripped);
}

bool LasAliasMap::knowsUnit(const QString &raw) const
{
  QMutexLocker lock(&m_mutex);
  return m_unitTable.contains(normalizeUnitKey(raw));
}

QString LasAliasMap::family(const QString &raw)
{
  const QString canon = canonicalCurve(raw);
  QMutexLocker lock(&m_mutex);
  return m_familyTable.value(canon, QStringLiteral("Other"));
}

LasAliasStats LasAliasMap::stats() const
{
  QMutexLocker lock(&m_mutex);
  return m_stats;
}

void LasAliasMap::clearMemo()
{
  QMutexLocker lock(&m_mutex);
  m_curveMemo.clear();
  m_unitMemo.clear();
  m_stats = LasAliasStats();
}
