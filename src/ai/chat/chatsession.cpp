// 层：功能
#include "chatsession.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
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
  // toJson 写的是 toUTC() 后的串：读回必须补 toLocalTime()，否则时间戳停在
  // UTC 墙钟，toMarkdown 的「创建」行静默偏差一个时区偏移（中国差 8 小时）。
  // 与 ChatMessage::fromJson 的 timestamp 同款口径（chatmessage.cpp:124）。
  session.createdAt = QDateTime::fromString(
    object.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs).toLocalTime();
  session.updatedAt = QDateTime::fromString(
    object.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs).toLocalTime();
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
  // 会话 id 直接进文件名：必须是「单段安全名」——否则 load/remove/save 会
  // 顺着拼接路径逃出 sessions 目录（remove 会删掉指向的任意文件）。校验
  // 范式同本模块 ai/modelregistry.cpp 的 isSafeModelName：空/./.. 与任何
  // 路径分隔符、驱动器冒号、NUL 一律拒绝（含 Windows 侧反斜杠与冒号）。
  if (!isSafeSessionId(sessionId))
    return QString();
  return directory() + QLatin1Char('/') + sessionId + QStringLiteral(".json");
}

bool ChatSessionStore::isSafeSessionId(const QString &sessionId) {
  if (sessionId.isEmpty() || sessionId == QLatin1String(".") ||
      sessionId == QLatin1String(".."))
    return false;
  for (const QChar &ch : sessionId) {
    if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') ||
        ch == QLatin1Char(':') || ch == QChar(0))
      return false;
  }
  return true;
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
  const QString path = pathFor(session.id);
  if (path.isEmpty()) {
    if (error)
      *error = QObject::tr("会话 id 含非法路径字符");
    return false;
  }
  QSaveFile file(path);
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
  const QString path = pathFor(sessionId);
  if (path.isEmpty()) {
    if (error)
      *error = QObject::tr("会话 id 含非法路径字符");
    return false;
  }
  QFile file(path);
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
  const QString path = pathFor(sessionId);
  if (path.isEmpty()) {
    if (error)
      *error = QObject::tr("会话 id 含非法路径字符");
    return false;
  }
  QFile file(path);
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
  // PW-123：每个文件只解析一次——首遍载入的会话直接留用，不再按 id 重读。
  QVector<ChatSession> loaded;
  loaded.reserve(ids.size());
  for (const QString &id : ids) {
    ChatSession session;
    if (!load(id, &session, nullptr))
      continue;
    loaded.append(std::move(session));
  }
  const auto stamp = [](const ChatSession &s) {
    return s.updatedAt.isValid() ? s.updatedAt : s.createdAt;
  };
  std::stable_sort(loaded.begin(), loaded.end(),
                   [&](const ChatSession &a, const ChatSession &b) {
                     return stamp(a) > stamp(b);
                   });
  if (limit > 0 && loaded.size() > limit)
    loaded.resize(limit);
  for (ChatSession &session : loaded)
    out.append(std::move(session));
  return out;
}

QString ChatSession::toMarkdown() const {
  QStringList out;
  const QString heading = displayTitle().isEmpty() ? QObject::tr("未命名会话")
                                                   : displayTitle();
  out << QStringLiteral("# %1").arg(heading);
  QStringList meta;
  if (!model.isEmpty())
    meta << QObject::tr("模型：%1").arg(model);
  if (createdAt.isValid())
    meta << QObject::tr("创建：%1")
                .arg(createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
  if (!meta.isEmpty())
    out << meta.join(QStringLiteral(" · "));
  for (const ChatMessage &message : messages) {
    QString role;
    switch (message.role) {
    case ChatRole::User:
      role = QObject::tr("你");
      break;
    case ChatRole::Assistant:
      role = QObject::tr("助手");
      break;
    case ChatRole::Tool:
      role = QObject::tr("工具");
      break;
    case ChatRole::System:
      continue; // 系统提示是内部装配产物，不进用户导出面
    }
    out << QStringLiteral("## %1").arg(role);
    if (!message.content.isEmpty())
      out << message.content;
    for (const ChatToolCall &call : message.toolCalls)
      out << QStringLiteral("- %1 `%2`").arg(
        QObject::tr("工具调用"), call.name); // argumentsJson 不进导出（防泄漏实参）
    out << QString(); // 消息间空行
  }
  return out.join(QStringLiteral("\n"));
}

bool ChatSessionStore::exportMarkdown(const ChatSession &session,
                                      const QString &filePath,
                                      QString *error) {
  QSaveFile file(filePath);
  const QByteArray bytes = session.toMarkdown().toUtf8();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  return true;
}
