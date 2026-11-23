// 层：功能
#include "aichatcontroller.h"
#include "aichatcontextprojection.h"
#include "aichatrecentsummaries.h"
#include "chatinvocationbinding.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>

// workflow/ — AI 对话助手编排实现（方向51 骨架，方向61 工具闭环）。
//
// 工具闭环状态机（一个用户轮内）：
//   sendUserText → LLM 轮（流式增量/工具帧累积）→ finished：
//     · 末条 assistant 带 toolCalls → 工具相：runner 逐帧执行 → 每结果一条
//       role=tool 消息（toolCallId 对账）→ 全部到齐 → 新 assistant 占位 +
//       再发一轮（带 tools[]），直到模型不再要工具（终答）。
//     · 往返数超 kMaxToolRounds → 准入前整帧拒绝，不执行、不加入后续请求。
//   cancel → 作废（口径见 aichattoolrunner.h）：在途结果不回灌，未应答的
//   tool_call_id 合成「已取消」role=tool 应答——否则历史里 assistant 带
//   calls 而缺应答，下轮请求直接违反协议（OpenAI 400）。

AiChatController::AiChatController(QObject *parent)
  : QObject(parent), m_client(this), m_configCommitter(this), m_runner(this),
    m_piBridge(this) {
  newSession();
  connect(&m_client, &LlmClient::deltaReceived, this,
          [this](const QString &text) {
            if (!m_streaming) return;
            if (m_piBridge.busy() && !m_piBridge.modelDelta(text)) return;
            if (m_session.messages.isEmpty() ||
                m_session.messages.last().role != ChatRole::Assistant)
              return; // 没有占位消息就不往历史里写（防御：协议外增量）
            m_session.messages.last().content += text;
            emit assistantDelta(text);
          });
  connect(&m_client, &LlmClient::toolCallReceived, this,
          [this](const ChatToolCall &call) {
            if (!m_streaming) return;
            ++m_streamToolCalls;
            if (m_streamToolCalls > kMaxCallsPerResponse ||
                call.argumentsJson.toUtf8().size() > kMaxToolArgumentBytes ||
                call.id.size() > 256 || call.name.size() > 128)
              m_roundRejected = true;
            if (m_roundRejected)
              return;
            if (!call.id.isEmpty()) {
              if (m_providerCalls.contains(call.id)) return;
              m_providerCalls.insert(call.id);
            }
            ChatToolCall admitted = hostAdmittedCall(call);
            if (admitted.id.isEmpty())
              admitted.id = QUuid::createUuid().toString(QUuid::Id128);
            if (!m_session.messages.isEmpty() &&
                m_session.messages.last().role == ChatRole::Assistant) {
              const auto &prior = m_session.messages.last().toolCalls;
              for (const auto &existing : prior)
                if (existing.id == admitted.id) return;
              m_session.messages.last().toolCalls.append(admitted);
            }
            AiToolDispatch dispatch;
            if (call.argumentsWithinBudget()) {
              const auto args = QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object();
              dispatch = dispatchAiTool(call.name, args);
              if (dispatch.status == AiToolDispatchStatus::Routed) {
                auto code = negotiatedToolError(call);
                // The card classifies a proposed call without reaching a live
                // provider. Runtime readiness is checked at actual dispatch.
                if (code.isEmpty())
                  code = dispatch.spec.validateExternalArguments(args);
                if (!code.isEmpty()) {
                  dispatch.status = AiToolDispatchStatus::Disabled;
                  dispatch.note = code;
                }
              }
            } else {
              dispatch.status = AiToolDispatchStatus::Disabled;
              dispatch.note = QStringLiteral("request_budget_exceeded");
            }
            // 卡片文案 = 分类标签 + 入口 + 原因（诚实：能不能路由、缺什么都
            // 写明；执行状态由 toolExecutionStarted/toolResultReady 后续更新）。
            const QString text =
              aiToolDispatchLabel(dispatch.status) +
              (dispatch.target.isEmpty() ? QString()
                                         : QStringLiteral(" · %1").arg(dispatch.target)) +
              (dispatch.note.isEmpty() ? QString()
                                       : QStringLiteral(" · %1").arg(dispatch.note));
            emit toolCallDispatched(admitted, text);
          });
  connect(&m_client, &LlmClient::finished, this,
          [this](const QString &reason, int promptTokens, int completionTokens) {
            Q_UNUSED(promptTokens)
            Q_UNUSED(completionTokens)
            onRoundFinished(reason);
          });
  connect(&m_client, &LlmClient::errorOccurred, this,
          [this](LlmErrorKind kind, const QString &message) {
            handleModelError(kind, message);
          });
  connect(&m_runner, &AiChatToolRunner::toolStarted, this,
          [this](const ChatToolCall &call) {
            emit toolExecutionStarted(call);
          });
  connect(&m_runner, &AiChatToolRunner::toolFinished, this,
          [this](const ChatToolCall &call, bool ok, const QString &resultJson) {
            onToolFinished(call, ok, resultJson);
          });
  connect(&m_piBridge, &PiChatBridge::modelRequested, this, [this] { if (m_streaming) sendRound(); else m_piBridge.abort(false); });
  connect(&m_piBridge, &PiChatBridge::toolRequested, this, [this](const ChatToolCall &call) {
    if (!m_streaming || !m_unansweredCalls.contains(call)) { m_piBridge.abort(false); return; }
    dispatchTurnTool(call);
  });
  connect(&m_piBridge, &PiChatBridge::failed, this, &AiChatController::failTurn);
  connect(&m_piBridge, &PiChatBridge::finished, this, [this](const QString &code) {
    if (!m_streaming) return;
    if (code != "ok") { failTurn(code); return; }
    const QPointer<AiChatController> guard(this); const auto sequence = m_turnSequence;
    setStreaming(false); if (guard && sequence == m_turnSequence) {
      // G2-W3：Pi 正常收尾的落盘失败不能被静默吞掉——回答与工具事实仍在
      // 内存（保存欠账），用既有错误信号如实呈现，不冒充持久成功。
      QString error;
      if (!saveCurrentSession(&error)) emit errorOccurred(tr("回合已完成，但会话保存失败（%1）。内容保留在内存").arg(error));
    }
  });
  // H2：轻量最近会话元信息查询协调器（子对象；结果原样转发，控制器不
  // 加第二套列表语义）。
  m_recentSummaries = new AiChatRecentSummaries(this);
  qRegisterMetaType<QVector<paleo::chat::ChatSessionSummary>>(
    "QVector<paleo::chat::ChatSessionSummary>");
  connect(m_recentSummaries, &AiChatRecentSummaries::ready, this,
          [this](const QVector<paleo::chat::ChatSessionSummary> &summaries,
                 bool complete) {
            emit recentSessionSummariesReady(summaries, complete);
          });
}

