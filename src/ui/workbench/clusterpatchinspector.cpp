// 层：视图
#include "clusterpatchinspector.h"
#include "../../workflow/mappingworkbench.h"
#include "../../domain/faciescatalog.h"
#include "../notifications/paleonotify.h"
#include "../paleotheme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
QString faciesName(const QVariantMap &row) {
  QStringList names;
  for (const auto key : {"facies", "subfacies", "microfacies"}) {
    const auto part = row.value(key).toString();
    if (!part.isEmpty()) names << part;
  }
  return names.isEmpty() ? row.value("name").toString() : names.join(QStringLiteral(" / "));
}
QString wellCount(const QVariant &value) {
  return value.isNull() ? QObject::tr("暂无统计") : QObject::tr("%1 口").arg(value.toLongLong());
}
QLabel *label(QWidget *parent, const char *name) {
  auto *result = new QLabel(parent);
  result->setObjectName(QString::fromLatin1(name));
  result->setTextFormat(Qt::PlainText);
  result->setWordWrap(true);
  return result;
}
}

ClusterPatchInspector::ClusterPatchInspector(MappingWorkbench *workbench, QWidget *parent)
    : QWidget(parent), m_workbench(workbench) {
  setObjectName(QStringLiteral("clusterPatchInspector"));
  auto *layout = new QVBoxLayout(this);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  m_heading = label(this, "clusterInspectorTitle");
  m_heading->setFont(PaleoTheme::bodyFont(PaleoTheme::kTitlePt));
  layout->addWidget(m_heading);
  m_source = label(this, "clusterInspectorSource");
  PaleoTheme::applyThemedStyleSheet(m_source, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_source);
  createEditor();
  layout->addWidget(m_editor);
  m_feedback = label(this, "clusterInspectorStatus");
  layout->addWidget(m_feedback);
  layout->addStretch();
  connect(workbench, &MappingWorkbench::catalogBound, this, [this] { m_drafts.clear(); clearSelection(); });
  clearSelection();
}

void ClusterPatchInspector::createEditor() {
  m_editor = new QWidget(this);
  auto *form = new QFormLayout(m_editor);
  form->setContentsMargins(0, 0, 0, 0);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  m_current = label(m_editor, "clusterInspectorCurrentFacies");
  m_evidence = label(m_editor, "clusterInspectorEvidence");
  form->addRow(tr("当前微相"), m_current);
  form->addRow(tr("井证据"), m_evidence);
  m_target = new QComboBox(m_editor);
  m_target->setObjectName(QStringLiteral("clusterInspectorTarget"));
  m_target->setAccessibleName(tr("目标微相"));
  m_target->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_target->setPlaceholderText(tr("选择目标微相"));
  form->addRow(tr("目标微相"), m_target);
  m_whole = new QCheckBox(tr("整簇图斑"), m_editor);
  m_whole->setObjectName(QStringLiteral("clusterInspectorWhole"));
  form->addRow(tr("调整范围"), m_whole);
  m_countLabel = label(m_editor, "clusterInspectorCount");
  form->addRow(m_countLabel);
  m_apply = new QPushButton(tr("应用到草稿"), m_editor);
  m_cancel = new QPushButton(tr("取消调整"), m_editor);
  m_cancel->setObjectName(QStringLiteral("clusterInspectorCancel"));
  auto *pending = new QHBoxLayout;
  pending->addWidget(m_apply, 1);
  pending->addWidget(m_cancel);
  form->addRow(pending);
  auto *actions = new QHBoxLayout;
  m_undo = new QPushButton(tr("撤销"), m_editor);
  m_redo = new QPushButton(tr("重做"), m_editor);
  m_save = new QPushButton(tr("保存新版本"), m_editor);
  actions->addWidget(m_undo);
  actions->addWidget(m_redo);
  actions->addWidget(m_save);
  form->addRow(actions);
  m_apply->setObjectName(QStringLiteral("clusterInspectorApply"));
  m_save->setObjectName(QStringLiteral("clusterInspectorSave"));
  connect(m_cancel, &QPushButton::clicked, this, [this] {
    m_target->setCurrentIndex(-1);
    m_whole->setChecked(false);
  });
  connect(m_whole, &QCheckBox::toggled, this, &ClusterPatchInspector::updateScope);
  connect(m_target, &QComboBox::currentIndexChanged, this, &ClusterPatchInspector::refresh);
  connect(m_apply, &QPushButton::clicked, this, &ClusterPatchInspector::apply);
  connect(m_undo, &QPushButton::clicked, this, [this] { step(true); });
  connect(m_redo, &QPushButton::clicked, this, [this] { step(false); });
  connect(m_save, &QPushButton::clicked, this, [this] { save(m_layer, true); });
}

