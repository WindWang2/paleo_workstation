// 层：视图
#include "wellcompositepanel.h"
#include "hiddentrackbar.h"
#include "wellpositionlegendwidget.h"
#include "../paleotheme.h"
#include <QBoxLayout>
#include <QMenu>
#include <QWidgetAction>

namespace WellComposite {
void WellCompositePanel::addPreviewAction(const QString &text,std::function<void()> action) {
  auto *more=findChild<QToolButton *>(QStringLiteral("wellPreviewOptions"));
  if (!more || !more->menu()) return;
  connect(more->menu()->addAction(text),&QAction::triggered,this,[action=std::move(action)]{action();});
}
void WellCompositePanel::setPreviewSummary(const QString &text) {
  m_lblWellName->setToolTip(text);
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
