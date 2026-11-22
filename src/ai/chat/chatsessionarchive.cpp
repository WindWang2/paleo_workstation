// 层：功能
#include "chatsessionarchive.h"
#include "services/toolresultprotocol.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMutex>
#include <atomic>
#include <limits>

namespace paleo::chat {
namespace {
// 生效预算：production() 恒为缺省；测试可经 setChatArchiveBudgetForTesting
// 注入缩放档（互斥保护，读取返回快照拷贝——修复 worker 线程同样安全）。
QMutex g_budgetMutex;
ChatArchiveBudget g_budget = ChatArchiveBudget::production();
// 权威完整载入计数（诊断口径，见头文件）。atomic：修复 worker 在池线程
// 也会走 ChatSessionStore::load。
std::atomic<qint64> g_fullLoadCount{0};

bool knownRoleKey(const QString &key)
{
  return key == QLatin1String("system") || key == QLatin1String("user") ||
         key == QLatin1String("assistant") || key == QLatin1String("tool");
}

// 单段小字段的长度护栏（与写入侧既有边界对齐：绑定行 callId/turnId ≤128、
// 工具名 ≤128、流式 call id ≤256——见 aichatcontroller/chatinvocationbinding）。
bool boundedString(const QJsonObject &object, const char *key, qsizetype maxChars,
                   bool required, QString *detail)
{
  const QJsonValue value = object.value(QLatin1String(key));
  if (value.isUndefined()) {
    if (!required)
      return true;
    if (detail)
      *detail = QString::fromLatin1(key) + QStringLiteral(":missing");
    return false;
  }
  if (!value.isString() || value.toString().size() > maxChars) {
    if (detail)
      *detail = QString::fromLatin1(key) + QStringLiteral(":type_or_length");
    return false;
  }
  return true;
}

ChatArchiveReject validateMessageObject(const QJsonObject &message, QString *detail)
{
  const QJsonValue role = message.value(QLatin1String("role"));
  if (!role.isString() || !knownRoleKey(role.toString())) {
    // 未知/缺失角色直接拒绝：降级为 user 会把坏结构伪装成合法历史
    //（读侧旧降级行为只保留在 ChatMessage::fromJson 的内存映射里）。
    if (detail)
      *detail = QStringLiteral("messages.role:unknown");
    return ChatArchiveReject::StructureInvalid;
  }
  if (!boundedString(message, "content", std::numeric_limits<qsizetype>::max(), true, detail))
    return ChatArchiveReject::StructureInvalid; // content 必须是 string（长度由档案字节上限约束）
  if (!boundedString(message, "tool_call_id", 256, false, detail))
    return ChatArchiveReject::StructureInvalid;
  if (!boundedString(message, "host_turn_id", 128, false, detail))
    return ChatArchiveReject::StructureInvalid;
  if (!boundedString(message, "timestamp", 64, false, detail))
    return ChatArchiveReject::StructureInvalid; // 时间格式解析保持旧宽容口径，类型必须对
  const QJsonValue calls = message.value(QLatin1String("tool_calls"));
  if (!calls.isUndefined()) {
    if (!calls.isArray()) {
      if (detail)
        *detail = QStringLiteral("messages.tool_calls:not_array");
      return ChatArchiveReject::StructureInvalid;
    }
    for (const QJsonValue &call : calls.toArray()) {
      if (!call.isObject()) {
        if (detail)
          *detail = QStringLiteral("tool_calls.entry:not_object");
        return ChatArchiveReject::StructureInvalid;
      }
      const QJsonObject callObject = call.toObject();
      if (!boundedString(callObject, "id", 256, false, detail))
        return ChatArchiveReject::StructureInvalid;
      if (!boundedString(callObject, "type", 32, false, detail))
        return ChatArchiveReject::StructureInvalid;
      const QJsonValue function = callObject.value(QLatin1String("function"));
      if (!function.isUndefined()) {
        if (!function.isObject()) {
          if (detail)
            *detail = QStringLiteral("tool_calls.function:not_object");
          return ChatArchiveReject::StructureInvalid;
        }
        const QJsonObject functionObject = function.toObject();
        if (!boundedString(functionObject, "name", 128, false, detail))
          return ChatArchiveReject::StructureInvalid;
        // arguments 兼容旧档：string（现行 writer）或 object（读侧历史容错
        // 形态）都保留；其它类型经 toVariant 会被静默改成 {}——那就是把
        // 坏结构伪装成空参，拒绝。
        const QJsonValue arguments = functionObject.value(QLatin1String("arguments"));
        if (!arguments.isUndefined() && !arguments.isString() && !arguments.isObject()) {
          if (detail)
            *detail = QStringLiteral("tool_calls.arguments:type");
          return ChatArchiveReject::StructureInvalid;
        }
      }
    }
  }
  return ChatArchiveReject::None;
}
} // namespace

QString chatArchiveRejectCode(ChatArchiveReject reject)
{
  switch (reject) {
  case ChatArchiveReject::None: return QStringLiteral("ok");
  case ChatArchiveReject::UnsafeId: return QStringLiteral("unsafe_session_id");
  case ChatArchiveReject::NotFound: return QStringLiteral("not_found");
  case ChatArchiveReject::NotRegularFile: return QStringLiteral("not_regular_file");
  case ChatArchiveReject::OutsideRoot: return QStringLiteral("outside_session_root");
  case ChatArchiveReject::IoError: return QStringLiteral("io_error");
  case ChatArchiveReject::SizeExceeded: return QStringLiteral("archive_size_exceeded");
  case ChatArchiveReject::ReadLimitExceeded: return QStringLiteral("archive_read_limit_exceeded");
  case ChatArchiveReject::JsonSyntax: return QStringLiteral("json_syntax");
  case ChatArchiveReject::NotObject: return QStringLiteral("not_an_object");
  case ChatArchiveReject::SchemaMissing: return QStringLiteral("schema_missing");
  case ChatArchiveReject::SchemaUnsupported: return QStringLiteral("schema_unsupported");
  case ChatArchiveReject::StructureInvalid: return QStringLiteral("structure_invalid");
  case ChatArchiveReject::IdentityMismatch: return QStringLiteral("identity_mismatch");
  case ChatArchiveReject::MessageBudget: return QStringLiteral("message_budget_exceeded");
  }
  return QStringLiteral("unknown");
}

ChatArchiveReject validateChatArchiveObject(const QJsonObject &object,
                                             const QString &identity, QString *detail)
{
  const QJsonValue schema = object.value(QStringLiteral("schema"));
  if (!schema.isDouble()) {
    if (detail)
      *detail = QStringLiteral("schema:missing_or_type");
    return ChatArchiveReject::SchemaMissing;
  }
  const double schemaVersion = schema.toDouble();
  if (schemaVersion != 1.0) {
    if (detail)
      *detail = QStringLiteral("schema:%1").arg(schemaVersion);
    return ChatArchiveReject::SchemaUnsupported;
  }
  const QJsonValue id = object.value(QStringLiteral("id"));
  if (!id.isString() || id.toString().isEmpty() || id.toString().size() > 128) {
    if (detail)
      *detail = QStringLiteral("id:type_or_length");
    return ChatArchiveReject::StructureInvalid;
  }
  if (!identity.isEmpty() && id.toString() != identity) {
    // 文件名 id 与 JSON id 不一致：拒绝完整载入。不做 trim、大小写折叠或
    // 字符替换式「修复」——那会让「打开 A、保存/删除 B」悄悄成立。
    if (detail)
      *detail = QStringLiteral("id:%1!=%2").arg(identity, id.toString());
    return ChatArchiveReject::IdentityMismatch;
  }
  // 标题/模型：可缺省（displayTitle 兜底），出现时必须是 string 且在容忍
  // 上限内（新写入上限是 maxTitleChars；更长的旧标题按 maxTitleCharsLoad
  // 继续可读，不让合法遗留档案被新字段上限挤掉）。
  const ChatArchiveBudget budget = chatArchiveBudget();
  if (!boundedString(object, "title", budget.maxTitleCharsLoad, false, detail))
    return ChatArchiveReject::StructureInvalid;
  if (!boundedString(object, "model", 256, false, detail))
    return ChatArchiveReject::StructureInvalid;
  if (!boundedString(object, "created_at", 64, false, detail))
    return ChatArchiveReject::StructureInvalid;
  if (!boundedString(object, "updated_at", 64, false, detail))
    return ChatArchiveReject::StructureInvalid;
  const QJsonValue messages = object.value(QStringLiteral("messages"));
  if (!messages.isArray()) {
    if (detail)
      *detail = QStringLiteral("messages:missing_or_type");
    return ChatArchiveReject::StructureInvalid;
  }
  const QJsonArray array = messages.toArray();
  if (array.size() > budget.maxMessages) {
    if (detail)
      *detail = QStringLiteral("messages:%1>%2").arg(array.size()).arg(budget.maxMessages);
    return ChatArchiveReject::MessageBudget;
  }
  for (const QJsonValue &value : array) {
    if (!value.isObject()) {
      if (detail)
        *detail = QStringLiteral("messages.entry:not_object");
      return ChatArchiveReject::StructureInvalid;
    }
    const auto verdict = validateMessageObject(value.toObject(), detail);
    if (verdict != ChatArchiveReject::None)
      return verdict;
  }
  const QJsonValue bindings = object.value(QStringLiteral("host_tool_bindings"));
  if (!bindings.isUndefined()) {
    // 只做形状校验（数组 + 行对象）。行内字段的深校验与授权语义仍在
    // validateInvocationBindings / validateInvocationRetry（宿主准入）——
    // 绑定形状合法不构成任何执行授权。
    if (!bindings.isArray()) {
      if (detail)
        *detail = QStringLiteral("host_tool_bindings:not_array");
      return ChatArchiveReject::StructureInvalid;
    }
    for (const QJsonValue &row : bindings.toArray())
      if (!row.isObject()) {
        if (detail)
          *detail = QStringLiteral("host_tool_bindings.entry:not_object");
        return ChatArchiveReject::StructureInvalid;
      }
  }
  return ChatArchiveReject::None;
}

QByteArray readChatArchiveBytes(QIODevice *device, qint64 limit,
                                ChatArchiveReject *reject)
{
  if (reject)
    *reject = ChatArchiveReject::None;
  QByteArray out;
  if (limit <= 0) {
    if (reject)
      *reject = ChatArchiveReject::ReadLimitExceeded;
    return {};
  }
  char buffer[65536];
  forever {
    const qint64 chunk = device->read(buffer, sizeof(buffer));
    if (chunk < 0) {
      if (reject)
        *reject = ChatArchiveReject::IoError;
      return {};
    }
    if (chunk == 0)
      break;
    if (out.size() + chunk > limit) {
      // 打开后增长也拦得住：上限按累计读取量执行，不依赖打开前 size。
      if (reject)
        *reject = ChatArchiveReject::ReadLimitExceeded;
      return {};
    }
    out.append(buffer, int(chunk));
  }
  return out;
}

qint64 ChatArchiveBudget::titleReserve() const
{
  return qint64(escapeWorstFactor) * maxTitleChars + contentSlack;
}

qint64 ChatArchiveBudget::turnReserve() const
{
  const qint64 assistant =
    qint64(1 + toolRounds) * (qint64(escapeWorstFactor) * maxResponseBytes + contentSlack);
  const qint64 toolResults =
    qint64(callsPerTurn) * (qint64(escapeWorstFactor) * (resultPageBytes + contentSlack) + messageMetaSlack);
  const qint64 toolFrames =
    qint64(callsPerTurn) * (qint64(escapeWorstFactor) * toolArgumentBytes + contentSlack);
  const qint64 bindingRows = qint64(callsPerTurn) * bindingRowBytes;
  const qint64 messageMeta =
    qint64(1 + toolRounds + callsPerTurn + 4) * messageMetaSlack;
  return assistant + toolResults + toolFrames + bindingRows + messageMeta;
}

ChatArchiveBudget ChatArchiveBudget::production()
{
  ChatArchiveBudget budget; // knobs 缺省即既有业务上限（单点登记见头文件）
  budget.resultPageBytes = kResultPageBytes;
  budget.maxTitleChars = 256;
  budget.maxTitleCharsLoad = 4096;
  budget.maxMessages = 65536;
  // 上限结构式推导：fresh 空会话必须容得下一整轮最大合法工作（W3）。
  //   cap = turnReserve() + titleReserve() + 32 MiB 余量
  // 生产数值 ≈ 290.4 MiB + 1.8 KiB + 32 MiB ≈ 322.5 MiB：空会话首轮恒可
  // 通过；一轮最大工作落盘后（~290 MiB）再发新轮被拒（建议新建会话），
  // 既有 newSession / 导出 / G2 保存欠账语义不受影响。
  budget.maxArchiveBytes =
    budget.turnReserve() + budget.titleReserve() + 32ll * 1024 * 1024;
  return budget;
}

const ChatArchiveBudget chatArchiveBudget()
{
  QMutexLocker lock(&g_budgetMutex);
  return g_budget;
}

void setChatArchiveBudgetForTesting(const ChatArchiveBudget &budget)
{
  QMutexLocker lock(&g_budgetMutex);
  g_budget = budget;
}

qint64 chatArchiveFullLoadCount()
{
  return g_fullLoadCount.load();
}

void resetChatArchiveFullLoadCountForTesting()
{
  g_fullLoadCount.store(0);
}

void noteChatArchiveFullLoad()
{
  g_fullLoadCount.fetch_add(1);
}

qint64 chatArchiveSerializedBytes(const ChatSession &session)
{
  return QJsonDocument(session.toJson()).toJson(QJsonDocument::Compact).size();
}

qint64 chatArchiveMessageBytes(const ChatMessage &message)
{
  return QJsonDocument(message.toJson()).toJson(QJsonDocument::Compact).size();
}

qint64 chatArchiveSerializedBytesWithMessage(const ChatSession &session,
                                             const ChatMessage &message)
{
  // JSON 数组追加一个元素 = 元素本体 + 1 个逗号（可加性是精确例的前提）。
  return chatArchiveSerializedBytes(session) + chatArchiveMessageBytes(message) + 1;
}

qint64 chatArchiveSerializedPrefixBytes(const ChatSession &session,
                                        qsizetype messageCount)
{
  ChatSession prefix = session; // bindings 与会话元字段完整保留（retry 不截绑定）
  if (messageCount < session.messages.size())
    prefix.messages.resize(qMax<qsizetype>(0, messageCount));
  return chatArchiveSerializedBytes(prefix);
}

bool chatArchiveAcceptsTurn(const ChatSession &session, const QString &userText,
                            const ChatArchiveBudget &budget, QString *reason)
{
  if (session.messages.size() + 1 > budget.maxMessages) {
    if (reason)
      *reason = QStringLiteral("message_budget_exceeded");
    return false;
  }
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = userText;
  // 与真实落盘形态一致：appendMessage 写 timestamp，startEngineTurn 赋
  // host_turn_id（QUuid::Id128 = 32 码元）——探针按此精确计，不留缺口。
  user.timestamp = QDateTime::currentDateTimeUtc();
  user.hostTurnId = QString(32, QLatin1Char('0'));
  // 用户消息本体按精确序列化计（此刻可算）；其后的一轮增量按最坏预留
  //（含轮中合法 rename 的标题余量——改名不消耗本轮响应余量）。
  const qint64 projected = chatArchiveSerializedBytesWithMessage(session, user) +
                           budget.turnReserve() + budget.titleReserve();
  if (projected > budget.maxArchiveBytes) {
    if (reason)
      *reason = QStringLiteral("archive_budget_exceeded");
    return false;
  }
  return true;
}

bool chatArchiveAcceptsRetryPrefix(const ChatSession &session, qsizetype lastUser,
                                   const ChatArchiveBudget &budget, QString *reason)
{
  if (lastUser < 0 || lastUser >= session.messages.size() ||
      session.messages.at(int(lastUser)).role != ChatRole::User) {
    if (reason)
      *reason = QStringLiteral("retry_prefix_unavailable");
    return false;
  }
  const qint64 prefixBytes = chatArchiveSerializedPrefixBytes(session, lastUser + 1);
  // 被重发的用户消息已在前缀里；reserve 只需覆盖其后的新回合增量与标题。
  const qint64 projected = prefixBytes + budget.turnReserve() + budget.titleReserve();
  if (projected > budget.maxArchiveBytes) {
    if (reason)
      *reason = QStringLiteral("archive_budget_exceeded");
    return false;
  }
  if (lastUser + 1 > budget.maxMessages) {
    if (reason)
      *reason = QStringLiteral("message_budget_exceeded");
    return false;
  }
  return true;
}

bool chatArchiveAcceptsTitle(const ChatSession &session, const QString &title,
                             const ChatArchiveBudget &budget, QString *reason)
{
  if (title.size() > budget.maxTitleChars) {
    if (reason)
      *reason = QStringLiteral("title_field_budget_exceeded");
    return false;
  }
  ChatSession candidate = session;
  candidate.title = title;
  // 候选档案成本：轮中 rename 由本轮 titleReserve 兜底（|title| ≤ maxTitleChars
  // 时代数上仍落在准入界内）；空闲会话由下一次 send/retry 准入复核。
  if (chatArchiveSerializedBytes(candidate) > budget.maxArchiveBytes) {
    if (reason)
      *reason = QStringLiteral("archive_budget_exceeded");
    return false;
  }
  return true;
}

} // namespace paleo::chat