bool ClusterPatchInspector::inspect(const QString &layer, qint64 fid) {
  QString error;
  const auto rows = m_workbench->clusterPatchChoices(layer, &error, fid, 1);
  if (rows.isEmpty() || rows.first().toMap().value("feature_id").toLongLong() != fid) {
    clearSelection();
    m_feedback->setText(error);
    return false;
  }
  m_layer = layer;
  m_row = rows.first().toMap();
  const auto version = m_workbench->versionForLayer(layer);
  m_schema = version.extra.contains("facies") ? version.extra.value("facies").toList()
      : m_workbench->facies(version.extra.value("horizon").toString());
  {
    const QSignalBlocker quiet(m_target);
    m_target->clear();
    for (const auto &item : m_schema) m_target->addItem(faciesName(item.toMap()), item.toMap().value("code"));
    m_target->setCurrentIndex(-1);
  }
  {
    const QSignalBlocker quiet(m_whole);
    m_whole->setChecked(false);
  }
  const auto cluster = m_row.value("cluster_id").toInt();
  m_whole->setEnabled(cluster > 0);
  m_heading->setText(cluster > 0 && !m_row.value("patch_id").isNull()
      ? tr("簇 %1 · 图斑 %2").arg(cluster).arg(m_row.value("patch_id").toLongLong()) : tr("合并图斑 · 要素 %1").arg(fid));
  const auto horizon = version.extra.value("horizon").toString();
  const auto unit = paleo::strat::fromMap(version.extra.value("unitSelection").toMap());
  QString title = version.extra.value("title").toString();
  if (!horizon.isEmpty() && !unit.displayName.isEmpty()) {
    if (title == horizon || title.startsWith(horizon + QStringLiteral(" · "))) title.replace(0, horizon.size(), unit.displayName);
    else if (!title.startsWith(unit.displayName)) title = tr("%1 · %2").arg(unit.displayName, title);
  }
  m_source->setText(tr("%1 · v%2").arg(title).arg(version.versionNumber));
  m_source->setToolTip(tr("%1\n%2").arg(horizon, version.id));
  const auto patchWells = m_row.value("patch_id").isNull() ? QVariant() : m_row.value("patch_wells");
  const auto clusterWells = cluster > 0 ? m_row.value("cluster_wells") : QVariant();
  QString evidence = tr("原图斑 %1\n全簇 %2").arg(wellCount(patchWells), wellCount(clusterWells));
  auto status = m_row.value("assignment_status").toString();
  for (const auto &entry : version.extra.value("cluster_facies_mapping").toList()) {
    const auto mapping = entry.toMap();
    if (mapping.value("cluster_id").toInt() == cluster) { status = mapping.value("status").toString(); break; }
  }
  if (status == "conflict") evidence += tr("\n井预测有分歧");
  if (status == "unsupported") evidence += tr("\n暂无井支持");
  const int suggestion = m_target->findData(m_row.value("well_suggestion"));
  if (suggestion >= 0) evidence += tr("\n井建议：%1").arg(m_target->itemText(suggestion));
  m_evidence->setText(evidence);
  m_editor->show();
  updateScope();
  refresh();
  return true;
}

void ClusterPatchInspector::clearSelection() {
  ++m_scopeRequest;
  m_layer.clear();
  m_row.clear();
  m_schema.clear();
  m_count = 0;
  m_heading->setText(tr("对象属性"));
  m_source->setText(tr("在地图上点选一个聚类图斑，查看微相和井证据。"));
  m_editor->hide();
  m_feedback->clear();
  m_scopeIds.clear();
  emit draftPreviewRequested({}, {});
  emit highlightRequested({});
  emit historyChanged();
}

void ClusterPatchInspector::updateScope() {
  const auto request = ++m_scopeRequest;
  const auto fid = m_row.value("feature_id").toLongLong();
  auto display = [this, request, fid](qint64 count, QList<qint64> ids, const QString &error) {
    if (request != m_scopeRequest) return;
    m_count = count;
    if (count > ids.size() && !ids.isEmpty() && !ids.contains(fid)) ids.last() = fid;
    m_countLabel->setText(!error.isEmpty() ? error : (count > ids.size()
        ? tr("涉及 %1 个图斑，已描亮 %2 个").arg(count).arg(ids.size()) : tr("涉及 %1 个图斑").arg(count)));
    m_scopeIds = ids;
    emit highlightRequested(ids);
    refresh();
  };
  if (!m_whole->isChecked()) { display(1, {fid}, {}); return; }
  m_count = 0;
  m_scopeIds.clear();
  m_countLabel->setText(tr("正在统计整簇图斑…"));
  emit highlightRequested({fid});
  refresh();
  m_workbench->previewClusterScope(m_layer, m_row.value("cluster_id").toInt(), this, display);
}

void ClusterPatchInspector::setReadOnly(bool value) { m_readOnly = value; refresh(); }

