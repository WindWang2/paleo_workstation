// 层：QGIS 封装
#include "draftautosave.h"
#include <QTimer>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayereditbuffer.h>

namespace {
constexpr char kObjectName[] = "paleoDraftAutosave";

// 几何 / 结构修改判定（flush() 的早退条件）。undo() 需要区分「缓冲含几何
// 修改」与「提交失败」两种 flush() 失败原因，各自文案不同，故抽出共用。
bool hasGeometryOrStructureEdits(const QgsVectorLayerEditBuffer *buffer) {
  return !buffer->addedFeatures().isEmpty() || !buffer->deletedFeatureIds().isEmpty() ||
         !buffer->changedGeometries().isEmpty() || !buffer->addedAttributes().isEmpty() ||
         !buffer->deletedAttributeIds().isEmpty();
}
}

DraftAutosave *DraftAutosave::of(QgsVectorLayer *layer) {
  return layer ? layer->findChild<DraftAutosave *>(QLatin1String(kObjectName),
                                                   Qt::FindDirectChildrenOnly)
               : nullptr;
}

DraftAutosave *DraftAutosave::attach(QgsVectorLayer *layer) {
  if (!layer)
    return nullptr;
  if (auto *existing = of(layer))
    return existing;
  return new DraftAutosave(layer);
}

DraftAutosave::DraftAutosave(QgsVectorLayer *layer)
    : QObject(layer), m_layer(layer) {
  setObjectName(QLatin1String(kObjectName));
  connect(layer, &QgsVectorLayer::editCommandEnded, this, &DraftAutosave::schedule);
  // 不经编辑命令的属性写入（属性表直接改值等）也覆盖；同一事件循环内的多次
  // 修改合并为一次提交（排队到命令结束之后）。
  connect(layer, &QgsVectorLayer::attributeValueChanged, this, &DraftAutosave::schedule);
  // 用户放弃编辑 / 外部提交后，旧历史的「前值」基准失效。
  connect(layer, &QgsVectorLayer::editingStopped, this, [this] {
    m_history.clear();
    emit historyChanged();
  });
}

void DraftAutosave::schedule() {
  if (m_pending || m_undoing)
    return;
  m_pending = true;
  QTimer::singleShot(0, this, [this] {
    m_pending = false;
    flush();
  });
}

bool DraftAutosave::flush() {
  QgsVectorLayer *layer = m_layer;
  if (!layer || !layer->isEditable() || layer->readOnly())
    return false;
  QgsVectorLayerEditBuffer *buffer = layer->editBuffer();
  if (!buffer || !buffer->isModified())
    return false;
  // 几何 / 结构修改：留给「保存编辑」，保住 QGIS 撤销栈。
  if (hasGeometryOrStructureEdits(buffer))
    return false;
  const QgsChangedAttributesMap changed = buffer->changedAttributeValues();
  if (changed.isEmpty())
    return false;
  QList<Change> entry;
  if (!m_undoing) {
    QgsFeatureIds ids;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it)
      ids.insert(it.key());
    QgsFeatureRequest request;
    request.setFilterFids(ids);
    request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
    QgsFeatureIterator stored = layer->dataProvider()->getFeatures(request);
    QgsFeature f;
    while (stored.nextFeature(f)) {
      const QgsAttributeMap attrs = changed.value(f.id());
      for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
        entry << Change{f.id(), a.key(), f.attribute(a.key())};
    }
  }
  if (!layer->commitChanges(false)) {
    m_lastError = layer->commitErrors().join(QLatin1Char('\n'));
    emit failed(m_lastError);
    return false;
  }
  m_lastError.clear();
  if (!m_undoing && !entry.isEmpty()) {
    m_history << entry;
    emit historyChanged();
  }
  emit saved(static_cast<int>(changed.size()));
  return true;
}

bool DraftAutosave::undo(QString *error) {
  QgsVectorLayer *layer = m_layer;
  const auto reject = [error](const QString &text) {
    if (error)
      *error = text;
    return false;
  };
  if (m_history.isEmpty())
    return reject(tr("没有可撤销的已自动保存修订"));
  if (!layer || !layer->isEditable())
    return reject(tr("请先开始编辑该工作副本"));
  // flush() 失败有两种原因，文案必须分开：缓冲含几何/结构修改 vs 提交出错
  // （磁盘满、缓冲含新增字段等）。一律报「含几何修改」会把真实错误吞掉。
  if (layer->editBuffer() && layer->editBuffer()->isModified()) {
    if (hasGeometryOrStructureEdits(layer->editBuffer()))
      return reject(tr("当前编辑缓冲含几何修改，请先保存或撤销几何编辑"));
    if (!flush())
      return reject(m_lastError.isEmpty() ? tr("无法保存当前修订，请重试") : m_lastError);
  }
  const QList<Change> entry = m_history.takeLast();
  m_undoing = true;
  layer->beginEditCommand(tr("撤销已自动保存的修订"));
  bool ok = true;
  for (const Change &c : entry)
    ok = layer->changeAttributeValue(c.fid, c.field, c.before) && ok;
  if (!ok) {
    layer->destroyEditCommand();
    m_undoing = false;
    m_history << entry;
    return reject(tr("撤销失败：要素已不存在或字段不可写"));
  }
  layer->endEditCommand();
  const bool saved = flush();
  m_undoing = false;
  if (!saved && layer->editBuffer() && layer->editBuffer()->isModified()) {
    m_history << entry;
    emit historyChanged();
    return reject(m_lastError.isEmpty() ? tr("撤销结果写回失败") : m_lastError);
  }
  emit historyChanged();
  layer->triggerRepaint();
  return true;
}
