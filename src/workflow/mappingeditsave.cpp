// 层：功能
#include "mappingworkbench.h"
#include "metadata/managedpath.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopeGuard>
#include <QUuid>

namespace {
QString journalPath(const QString &root, const QString &id) {
  if (!id.startsWith("draft.") || !DataCatalog::isSafePathSegment(id)) return {};
  return paleo::managedpath::resolveManagedPath(root, QStringLiteral("artifacts/working/%1/pending-save").arg(id));
}
QString readToken(const QString &path) {
  QFile journal(path);
  if (!journal.open(QIODevice::ReadOnly)) return {};
  const QString token = QString::fromLatin1(journal.read(64)).trimmed();
  return QUuid(token).isNull() ? QString{} : token;
}
bool writeToken(const QString &path, const QString &token) {
  QSaveFile journal(path);
  const auto bytes = token.toLatin1();
  return journal.open(QIODevice::WriteOnly) && journal.write(bytes) == bytes.size() && journal.commit();
}
}

MappingWorkbench::EditSaveStage MappingWorkbench::pendingEditSave(const QString &id) const {
  const auto path = journalPath(m_dir, id);
  if (path.isEmpty() || !QFileInfo::exists(path)) return EditSaveStage::Idle;
  const auto token = readToken(path);
  if (!token.isEmpty() && versionForLayer(id).extra.value("edit_save_token").toString() == token)
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
  const auto path = journalPath(m_dir, id);
  if (path.isEmpty()) return reject(tr("无法定位工作副本保存记录"));
  if (m_savingDrafts.contains(id)) return reject(tr("正在保存该工作副本"));
  m_savingDrafts.insert(id);
  const auto cleanup = qScopeGuard([this, id] { m_savingDrafts.remove(id); });
  QString token;
  if (QFileInfo::exists(path)) {
    token = readToken(path);
    if (token.isEmpty()) return reject(tr("工作副本的保存记录损坏，无法继续登记"));
  } else {
    token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!writeToken(path, token)) return reject(tr("工作副本已落盘，但无法写入版本登记记录"));
  }
  // The token is part of the immutable version. A restart between publication and
  // projection resumes the same version, even if the editable layer has since changed.
  auto saved = versionForLayer(id);
  QString why;
  if (saved.extra.value("edit_save_token").toString() != token) {
    if (!registerEditingVersion(id, token, &why))
      return reject(tr("工作副本已保存，成果版本未登记。%1").arg(why));
    saved = versionForLayer(id);
  }
  if (saved.extra.value("edit_save_token").toString() == token &&
      !syncSavedWellAttributes(saved, &why))
    return reject(tr("成果版本 %1 已登记，井属性尚未同步。%2").arg(saved.id, why));
  if (!QFile::remove(path))
    return reject(tr("成果版本 %1 已登记，保存记录尚未清理，可重试。").arg(saved.id));
  emit displayChanged(id);
  return true;
}