namespace {
// H2-W6：rename 容量拒绝的用户可读原因——标题字段超限与「候选档案成本超限」
// 是两件事，不混报（后者时标题本身并不长）。
QString renameCapacityError(const QString &reason)
{
  if (reason == QLatin1String("archive_budget_exceeded"))
    return AiChatController::tr("会话容量不足（%1）：改名会使会话超过本机档案上限；"
                                "请缩短标题，或用「新会话」继续新话题")
      .arg(reason);
  return AiChatController::tr("会话标题超限（%1）").arg(reason);
}
} // namespace

// 协议完整性（见文件头注释）：尾部 assistant 的 tool_calls 若没有对应
// role=tool 应答，下一轮请求就违反「每个 tool_call 必有一条应答」
// （OpenAI 400，且该会话每轮复现，直到该轮滑出历史窗口）。按 reason 合成
// 错误应答补齐缺口——取消 / 请求出错 / 往返超限三条停轮路径共用同一补齐，
// 缺一条都会把协议违规留给后续每一轮。
QString AiChatController::statusText() const {
  if (m_streaming && m_pendingToolResults > 0)
    return tr("正在执行领域工具…");
  if (m_streaming)
    return tr("正在生成回答…");
  // G2-W3：保存欠账是一等状态——不冒充就绪，指明内容仍在内存与恢复入口。
  if (m_saveArrears && !m_session.messages.isEmpty())
    return tr("会话保存失败：%1（内容保留在内存，重发上一条可先补保存）").arg(m_saveArrearsError);
  const QString invalid = m_config.validate();
  if (!invalid.isEmpty())
    return invalid; // 禁用原因原样给 UI（「尚未配置 API 密钥——助手处于禁用态」等）
  return tr("就绪：%1").arg(m_config.model);
}

