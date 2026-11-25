// 层：视图
#include "mappingreferencepanel.h"

#include <QComboBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>

QSize MappingEvidencePanel::sizeHint() const
{
  return {640, m_compact ? layout()->sizeHint().height() : 340};
}

void MappingEvidencePanel::enableCompactDock()
{
  m_dockMode = true;
  setSources(m_sources, m_horizon);
}

void MappingEvidencePanel::setCompact(bool compact)
{
  m_compact = compact;
  m_picker->setVisible(compact);
  m_count->setVisible(!compact);
  m_add->setVisible(!compact);
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
