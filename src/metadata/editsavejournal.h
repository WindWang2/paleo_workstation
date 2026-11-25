// 层：数据
#pragma once
#include <QString>

// Accessed on the project's single writer. The token pins the publication
// being resumed, even after a later edit changes the working copy.
class EditSaveJournal {
public:
  enum class State { InvalidLocation, Absent, Corrupt, Ready };
  struct Record { State state; QString token; };
  EditSaveJournal(const QString &project, const QString &draft);
  Record read() const;
  bool reserve(QString &token, bool &created) const;
  bool renew(const QString &expected) const;
  bool clear(const QString &expected) const;
private:
  bool write(const QString &token) const;
  QString m_path;
};