QString AiChatController::systemPrompt() const {
  return buildSystemPrompt(tools(), m_projectBrief);
}

QString AiChatController::buildSystemPrompt(const QVector<AiToolSpec> &tools, const QString &brief) const {
  QString declarations;
  for (const AiToolSpec &spec : tools)
    declarations += QStringLiteral("  · %1 — %2\n").arg(spec.name, spec.description);
  QString prompt = tr(
    "你是 Paleo Workbench 里的地质解释助手。回答必须用中文，术语按石油地质/"
    "地震解释行业惯例。\n"
    "红线（不可越过）：\n"
    "  1. 按用户已授权的意图调用工具。查询只读；prepare 和 compute-only 写私有暂存；"
    "publish 会登记成果及呈现图层；测井相预测会向已配置服务传输输入。"
    "工具的 execution、commitState、presentation 分别报告执行、数据提交及呈现事实，"
    "不能把已提交但呈现待修复说成未提交。追踪建议仍须逐条接受后才写解释。\n"
    "  2. 数据不足就直说数据不足，不得编造井号、层位、深度或数值；工具报错时"
    "如实转述错误，不得虚构结果。\n"
    "  3. 涉及计算时说明所用方法与假设，不确定的地方标注不确定。\n"
    "当前可调用的领域工具（如实解读结果及副作用）：\n%1")
    .arg(declarations);
  // 方向77：工程概况常驻轻注入（≤3 行，预算口径见 aichattoolrunner 的
  // projectBrief；数据以工具实查为准——这段只是开局的指北针）。
  if (!brief.isEmpty())
    prompt += tr("\n当前工程概况（绑定时的 catalog 计数，实查请用 "
                 "paleo.query_project）：\n%1\n")
                .arg(brief);
  return prompt;
}

QVector<ChatSession> AiChatController::recentSessions(int limit) const {
  return ChatSessionStore::loadRecent(limit);
}

void AiChatController::requestRecentSessionSummaries(int limit) {
  // 任务服务取装配根在 runner 上的既有绑定（只读取用，不新增装配/改
  // PaleoTaskService）；未绑定时协调器只发布可信缓存的部分结果。
  m_recentSummaries->setTaskService(m_runner.taskService());
  m_recentSummaries->refresh(limit);
}

bool AiChatController::recentSummaryRepairInFlight() const {
  return m_recentSummaries && m_recentSummaries->repairInFlight();
}

void AiChatController::newSession() {
  ++m_turnSequence;
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence + (m_streaming ? 1 : 0);
  // 切换语义（方向62 钉死）：弃当前会话前先取消在飞请求（迟到增量
  // 不许写进新会话），再落盘——未保存的半轮回答不因「新会话」丢失；
  // 空会话不落盘（saveCurrentSession 内部判空）。
  if (m_streaming)
    interrupt();
  if (!guard || sequence != m_turnSequence) return;
  saveCurrentSession();
  if (!guard || sequence != m_turnSequence) return;
  // G2-W3 薄门：保存欠账未清（落盘仍失败）时拒绝切换——未保存的半轮回答
  // 与工具结果不能被「新会话」无声丢弃；恢复存储后同一入口即可正常切换。
  if (m_saveArrears && !m_session.messages.isEmpty()) {
    emit errorOccurred(tr("会话尚未保存成功（%1），已保留当前会话；请恢复存储后重试").arg(m_saveArrearsError));
    emit sessionListChanged(); // 显示态回到真实当前会话
    return;
  }
  m_session = ChatSession();
  m_session.id = ChatSessionStore::newSessionId();
  // G2-W1：会话已切换——上一轮的冻结快照不再归属新会话，落盘回退当前配置。
  m_turnFrozen = false;
  m_hostTurn.clear();
  m_turnContext = {};
  m_runner.setResultSession(m_session.id);
  if (!guard || sequence != m_turnSequence) return;
  m_session.model = m_config.model;
  m_session.createdAt = QDateTime::currentDateTime();
  m_session.updatedAt = m_session.createdAt;
  emit sessionChanged();
  if (guard && sequence == m_turnSequence) emit sessionListChanged();
}

