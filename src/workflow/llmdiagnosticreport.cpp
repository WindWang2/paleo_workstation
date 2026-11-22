// 层：功能
#include "llmdiagnosticreport.h"
#include "../ai/chat/llmcredentialguard.h"

#include <QCoreApplication>

namespace paleo::chat {
namespace {
// 有界渲染：单行 detail 截断 + 整体行数上限（报告只在诊断对象内留存）。
constexpr int kMaxDetailChars = 240;
constexpr int kMaxSummaryLines = 40;

QString bounded(QString text) {
  text = redactCredential(std::move(text), QByteArray()).trimmed();
  if (text.size() > kMaxDetailChars)
    text = text.left(kMaxDetailChars) + QStringLiteral("…");
  return text;
}

QString usageLine(const LlmProbeObservation &observation) {
  if (!observation.usageReported)
    return QObject::tr("usage：服务端未报告（缺省不显示为精确零）");
  return QObject::tr("usage：服务端报告 prompt=%1 completion=%2")
    .arg(observation.promptTokens)
    .arg(observation.completionTokens);
}

void appendProbe(QStringList *lines, const QString &title,
                 const LlmProbeObservation &observation) {
  QString line = QStringLiteral("%1：%2")
                   .arg(title, llmDiagnosticStatusLabel(observation.status));
  if (observation.status == LlmDiagnosticStatus::Passed ||
      observation.status == LlmDiagnosticStatus::Indeterminate) {
    if (!observation.finishReason.isEmpty())
      line += QObject::tr("（finish=%1").arg(observation.finishReason);
    else
      line += QStringLiteral("（");
    line += QObject::tr("；增量 %1 次；%2）")
              .arg(observation.deltaCount)
              .arg(usageLine(observation));
  }
  lines->append(line);
  if (!observation.detail.isEmpty())
    lines->append(QStringLiteral("　└ %1").arg(bounded(observation.detail)));
}
} // namespace

QString llmDiagnosticStatusLabel(LlmDiagnosticStatus status) {
  switch (status) {
  case LlmDiagnosticStatus::Untested:
    return QObject::tr("未测试");
  case LlmDiagnosticStatus::Running:
    return QObject::tr("进行中");
  case LlmDiagnosticStatus::Passed:
    return QObject::tr("通过");
  case LlmDiagnosticStatus::Unsupported:
    return QObject::tr("不支持（目录未实现）");
  case LlmDiagnosticStatus::UnknownShape:
    return QObject::tr("未知形状");
  case LlmDiagnosticStatus::Empty:
    return QObject::tr("空目录");
  case LlmDiagnosticStatus::Unauthorized:
    return QObject::tr("无权限（401/403）");
  case LlmDiagnosticStatus::RateLimited:
    return QObject::tr("限流（429）");
  case LlmDiagnosticStatus::Rejected:
    return QObject::tr("被拒绝");
  case LlmDiagnosticStatus::ServerError:
    return QObject::tr("服务端错误");
  case LlmDiagnosticStatus::Network:
    return QObject::tr("网络错误");
  case LlmDiagnosticStatus::Timeout:
    return QObject::tr("超时");
  case LlmDiagnosticStatus::TooLarge:
    return QObject::tr("超预算");
  case LlmDiagnosticStatus::Malformed:
    return QObject::tr("格式/协议失败");
  case LlmDiagnosticStatus::Cancelled:
    return QObject::tr("已取消");
  case LlmDiagnosticStatus::Indeterminate:
    return QObject::tr("无法确定");
  case LlmDiagnosticStatus::LocalRejected:
    return QObject::tr("本地拒绝");
  }
  return QObject::tr("未知状态");
}

QString llmKeySourceLabel(LlmDiagnosticCandidate::KeySource source) {
  switch (source) {
  case LlmDiagnosticCandidate::KeySource::ExplicitInput:
    return QObject::tr("本次显式输入（仅用于本次诊断）");
  case LlmDiagnosticCandidate::KeySource::InMemoryCurrent:
    return QObject::tr("沿用当前内存值（端点身份一致）");
  case LlmDiagnosticCandidate::KeySource::None:
    return QObject::tr("无");
  }
  return QObject::tr("未知来源");
}

QString suggestNextStep(const LlmDiagnosticReport &report) {
  // 只基于已有观测给一步建议；不自动修改配置、不替换密钥、不打开服务器
  // 给出的任意 URL。目录缺席 ≠ 文本不可用（诚实口径贯穿建议面）。
  const bool textOk = report.text.status == LlmDiagnosticStatus::Passed;
  const bool streamOk = report.stream.status == LlmDiagnosticStatus::Passed;
  const bool toolOk = report.toolRoundTrip.status == LlmDiagnosticStatus::Passed;
  const bool toolIndeterminate =
    report.toolRoundTrip.status == LlmDiagnosticStatus::Indeterminate;
  if (report.catalog.status == LlmDiagnosticStatus::Unsupported && textOk)
    return QObject::tr("目录未实现但不影响使用——手填模型 ID 后由原「应用」"
                       "流程提交生效");
  if (report.catalog.status == LlmDiagnosticStatus::Unauthorized)
    return QObject::tr("目录无权限：核对该密钥是否有列表权限，或改用手填"
                       "模型 ID 继续文本探测");
  if (report.catalog.status == LlmDiagnosticStatus::Passed &&
      !report.catalog.models.isEmpty() && report.model.trimmed().isEmpty())
    return QObject::tr("从目录选择模型 ID 填入草稿后，可用「测试文本」验证"
                       "补全（目录列出 ≠ 模型可用）");
  if (report.text.status == LlmDiagnosticStatus::Unauthorized ||
      report.stream.status == LlmDiagnosticStatus::Unauthorized)
    return QObject::tr("密钥被拒绝：核对密钥、其适用端点与模型权限");
  if (report.text.status == LlmDiagnosticStatus::RateLimited ||
      report.stream.status == LlmDiagnosticStatus::RateLimited)
    return QObject::tr("服务端限流：稍后重试或降低请求频率");
  if (report.text.status == LlmDiagnosticStatus::Timeout ||
      report.stream.status == LlmDiagnosticStatus::Timeout)
    return QObject::tr("请求超时：核对网络可达性与服务端响应时长");
  if (report.stream.status == LlmDiagnosticStatus::Passed && !textOk)
    return QObject::tr("流式请求已被客户端接受；文本请求未测——两者独立");
  if (toolOk)
    return QObject::tr("工具帧协议兼容（仅协议往返；不代表业务工具执行"
                       "验证）");
  if (toolIndeterminate)
    return QObject::tr("模型未调用诊断工具（auto 允许直接作答）——重试或"
                       "保持结论为无法确定");
  if (textOk && !streamOk && report.stream.status == LlmDiagnosticStatus::Untested)
    return QObject::tr("文本可用；可用「测试流式请求」继续核对流式模式");
  if (textOk)
    return QObject::tr("文本请求可用；其余分项按各自观测结论处理");
  return QObject::tr("按上方分项观测处理：核对端点/模型/凭据，或改用手填"
                     "模型 ID");
}

QString renderLlmDiagnosticSummary(const LlmDiagnosticReport &report) {
  QStringList lines;
  lines.reserve(16);
  if (report.generation == 0) {
    return QObject::tr("尚未运行诊断。所有动作均为显式启动：打开设置、编辑"
                       "草稿、关闭窗口都不会产生任何网络请求。");
  }
  lines.append(
    QObject::tr("诊断 #%1 · %2")
      .arg(report.generation)
      .arg(report.startedAt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))));
  lines.append(
    QObject::tr("测试值：端点=%1 模型=%2 流式=%3 探测输出上限=%4/%5 "
                "凭据来源=%6")
      .arg(report.endpoint.toString(), report.model,
           report.draftStream ? QStringLiteral("开") : QStringLiteral("关"))
      .arg(report.probeMaxTokens)
      .arg(report.candidateMaxTokens)
      .arg(llmKeySourceLabel(report.keySource)));
  lines.append(QStringLiteral("――――――――――――――――――――"));

  // 目录分项
  QString catalog =
    QStringLiteral("模型目录：%1")
      .arg(llmDiagnosticStatusLabel(report.catalog.status));
  if (!report.catalog.models.isEmpty())
    catalog += QObject::tr("（%1 项模型）").arg(report.catalog.models.size());
  if (report.catalog.truncated)
    catalog += QObject::tr("·部分（已截断）");
  if (report.catalog.credentialFiltered)
    catalog += QObject::tr("·已剔除回显凭据条目");
  lines.append(catalog);
  if (!report.catalog.detail.isEmpty())
    lines.append(QStringLiteral("　└ %1").arg(bounded(report.catalog.detail)));

  appendProbe(&lines, QObject::tr("文本请求"), report.text);
  appendProbe(&lines, QObject::tr("流式请求"), report.stream);

  // 工具往返分项（观测事实逐一列出，不合并）
  const LlmProbeObservation &tool = report.toolRoundTrip;
  QString toolLine = QStringLiteral("工具往返：%1")
                       .arg(llmDiagnosticStatusLabel(tool.status));
  lines.append(toolLine);
  lines.append(
    QStringLiteral("　└ ")
      + QObject::tr("工具帧：%1｜内存结果回送：%2｜后续文本：%3｜模型请求 %4 次")
          .arg(tool.toolFrameObserved ? QObject::tr("观察到")
                                      : QObject::tr("未观察到"),
               tool.toolResultSentBack ? QObject::tr("已送回")
                                       : QObject::tr("未送回"),
               tool.followUpTextCompleted ? QObject::tr("完成")
                                          : QObject::tr("未完成"),
               QString::number(tool.httpRequests)));
  if (tool.secondToolCallSeen)
    lines.append(QStringLiteral("　└ ")
                 + QObject::tr("后续请求再次要求工具（已忽略，零第三轮）"));
  if (!tool.detail.isEmpty())
    lines.append(QStringLiteral("　└ %1").arg(bounded(tool.detail)));

  lines.append(QStringLiteral("――――――――――――――――――――"));
  lines.append(
    QObject::tr("真实 SSE：未独立确认（公开信号不能区分真实 SSE 与普通 JSON "
                "兼容应答）"));
  lines.append(QObject::tr("建议：%1").arg(bounded(suggestNextStep(report))));
  lines.append(
    QObject::tr("（历史报告仅说明当时快照；不构成当前配置已启用或当前服务"
                "健康的证明）"));
  if (lines.size() > kMaxSummaryLines)
    lines = lines.mid(0, kMaxSummaryLines)
            << QStringLiteral("…（报告行数超限，已截断）");
  return lines.join(QLatin1Char('\n'));
}

} // namespace paleo::chat
