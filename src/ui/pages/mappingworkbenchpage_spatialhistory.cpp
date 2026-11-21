// 层：视图
#include "mappingworkbenchpage.h"
#include "../windowsizing_internal.h"
#include "../../workflow/mappingworkbench.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

void MappingWorkbenchPage::buildSpatialEditHistory(QWidget *page, QVBoxLayout *layout) {
  auto *title = new QLabel(tr("图件空间编辑历史"), page);
  title->setObjectName("spatialEditHistoryTitle");
  title->setWordWrap(true);
  layout->addWidget(title);
  m_spatialHistory = new QPlainTextEdit(page);
  m_spatialHistory->setObjectName("spatialEditHistory");
  m_spatialHistory->setReadOnly(true);
  m_spatialHistory->setAccessibleName(tr("图件空间编辑历史"));
  m_spatialHistory->setMinimumHeight(paleo::ui_detail::lineRows(m_spatialHistory, 4));
  layout->addWidget(m_spatialHistory, 1);
  auto *older = new QPushButton(tr("更早空间编辑"), page);
  older->setObjectName("workbench.spatialHistoryOlder");
  older->setAccessibleName(older->text());
  auto *newer = new QPushButton(tr("较新空间编辑"), page);
  newer->setObjectName("workbench.spatialHistoryNewer");
  newer->setAccessibleName(newer->text());
  connect(older, &QPushButton::clicked, this, [this] {
    if (m_spatialNextCursor.isEmpty()) return;
    m_spatialCursorStack << m_spatialCursor;
    m_spatialCursor = m_spatialNextCursor;
    renderSpatialEditHistory();
  });
  connect(newer, &QPushButton::clicked, this, [this] {
    if (m_spatialCursorStack.isEmpty()) return;
    m_spatialCursor = m_spatialCursorStack.takeLast();
    renderSpatialEditHistory();
  });
  auto *navigation = new QHBoxLayout;
  navigation->addWidget(older);
  navigation->addWidget(newer);
  layout->addLayout(navigation);
}

void MappingWorkbenchPage::renderSpatialEditHistory() {
  if (!m_spatialHistory || !m_workbench) return;
  const auto versionId = m_evidenceSnapshot.value(QStringLiteral("version_id")).toString();
  const auto featureUid = m_evidenceSnapshot.value(QStringLiteral("feature_uid")).toString();
  if (versionId != m_spatialVersion || featureUid != m_spatialFeature) {
    m_spatialVersion = versionId;
    m_spatialFeature = featureUid;
    m_spatialCursor.clear();
    m_spatialNextCursor.clear();
    m_spatialCursorStack.clear();
  }
  auto *older = commandButton(QStringLiteral("spatialHistoryOlder"));
  auto *newer = commandButton(QStringLiteral("spatialHistoryNewer"));
  if (versionId.isEmpty()) {
    m_spatialHistory->setPlainText(tr("尚无图件版本，不能读取空间编辑历史。"));
    if (older) older->setEnabled(false);
    if (newer) newer->setEnabled(false);
    return;
  }
  const auto page = m_workbench->spatialEditHistory(versionId, featureUid, 50, m_spatialCursor);
  m_spatialNextCursor = page.value(QStringLiteral("next_cursor")).toString();
  const auto events = page.value(QStringLiteral("events")).toList();
  QStringList lines;
  lines << tr("图件空间编辑历史 · 本页 %1 条").arg(events.size());
  if (!featureUid.isEmpty()) lines << tr("图斑 %1").arg(featureUid);
  else lines << tr("未选中活要素，显示本版本链上的编辑，包括已删除对象");
  const auto diagnostics = page.value(QStringLiteral("diagnostics")).toStringList();
  for (const auto &diagnostic : diagnostics) lines << tr("诊断：%1").arg(diagnostic);
  if (!page.value(QStringLiteral("complete")).toBool() && diagnostics.isEmpty() && !m_spatialNextCursor.isEmpty())
    lines << tr("还有更早的记录，使用翻页继续读取。");
  for (const auto &entry : events) {
    const auto event = entry.toMap();
    const auto action = event.value(QStringLiteral("deleted")).toBool() ? tr("删除")
        : event.value(QStringLiteral("action")).toString();
    lines << tr("%1 · %2 · %3\n对象 %4 · 版本 %5")
                 .arg(event.value(QStringLiteral("timestamp")).toString(),
                      event.value(QStringLiteral("actor_id")).toString(), action,
                      event.value(QStringLiteral("target_object_id"),
                                  event.value(QStringLiteral("before_uid"),
                                              event.value(QStringLiteral("after_uid"))))
                          .toString(),
                      event.value(QStringLiteral("version_id")).toString());
  }
  if (events.isEmpty()) lines << tr("这一页没有空间编辑事件。");
  m_spatialHistory->setPlainText(lines.join(QStringLiteral("\n\n")));
  if (older) {
    older->setEnabled(!m_spatialNextCursor.isEmpty());
    older->setToolTip(tr("继续读取更早的图件空间编辑，每次最多 50 条"));
  }
  if (newer) {
    newer->setEnabled(!m_spatialCursorStack.isEmpty());
    newer->setToolTip(tr("返回较新的图件空间编辑"));
  }
}