bool AiChatController::loadSession(const QString &sessionId, QString *error) {
  ++m_turnSequence;
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence + (m_streaming ? 1 : 0);
  // 切换语义同 newSession：先取消在飞请求（防迟到增量污染目标会话），
  // 再落盘再换，未保存内容不丢。
  if (m_streaming)
    interrupt();
  if (!guard || sequence != m_turnSequence) return false;
  saveCurrentSession();
  if (!guard || sequence != m_turnSequence) return false;
  // G2-W3 薄门：欠账未清时拒绝切换——不发成功 sessionChanged（G5 侧不因此
  // 清当前草稿），sessionListChanged 让会话下拉回到真实当前选择。
  if (m_saveArrears && !m_session.messages.isEmpty()) {
    const auto refusal = tr("会话尚未保存成功（%1），已保留当前会话；请恢复存储后重试").arg(m_saveArrearsError);
    if (error) *error = refusal;
    emit errorOccurred(refusal);
    emit sessionListChanged();
    return false;
  }
  ChatSession loaded;
  if (!ChatSessionStore::load(sessionId, &loaded, error)) {
    // 失败也要发列表信号：UI 下拉可能已经把显示切到目标会话，
    // 刷新让它回到真实的当前会话（不许显示态与事实脱钩）。
    emit sessionListChanged();
    return false;
  }
  m_session = loaded;
  // G2-W1：同 newSession——载入的会话不属于任何在途回合，冻结随之失效。
  m_turnFrozen = false;
  m_hostTurn.clear();
  m_turnContext = {};
  m_runner.setResultSession(m_session.id);
  if (!guard || sequence != m_turnSequence) return false;
  emit sessionChanged();
  if (guard && sequence == m_turnSequence) emit sessionListChanged();
  return true;
}

bool AiChatController::removeSession(const QString &sessionId, QString *error) {
  const QPointer<AiChatController> guard(this);
  if (sessionId == m_session.id)
    newSession();
  if (!guard) return false;
  // G2-W3：欠账薄门拒绝了切换（newSession 未生效）时，当前会话仍是
  // sessionId——此刻删除文件等于销毁唯一证据，必须拒绝并保留原会话。
  if (sessionId == m_session.id) {
    if (error) *error = tr("会话尚未保存成功（%1），暂不能删除；请恢复存储后重试").arg(m_saveArrearsError);
    return false;
  }
  const bool ok = ChatSessionStore::remove(sessionId, error);
  if (ok)
    emit sessionListChanged();
  return ok;
}

bool AiChatController::saveCurrentSession(QString *error) {
  if (m_session.messages.isEmpty())
    return true; // 空会话不落盘（避免用户目录里堆垃圾文件）
  m_session.model = currentSessionModel(); // G2-W1：回合内冻结模型记录字段
  m_session.updatedAt = QDateTime::currentDateTime();
  QString localError;
  const bool ok = ChatSessionStore::save(m_session, &localError);
  // G2-W3：保存欠账统一在此记账——失败=true（内存有未落盘内容），成功清账。
  if (ok) {
    m_saveArrears = false;
    m_saveArrearsError.clear();
    emit sessionListChanged(); // 标题/updatedAt 变化，列表要刷新
  } else {
    m_saveArrears = true;
    m_saveArrearsError = localError.isEmpty() ? tr("未知存储错误") : localError;
    if (error) *error = m_saveArrearsError;
  }
  return ok;
}

