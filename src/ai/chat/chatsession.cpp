// 层：功能
#include "chatsession.h"

#include "chatsessionarchive.h"
#include "chatsessionindex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
  if (!hostToolBindings.isEmpty()) object.insert(QStringLiteral("host_tool_bindings"), hostToolBindings);
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
  session.hostBindingsValid = !object.contains("host_tool_bindings") || object.value("host_tool_bindings").isArray();
  session.hostToolBindings = object.value("host_tool_bindings").toArray();
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
  const QByteArray bytes =
    QJsonDocument(session.toJson()).toJson(QJsonDocument::Compact);
  // H2-W3 对称容量：save 与 load 共用同一尺——成功写出的档案必须仍可被
  // 自己完整载入（字节上限 + 结构护栏：消息数/关键字段长度，不产生 load
  // 永远拒绝的档案）。
  const paleo::chat::ChatArchiveBudget budget = paleo::chat::chatArchiveBudget();
  if (bytes.size() > budget.maxArchiveBytes) {
    if (error)
      *error = QObject::tr("会话超过本机档案容量上限（%1 MiB）")
                 .arg(budget.maxArchiveBytes / (1024 * 1024));
    return false;
  }
  if (session.messages.size() > budget.maxMessages) {
    if (error)
      *error = QObject::tr("会话消息数超过档案结构上限（%1）").arg(budget.maxMessages);
    return false;
  }
  if (session.model.size() > budget.maxModelChars ||
      session.title.size() > budget.maxTitleCharsLoad) {
    if (error)
      *error = QObject::tr("会话字段长度超过档案合同上限");
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  // H2-W6：权威保存成功之后才更新派生缓存；sidecar 写失败不影响 save=true
  //（不产生假欠账）——过期条目由新鲜度戳自动判为待修复，下次列表补齐。
  const QFileInfo info(path);
  const qint64 size = info.size();
  const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
  paleo::chat::ChatSessionIndex::write(
    paleo::chat::ChatSessionIndex::summarize(session, size), size, mtimeMs);
  return true;
}

bool ChatSessionStore::load(const QString &sessionId, ChatSession *out,
                            QString *error,
                            paleo::chat::ChatArchiveReject *reject) {
  const auto fail = [&](paleo::chat::ChatArchiveReject code,
                        const QString &message) {
    if (error)
      *error = message;
    if (reject)
      *reject = code;
    return false;
  };
  const QString path = pathFor(sessionId);
  if (path.isEmpty())
    return fail(paleo::chat::ChatArchiveReject::UnsafeId,
                QObject::tr("会话 id 含非法路径字符"));
  // H2-W2 私有文件边界：先验身份与形态，再有界读取。目录/设备/叶子符号
  // 链接一律拒绝；根与叶同规则——把会话根解析成真实路径后，要求文件解析
  // 后的真实路径恰为 <root>/<id>.json，中间目录的符号链接/junction 穿越
  // 与叶子别名同拒。根与叶的处理保持一致（不是只查传入字符串含不含斜杠）。
  QFileInfo info(path);
  if (!info.exists())
    return fail(paleo::chat::ChatArchiveReject::NotFound,
                QObject::tr("会话文件不存在"));
  if (info.isSymLink() || !info.isFile())
    return fail(paleo::chat::ChatArchiveReject::NotRegularFile,
                QObject::tr("会话文件不是常规文件（目录/链接伪装被拒绝）"));
  const QString rootCanonical =
    QFileInfo(directory()).canonicalFilePath();
  const QString expected =
    rootCanonical + QLatin1Char('/') + sessionId + QStringLiteral(".json");
  if (rootCanonical.isEmpty() || info.canonicalFilePath() != expected)
    return fail(paleo::chat::ChatArchiveReject::OutsideRoot,
                QObject::tr("会话文件路径越过私有会话根（链接/别名被拒绝）"));
  const paleo::chat::ChatArchiveBudget budget = paleo::chat::chatArchiveBudget();
  if (info.size() > budget.maxArchiveBytes)
    return fail(paleo::chat::ChatArchiveReject::SizeExceeded,
                QObject::tr("会话档案超过 %1 MiB 存储上限（内容保留在磁盘，"
                            "仅元信息可列出）")
                  .arg(budget.maxArchiveBytes / (1024 * 1024)));
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return fail(paleo::chat::ChatArchiveReject::IoError, file.errorString());
  // 累计上限在读取过程中执行：打开后增长的文件同样被拦住（不只看打开前
  // 的 size），原始字节超限即停、原文件不动。
  paleo::chat::ChatArchiveReject readReject = paleo::chat::ChatArchiveReject::None;
  const QByteArray bytes =
    paleo::chat::readChatArchiveBytes(&file, budget.maxArchiveBytes, &readReject);
  if (readReject != paleo::chat::ChatArchiveReject::None) {
    const bool grew = readReject == paleo::chat::ChatArchiveReject::ReadLimitExceeded;
    return fail(readReject,
                grew ? QObject::tr("会话文件在读取中超过 %1 MiB 上限")
                             .arg(budget.maxArchiveBytes / (1024 * 1024))
                     : file.errorString());
  }
  // 正文解析成本在此发生（与后续成败无关）——热列表路径不应增长此计数。
  paleo::chat::noteChatArchiveFullLoad();
  QJsonParseError parseError;
  const QJsonDocument document =
    QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (error)
      *error = QObject::tr("会话文件不是合法 JSON：%1").arg(parseError.errorString());
    if (reject)
      *reject = parseError.error == QJsonParseError::NoError
                  ? paleo::chat::ChatArchiveReject::NotObject
                  : paleo::chat::ChatArchiveReject::JsonSyntax;
    return false;
  }
  // H2-W1 结构 + 身份合同：文件名 id 与 JSON 内部 id 必须一致（不做 trim/
  // 大小写折叠式「修复」）；schema/字段形状明确校验，toObject/toArray 的
  // 默认空值不再把坏结构伪装成合法空历史。未知未来 schema 拒绝完整载入。
  QString detail;
  const auto verdict = paleo::chat::validateChatArchiveObject(
    document.object(), sessionId, &detail);
  if (verdict != paleo::chat::ChatArchiveReject::None) {
    const QString code = paleo::chat::chatArchiveRejectCode(verdict);
    QString message;
    switch (verdict) {
    case paleo::chat::ChatArchiveReject::SchemaUnsupported:
      message = QObject::tr("会话档案 schema 版本不受支持（%1），拒绝载入")
                  .arg(detail);
      break;
    case paleo::chat::ChatArchiveReject::IdentityMismatch:
      message = QObject::tr("会话文件名与内部 id 不一致（%1），拒绝载入")
                  .arg(detail);
      break;
    default:
      message = QObject::tr("会话档案结构不合法：%1").arg(code);
      break;
    }
    return fail(verdict, message);
  }
  // 成功才覆盖 out：失败路径不部分覆盖、不清空既有会话。
  if (out)
    *out = ChatSession::fromJson(document.object());
  if (reject)
    *reject = paleo::chat::ChatArchiveReject::None;
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
  // H2-W6：删除成功后同步失效派生缓存（best-effort；失败仅留孤儿，
  // 由扫描期 prune 兜底——缓存永不反向「复活」或误报已删除的会话）。
  paleo::chat::ChatSessionIndex::removeFor(sessionId);
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
