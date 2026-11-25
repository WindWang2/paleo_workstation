// 层：数据
#include "editsavejournal.h"
#include "managedpath.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

EditSaveJournal::EditSaveJournal(const QString &project, const QString &draft) {
  if (draft.startsWith("draft.") && paleo::managedpath::isSafePathSegment(draft))
    m_path = paleo::managedpath::resolveManagedPath(project, QStringLiteral("artifacts/working/%1/pending-save").arg(draft));
}

EditSaveJournal::Record EditSaveJournal::read() const {
  if (m_path.isEmpty()) return {State::InvalidLocation, {}};
  if (!QFileInfo::exists(m_path)) return {State::Absent, {}};
  QFile file(m_path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 64) return {State::Corrupt, {}};
  const auto token = QString::fromLatin1(file.read(64)).trimmed();
  if (file.error() != QFile::NoError || QUuid(token).isNull()) return {State::Corrupt, {}};
  return {State::Ready, token};
}

bool EditSaveJournal::write(const QString &token) const {
  QSaveFile journal(m_path);
  journal.setDirectWriteFallback(false);
  const auto bytes = token.toLatin1();
  return journal.open(QIODevice::WriteOnly) && journal.write(bytes) == bytes.size() && journal.commit();
}

bool EditSaveJournal::reserve(QString &token, bool &created) const {
  created = false;
  auto existing = read();
  token = existing.token;
  if (existing.state == State::Ready) return true;
  if (existing.state != State::Absent) return false;
  const QString next = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!write(next)) return false;
  token = next;
  created = true;
  return true;
}

bool EditSaveJournal::renew(const QString &expected) const {
  return !expected.isEmpty() && read().token == expected && write(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

bool EditSaveJournal::clear(const QString &expected) const {
  return !expected.isEmpty() && read().token == expected && QFile::remove(m_path);
}
