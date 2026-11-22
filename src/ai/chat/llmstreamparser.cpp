// 层：功能
#include "llmclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <cmath>

// G1（模型通信）——SSE 增量解析器实现。
//
// 分帧按 WHATWG event-stream 口径做**字节级增量**判定（W3）：
//   · 行结束符 = LF / CRLF / 裸 CR 三选一；CRLF 两个字节被 TCP 在任意位置
//     拆开时只算一次换行——缓冲末尾的孤立 CR 留待下一块（可能是 CRLF 前半）；
//   · UTF-8 多字节序列不可能包含 0x0A/0x0D（续字节恒 ≥0x80），字节级扫行
//     不会把多字节字符劈断；完整行只在解码一次；
//   · 流首一次性剥 UTF-8 BOM（EF BB BF）——首字节不是 EF 即判定无 BOM，
//     首字节是 EF 但不足三字节时留待下一块；
//   · 字段名后的**单个**前导空格按规范剥离，多出的空格属于字段值——不做
//     indiscriminate trimmed（那会改写数据语义）；
//   · ':' 开头是注释（心跳），空行分发事件；event/id/retry 等无关字段忽略。
// 终局（[DONE] 或 markTerminal）后输入与产出一并停止：同一 TCP chunk 内
// [DONE] 之后附带的内容必须被忽略（W3/W5 共同要求）。
namespace {
// WHATWG：data 字段值 = 冒号后去掉至多一个前导空格的原文。
QByteArray sseFieldValue(const QByteArray &line, int colon) {
  QByteArray value = line.mid(colon + 1);
  if (!value.isEmpty() && value.startsWith(' '))
    value.remove(0, 1);
  return value;
}
} // namespace

void LlmStreamParser::feed(const QByteArray &chunk, QStringList *deltas,
                           bool *done, QString *error) {
  if (deltas)
    deltas->clear();
  if (m_terminal)
    return; // [DONE]/终局已过：后续字节不再产出（也不再报错）
  m_pending.append(chunk);

  // 流首 BOM 判定（一次性）。
  if (!m_bomChecked) {
    if (m_pending.isEmpty())
      return;
    if (static_cast<unsigned char>(m_pending.at(0)) == 0xEF) {
      if (m_pending.size() < 3)
        return; // 可能是 BOM 前半，留待下一块
      if (static_cast<unsigned char>(m_pending.at(1)) == 0xBB &&
          static_cast<unsigned char>(m_pending.at(2)) == 0xBF)
        m_pending.remove(0, 3);
    }
    m_bomChecked = true;
  }

  // Scan LF and CR together. Searching for an absent delimiter once per line
  // would rescan the entire suffix. Resume at the first unchecked byte when a
  // long line spans chunks, leaving a trailing CR unchecked until its pair arrives.
  const char *bytes = m_pending.constData();
  const qsizetype size = m_pending.size();
  qsizetype lineStart = 0;
  qsizetype scan = m_scanOffset;
  while (!m_terminal && scan < size) {
    const char byte = bytes[scan];
    if (byte != '\n' && byte != '\r') {
      ++scan;
      continue;
    }
    if (byte == '\r' && scan + 1 == size)
      break;
    processLine(m_pending.mid(lineStart, scan - lineStart), deltas, done, error);
    scan += byte == '\r' && bytes[scan + 1] == '\n' ? 2 : 1;
    lineStart = scan;
  }
  if (m_terminal) {
    m_pending.clear();
    m_scanOffset = 0;
  } else {
    m_pending.remove(0, lineStart);
    m_scanOffset = scan - lineStart;
  }
}

void LlmStreamParser::processLine(const QByteArray &line, QStringList *deltas,
                                  bool *done, QString *error) {
  if (line.isEmpty()) {
    // 空行：分发当前事件并复位字段状态。
    dispatchEvent(deltas, done, error);
    m_data.clear();
    m_sawDataLine = false;
    return;
  }
  if (line.startsWith(':'))
    return; // 注释/心跳行：SSE 允许，不参与解析
  const int colon = line.indexOf(':');
  if (colon < 0)
    return; // 只有字段名没有值（规范允许；本方向仅认 data）
  const QByteArray name = line.left(colon);
  if (name != QByteArrayLiteral("data"))
    return; // event:/id:/retry: 等字段：不使用，忽略而非报错
  m_sawDataLine = true;
  m_data += QString::fromUtf8(sseFieldValue(line, colon)) + QLatin1Char('\n');
}

