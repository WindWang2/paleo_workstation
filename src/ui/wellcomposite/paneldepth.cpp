// 层：视图
// 综合图面板·深度交互（区间统计/标注钉/跳转/书签/单位/gap/井斜时深表/TWT 读数）——自 wellcompositepanel.cpp 拆出（方向 66，行为零变更）
#include "wellcompositepanel.h"
#include "../paleotheme.h"
#include "intervalstatistics.h"
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

namespace WellComposite
{
// ----------------------------------------------------------------------------
// D2.x 深度交互槽
// ----------------------------------------------------------------------------
void WellCompositePanel::onIntervalSelected(double top, double bottom)
{
  // D2.2 区间统计对话框
  const IntervalStatsReport rep = computeIntervalStats(top, bottom, m_data);
  if (!rep.isValid())
    return;

  QDialog dlg(this);
  dlg.setObjectName(QStringLiteral("wellCompositeIntervalStatsDialog"));
  dlg.setWindowTitle(tr("区间统计 [%1~%2m]").arg(QString::number(top, 'f', 1),
                                                QString::number(bottom, 'f', 1)));
  dlg.setMinimumSize(QSize(520, 380));
  auto *lay = new QVBoxLayout(&dlg);
  auto *edit = new QPlainTextEdit(&dlg);
  edit->setReadOnly(true);
  edit->setFont(PaleoTheme::monoFont());
  edit->setPlainText(rep.toTsv());
  lay->addWidget(edit, 1);

  auto *btnRow = new QWidget(&dlg);
  auto *btnLay = new QHBoxLayout(btnRow);
  auto *btnCopy = new QPushButton(tr("复制 TSV"), btnRow);
  auto *btnClose = new QPushButton(tr("关闭"), btnRow);
  btnLay->addStretch(1);
  btnLay->addWidget(btnCopy);
  btnLay->addWidget(btnClose);
  lay->addWidget(btnRow);
  connect(btnCopy, &QPushButton::clicked, this, [edit]() {
    QApplication::clipboard()->setText(edit->toPlainText());
  });
  connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
  dlg.exec();
}

void WellCompositePanel::onPinCreateRequested(double depth)
{
  bool ok = false;
  const QString text = QInputDialog::getText(this, tr("添加深度标注"),
                                             tr("深度 %1 m 的标注文字:").arg(QString::number(depth, 'f', 1)),
                                             QLineEdit::Normal, QString(), &ok);
  if (!ok)
    return;

  QList<DepthPin> pins = m_canvas->pins();
  DepthTools::addPin(&pins, depth, text);
  m_canvas->setPins(pins);
  if (m_store)
  {
    m_store->setPins(pins);
    m_store->save();
  }
}

void WellCompositePanel::onPinEditRequested(int pinIndex)
{
  QList<DepthPin> pins = m_canvas->pins();
  if (pinIndex < 0 || pinIndex >= pins.size())
    return;

  bool ok = false;
  const QString text = QInputDialog::getText(this, tr("编辑深度标注"),
                                             tr("深度 %1 m 的标注文字:").arg(QString::number(pins.at(pinIndex).depth, 'f', 1)),
                                             QLineEdit::Normal, pins.at(pinIndex).text, &ok);
  if (!ok)
    return;

  DepthTools::updatePinText(&pins, pinIndex, text);
  m_canvas->setPins(pins);
  if (m_store)
  {
    m_store->setPins(pins);
    m_store->save();
  }
}

void WellCompositePanel::clearPins()
{
  m_canvas->setPins({});
  if (m_store)
  {
    m_store->setPins({});
    m_store->save();
  }
}

void WellCompositePanel::addPinAt(double depth, const QString &text)
{
  QList<DepthPin> pins = m_canvas->pins();
  DepthTools::addPin(&pins, depth, text);
  m_canvas->setPins(pins);
  if (m_store)
  {
    m_store->setPins(pins);
    m_store->save();
  }
}

void WellCompositePanel::openGotoDepthDialog()
{
  GotoDepthDialog dlg(m_canvas->minDepth(), m_canvas->maxDepth(),
                      m_canvas->visibleTopDepth(), m_depthFeet, this);
  if (dlg.exec() == QDialog::Accepted)
    m_canvas->setScrollDepth(dlg.selectedDepth());
}

void WellCompositePanel::onBookmarkMenuAboutToShow()
{
  QMenu *menu = m_btnBookmarks->menu();
  menu->clear();

  QAction *actAdd = menu->addAction(tr("在视口顶部添加书签…"));
  connect(actAdd, &QAction::triggered, this, [this]() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("添加书签"),
                                               tr("书签名（深度 %1 m）:")
                                                   .arg(QString::number(m_canvas->visibleTopDepth(), 'f', 1)),
                                               QLineEdit::Normal, QString(), &ok);
    if (ok)
      addBookmark(name, m_canvas->visibleTopDepth());
  });

  if (!m_bookmarks.isEmpty())
  {
    menu->addSeparator();
    for (const auto &bm : std::as_const(m_bookmarks))
    {
      QAction *act = menu->addAction(QStringLiteral("%1 @ %2 m").arg(bm.name, QString::number(bm.depth, 'f', 1)));
      connect(act, &QAction::triggered, this, [this, name = bm.name]() { jumpToBookmark(name); });
    }
    menu->addSeparator();
    QAction *actClear = menu->addAction(tr("清除全部书签"));
    connect(actClear, &QAction::triggered, this, [this]() {
      setBookmarks({});
    });
  }
}

