// 层：功能
#include "aiconversationpresentation.h"
#include "chatinvocationbinding.h"
#include "../services/toolresultprotocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringConverter>

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
  using TurnCall = QPair<int, QString>;
  struct CallSite {
    const ChatToolCall *call = nullptr;
    int messageIndex = -1;
    int declarations = 0;
    int results = 0;
    bool answered = false;
  };
  QHash<TurnCall, CallSite> sites;
  QHash<QString, int> turnCounts;
  int turnUserIndex = -1;
  for (int index = 0; index < session.messages.size(); ++index) {
    const auto &message = session.messages[index];
    if (message.role == ChatRole::User) {
      turnUserIndex = index;
      ++turnCounts[message.hostTurnId];
    } else if (message.role == ChatRole::Assistant) {
      for (const auto &call : message.toolCalls) {
        if (call.id.isEmpty())
          continue;
        auto &site = sites[{turnUserIndex, call.id}];
        if (++site.declarations == 1) {
          site.call = &call;
          site.messageIndex = index;
        }
      }
    } else if (message.role == ChatRole::Tool && !message.toolCallId.isEmpty()) {
      ++sites[{turnUserIndex, message.toolCallId}].results;
    }
  }

  QHash<QPair<QString, QString>, QJsonObject> bindings;
  if (validateInvocationBindings(session).isEmpty()) {
    for (const auto &value : session.hostToolBindings) {
      const auto row = value.toObject();
      bindings.insert({row.value("turnId").toString(), row.value("callId").toString()}, row);
    }
  }

  const auto makeEntry = [&](const TurnCall &key, const CallSite &site, int resultIndex) {
    HistoryToolCall entry;
    entry.toolMessageIndex = resultIndex;
    entry.turnUserIndex = key.first;
    entry.toolCallId = key.second;
    if (key.first >= 0)
      entry.turnId = session.messages.at(key.first).hostTurnId;
    entry.ambiguousCallId = site.declarations > 1 || site.results > 1;
    if (site.call && (resultIndex < 0 || site.messageIndex < resultIndex)) {
      entry.callPaired = true;
      entry.assistantMessageIndex = site.messageIndex;
      entry.toolName = site.call->name;
      entry.argumentsJson = site.call->argumentsJson;
      if (!entry.ambiguousCallId && !entry.turnId.isEmpty() && turnCounts.value(entry.turnId) == 1) {
        const auto binding = bindings.constFind({entry.turnId, key.second});
        if (binding != bindings.constEnd() && invocationMatchesBinding(*site.call, *binding)) {
          entry.bindingKnown = true;
          entry.bindingDocumentId = binding->value("documentId").toString();
        }
      }
    }
    entry.hasResult = resultIndex >= 0;
    if (entry.hasResult)
      entry.facts = projectToolResult(entry.toolName, session.messages.at(resultIndex).content);
    return entry;
  };

  QVector<HistoryToolCall> out;
  turnUserIndex = -1;
  for (int index = 0; index < session.messages.size(); ++index) {
    const auto &message = session.messages[index];
    if (message.role == ChatRole::User) {
      turnUserIndex = index;
    } else if (message.role == ChatRole::Tool) {
      const TurnCall key{turnUserIndex, message.toolCallId};
      auto site = sites.find(key);
      auto entry = makeEntry(key, site == sites.end() ? CallSite{} : *site, index);
      if (entry.callPaired)
        site->answered = true;
      out.append(std::move(entry));
    }
  }
  for (auto site = sites.cbegin(); site != sites.cend(); ++site) {
    if (site->call && !site->answered)
      out.append(makeEntry(site.key(), *site, -1));
  }
  std::sort(out.begin(), out.end(), [](const HistoryToolCall &a, const HistoryToolCall &b) {
    const int rankA = a.hasResult ? a.toolMessageIndex : a.assistantMessageIndex;
    const int rankB = b.hasResult ? b.toolMessageIndex : b.assistantMessageIndex;
    return rankA != rankB ? rankA < rankB : a.toolCallId < b.toolCallId;
  });
  return out;
}

TextPage decodeUtf8Display(const QByteArray &bytes, bool holdTail) {
  TextPage page;
  if (holdTail && !bytes.isEmpty()) {
    auto lead = bytes.size() - 1;
    while (lead >= 0 && bytes.size() - lead <= 3 && (quint8(bytes.at(lead)) & 0xC0) == 0x80)
      --lead;
    if (lead >= 0 && bytes.size() - lead <= 3) {
      const auto first = quint8(bytes.at(lead));
      const auto length = bytes.size() - lead;
      const int expected = first >= 0xF0 && first <= 0xF4 ? 4 :
                           first >= 0xE0 && first <= 0xEF ? 3 :
                           first >= 0xC2 && first <= 0xDF ? 2 : 1;
      const auto second = length > 1 ? quint8(bytes.at(lead + 1)) : quint8(0);
      const bool validPrefix = length == 1 ||
        ((first != 0xE0 || second >= 0xA0) && (first != 0xED || second <= 0x9F) &&
         (first != 0xF0 || second >= 0x90) && (first != 0xF4 || second <= 0x8F));
      if (length < expected && validPrefix)
        page.trailingBytes = int(length);
    }
  }
  QStringDecoder decoder(QStringConverter::Utf8,
                         QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
  page.text = decoder(QByteArrayView(bytes).first(bytes.size() - page.trailingBytes));
  page.invalid = decoder.hasError();
  return page;
}

} // namespace paleo::chat