void LlmStreamParser::dispatchEvent(QStringList *deltas, bool *done,
                                    QString *error) {
  if (!m_sawDataLine)
    return; // 注释行/无关字段构成的事件：无载荷
  // WHATWG：事件载荷 = 各 data 行以 '\n' 连接（累积时每行已追加 '\n'，
  // 分发时去掉最后一个）。
  if (m_data.endsWith(QLatin1Char('\n')))
    m_data.chop(1);
  const QString payload = m_data;
  if (payload == QLatin1String("[DONE]")) {
    m_terminal = true;
    if (done)
      *done = true;
    return;
  }
  if (payload.isEmpty())
    return; // 空 data 事件：合法，无内容
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(payload.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (error && error->isEmpty())
      *error = QObject::tr("流式事件不是合法 JSON：%1")
                   .arg(parseError.errorString());
    return;
  }
  const QJsonObject object = document.object();
  const QJsonObject usage = object.value(QStringLiteral("usage")).toObject();
  if (!usage.isEmpty()) {
    // usage 只在收尾事件（或全部事件）里出现，取最后一次即可。
    promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt(promptTokens);
    completionTokens =
      usage.value(QStringLiteral("completion_tokens")).toInt(completionTokens);
  }
  const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
  for (const QJsonValue &choiceValue : choices) {
    const QJsonObject choice = choiceValue.toObject();
    // choice 隔离（W4）：本客户端请求恒为单一回答（n=1），只有 index==0
    // （或缺省视为 0）参与；其余 choice 的文本/工具一律丢弃，禁止混拼。
    if (choice.contains(QStringLiteral("index"))) {
      const QJsonValue indexValue = choice.value(QStringLiteral("index"));
      if (!indexValue.isDouble())
        continue; // 非数值 index：无法归段，如实丢弃该 choice
      const double index = indexValue.toDouble();
      if (index != 0.0)
        continue;
    }
    accumulateChoice(choice, deltas, error);
  }
}

void LlmStreamParser::accumulateChoice(const QJsonObject &choice,
                                       QStringList *deltas, QString *error) {
  const QJsonValue reason = choice.value(QStringLiteral("finish_reason"));
  if (reason.isString() && !reason.toString().isEmpty())
    m_finishReason = reason.toString(); // 取最后一个非空（收尾帧权威）
  const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
  const QJsonValue content = delta.value(QStringLiteral("content"));
  if (content.isString() && !content.toString().isEmpty() && deltas)
    deltas->append(content.toString());
  const QJsonArray calls = delta.value(QStringLiteral("tool_calls")).toArray();
  for (const QJsonValue &callValue : calls) {
    const QJsonObject callObject = callValue.toObject();
    const QJsonValue indexValue = callObject.value(QStringLiteral("index"));
    if (!indexValue.isDouble())
      continue; // 无 index/非数值的增量无法归段——如实丢弃（不猜下标）
    // 类型/范围校验：非整数、越界（如 1e300 的 toInt 为实现定义）或负值
    // 一律丢弃——与真实 index 0 合并会静默混段，比丢帧更危险。
    const double rawIndex = indexValue.toDouble();
    if (!(rawIndex >= 0.0) || rawIndex > 2147483647.0 ||
        rawIndex != std::floor(rawIndex))
      continue;
    const int index = int(rawIndex);
    if (m_emitted.contains(index)) {
      // W4：调用已按完整帧交付，后续同 index 分片意味着「已交付内容被追加
      // 推翻」——协议翻覆，如实报错，绝不静默改写。
      if (error && error->isEmpty())
        *error = QObject::tr("工具调用 index=%1 在完整交付后再次收到分片")
                     .arg(index);
      continue;
    }
    LlmToolDraft draft = m_drafts.value(index);
    draft.index = index;
    const QJsonObject function =
      callObject.value(QStringLiteral("function")).toObject();
    const QJsonValue idValue = callObject.value(QStringLiteral("id"));
    if (idValue.isString() && !idValue.toString().isEmpty()) {
      const QString id = idValue.toString();
      if (!draft.id.isEmpty() && draft.id != id) {
        // 身份冲突（同 index 先后给出不同 id）：禁止无声覆盖。
        if (error && error->isEmpty())
          *error = QObject::tr("工具调用 index=%1 的 id 前后冲突（%2 → %3）")
                       .arg(index).arg(draft.id, id);
      } else {
        draft.id = id;
      }
    }
    const QJsonValue nameValue = function.value(QStringLiteral("name"));
    if (nameValue.isString() && !nameValue.toString().isEmpty()) {
      const QString name = nameValue.toString();
      if (!draft.name.isEmpty() && draft.name != name) {
        if (error && error->isEmpty())
          *error = QObject::tr("工具调用 index=%1 的 name 前后冲突（%2 → %3）")
                       .arg(index).arg(draft.name, name);
      } else {
        draft.name = name;
      }
    }
    const QJsonValue argumentsValue = function.value(QStringLiteral("arguments"));
    if (argumentsValue.isString()) {
      const QString fragment = argumentsValue.toString();
      if (!fragment.isEmpty()) {
        if (draft.argumentsComplete) {
          // 已封印后再到非空分片 = 协议翻覆（与是否已交付无关，判在事件
          // 处理期——切分不变）。
          if (error && error->isEmpty())
            *error = QObject::tr(
                         "工具调用 index=%1 的 arguments 在已完整后又收到分片")
                         .arg(index);
        } else {
          draft.arguments += fragment;
          ChatToolCall probe;
          probe.argumentsJson = draft.arguments;
          if (!draft.arguments.trimmed().isEmpty() && probe.hasValidArguments()) {
            draft.argumentsComplete = true;
            draft.sealSequence = ++m_sealCounter;
          }
        }
      }
    }
    m_drafts.insert(index, draft);
  }
}

