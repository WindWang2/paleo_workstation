// 层：功能
#include "chatsession.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

// apo file naming stays off-project: see ChatSessionStore::directory().

QString ChatSession::displayTitle() const {
  if (!title.trimmed().isEmpty())
    return title;
  for (const ChatMessage &message : messages) {
    if (message.role != ChatRole::User || message.content.trimmed().isEmpty())
      continue;
    const QString text = message.content.trimmed();
    return text.length() <= 40 ? text : text.left(40) + QStringLiteral("…");
  }
  return QString();
}

QJsonObject ChatSession::toJson() const {
  QJsonObject object;
  object.insert(QStringLiteral("schema"), 1);
  object.insert(QStringLiteral("id"), id);
  object.insert(QStringLiteral("title"), title);
  object.insert(QStringLiteral("model"), model);
  object.insert(QStringLiteral("created_at"),
                createdAt.toUTC().toString(Qt::ISODateWithMs));
  object.insert(QStringLiteral("updated_at"),
                updatedAt.toUTC().toString(Qt::ISODateWithMs));
  QJsonArray array;
  for (const ChatMessage &message : messages)
    array.append(message.toJson());
  object.insert(QStringLiteral("messages"), array);
  return object;
}

ChatSession ChatSession::fromJson(const QJsonObject &object) {
  ChatSession session;
  session.id = object.value(QStringLiteral("id")).toString();
  session.title = object.value(QStringLiteral("title")).toString();
  session.model = object.value(QStringLiteral("model")).toString();
  session.createdAt = QDateTime::fromString(
    object.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
  session.updatedAt = QDateTime::fromString(
    object.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
  const QJsonArray array = object.value(QStringLiteral("messages")).toArray();
  for (const QJsonValue &value : array)
    session.messages.append(ChatMessage::fromJson(value.toObject()));
  return session;
}

QString ChatSessionStore::directory() {
  // AppLocalDataLocation：Windows=%LOCALAPPDATA%，Linux≈$XDG_DATA_HOME，
  // macOS≈~/Library/Application Support。三者都在工程目录之外——这是本类
  // 存在的前提（对话历史是本机私有状态，不属于解释成果）。
  const QString root =
    QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
    QStringLiteral("/paleo/ai-chat/sessions");
  QDir().mkpath(root);
  return root;
}

QString ChatSessionStore::pathFor(const QString &sessionId) {
  return directory() + QLatin1Char('/') + sessionId + QStringLiteral(".json");
}

QString ChatSessionStore::newSessionId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QStringList ChatSessionStore::listIds() {
  QStringList ids;
  const QStringList files =
    QDir(directory()).entryList({QStringLiteral("*.json")}, QDir::Files,
                                QDir::Time);
  for (const QString &file : files)
    ids.append(file.chopped(5)); // 去掉 ".json"
  return ids;
}

bool ChatSessionStore::save(const ChatSession &session, QString *error) {
  if (session.id.isEmpty()) {
    if (error)
      *error = QObject::tr("会话缺少 id");
    return false;
  }
  QSaveFile file(pathFor(session.id));
  const QByteArray bytes =
    QJsonDocument(session.toJson()).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  return true;
}

bool ChatSessionStore::load(const QString &sessionId, ChatSession *out,
                            QString *error) {
  QFile file(pathFor(sessionId));
  if (!file.open(QIODevice::ReadOnly)) {
    if (error)
      *error = file.errorString();
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
    QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError ||
      !document.isObject()) {
    if (error)
      *error = QObject::tr("会话文件不是合法 JSON：%1").arg(parseError.errorString());
    return false;
  }
  if (out)
    *out = ChatSession::fromJson(document.object());
  return true;
}

bool ChatSessionStore::remove(const QString &sessionId, QString *error) {
  QFile file(pathFor(sessionId));
  if (file.exists() && !file.remove()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  return true;
}

QVector<ChatSession> ChatSessionStore::loadRecent(int limit) {
  QVector<ChatSession> out;
  QStringList ids = listIds();
  QVector<QPair<QDateTime, QString>> ordered;
  for (const QString &id : ids) {
    ChatSession session;
    if (!load(id, &session, nullptr))
      continue;
    ordered.append({session.updatedAt.isValid() ? session.updatedAt
                                                : session.createdAt,
                    id});
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const QPair<QDateTime, QString> &a,
               const QPair<QDateTime, QString> &b) {
              return a.first > b.first;
            });
  if (limit > 0 && ordered.size() > limit)
    ordered.resize(limit);
  for (const auto &entry : ordered) {
    ChatSession session;
    if (load(entry.second, &session, nullptr))
      out.append(session);
  }
  return out;
}
