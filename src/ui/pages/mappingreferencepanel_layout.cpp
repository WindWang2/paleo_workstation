// 层：视图
#include "mappingreferencepanel.h"
#include "../paleotheme.h"

#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>

namespace {
void applyMapSpacing(QLayout *layout, bool mapOnly)
{
  const auto &tokens = PaleoTheme::tokens();
  const int margin = mapOnly ? 0 : tokens.spacingSm;
  layout->setContentsMargins(margin, margin, margin, margin);
  layout->setSpacing(mapOnly ? 0 : tokens.spacingXs);
}
}

QString MappingReferencePanel::compactTabTitle() const
{
  const auto row = selectedSource();
  QStringList parts{tr("只读参考")};
  const QString horizon = row.value("horizon").toString();
  if (!horizon.isEmpty()) parts << horizon;
  const int version = row.value("version").toInt();
  if (version > 0) parts << tr("v%1").arg(version);
  return parts.join(QStringLiteral(" · "));
}

QString MappingReferencePanel::sourceDescription() const
{
  return m_sources->currentText() + QLatin1Char('\n') + m_caption->toolTip();
}

void MappingReferencePanel::setMapOnly(bool mapOnly)
{
  if (m_mapOnly == mapOnly) return;
  m_mapOnly = mapOnly;
  applyMapSpacing(layout(), mapOnly);
  m_toolbar->setVisible(!mapOnly);
  m_caption->setVisible(!mapOnly && !sourceId().isEmpty());
  if (m_clusterLegendButton) {
    m_clusterLegendButton->setVisible(!mapOnly && m_clusterLegend->count() > 0);
    m_clusterLegend->setVisible(!mapOnly && m_clusterLegendButton->isChecked());
  }
}

void MappingEvidencePanel::setMapOnly(bool mapOnly)
{
  if (m_mapOnly == mapOnly) return;
  m_mapOnly = mapOnly;
  applyMapSpacing(layout(), mapOnly);
  m_toolbar->setVisible(!mapOnly);
  const auto policy = mapOnly ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded;
  m_scroll->setHorizontalScrollBarPolicy(policy);
  m_scroll->setVerticalScrollBarPolicy(policy);
  for (qsizetype i = 0; i < m_views.size(); ++i) {
    m_views[i]->setMapOnly(mapOnly);
    m_views[i]->setMinimumHeight(mapOnly ? 0 : minViewHeight());
    m_handles[i]->setVisible(!mapOnly);
  }
  updateViewSizes();
}

QSize MappingEvidencePanel::sizeHint() const
{
  return {640, m_compact ? layout()->sizeHint().height() : 340};
}

void MappingEvidencePanel::enableCompactDock()
{
  m_dockMode = true;
  setSources(m_sources, m_horizon);
}

void MappingEvidencePanel::setInWorkspace(bool inWorkspace)
{
  m_inWorkspace = inWorkspace;
  if (!inWorkspace) setMapOnly(false);
  m_workspaceButton->setVisible(!inWorkspace && !m_compact);
  updateViewSizes();
}

void MappingEvidencePanel::reloadProjectLayout()
{
  m_widthKey.clear();
  setSources({}, {});
}

void MappingEvidencePanel::setCompact(bool compact)
{
  m_compact = compact;
  m_picker->setVisible(compact);
  m_count->setVisible(!compact);
  m_add->setVisible(!compact);
  m_workspaceButton->setVisible(m_dockMode && !m_inWorkspace && !compact);
  m_emptyHint->setVisible(false);
  m_scroll->setVisible(!compact && !m_views.isEmpty());
  updateGeometry();
  emit compactChanged(compact);
}

void MappingEvidencePanel::restoreDockLayout(const QString &widthKey)
{
  if (m_widthKey == widthKey) return;
  m_widthKey = widthKey;
  m_restoring = true;
  while (!m_views.isEmpty()) removeView(m_views.last());
  const QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  m_savedWidths.clear();
  for (const auto &v : settings.value(widthKey).toList()) m_savedWidths << v.toInt();
  m_pendingSources = settings.value(widthKey + QStringLiteral("/sources")).toStringList();
  const auto slotsToRestore = qMax(m_savedWidths.size(), m_pendingSources.size());
  for (qsizetype i = 0; i < slotsToRestore; ++i) addView();
  m_autoFill = slotsToRestore == 0;
  setCompact(slotsToRestore == 0);
  m_restoring = false;
}

void MappingEvidencePanel::refreshDockSources()
{
  const QSignalBlocker blocked(m_picker);
  m_picker->clear();
  m_picker->addItem(tr("添加参考"), QString());
  for (const auto &value : m_sources) {
    const auto row = value.toMap();
    if (row.value("type") == "vector" || row.value("type") == "raster")
      m_picker->addItem(row.value("name").toString(), row.value("id"));
  }
  m_picker->setEnabled(m_picker->count() > 1);
  m_restoring = true;
  for (qsizetype i = 0; i < m_pendingSources.size() && i < m_views.size(); ++i) {
    const QString id = m_pendingSources.at(i);
    if (id.isEmpty() || m_picker->findData(id) < 1) continue;
    m_views.at(i)->selectSource(id);
    m_pendingSources[i].clear();
  }
  m_restoring = false;
}

void MappingEvidencePanel::saveDockSources()
{
  if (!m_dockMode || m_widthKey.isEmpty()) return;
  QStringList sources;
  for (const auto *view : m_views) sources << view->sourceId();
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  settings.setValue(m_widthKey + QStringLiteral("/sources"), sources);
}
