// 层：视图
#include "wellcompositepanel.h"
#include "hiddentrackbar.h"
#include "wellpositionlegendwidget.h"
#include "../paleotheme.h"
#include <QBoxLayout>
#include <QEvent>
#include <QHash>
#include <QMenu>
#include <QWidgetAction>

namespace WellComposite {
bool WellCompositePanel::event(QEvent *event) {
  const bool handled = QWidget::event(event);
  if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
    emit previewVisibilityChanged(event->type() == QEvent::Show);
  return handled;
}

void WellCompositePanel::addPreviewAction(const QString &text,std::function<void()> action) {
  auto *more=findChild<QToolButton *>(QStringLiteral("wellPreviewOptions"));
  if (!more || !more->menu()) return;
  connect(more->menu()->addAction(text),&QAction::triggered,this,[action=std::move(action)]{action();});
}
void WellCompositePanel::setPreviewSummary(const QString &text) {
  m_lblWellName->setToolTip(text);
}
void WellCompositePanel::setDocumentCurveTracks(const QList<QPair<QString, QString>> &tracks,
                                                bool editable) {
  const auto current = m_canvas->tracks();
  QHash<QString, QString> labels;
  for (const auto &[id, label] : tracks)
    labels.insert(id, label);

  QList<std::shared_ptr<WellTrack>> layout;
  for (const auto &track : current) {
    if (!track)
      continue;
    const auto &id = track->documentTrackId();
    if (id.isEmpty()) {
      layout.append(track);
    } else if (labels.contains(id)) {
      track->setTitle(labels.take(id));
      track->setDocumentTrack(id, editable);
      layout.append(track);
    }
  }
  for (const auto &[id, label] : tracks) {
    if (id.isEmpty() || !labels.remove(id))
      continue;
    auto track = std::make_shared<CurveTrack>(label);
    track->setDocumentTrack(id, editable);
    track->setParticipatesInStandardLayout(false);
    track->setPrintIncluded(false); // 仅持有当前视口样点。
    track->setWindowStatus(tr("正在读取曲线…"));
    layout.append(track);
  }
  if (layout != current) {
    m_canvas->setTracks(layout);
    m_hiddenBar->setTracks(currentSpecs());
  }
}
void WellCompositePanel::setDocumentCurveWindow(const QString &id, const CurveData &curve,
                                                const QString &error) {
  if (id.isEmpty())
    return;
  for (const auto &track : m_canvas->tracks()) {
    if (!track || track->documentTrackId() != id)
      continue;
    auto source = std::static_pointer_cast<CurveTrack>(track);
    const bool usable = error.isEmpty() && !curve.isEmpty();
    source->setCurves(usable ? QVector<CurveData>{curve} : QVector<CurveData>{});
    source->setWindowStatus(usable ? QString() :
        error.isEmpty() ? tr("当前测深范围没有曲线样点") : error);
    m_canvas->updateAll();
    return;
  }
}
void WellCompositePanel::setupPreviewFooter(QBoxLayout *root) {
  auto *footer=new QWidget(this);footer->setObjectName(QStringLiteral("wellPreviewFooter"));
  auto *row=new QHBoxLayout(footer);row->setContentsMargins(0,0,0,0);row->setSpacing(PaleoTheme::tokens().spacingXs);
  m_legendWidget->setCompactPreview();row->addWidget(m_legendWidget,1);
  m_faciesStatus->setWordWrap(false);m_faciesStatus->setMinimumWidth(0);
  m_faciesStatus->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
  if(property("comparisonPreview").toBool())m_faciesStatus->hide();else row->addWidget(m_faciesStatus,1);
  auto *hidden=new QToolButton(footer);hidden->setText(tr("隐藏道"));hidden->setObjectName(QStringLiteral("previewHiddenTracks"));
  auto *menu=new QMenu(hidden);auto *action=new QWidgetAction(menu);action->setDefaultWidget(m_hiddenBar);menu->addAction(action);
  hidden->setMenu(menu);hidden->setPopupMode(QToolButton::InstantPopup);row->addWidget(hidden);
  connect(menu,&QMenu::aboutToShow,this,[this,action]{action->setEnabled(m_hiddenBar->hiddenCount()>0);});
  auto *legend=m_legendWidget->findChild<QToolButton *>(QStringLiteral("btnOpenLegend"));
  if (legend) addPreviewAction(tr("地质图例"),[legend]{legend->click();});
  root->addWidget(footer);
}
}