bool AiChatController::renameSession(const QString &sessionId,
                                     const QString &title, QString *error) {
  const QString trimmed = title.trimmed();
  if (trimmed.isEmpty()) {
    if (error)
      *error = tr("会话标题不能为空");
    return false;
  }
  // H2-W6：标题赋值前的纯容量检查（新标题字段上限 + 候选档案成本；轮中
  // rename 由本轮预留的标题余量兜底，不消耗响应余量）。容量拒绝不留新
  // 标题、不发成功列表更新、不制造保存欠账；真实写盘失败继续走既有
  // G2 欠账语义。
  const paleo::chat::ChatArchiveBudget budget = paleo::chat::chatArchiveBudget();
  QString capacityReason;
  if (sessionId == m_session.id) {
    if (!paleo::chat::chatArchiveAcceptsTitle(m_session, trimmed, budget,
                                              &capacityReason)) {
      if (error)
        *error = renameCapacityError(capacityReason);
      return false;
    }
    // 当前会话：改内存态；空会话不落盘（守住「空会话不出文件」的既有
    // 纪律——标题跟着首次真实落盘一起持久化）。
    m_session.title = trimmed;
    if (m_session.messages.isEmpty()) {
      emit sessionListChanged();
      return true;
    }
    // G2-W3：走统一落盘入口——成功即清保存欠账（内部已发 sessionListChanged），
    // 失败则欠账如实保留，不再绕过记账直写文件。
    return saveCurrentSession(error);
  }
  ChatSession stored;
  if (!ChatSessionStore::load(sessionId, &stored, error))
    return false;
  // 非当前会话：先按候选标题校验再保存（拒绝时磁盘与内存都不动）。
  if (!paleo::chat::chatArchiveAcceptsTitle(stored, trimmed, budget,
                                            &capacityReason)) {
    if (error)
      *error = renameCapacityError(capacityReason);
    return false;
  }
  stored.title = trimmed;
  const bool ok = ChatSessionStore::save(stored, error);
  if (ok)
    emit sessionListChanged();
  return ok;
}

bool AiChatController::exportCurrentSession(const QString &filePath,
                                            QString *error) {
  if (filePath.isEmpty()) {
    if (error)
      *error = tr("导出路径为空");
    return false;
  }
  // 先落盘：导出内容与磁盘会话一致（落盘失败如实中断导出，不静默分叉）。
  if (!saveCurrentSession(error))
    return false;
  return ChatSessionStore::exportMarkdown(m_session, filePath, error);
}

void AiChatController::applyConfig(
  const LlmConfig &config, const QByteArray &newKey, bool clearKey,
  QObject *callbackContext, std::function<void(bool, const QString &)> done) {
  // G1（W7）：提交协议收编进 LlmConfigCommitter（串行链 + 文件快照补偿，
  // 细节见 llmconfigcommit.h）。对外签名与语义不变：结构校验失败同步拒绝；
  // 密钥语义保持 newKey 优先 / clearKey 清除 / 沿用当前；成功仍经 setConfig
  // 生效；done(ok) 时配置已生效，callbackContext 先亡则回调静默丢弃。
  m_configCommitter.submit(config, newKey, clearKey, m_config,
                           callbackContext, std::move(done),
                           [this](const LlmConfig &applied) {
                             setConfig(applied);
                           });
}

bool AiChatController::canRetry() const {
  if (m_streaming || m_runner.busy() || m_piBridge.busy())
    return false;
  for (int i = m_session.messages.size() - 1; i >= 0; --i)
    if (m_session.messages.at(i).role == ChatRole::User)
      return true;
  return false;
}

