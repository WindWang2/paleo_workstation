// 层：功能
#include "aiconversationpresentation.h"
#include "../services/toolresultprotocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace paleo::chat {
namespace {
// 不可解析内容的证据保留上限（呈现层另有显示上限；这里只防无界复制源）。
constexpr int kEvidenceCharCap = 4000;

bool isOperationObject(const QJsonObject &object) {
  return object.value(QStringLiteral("contract")) == QJsonValue(QStringLiteral("paleo.operation.v2")) ||
         (!object.value(QStringLiteral("operationId")).toString().isEmpty() &&
          (!object.value(QStringLiteral("execution")).toString().isEmpty() ||
           !object.value(QStringLiteral("commitState")).toString().isEmpty() ||
           !object.value(QStringLiteral("presentation")).toString().isEmpty()));
}

// envelope.status / 调用 ok 的 precedence：显式 failed 优先（不吞失败）；
// committed/succeeded 是已知成功；commit_uncertain/unknown 保持未知。
bool outcomeFromEnvelope(const QString &status, ToolResultFacts::Outcome *out) {
  if (status == QLatin1String("failed"))
    *out = ToolResultFacts::Outcome::Failed;
  else if (status == QLatin1String("succeeded") || status == QLatin1String("committed"))
    *out = ToolResultFacts::Outcome::Succeeded;
  else
    return false;
  return true;
}

void readOperationFacts(const QJsonObject &object, ToolResultFacts &facts) {
  if (!isOperationObject(object))
    return;
  facts.operationFacts = true;
  facts.operationId = object.value(QStringLiteral("operationId")).toString();
  facts.execution = object.value(QStringLiteral("execution")).toString();
  facts.commitState = object.value(QStringLiteral("commitState")).toString();
  facts.presentation = object.value(QStringLiteral("presentation")).toString();
  const auto revision = object.value(QStringLiteral("revision"));
  if (revision.isString() || revision.isDouble())
    facts.revision = revision.toString(QString::number(revision.toDouble()));
  const auto compute = object.value(QStringLiteral("computeCount"));
  if (compute.isDouble()) {
    facts.computeCount = compute.toDouble();
    facts.computeCountKnown = true;
  }
  const auto cancel = object.value(QStringLiteral("cancelRequested"));
  if (cancel.isBool())
    facts.cancelRequested = cancel.toBool();
  facts.commitReceiptRef = object.value(QStringLiteral("commitReceiptRef")).toString();
  facts.requestHash = object.value(QStringLiteral("requestHash")).toString();
  const auto ok = object.value(QStringLiteral("ok"));
  if (ok.isBool()) {
    facts.responseOk = ok.toBool();
    facts.responseOkKnown = true;
  }
}
} // namespace

