// 层：功能
#include "chatsessionindex.h"
#include "chatsessionarchive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

namespace paleo::chat {
namespace {
// sidecar 体积上限：列表元信息是几百字节量级；超限即判缓存损坏（重建），
// 不解释超大「索引」。
constexpr qint64 kMaxSidecarBytes = 2048;
// 缓存侧标题截取（仅影响列表显示；载入后仍用正文完整标题）。
constexpr qsizetype kSummaryTitleChars = 120;

QString boundedTitle(const QString &title)
{
  return title.size() <= kSummaryTitleChars
           ? title
           : title.left(kSummaryTitleChars) + QStringLiteral("…");
}

QString stateKey(ChatSessionSummary::State state)
{
  switch (state) {
  case ChatSessionSummary::State::Ok: return QStringLiteral("ok");
  case ChatSessionSummary::State::OverLimit: return QStringLiteral("over_limit");
  case ChatSessionSummary::State::Invalid: return QStringLiteral("invalid");
  }
  return QStringLiteral("invalid");
}

bool stateFromKey(const QString &key, ChatSessionSummary::State *out)
{
  if (key == QLatin1String("ok")) { *out = ChatSessionSummary::State::Ok; return true; }
  if (key == QLatin1String("over_limit")) { *out = ChatSessionSummary::State::OverLimit; return true; }
  if (key == QLatin1String("invalid")) { *out = ChatSessionSummary::State::Invalid; return true; }
  return false;
}
} // namespace

ChatArchiveScan scanChatArchiveFiles(const QString &root, int maxEntries)
{
  ChatArchiveScan scan;
  const QFileInfo rootInfo(root);
  if (!rootInfo.isDir())
    return scan;
  const auto infos = QDir(root).entryInfoList({QStringLiteral("*.json")},
                                              QDir::Files, QDir::NoSort);
  for (const QFileInfo &info : infos) {
    // 叶子边界与 load 同口径：符号链接/非常规文件不进列表（计数上报）；
    // 文件名还原出的 id 必须仍是单段安全名（防 "..json" 之类伪装）。
    if (info.isSymLink() || !info.isFile() ||
        !ChatSessionStore::isSafeSessionId(info.completeBaseName())) {
      ++scan.skippedIrregular;
      continue;
    }
    if (int(scan.files.size()) >= maxEntries) {
      scan.truncated = true;
      break;
    }
    ChatArchiveFileStamp stamp;
    stamp.id = info.completeBaseName();
    stamp.bytes = info.size();
    stamp.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    scan.files.append(stamp);
  }
  // 确定序：mtime 新者优先；同 mtime 按 id 升序（同秒批量写入也稳定）。
  std::sort(scan.files.begin(), scan.files.end(),
            [](const ChatArchiveFileStamp &a, const ChatArchiveFileStamp &b) {
              if (a.mtimeMs != b.mtimeMs)
                return a.mtimeMs > b.mtimeMs;
              return a.id < b.id;
            });
  return scan;
}

ChatSessionSummary ChatSessionIndex::summarize(const ChatSession &session,
                                               qint64 fileBytes)
{
  ChatSessionSummary summary;
  summary.id = session.id;
  summary.displayTitle = boundedTitle(session.displayTitle());
  summary.model = session.model;
  summary.createdAt = session.createdAt;
  summary.updatedAt = session.updatedAt;
  summary.fileBytes = fileBytes;
  return summary;
}

ChatSessionSummary ChatSessionIndex::Entry::toSummary() const
{
  ChatSessionSummary summary;
  summary.id = id;
  summary.displayTitle = displayTitle;
  summary.model = model;
  summary.createdAt = createdAt;
  summary.updatedAt = updatedAt;
  summary.state = state;
  summary.diagnostic = diagnostic;
  summary.fileBytes = sourceBytes;
  return summary;
}

QString ChatSessionIndex::indexPath()
{
  return ChatSessionStore::directory() + QStringLiteral("/index");
}

QString ChatSessionIndex::sidecarPathFor(const QString &sessionId)
{
  if (!ChatSessionStore::isSafeSessionId(sessionId))
    return QString();
  return indexPath() + QLatin1Char('/') + sessionId + QStringLiteral(".meta.json");
}

bool ChatSessionIndex::write(const ChatSessionSummary &summary,
                             qint64 sourceBytes, qint64 sourceMtimeMs)
{
  const QString path = sidecarPathFor(summary.id);
  if (path.isEmpty())
    return false;
  QDir().mkpath(indexPath());
  QJsonObject object;
  object.insert(QStringLiteral("schema"), 1);
  object.insert(QStringLiteral("id"), summary.id);
  object.insert(QStringLiteral("state"), stateKey(summary.state));
  object.insert(QStringLiteral("title"), boundedTitle(summary.displayTitle));
  object.insert(QStringLiteral("model"), boundedTitle(summary.model));
  object.insert(QStringLiteral("created_at"),
                summary.createdAt.toUTC().toString(Qt::ISODateWithMs));
  object.insert(QStringLiteral("updated_at"),
                summary.updatedAt.toUTC().toString(Qt::ISODateWithMs));
  object.insert(QStringLiteral("diagnostic"), summary.diagnostic);
  object.insert(QStringLiteral("source_bytes"), double(sourceBytes));
  object.insert(QStringLiteral("source_mtime_ms"), double(sourceMtimeMs));
  QSaveFile file(path);
  const QByteArray bytes =
    QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit())
    return false;
  return true;
}