void AiChatController::retryLast() {
  if (!canRetry())
    return;
  if (!enabled()) {
    emit errorOccurred(m_config.validate()); // 与 sendUserText 同一诚实口径
    return;
  }
  const QPointer<AiChatController> guard(this); const auto sequence = ++m_turnSequence; const auto sessionId = m_session.id;
  const auto context = m_runner.hostContextIdentity();
  int lastUser = -1;
  for (int i = m_session.messages.size() - 1; i >= 0; --i) {
    if (m_session.messages.at(i).role == ChatRole::User) {
      lastUser = i;
      break;
    }
  }
  const auto retryCode = paleo::chat::validateInvocationRetry(m_session, lastUser);
  if (!retryCode.isEmpty()) { emit errorOccurred(retryCode); return; }
  // G2-W7：重试是清保存欠账的生产行为入口——欠账仍无法落盘时不得截掉
  // 唯一内存证据，也不发新的模型请求；恢复存储后同一入口先补保存再续轮。
  QString arrearsError;
  if (!settleSaveArrears(&arrearsError)) {
    emit errorOccurred(tr("会话尚未保存成功（%1），已保留原回答；请恢复存储后重试").arg(arrearsError));
    return;
  }
  if (!guard || sequence != m_turnSequence || sessionId != m_session.id) return;
  // H2-W3：重试与发送同门——「截至最后 User 的候选前缀 + 仍保留的完整
  // bindings/会话元字段 + 一轮最坏预留」的纯评估，必须先于 resize 删除旧
  // 回答（合法近上限旧档案即使可载入，也可能不足重试预留）。拒绝时旧回
  // 答、工具事实、bindings 与既有欠账状态原样保留，不发新模型请求，也不
  // 发成功 sessionChanged。
  QString capacityReason;
  if (!paleo::chat::chatArchiveAcceptsRetryPrefix(m_session, lastUser,
                                                  paleo::chat::chatArchiveBudget(),
                                                  &capacityReason)) {
    emit errorOccurred(tr("会话容量不足（%1）：已保留原回答与工具记录；"
                          "请用「新会话」继续新话题。")
                         .arg(capacityReason));
    return;
  }
  // 截掉旧回答（含其携带的工具帧），历史回到「刚发完这条用户消息」。
  m_session.messages.resize(lastUser + 1);
  emit sessionChanged(); // 历史被截断：整页重画（UI 不做增量反演）
  if (!guard || sequence != m_turnSequence || sessionId != m_session.id) return;

  // C-1（full-audit-20261021）：收编进 sendRound——此前手工复制占位+上送
  // 序列时漏了 tools() 实参，重试轮发出的请求不含领域工具，模型静默失去
  // 工具调用能力（与上方注释自述的「重试轮恢复工具」意图相悖）。
  startEngineTurn(context);
}

void AiChatController::appendMessage(const ChatMessage &message) {
  m_session.messages.append(message);
  m_session.updatedAt = QDateTime::currentDateTime();
  if (m_session.messages.size() == 1 && message.role == ChatRole::User &&
      m_session.title.isEmpty())
    m_session.title = message.content.trimmed().left(40);
  emit messageAppended(message);
}

void AiChatController::setStreaming(bool streaming) {
  if (m_streaming == streaming)
    return;
  m_streaming = streaming;
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence;
  emit streamingChanged(streaming);
  if (!guard || sequence != m_turnSequence || m_streaming != streaming) return;
  emit statusChanged(statusText());
}

int AiChatController::estimateTokens(const QString &text) {
  // 字符近似（非精确 tokenizer）：ASCII ≈ 4 字符/token，其余（CJK 为主）
  // ≈ 1 字符/token。只用于历史开窗的保守预算，不进任何用户可见的
  // 「精确 token 数」声明。
  int ascii = 0, wide = 0;
  for (const QChar &c : text) {
    if (c.unicode() < 0x80)
      ++ascii;
    else
      ++wide;
  }
  return ascii / 4 + wide;
}

// 历史→请求投影（原 requestMessages 的职责）已收编进只读投影组件
// aichatcontextprojection.{h,cpp}（H1）：按最终发送形态计价、连续后缀选择、
// 逐轮 tool 配对校验、唯一宿主 System、空占位不发送——语义与预算口径见该
// 头注；controller 只在 sendRound 消费投影结果。

