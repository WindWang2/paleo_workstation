// 层：数据
#include "laswriter.h"

#include <QSaveFile>
#include <QTextStream>
#include <cmath>

namespace LasWriter
{
namespace
{
QString fmtValue(double v, const LasWriteOptions &opt)
{
  if (std::isnan(v))
    return QString::number(opt.nullToken, 'f', 4);
  return QString::number(v, 'f', opt.valuePrecision);
}

// #414：头字段（WELL/曲线名/描述）不得带行结构——内嵌 \r\n 折叠为空格。
QString singleLine(const QString &s)
{
  QString out = s;
  out.replace(QLatin1Char('\r'), QLatin1Char(' '));
  out.replace(QLatin1Char('\n'), QLatin1Char(' '));
  return out;
}
} // namespace

bool writeLasFile(const QString &path, const QList<LasCurve> &curves,
                  const LasWriteOptions &opt, QString *error)
{
  if (curves.isEmpty())
  {
    if (error)
      *error = QStringLiteral("no curves to write");
    return false;
  }
  const qsizetype n = curves.first().values.size();
  if (n == 0)
  {
    if (error)
      *error = QStringLiteral("depth channel is empty");
    return false;
  }
  for (const LasCurve &c : curves)
  {
    if (c.values.size() != n)
    {
      if (error)
        *error = QStringLiteral("curve %1 length %2 != depth length %3")
                     .arg(c.name.isEmpty() ? QStringLiteral("?") : c.name)
                     .arg(c.values.size())
                     .arg(n);
      return false;
    }
    if (c.name.trimmed().isEmpty() || c.name.contains(QLatin1Char(' ')))
    {
      if (error)
        *error = QStringLiteral("curve mnemonic '%1' is not a valid LAS mnemonic")
                     .arg(c.name);
      return false;
    }
  }

  const double firstDepth = curves.first().values.front();
  const double lastDepth = curves.first().values.back();
  const double step = n > 1 ? (lastDepth - firstDepth) / (n - 1) : 0.0;

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
  {
    if (error)
      *error = QStringLiteral("cannot open '%1' for write: %2")
                   .arg(path, file.errorString());
    return false;
  }
  QTextStream ts(&file);
  ts << "# LAS 2.0 derived curve product (paleo_workstation petrophysics)\n";
  ts << "~Version Information\n";
  ts << "VERS.  2.0 :\n";
  ts << "WRAP.  NO :\n";
  ts << "~Well\n";
  ts << "STRT ." << curves.first().unit << "  " << fmtValue(firstDepth, opt) << " :\n";
  ts << "STOP ." << curves.first().unit << "  " << fmtValue(lastDepth, opt) << " :\n";
  ts << "STEP ." << curves.first().unit << "  " << fmtValue(step, opt) << " :\n";
  ts << "NULL .  " << QString::number(opt.nullToken, 'f', 4) << " :\n";
  if (!opt.wellName.trimmed().isEmpty())
    ts << "WELL.  " << singleLine(opt.wellName) << " :\n";
  ts << "~Curve\n";
  for (const LasCurve &c : curves)
    // #414：名字/描述内嵌换行会破坏行结构（回读即坏）——写前折叠空白。
    ts << singleLine(c.name) << " ." << singleLine(c.unit) << " : "
       << singleLine(c.descr) << "\n";
  ts << "~ASCII\n";
  for (qsizetype i = 0; i < n; ++i)
  {
    QStringList cols;
    cols.reserve(curves.size());
    for (const LasCurve &c : curves)
      cols.append(fmtValue(c.values.at(i), opt));
    ts << cols.join(QLatin1Char(' ')) << "\n";
  }
  if (!file.commit())
  {
    if (error)
      *error = QStringLiteral("commit failed for '%1': %2").arg(path, file.errorString());
    return false;
  }
  return true;
}
} // namespace LasWriter