bool WellCompositePanel::addBookmark(const QString &name, double depth)
{
  if (DepthTools::addBookmark(&m_bookmarks, name, depth) < 0)
    return false;
  if (m_store)
  {
    m_store->setBookmarks(m_bookmarks);
    m_store->save();
  }
  return true;
}

void WellCompositePanel::setBookmarks(const QList<DepthBookmark> &bms)
{
  m_bookmarks = bms;
  if (m_store)
  {
    m_store->setBookmarks(m_bookmarks);
    m_store->save();
  }
}

bool WellCompositePanel::jumpToBookmark(const QString &name)
{
  for (const auto &bm : m_bookmarks)
    if (bm.name == name)
    {
      m_canvas->setScrollDepth(bm.depth);
      return true;
    }
  return false;
}

void WellCompositePanel::setDepthUnitFeet(bool feet)
{
  if (m_depthFeet == feet)
    return;
  m_depthFeet = feet;
  m_canvas->setDepthUnitLabel(feet ? QStringLiteral("ft") : QString());
  m_lblReadout->setText(QStringLiteral("— ") + (feet ? QStringLiteral("ft") : QStringLiteral("m")));
}

void WellCompositePanel::setGapThresholdMeters(double meters)
{
  m_gapThresholdM = meters;
  m_canvas->setGapThresholdMeters(meters);
}

// ----------------------------------------------------------------------------
// D1 深度装配链（wave/deepen-perf）
// ----------------------------------------------------------------------------
void WellCompositePanel::applyDepthTables(const QVector<DeviationStation> &stations,
                                          const QVector<QPair<double, double>> &tvdTwtPairs,
                                          double kbElevation)
{
  if (!stations.isEmpty())
    m_depthTransform.setDeviationSurvey(stations);
  if (!tvdTwtPairs.isEmpty())
    m_depthTransform.setTimeDepthTable(tvdTwtPairs);
  if (!std::isnan(kbElevation))
    m_depthTransform.setKbElevation(kbElevation);
  refreshTwtLabels();
  // 刷新读数条（悬停深度沿用当前值；无悬停时下次移动自然带出）
  m_lblStatus->setText(
      tr("深度装配：井斜表%1、时深表%2（TVD/TWT 副刻度已%3）")
          .arg(hasDeviationSurvey() ? tr("已加载") : tr("缺失"))
          .arg(hasTimeDepthTable() ? tr("已加载") : tr("缺失"))
          .arg(hasTimeDepthTable() ? tr("启用") : tr("禁用")));
}

void WellCompositePanel::clearDepthTables()
{
  m_depthTransform = DepthTransform{};
  refreshTwtLabels();
}

void WellCompositePanel::refreshTwtLabels()
{
  QVector<QPair<double, QString>> labels;
  if (m_depthTransform.hasTimeDepthTable())
  {
    // 副刻度步长：深度跨度/12 取整到 1-2-5 序列（与标尺主刻度密度同量级）
    const double span = qMax(1.0, m_canvas->maxDepth() - m_canvas->minDepth());
    double step = span / 12.0;
    double mag = std::pow(10.0, std::floor(std::log10(step)));
    double norm = step / mag;
    step = (norm <= 1.0 ? 1.0 : norm <= 2.0 ? 2.0 : norm <= 5.0 ? 5.0 : 10.0) * mag;
    for (double md = std::ceil(m_canvas->minDepth() / step) * step;
         md <= m_canvas->maxDepth(); md += step)
    {
      const double twt = twtAtDepth(md);
      if (!std::isnan(twt) && twt > 0.0)
        labels.append({md, QString::number(qRound(twt))});
    }
  }
  m_canvas->setTwtLabels(labels);
}

QString WellCompositePanel::depthReadoutSuffix(double md) const
{
  if (!hasDeviationSurvey() && !hasTimeDepthTable())
    return QString();
  QString suffix;
  if (hasDeviationSurvey())
    suffix += QStringLiteral(" | TVD %1").arg(QString::number(mdToTvd(md), 'f', 1));
  if (hasTimeDepthTable())
  {
    const double twt = twtAtDepth(md);
    if (!std::isnan(twt))
      suffix += QStringLiteral(" | %1 ms").arg(QString::number(twt, 'f', 0));
  }
  return suffix;
}

} // namespace WellComposite