void AiChatController::sendRound() {
  if (!ensureTurnContext()) return;
  const QPointer<AiChatController> guard(this); const auto session = m_session.id;
  const auto sequence = m_turnSequence;
  ++m_modelRound;
  m_roundRejected = false;
  m_streamToolCalls = 0;
  m_providerCalls.clear();
  ChatMessage placeholder; // 流式增量往这条上累积
  placeholder.role = ChatRole::Assistant;
  placeholder.timestamp = QDateTime::currentDateTime();
  appendMessage(placeholder);
  if (!guard || m_session.id != session || sequence != m_turnSequence || !ensureTurnContext()) return;
  // H1：完整会话 → 只读投影（冻结 prompt/tools/config 消费）→ 装配诊断。
  // 投影不可发送（当前轮协议损坏/超历史预算、工具声明或请求体超字节闸）
  // 时零 HTTP：failTurn 走既有停轮路径（撤空占位、落盘、单次错误上报），
  // 已发生的工具结果与提交事实原样保留，保存欠账语义不变。
  paleo::chat::ContextProjectionLimits limits;
  limits.maxHistoryTurns = kMaxHistoryTurns;
  limits.historyTokenBudget = kHistoryTokenBudget;
  const auto projection = paleo::chat::projectChatRequest(
    m_session, m_turnPrompt, m_turnTools, m_turnConfig, limits);
  if (!projection.diagnostics.sendable) {
    const auto &diagnostics = projection.diagnostics;
    failTurn(tr("本轮上下文超出请求预算（%1%2），已停止本轮，未发送模型请求")
               .arg(diagnostics.rejectionCode,
                    diagnostics.rejectionDetail.isEmpty()
                      ? QString()
                      : QStringLiteral("：%1").arg(diagnostics.rejectionDetail)));
    return;
  }
  m_client.setConfig(m_turnConfig);
  m_client.send(projection.messages, m_turnTools);
}

void AiChatController::sendUserText(const QString &text) {
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty())
    return;
  if (!enabled()) {
    // 禁用态：如实报错，不冒充回答。
    emit errorOccurred(m_config.validate());
    return;
  }
  if (m_streaming || m_runner.busy() || m_piBridge.busy()) {
    emit errorOccurred(tr("上一轮仍在进行（生成或工具执行中），请先停止"));
    return;
  }
  const QPointer<AiChatController> guard(this); const auto sequence = ++m_turnSequence; const auto sessionId = m_session.id;
  const auto context = m_runner.hostContextIdentity();
  QString arrearsError;
  if (!settleSaveArrears(&arrearsError)) {
    emit errorOccurred(tr("会话尚未保存成功（%1），已保留原回答；请恢复存储后重试").arg(arrearsError));
    return;
  }
  if (!guard || sequence != m_turnSequence || sessionId != m_session.id) return;
  // H2-W3 纯档案容量准入：在追加 User 消息与任何新副作用之前，按「当前
  // 序列化成本 + 实际用户输入 + 一轮最坏预留（含轮中 rename 标题余量）」
  // 评估。拒绝时零 append/零 HTTP/零工具派发、不制造新欠账（既有欠账已在
  // 上方按 G2 语义先补存）；newSession/导出等既有出口不受影响。
  QString capacityReason;
  if (!paleo::chat::chatArchiveAcceptsTurn(m_session, trimmed,
                                           paleo::chat::chatArchiveBudget(),
                                           &capacityReason)) {
    emit errorOccurred(tr("会话容量不足（%1）：继续本轮会超出本机会话档案上限。"
                          "请用「新会话」开始新话题；当前会话仍可导出。")
                         .arg(capacityReason));
    return;
  }
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = trimmed;
  user.timestamp = QDateTime::currentDateTime();
  appendMessage(user);
  if (!guard || sequence != m_turnSequence || sessionId != m_session.id) return;

  startEngineTurn(context);
}

void AiChatController::setConfig(const LlmConfig &config) {
  m_config = config;
  // G2-W1：记录的 model 字段不由 setConfig 直接改写——落盘时按
  // currentSessionModel() 取「本轮开场模型」（冻结），新配置自下一轮生效；
  // 续轮请求同样用冻结配置（见 sendRound）。
  emit statusChanged(statusText());
}
