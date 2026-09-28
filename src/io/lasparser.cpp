// 层：数据
#include "lasparser.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <limits>

// ---------------------------------------------------------------------------
// LAS 2.x item lines have the form "MNEM.UNIT VALUE : DESCRIPTION":
//   mnemonic    — up to the first '.'
//   unit        — between '.' and the first whitespace (may be empty)
//   value       — between unit and ':' (may be empty, e.g. ~C lines)
//   description — everything after ':'
// Section headers start with '~' followed by a code letter (V/W/C/A/…);
// comment lines start with '#'. ~A is the only section whose body is data
// rows rather than item lines.
// ---------------------------------------------------------------------------
namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  struct LasItem
  {
    QString mnem;
    QString unit;
    QString value;
    QString descr;
  };

  bool parseItemLine(const QString &line, LasItem &item)
  {
    const int colon = line.indexOf(QLatin1Char(':'));
    const QString left = (colon >= 0 ? line.left(colon) : line).trimmed();
    item.descr = (colon >= 0 ? line.mid(colon + 1) : QString()).trimmed();

    const int dot = left.indexOf(QLatin1Char('.'));
    if (dot < 0)
      return false; // every LAS item carries "MNEM." at minimum

    item.mnem = left.left(dot).trimmed().toUpper();
    const QString rest = left.mid(dot + 1);
    const int sp = rest.indexOf(QRegularExpression(QStringLiteral("\\s")));
    item.unit = (sp < 0 ? rest : rest.left(sp)).trimmed();
    item.value = (sp < 0 ? QString() : rest.mid(sp + 1)).trimmed();
    return !item.mnem.isEmpty();
  }

  double nan()
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
} // namespace

bool LasParser::parse(const QString &path, QStringList &curveNames,
                      QList<LasCurve> &curves, QString *error)
{
  curveNames.clear();
  curves.clear();

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    setError(error, QStringLiteral("cannot open %1").arg(path));
    return false;
  }

  enum class Section { None, Version, Well, Curves, Ascii, Other };
  Section section = Section::None;

  double nullValue = -999.25; // CWLS default when ~W has no usable NULL item
  bool sawAscii = false;
  QStringList names;
  QList<LasCurve> cols;

  QTextStream in(&f);
  while (!in.atEnd())
  {
    const QString line = in.readLine().trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;

    if (line.startsWith(QLatin1Char('~')))
    {
      const QChar code = line.size() > 1 ? line.at(1).toUpper() : QChar();
      if (code == QLatin1Char('V'))      section = Section::Version;
      else if (code == QLatin1Char('W')) section = Section::Well;
      else if (code == QLatin1Char('C')) section = Section::Curves;
      else if (code == QLatin1Char('A')) { section = Section::Ascii; sawAscii = true; }
      else                               section = Section::Other;
      continue;
    }

    switch (section)
    {
      case Section::Version:
      {
        LasItem it;
        if (parseItemLine(line, it) && it.mnem == QStringLiteral("WRAP") &&
            it.value.startsWith(QStringLiteral("YES"), Qt::CaseInsensitive))
        {
          setError(error, QStringLiteral("wrap mode (WRAP YES) is not supported: %1").arg(path));
          return false;
        }
        break;
      }
      case Section::Well:
      {
        LasItem it;
        if (parseItemLine(line, it) && it.mnem == QStringLiteral("NULL"))
        {
          bool ok = false;
          const double v = it.value.toDouble(&ok);
          if (ok)
            nullValue = v;
        }
        break;
      }
      case Section::Curves:
      {
        LasItem it;
        if (parseItemLine(line, it))
        {
          names.append(it.mnem);
          cols.append({it.mnem, it.unit, it.descr, {}});
        }
        break;
      }
      case Section::Ascii:
      {
        // Whitespace-separated floats in ~C column order; missing/trailing
        // tokens and unparseable cells land as NaN.
        const QStringList tokens =
            line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tokens.isEmpty())
          break;
        for (int i = 0; i < cols.size(); ++i)
        {
          double v = nan();
          if (i < tokens.size())
          {
            bool ok = false;
            const double t = tokens.at(i).toDouble(&ok);
            if (ok && t != nullValue)
              v = t;
          }
          cols[i].values.append(v);
        }
        break;
      }
      default:
        break;
    }
  }

  if (cols.isEmpty())
  {
    setError(error, QStringLiteral("no curve definitions (~C) found in %1").arg(path));
    return false;
  }
  if (!sawAscii)
  {
    setError(error, QStringLiteral("no ASCII data section (~A) found in %1").arg(path));
    return false;
  }

  curveNames = names;
  curves = cols;
  return true;
}

bool LasParser::readWellInfo(const QString &path, QString &wellName, QString *error)
{
  wellName.clear();

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    setError(error, QStringLiteral("cannot open %1").arg(path));
    return false;
  }

  // 只需 ~W 段的 WELL item——读到 ~C 即止。
  bool inWell = false;
  QTextStream in(&f);
  while (!in.atEnd())
  {
    const QString line = in.readLine().trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    if (line.startsWith(QLatin1Char('~')))
    {
      const QChar code = line.size() > 1 ? line.at(1).toUpper() : QChar();
      if (code == QLatin1Char('W'))
        inWell = true;
      else if (inWell)
        break; // 离开 ~W，身份字段已收齐
      continue;
    }
    if (!inWell)
      continue;
    LasItem it;
    if (!parseItemLine(line, it))
      continue;
    if (it.mnem == QStringLiteral("WELL") && wellName.isEmpty())
      wellName = it.value;
  }
  return true;
}