bool ClusterPatchInspector::canStep(bool undo) const {
  const auto draft = m_drafts.constFind(m_layer);
  return !m_readOnly && draft != m_drafts.cend() && (undo ? draft->cursor > 0 : draft->cursor < draft->edits.size());
}

bool ClusterPatchInspector::step(bool undo) {
  if (!canStep(undo)) return false;
  m_drafts[m_layer].cursor += undo ? -1 : 1;
  refresh();
  return true;
}

void ClusterPatchInspector::refresh() {
  if (m_layer.isEmpty()) return;
  int code = m_row.value("facies_code").toInt();
  const auto draft = m_drafts.constFind(m_layer);
  const auto count = draft == m_drafts.cend() ? 0 : draft->cursor;
  for (qsizetype i = 0; i < count; ++i) {
    const auto edit = draft->edits.at(i).toMap();
    if (edit.value("whole_cluster").toBool() ? edit.value("cluster_id") == m_row.value("cluster_id")
                                            : edit.value("feature_id") == m_row.value("feature_id"))
      code = edit.value("facies_code").toInt();
  }
  const int index = m_target->findData(code);
  m_current->setText(index < 0 ? tr("待定") : m_target->itemText(index));
  m_target->setEnabled(!m_readOnly);
  m_cancel->setEnabled(m_whole->isChecked() || m_target->currentIndex() >= 0);
  m_apply->setEnabled(!m_readOnly && m_count > 0 && m_target->currentIndex() >= 0);
  m_save->setEnabled(!m_readOnly && count > 0);
  m_redo->setEnabled(canStep(false));
  m_undo->setEnabled(canStep(true));
  m_feedback->setText(m_readOnly ? tr("工程只读") : (count ? tr("未保存 · %1 次调整").arg(count) : QString()));
  emit draftPreviewRequested(m_layer, count ? draft->edits.mid(0, count) : QVariantList{});
  emit historyChanged();
}

void ClusterPatchInspector::apply() {
  if (m_readOnly || !m_apply->isEnabled() || m_layer.isEmpty()) return;
  auto &draft = m_drafts[m_layer];
  if (draft.cursor > 0 && draft.schema != m_schema) {
    m_feedback->setText(tr("微相分类已经改变，请核对原草稿"));
    return;
  }
  if (draft.cursor == 0) {
    draft.version = m_workbench->versionForLayer(m_layer).id;
    draft.schema = m_schema;
  }
  const auto appearance = FaciesCatalog::appearance(FaciesCatalog::find(m_schema, m_target->currentData()));
  draft.edits.resize(draft.cursor);
  draft.edits << QVariantMap{{"feature_id", m_row.value("feature_id")}, {"patch_id", m_row.value("patch_id")},
                             {"cluster_id", m_row.value("cluster_id")}, {"whole_cluster", m_whole->isChecked()},
                             {"facies_code", m_target->currentData()}, {"preview_ids", QVariant::fromValue(m_scopeIds)},
                             {"preview_color", appearance.value("color")}};
  ++draft.cursor;
  m_target->setCurrentIndex(-1);
  refresh();
}

bool ClusterPatchInspector::save(const QString &layer, bool activate) {
  const auto draft = m_drafts.value(layer);
  if (!draft.cursor || m_readOnly || m_workbench->versionForLayer(layer).id != draft.version) return false;
  QString error;
  const auto fid = m_row.value("feature_id", -1).toLongLong();
  const auto key = layer; // productReady may synchronously switch the displayed map.
  QVariantList assignments;
  for (qsizetype i = 0; i < draft.cursor; ++i) {
    auto edit = draft.edits.at(i).toMap();
    edit.remove("preview_color");
    edit.remove("preview_ids");
    assignments << edit;
  }
  const auto output = m_workbench->saveClusterAssignments(key, assignments, draft.schema, &error);
  if (output.isEmpty()) { m_feedback->setText(error); return false; }
  m_drafts.remove(key);
  refresh();
  if (activate) emit versionSaved(output, fid);
  return true;
}

bool ClusterPatchInspector::resolveDrafts() {
  QStringList dirty;
  for (auto it = m_drafts.cbegin(); it != m_drafts.cend(); ++it) if (it->cursor > 0) dirty << it.key();
  if (dirty.isEmpty()) return true;
  const auto choice = PaleoNotify::askSaveDiscard(this, PaleoNotify::AskIcon::Question, tr("图斑解释尚未保存"),
      tr("有 %1 幅图件包含未保存的人工解释。").arg(dirty.size()),
      m_readOnly ? QString() : tr("保存新版本"), tr("放弃草稿"), tr("取消"));
  if (choice == PaleoNotify::SaveChoice::Cancel) return false;
  if (choice == PaleoNotify::SaveChoice::Save)
    for (const auto &layer : dirty) if (!save(layer, false)) return false;
  // Discard takes effect at catalogBound, after the rest of the close checks have accepted.
  return true;
}