ToolResultFacts projectToolResult(const QString &tool, const QString &resultJson,
                                  bool okKnown, bool ok) {
  ToolResultFacts facts;
  facts.tool = tool;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(resultJson.toUtf8(), &error);
  const bool parseOk = error.error == QJsonParseError::NoError;
  const QJsonObject object = parseOk ? document.object() : QJsonObject();

  if (!parseOk || (!document.isObject() && !document.isArray())) {
    // 不可解析（QJsonDocument 顶层只收 object/array，标量根同样落这里）：
    // 保留原文证据，绝不呈现为「成功的空结果」。
    facts.source = ToolResultFacts::Source::Unparseable;
    facts.rawText = resultJson;
    if (facts.rawText.size() > kEvidenceCharCap) {
      facts.rawText = facts.rawText.left(kEvidenceCharCap);
      facts.rawTruncated = true;
    }
    if (okKnown) {
      facts.outcome = ok ? ToolResultFacts::Outcome::Succeeded : ToolResultFacts::Outcome::Failed;
      facts.outcomeKnown = true;
    }
    return facts;
  }

  if (paleo::chat::isResultEnvelope(object)) {
    facts.source = ToolResultFacts::Source::Envelope;
    facts.status = object.value(QStringLiteral("status")).toString();
    facts.code = object.value(QStringLiteral("code")).toString();
    facts.payloadContract = object.value(QStringLiteral("payloadContract")).toString();
    const auto transport = object.value(QStringLiteral("transport")).toObject();
    facts.transportStatus = transport.value(QStringLiteral("status")).toString();
    facts.transportCode = transport.value(QStringLiteral("code")).toString();
    facts.transportKnown = true;
    const auto coverage = object.value(QStringLiteral("coverage")).toObject();
    if (coverage.contains(QStringLiteral("complete"))) {
      facts.coverageComplete = coverage.value(QStringLiteral("complete")).toBool();
      facts.coverageKnown = true;
    }
    facts.retryAdviceNoRepeat = object.value(QStringLiteral("retryAdvice")).toString() ==
                                QLatin1String("do_not_repeat_execution");
    const double encodedBytes = object.value(QStringLiteral("summary"))
                                  .toObject()
                                  .value(QStringLiteral("encodedBytes"))
                                  .toDouble();
    facts.encodedBytesNote = encodedBytes > 0
                               ? QString::number(encodedBytes, 'f', 0)
                               : QString();
    facts.data = object.value(QStringLiteral("data"));
    facts.hasData = !facts.data.isUndefined();
    facts.dataIsObject = facts.data.isObject();
    facts.resultRefs = object.value(QStringLiteral("resultRefs")).toArray();
    const auto omitted = object.value(QStringLiteral("omitted")).toArray();
    for (const auto &field : omitted)
      facts.omittedFields.append(field.toString());
    // envelope 顶层透传的 operation 事实（resultEnvelope 复制段）；顶层没带
    // 时回看 data（chatToolResponse 把三轴/operationId 放在出参根部）。
    readOperationFacts(object, facts);
    if (!facts.operationFacts && facts.dataIsObject)
      readOperationFacts(facts.data.toObject(), facts);
    ToolResultFacts::Outcome fromStatus;
    if (outcomeFromEnvelope(facts.status, &fromStatus)) {
      facts.outcome = fromStatus;
      facts.outcomeKnown = true;
    } else if (okKnown) {
      facts.outcome = ok ? ToolResultFacts::Outcome::Succeeded : ToolResultFacts::Outcome::Failed;
      facts.outcomeKnown = true;
    }
    // envelope 里 status=unknown/commit_uncertain 时显式保留未知。
    if (facts.status == QLatin1String("unknown") && !facts.outcomeKnown)
      facts.outcome = ToolResultFacts::Outcome::Unknown;
    return facts;
  }

  // 实时 raw 出参（含 chatToolResponse 包裹的 operation 事实）。
  facts.source = ToolResultFacts::Source::RawPayload;
  if (document.isArray()) { // 数组根：值本身 + 紧凑原文（dataIsObject=false）
    facts.data = document.array();
    facts.hasData = true;
    facts.dataIsObject = false;
    facts.rawText = QString::fromUtf8(paleo::chat::compactJson(facts.data));
  } else {
    facts.data = object;
    facts.hasData = true;
    facts.dataIsObject = true;
    facts.code = object.value(QStringLiteral("code")).toString();
    facts.status = object.value(QStringLiteral("status")).toString();
    readOperationFacts(object, facts);
  }
  if (okKnown) {
    facts.outcome = ok ? ToolResultFacts::Outcome::Succeeded : ToolResultFacts::Outcome::Failed;
    facts.outcomeKnown = true;
  } else {
    ToolResultFacts::Outcome fromStatus;
    if (outcomeFromEnvelope(facts.status, &fromStatus)) {
      facts.outcome = fromStatus;
      facts.outcomeKnown = true;
    }
    const auto okField = object.value(QStringLiteral("ok"));
    if (!facts.outcomeKnown && okField.isBool()) {
      facts.outcome = okField.toBool() ? ToolResultFacts::Outcome::Succeeded
                                       : ToolResultFacts::Outcome::Failed;
      facts.outcomeKnown = true;
    }
  }
  return facts;
}

