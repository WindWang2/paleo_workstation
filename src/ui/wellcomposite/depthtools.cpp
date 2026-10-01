// 层：视图
#include "../paleotheme.h"
#include "depthtools.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace WellComposite
{
namespace DepthTools
{

double niceStepFor(double pxPerMeter, double minPixelSpacing)
{
  const double targetStep = minPixelSpacing / qMax(0.01, pxPerMeter);
  static const double niceSteps[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
  for (double s : niceSteps)
  {
    if (s >= targetStep)
      return s;
  }
  return 1000.0;
}

double snapDepth(double rawDepth, const QVector<MarkerLine> &markers,
                 double thresholdMeters, bool snapToMarkers, bool snapToGrid,
                 double gridStep)
{
  double snapped = rawDepth;

  // 标志层线优先（距离最近者）
  if (snapToMarkers && !markers.isEmpty() && thresholdMeters > 0.0)
  {
    double bestDist = std::numeric_limits<double>::max();
    double bestDepth = rawDepth;
    for (const auto &m : markers)
    {
      const double dist = std::abs(m.first - rawDepth);
      if (dist < bestDist)
      {
        bestDist = dist;
        bestDepth = m.first;
      }
    }
    if (bestDist <= thresholdMeters)
      snapped = bestDepth;
    else if (!snapToGrid || gridStep <= 0.0)
      return snapped;
  }

  // 整刻度网格吸附
  if (snapToGrid && gridStep > 0.0)
  {
    const double grid = std::round(snapped / gridStep) * gridStep;
    // 网格吸附也受阈值约束，避免远距离跳变
    if (std::abs(grid - snapped) <= thresholdMeters)
      snapped = grid;
  }

  return snapped;
}

QString nearestMarkerName(double depth, const QVector<MarkerLine> &markers,
                          double maxDistanceMeters, double *distanceOut)
{
  QString bestName;
  double bestDist = std::numeric_limits<double>::max();
  for (const auto &m : markers)
  {
    const double dist = std::abs(m.first - depth);
    if (dist < bestDist)
    {
      bestDist = dist;
      bestName = m.second;
    }
  }
  if (bestDist > maxDistanceMeters || bestName.isEmpty())
  {
    if (distanceOut)
      *distanceOut = bestDist;
    return QString();
  }
  if (distanceOut)
    *distanceOut = bestDist;
  return bestName;
}

QString readoutText(double depth, const QVector<MarkerLine> &markers)
{
  double dist = 0.0;
  const QString name = nearestMarkerName(depth, markers, 50.0, &dist);
  if (name.isEmpty())
    return QStringLiteral("%1 m").arg(QString::number(depth, 'f', 1));
  return QStringLiteral("%1 m   ⟂ %2 (%3 m)")
      .arg(QString::number(depth, 'f', 1), name, QString::number(dist, 'f', 1));
}

QVector<DepthTools::GapSegment> gapSegments(const QVector<MarkerLine> &markers, double thresholdMeters)
{
  QVector<GapSegment> gaps;
  if (markers.size() < 2 || thresholdMeters <= 0.0)
    return gaps;

  // 按深度排序后逐对检查
  QVector<MarkerLine> sorted = markers;
  std::sort(sorted.begin(), sorted.end(),
            [](const MarkerLine &a, const MarkerLine &b) { return a.first < b.first; });
  for (int i = 1; i < sorted.size(); ++i)
  {
    const double span = sorted.at(i).first - sorted.at(i - 1).first;
    if (span > thresholdMeters)
    {
      GapSegment g;
      g.top = sorted.at(i - 1).first;
      g.bottom = sorted.at(i).first;
      g.upperMarker = sorted.at(i - 1).second;
      g.lowerMarker = sorted.at(i).second;
      g.span = span;
      gaps << g;
    }
  }
  return gaps;
}

QString formatDepth(double depthMeters, bool feetDisplay)
{
  if (!feetDisplay)
    return QStringLiteral("%1").arg(QString::number(depthMeters, 'f', 1));
  return QStringLiteral("%1").arg(QString::number(metersToFeet(depthMeters), 'f', 1));
}

int addBookmark(QList<DepthBookmark> *bookmarks, const QString &name, double depth)
{
  if (!bookmarks || name.trimmed().isEmpty())
    return -1;
  // 同名书签 = 覆盖旧深度
  for (int i = 0; i < bookmarks->size(); ++i)
  {
    if (bookmarks->at(i).name == name.trimmed())
    {
      (*bookmarks)[i].depth = depth;
      return i;
    }
  }
  DepthBookmark bm;
  bm.name = name.trimmed();
  bm.depth = depth;
  bookmarks->append(bm);
  return bookmarks->size() - 1;
}

bool removeBookmark(QList<DepthBookmark> *bookmarks, const QString &name)
{
  if (!bookmarks)
    return false;
  for (int i = 0; i < bookmarks->size(); ++i)
  {
    if (bookmarks->at(i).name == name)
    {
      bookmarks->removeAt(i);
      return true;
    }
  }
  return false;
}

int addPin(QList<DepthPin> *pins, double depth, const QString &text)
{
  if (!pins)
    return -1;
  DepthPin p;
  p.depth = depth;
  p.text = text;
  pins->append(p);
  return pins->size() - 1;
}

bool removePinAt(QList<DepthPin> *pins, int index)
{
  if (!pins || index < 0 || index >= pins->size())
    return false;
  pins->removeAt(index);
  return true;
}

bool updatePinText(QList<DepthPin> *pins, int index, const QString &text)
{
  if (!pins || index < 0 || index >= pins->size())
    return false;
  (*pins)[index].text = text;
  return true;
}

} // namespace DepthTools

// ----------------------------------------------------------------------------
// D2.7 GotoDepthDialog
// ----------------------------------------------------------------------------
GotoDepthDialog::GotoDepthDialog(double minDepth, double maxDepth, double currentDepth,
                                 bool feetDisplay, QWidget *parent)
  : QDialog(parent),
    m_minDepth(minDepth),
    m_maxDepth(maxDepth),
    m_feetDisplay(feetDisplay),
    m_depth(currentDepth)
{
  setObjectName(QStringLiteral("wellCompositeGotoDialog"));
  setWindowTitle(tr("跳转到深度 (Ctrl+G)"));

  auto *root = new QVBoxLayout(this);

  auto *row = new QHBoxLayout();
  row->addWidget(new QLabel(tr("深度:"), this));

  m_spin = new QDoubleSpinBox(this);
  m_spin->setObjectName(QStringLiteral("gotoDepthSpin"));
  const double lo = feetDisplay ? DepthTools::metersToFeet(minDepth) : minDepth;
  const double hi = feetDisplay ? DepthTools::metersToFeet(maxDepth) : maxDepth;
  m_spin->setRange(lo, hi);
  m_spin->setDecimals(1);
  m_spin->setValue(feetDisplay ? DepthTools::metersToFeet(currentDepth) : currentDepth);
  m_spin->setSuffix(feetDisplay ? QStringLiteral(" ft") : QStringLiteral(" m"));
  m_spin->setMinimumWidth(160);
  row->addWidget(m_spin);
  row->addStretch(1);
  root->addLayout(row);

  m_lblHint = new QLabel(tr("井深范围 %1 ~ %2")
                             .arg(QString::number(lo, 'f', 1), QString::number(hi, 'f', 1)),
                         this);
  PaleoTheme::applyThemedStyleSheet(m_lblHint, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + QStringLiteral(" font-size: 8pt;");
  });
  root->addWidget(m_lblHint);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void GotoDepthDialog::accept()
{
  const double raw = m_spin->value();
  m_depth = m_feetDisplay ? DepthTools::feetToMeters(raw) : raw;
  m_depth = qBound(m_minDepth, m_depth, m_maxDepth);
  QDialog::accept();
}

} // namespace WellComposite