bool ChatSessionIndex::read(const QString &sessionId, Entry *out)
{
  const QString path = sidecarPathFor(sessionId);
  if (path.isEmpty())
    return false;
  const QFileInfo info(path);
  if (!info.exists() || info.isSymLink() || !info.isFile() ||
      info.size() > kMaxSidecarBytes)
    return false; // 缺失/叶子别名/超大：一律当作待修复，不解释
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  ChatArchiveReject reject = ChatArchiveReject::None;
  const QByteArray bytes = readChatArchiveBytes(&file, kMaxSidecarBytes, &reject);
  if (reject != ChatArchiveReject::None)
    return false;
  const auto document = QJsonDocument::fromJson(bytes);
  if (!document.isObject())
    return false;
  const QJsonObject object = document.object();
  // 缓存只认自己的 schema=1 与文件名身份；对不上就丢弃重建。
  if (object.value(QStringLiteral("schema")).toDouble() != 1.0)
    return false;
  const QJsonValue id = object.value(QStringLiteral("id"));
  if (!id.isString() || id.toString() != sessionId)
    return false;
  const QJsonValue state = object.value(QStringLiteral("state"));
  Entry entry;
  entry.id = sessionId;
  if (!state.isString() || !stateFromKey(state.toString(), &entry.state))
    return false;
  const auto stringField = [&object](const char *key) {
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : QString();
  };
  entry.displayTitle = stringField("title");
  entry.model = stringField("model");
  entry.diagnostic = stringField("diagnostic");
  const auto stampField = [&object](const char *key, qint64 *out) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isDouble())
      return false;
    *out = qint64(value.toDouble());
    return true;
  };
  if (!stampField("source_bytes", &entry.sourceBytes) ||
      !stampField("source_mtime_ms", &entry.sourceMtimeMs) ||
      entry.sourceBytes < 0 || entry.sourceMtimeMs < 0)
    return false;
  entry.createdAt = QDateTime::fromString(stringField("created_at"), Qt::ISODateWithMs).toLocalTime();
  entry.updatedAt = QDateTime::fromString(stringField("updated_at"), Qt::ISODateWithMs).toLocalTime();
  if (out)
    *out = entry;
  return true;
}

void ChatSessionIndex::removeFor(const QString &sessionId)
{
  const QString path = sidecarPathFor(sessionId);
  if (!path.isEmpty())
    QFile::remove(path);
}

void ChatSessionIndex::pruneOrphans(const QStringList &liveIds)
{
  const QDir dir(indexPath());
  const auto sidecars = dir.entryInfoList({QStringLiteral("*.meta.json")}, QDir::Files);
  const QSet<QString> live(liveIds.cbegin(), liveIds.cend());
  for (const QFileInfo &sidecar : sidecars) {
    // 去掉 ".meta.json"（10 字符）——completeBaseName 只剥最后一个后缀。
    const QString id = sidecar.fileName().chopped(10);
    if (!live.contains(id))
      QFile::remove(sidecar.absoluteFilePath());
  }
}

ChatArchiveRepairOutcome repairChatArchiveSummaries(
    const QVector<ChatArchiveFileStamp> &candidates,
    const std::function<bool()> &shouldContinue)
{
  ChatArchiveRepairOutcome outcome;
  const ChatArchiveBudget budget = chatArchiveBudget();
  for (const ChatArchiveFileStamp &candidate : candidates) {
    if (shouldContinue && !shouldContinue())
      break; // 协作取消：已完成的条目照常上缴，剩余留待下一次修复
    const QString path = ChatSessionStore::pathFor(candidate.id);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) {
      ChatSessionIndex::removeFor(candidate.id);
      ++outcome.vanished;
      continue;
    }
    if (info.isSymLink() || !info.isFile()) {
      ++outcome.irregular;
      continue; // 目录/链接伪装：不读、不缓存，留待权威 load 分类拒绝
    }
    ChatSessionSummary summary;
    summary.id = candidate.id;
    summary.fileBytes = info.size();
    ChatArchiveReject reject = ChatArchiveReject::None;
    if (info.size() > budget.maxArchiveBytes) {
      // 超限旧档案：保留字节、只给元信息状态，不读正文、不伪称完整载入。
      summary.state = ChatSessionSummary::State::OverLimit;
      summary.diagnostic = chatArchiveRejectCode(ChatArchiveReject::SizeExceeded);
    } else {
      ChatSession session;
      if (!ChatSessionStore::load(candidate.id, &session, nullptr, &reject)) {
        summary.state = ChatSessionSummary::State::Invalid;
        summary.diagnostic = chatArchiveRejectCode(reject);
      } else {
        summary = ChatSessionIndex::summarize(session, info.size());
      }
    }
    ChatArchiveRepairItem item;
    item.stamp.id = candidate.id;
    item.stamp.bytes = info.size();
    item.stamp.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    item.summary = summary;
    item.written = ChatSessionIndex::write(summary, item.stamp.bytes, item.stamp.mtimeMs);
    outcome.items.append(item);
  }
  return outcome;
}

} // namespace paleo::chat