QVector<HistoryToolCall> projectSessionTools(const ChatSession &session) {
  struct CallSite {
    int messageIndex = -1;
    QString name;
    QString argumentsJson;
    int occurrences = 0;
  };
  QHash<QString, CallSite> sites; // callId → 最后一个声明位（occurrences 记歧义）
  for (int index = 0; index < session.messages.size(); ++index) {
    const auto &message = session.messages[index];
    if (message.role != ChatRole::Assistant)
      continue;
    for (const auto &call : message.toolCalls) {
      auto &site = sites[call.id];
      site.messageIndex = index;
      site.name = call.name;
      site.argumentsJson = call.argumentsJson;
      ++site.occurrences;
    }
  }

  QVector<HistoryToolCall> out;
  QString turnId;
  int turnUserIndex = -1;
  for (int index = 0; index < session.messages.size(); ++index) {
    const auto &message = session.messages[index];
    if (message.role == ChatRole::User) {
      turnId = message.hostTurnId; // 旧记录未记 → 空（归属未验证）
      turnUserIndex = index;
      continue;
    }
    if (message.role != ChatRole::Tool)
      continue;
    HistoryToolCall entry;
    entry.toolMessageIndex = index;
    entry.toolCallId = message.toolCallId;
    entry.turnId = turnId;
    entry.turnUserIndex = turnUserIndex;
    const auto site = sites.constFind(message.toolCallId);
    if (site != sites.constEnd()) {
      entry.callPaired = true;
      entry.ambiguousCallId = site->occurrences > 1;
      entry.assistantMessageIndex = site->messageIndex;
      entry.toolName = site->name;
      entry.argumentsJson = site->argumentsJson;
    }
    if (session.hostBindingsValid) {
      for (const auto &value : session.hostToolBindings) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("turnId")).toString() == turnId &&
            row.value(QStringLiteral("callId")).toString() == message.toolCallId) {
          // 绑定只证明「当时宿主承认过这次调用」，不是现在的执行授权。
          entry.bindingKnown = true;
          entry.bindingDocumentId = row.value(QStringLiteral("documentId")).toString();
          break;
        }
      }
    }
    entry.hasResult = true;
    entry.facts = projectToolResult(entry.toolName, message.content);
    out.append(entry);
  }
  // 有调用、无结果帧（遗留未应答）：如实列出，结果缺失。
  QSet<QString> answered;
  for (const auto &entry : out)
    answered.insert(entry.toolCallId);
  for (auto it = sites.constBegin(); it != sites.constEnd(); ++it) {
    if (answered.contains(it.key()))
      continue;
    HistoryToolCall entry;
    entry.toolCallId = it.key();
    entry.toolName = it.value().name;
    entry.argumentsJson = it.value().argumentsJson;
    entry.assistantMessageIndex = it.value().messageIndex;
    entry.callPaired = true;
    entry.ambiguousCallId = it.value().occurrences > 1;
    // 无 tool 消息可回溯回合：以调用位之前最近的 user 帧计。
    for (int index = it.value().messageIndex; index >= 0; --index) {
      if (session.messages[index].role == ChatRole::User) {
        entry.turnUserIndex = index;
        entry.turnId = session.messages[index].hostTurnId;
        break;
      }
    }
    out.append(entry);
  }
  // 保持消息顺序稳定（补录的未应答项按 assistant 位排回）。
  std::sort(out.begin(), out.end(), [](const HistoryToolCall &a, const HistoryToolCall &b) {
    const int rankA = a.assistantMessageIndex >= 0 ? a.assistantMessageIndex : a.toolMessageIndex;
    const int rankB = b.assistantMessageIndex >= 0 ? b.assistantMessageIndex : b.toolMessageIndex;
    if (rankA != rankB)
      return rankA < rankB;
    return a.toolMessageIndex < b.toolMessageIndex;
  });
  return out;
}

TextPage decodeUtf8Display(const QByteArray &bytes, bool holdTail) {
  TextPage page;
  int hold = 0;
  for (int index = bytes.size() - 1; index >= 0 && hold <= 3; --index) {
    const auto byte = quint8(bytes.at(index));
    if ((byte & 0xC0) == 0x80) { // continuation
      ++hold;
      continue;
    }
    int length = 1;
    if (byte >= 0xF0 && byte <= 0xF7)
      length = 4;
    else if (byte >= 0xE0 && byte <= 0xEF)
      length = 3;
    else if (byte >= 0xC0 && byte <= 0xDF)
      length = 2;
    // lead 找到：序列完整则无需保留；不完整则连同 lead 一起留给下一页。
    if (length > 1 && hold == length - 1)
      hold = 0;
    else if (length > 1)
      hold += 1; // lead 也留下
    break;
  }
  // 连续 ≥4 个 continuation（无合法 lead）：不猜，整体按非法呈现。
  if (hold > 3)
    hold = 0;
  if (!holdTail)
    hold = 0; // 末页：不再为下一页保留尾巴，残缺序列按非法替换呈现
  page.trailingBytes = hold;
  const auto body = bytes.left(bytes.size() - hold);
  page.text = QString::fromUtf8(body);
  page.invalid = page.text.contains(QChar::ReplacementCharacter);
  return page;
}

} // namespace paleo::chat
