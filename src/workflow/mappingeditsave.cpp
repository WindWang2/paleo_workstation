// 层：功能
#include "mappingworkbench.h"
#include "metadata/editsavejournal.h"
#include <QScopeGuard>

bool MappingWorkbench::reserveEditSave(const QString &id, QString *newToken) const {
  QString token;
  bool created;
  if (!EditSaveJournal(m_dir, id).reserve(token, created)) return false;
  if (newToken) *newToken = created ? token : QString{};
  return true;
}

void MappingWorkbench::discardEditSaveReservation(const QString &id, const QString &token) const {
  EditSaveJournal(m_dir, id).clear(token);
}

MappingWorkbench::EditSaveStage MappingWorkbench::pendingEditSave(const QString &id) const {
  const auto record = EditSaveJournal(m_dir, id).read();
  if (record.state == EditSaveJournal::State::Absent || record.state == EditSaveJournal::State::InvalidLocation)
    return EditSaveStage::Idle;
  if (record.state == EditSaveJournal::State::Ready && versionForLayer(id).extra.value("edit_save_token").toString() == record.token)
    return EditSaveStage::WellSync;
  return EditSaveStage::Registration;
}

bool MappingWorkbench::saveEditingVersion(const QString &id, QString *error) {
  if (error) error->clear();
  const auto reject = [error](const QString &message) {
    if (error) *error = message;
    return false;
  };
  if (!id.startsWith("draft.")) return reject(tr("只有人工编辑副本可以保存编辑版本"));
  if (!ready(declaration(id).horizon, error)) return false;
  if (m_catalog->refusesWrites()) return reject(tr("当前工程只读，无法保存图件版本"));
  const EditSaveJournal journal(m_dir, id);
  const auto state = journal.read().state;
  if (state == EditSaveJournal::State::InvalidLocation) return reject(tr("无法定位工作副本保存记录"));
  if (isWorkingLayerEditing(id) && pendingEditSave(id) != EditSaveStage::WellSync)
    return reject(tr("请先点击“保存编辑”提交当前编辑，再保存图件版本"));
  if (m_savingDrafts.contains(id)) return reject(tr("正在保存该工作副本"));
  m_savingDrafts.insert(id);
  const auto cleanup = qScopeGuard([this, id] { m_savingDrafts.remove(id); });
  QString token;
  if (state == EditSaveJournal::State::Corrupt) return reject(tr("工作副本的保存记录损坏，无法继续登记"));
  bool created;
  if (!journal.reserve(token, created)) return reject(tr("工作副本已落盘，但无法写入版本登记记录"));
  // The token is part of the immutable version. A restart between publication and
  // projection resumes the same version, even if the editable layer has since changed.
  auto saved = versionForLayer(id);
  QString why;
  const bool resumingSync = saved.extra.value("edit_save_token").toString() == token;
  if (!resumingSync) {
    if (!registerEditingVersion(id, token, &why)) {
      emit displayChanged(id);
      return reject(tr("工作副本已保存，成果版本未登记。%1").arg(why));
    }
    saved = versionForLayer(id);
  }
  if (saved.extra.value("edit_save_token").toString() == token &&
      !syncSavedWellAttributes(saved, &why)) {
    emit displayChanged(id);
    return reject(tr("成果版本 %1 已登记，井属性尚未同步。%2").arg(saved.id, why));
  }
  if (resumingSync) {
    const auto revision = committedWorkingRevision(id);
    if (revision.isEmpty())
      return reject(tr("成果版本 %1 已同步，无法读取工作副本来核对最新修订。").arg(saved.id));
    if (saved.extra.value("edit_revision").toString() != revision || hasPendingExplanationAdoption(id)) {
      if (!journal.renew(token))
        return reject(tr("成果版本 %1 已同步，工作副本还有未登记的修订，更新保存记录失败。").arg(saved.id));
      emit displayChanged(id);
      return reject(tr("成果版本 %1 已同步，工作副本还有更新的修订，请登记当前工作副本。").arg(saved.id));
    }
  }
  if (!journal.clear(token))
    return reject(tr("成果版本 %1 已登记，保存记录尚未清理，可重试。").arg(saved.id));
  emit displayChanged(id);
  return true;
}
