#pragma once

#include "ai/chat/chatsession.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

// A directory at the exact target prevents QSaveFile from replacing it on
// every supported OS. Other sessions and the preceding checkpoint stay readable.
struct ChatSessionStorageBreaker {
  QString target;
  QString checkpoint;
  bool engage(const QString &sessionId) {
    const auto path = ChatSessionStore::pathFor(sessionId);
    if (path.isEmpty()) return false;
    const auto saved = path + QStringLiteral(".test-checkpoint");
    const bool existed = QFileInfo::exists(path);
    if (existed && !QFile::rename(path, saved))
      return false;
    if (!QDir().mkdir(path)) {
      if (existed) QFile::rename(saved, path);
      return false;
    }
    target = path;
    if (existed) checkpoint = saved;
    return true;
  }
  bool restore() {
    if (target.isEmpty()) return true;
    if (QFileInfo(target).isDir() && !QDir().rmdir(target)) return false;
    if (!checkpoint.isEmpty() && !QFile::rename(checkpoint, target)) return false;
    target.clear();
    checkpoint.clear();
    return true;
  }
  bool loadCheckpoint(ChatSession *session) const {
    QFile file(checkpoint);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto record = QJsonDocument::fromJson(file.readAll());
    if (!record.isObject()) return false;
    *session = ChatSession::fromJson(record.object());
    return true;
  }
};
