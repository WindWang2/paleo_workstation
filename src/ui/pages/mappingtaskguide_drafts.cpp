// 层：视图
#include "mappingtaskguide.h"
#include "mappingworkbenchpage.h"
#include "../edittools/editingtoolbar.h"
#include "../../qgis/faciespartitioneditor.h"
#include "../../workflow/mappingworkbench.h"
#include <QAction>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>

void MappingTaskGuide::watchEditor(PaleoEditingToolbar *editor) {
  if (!editor || m_mode != QLatin1String("compose")) return;
  m_editor = editor;
  const auto sync = [this] {
    const QString id = m_editor && m_editor->isEditing() ? FaciesPartitionEditor::declaredLayerId(m_editor->currentLayer()) : QString();
    if (!id.startsWith(QLatin1String("draft.")) || id == m_activeDraft) return;
    m_activeDraft = id;
    refresh();
  };
  connect(editor, &PaleoEditingToolbar::stateChanged, this, sync);
  for (auto *action : {editor->actionSelect(), editor->actionPaintFacies(), editor->actionSplitFacies(),
                       editor->actionVertexEdit(), editor->actionAddPolygon()}) {
    connect(action, &QAction::triggered, this, [this, action] {
      if (m_editor && m_editor->isEditing() && action->isChecked()) {
        const auto id = FaciesPartitionEditor::declaredLayerId(m_editor->currentLayer());
        if (id.startsWith(QLatin1String("draft."))) m_draftTools.insert(id, action);
      }
    });
  }
  sync();
}

QString MappingTaskGuide::resolveDrafts() {
  m_drafts.clear();
  auto *page = qobject_cast<MappingWorkbenchPage *>(m_page);
  auto *tree = m_page->findChild<QTreeWidget *>(QStringLiteral("workbenchResults"));
  if (!tree || !page) return {};
  for (int i = 0; i < tree->topLevelItemCount(); ++i) {
    const auto row = tree->topLevelItem(i)->data(0, Qt::UserRole).toMap();
    const auto id = row.value("id").toString();
    if (row.value("horizon").toString() == page->horizon() && row.value("draft").toBool() &&
        id.startsWith(QLatin1String("draft.")) && !m_workbench->isWellPrediction(id))
      m_drafts.insert(id, row.value("name", id).toString());
  }
  if (m_editor && m_editor->isEditing() && m_drafts.contains(m_activeDraft) &&
      FaciesPartitionEditor::declaredLayerId(m_editor->currentLayer()) == m_activeDraft) return m_activeDraft;
  if (m_drafts.contains(page->selectedLayer())) return page->selectedLayer();
  if (m_drafts.contains(m_activeDraft)) return m_activeDraft;
  return m_drafts.size() == 1 ? m_drafts.firstKey() : QString();
}

void MappingTaskGuide::resumeDraft(const QString &id) {
  auto *page = qobject_cast<MappingWorkbenchPage *>(m_page);
  if (!page || !m_drafts.contains(id)) return;
  const QPointer<QAction> tool = m_draftTools.value(id);
  emit page->commandRequested(QStringLiteral("resumeEdit"), {{"layer", id}, {"horizon", page->horizon()}});
  if (m_editor && m_editor->isEditing() && FaciesPartitionEditor::declaredLayerId(m_editor->currentLayer()) == id &&
      tool && tool->isEnabled() && !tool->isChecked()) tool->trigger();
}

void MappingTaskGuide::chooseDraft() {
  const auto current = resolveDrafts();
  if (m_drafts.size() < 2) {
    if (!current.isEmpty()) resumeDraft(current);
    return;
  }
  auto *menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  menu->setObjectName(QStringLiteral("resumeDraftMenu"));
  const auto epoch = m_projectEpoch;
  auto *page = qobject_cast<MappingWorkbenchPage *>(m_page);
  const auto horizon = page->horizon();
  for (auto it = m_drafts.cbegin(); it != m_drafts.cend(); ++it) {
    const auto id = it.key();
    const QString label = m_drafts.values().count(it.value()) == 1 ? it.value() : tr("%1 · %2").arg(it.value(), id);
    auto *action = menu->addAction(label);
    action->setData(id);
    if (id == current) menu->setDefaultAction(action);
    connect(action, &QAction::triggered, this, [this, id, epoch, page, horizon] {
      if (epoch != m_projectEpoch || page->horizon() != horizon) return;
      resolveDrafts();
      resumeDraft(id);
    });
  }
  menu->popup(m_next->mapToGlobal(QPoint(0, m_next->height())));
}