QVector<ChatToolCall> LlmStreamParser::takeCompleteCalls() {
  QString error;
  const QVector<ChatToolCall> calls = takeCompleteCalls(&error);
  return calls;
}

QVector<ChatToolCall> LlmStreamParser::takeCompleteCalls(QString *error) {
  QVector<ChatToolCall> out;
  // 流中：只交付已封印且有身份的调用，按 **index 升序**（同位次以封印序
  // 稳定）——与 pre-G1 实现同一口径（pre-G1 也是「每次 takeCompleteCalls
  // 内按 index 排序」，见 dd73a322 版注释）。
  // 边界如实说明：index 升序是**批次内**口径；跨批次（多次 take）按到达序。
  // 对协议合规流（tool_calls 按 index 顺序分片下发，Chat Completions 的
  // 既定形状）封印序 == index 序，全局序列与一次喂入完全一致——等价表
  // 的「tool-calls-in-order」行即钉这一点。病态流（先给 index=1 完整帧
  // 再给 index=0）跨批次会按 [1,0] 到达，与 pre-G1 行为相同；不做跨批次
  // 重排缓冲：那要么推迟到流尾（违反「完整工具帧先于 finished 交付」的
  // 调用契约），要么在低 index 尚未出现时无法预知（信息不存在）。
  QList<LlmToolDraft> ready;
  QList<int> indices = m_drafts.keys();
  for (int index : indices) {
    const LlmToolDraft draft = m_drafts.value(index);
    if (draft.argumentsComplete && draft.hasIdentity())
      ready.append(draft);
  }
  std::sort(ready.begin(), ready.end(),
            [](const LlmToolDraft &a, const LlmToolDraft &b) {
              return a.index != b.index ? a.index < b.index
                                        : a.sealSequence < b.sealSequence;
            });
  for (const LlmToolDraft &draft : ready) {
    ChatToolCall call;
    call.id = draft.id;
    call.name = draft.name;
    call.argumentsJson = draft.arguments;
    out.append(call);
    m_drafts.remove(draft.index);
    m_emitted.insert(draft.index);
  }
  if (!m_terminal)
    return out;
  // 终局：剩余草稿逐一裁决——空实参 = 真无参调用（#278 兼容）；非空未封印
  // = 残缺/非对象/超预算，不得改造成 {}，如实报错；缺 name 同理。
  QList<int> remaining = m_drafts.keys();
  std::sort(remaining.begin(), remaining.end());
  for (int index : remaining) {
    const LlmToolDraft draft = m_drafts.value(index);
    if (!draft.hasIdentity()) {
      if (error && error->isEmpty())
        *error = QObject::tr("工具调用 index=%1 缺 name，无法构成调用").arg(index);
      continue;
    }
    if (draft.arguments.trimmed().isEmpty()) {
      ChatToolCall call;
      call.id = draft.id;
      call.name = draft.name;
      call.argumentsJson = draft.arguments;
      out.append(call);
      m_drafts.remove(index);
      m_emitted.insert(index);
      continue;
    }
    // 非空但未封印：合法 JSON 对象在封印期就该封上——这里是残缺/非对象/
    // 超预算实参。
    if (error && error->isEmpty()) {
      ChatToolCall probe;
      probe.argumentsJson = draft.arguments;
      if (!probe.argumentsWithinBudget())
        *error = QObject::tr("工具调用 index=%1 的 arguments 超出 %2 字节预算")
                     .arg(index)
                     .arg(ChatToolCall::kMaxArgumentBytes);
      else
        *error = QObject::tr("工具调用 index=%1 的 arguments 不是合法 JSON 对象")
                     .arg(index);
    }
  }
  return out;
}

void LlmStreamParser::markTerminal() {
  if (m_terminal)
    return;
  m_terminal = true;
  // EOF 兜底：残余缓冲按最后一行处理（无行结束符也构成一行——PW-061 的
  // 「最后一个事件没有空行结尾」场景；末尾孤立 CR 是行结束符不是内容），
  // 再分发挂起的事件。
  if (!m_pending.isEmpty()) {
    QByteArray line = m_pending;
    if (line.endsWith('\r'))
      line.chop(1);
    m_pending.clear();
    m_scanOffset = 0;
    QStringList deltas;
    QString error;
    processLine(line, &deltas, nullptr, &error);
    dispatchEvent(&deltas, nullptr, &error);
    // markTerminal 之后不再向调用方产出文本：这些增量属于「终局补行」，
    // 只用于清空解析器状态；错误同样不再上抛（终局由调用方统一裁决）。
  }
}

QVector<ChatToolCall> LlmStreamParser::flush() {
  QString error;
  return flush(&error);
}

QVector<ChatToolCall> LlmStreamParser::flush(QString *error) {
  markTerminal();
  return takeCompleteCalls(error);
}
