// 层：视图
#include "aicontextbar.h"
#include "../paleotheme.h"

#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

// ui/ai — G5 上下文条实现。色/字体一律 PaleoTheme（不写字面色值）；
// 状态词进文案（不只靠颜色）；内容只读（不可选中复制密钥类的面不存在）。

namespace {
QString firstLine(const QString &text) {
  const int cut = text.indexOf(QLatin1Char('\n'));
  return cut < 0 ? text : text.left(cut);
}
QString providedOrUnknown(const QString &value) {
  return value.isEmpty() ? QObject::tr("未提供") : value;
}
} // namespace

AiContextBar::AiContextBar(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *row = new QWidget(this);
  row->setObjectName(QStringLiteral("aiContextRow"));
  auto *rowLayout = new QHBoxLayout(row);
  rowLayout->setContentsMargins(tokens.spacingXs, 0, 0, 0);
  rowLayout->setSpacing(tokens.spacingXs);
  m_summary = new QLabel(row);
  m_summary->setObjectName(QStringLiteral("aiContextSummary"));
  m_summary->setTextFormat(Qt::PlainText);
  m_summary->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(
    m_summary, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  rowLayout->addWidget(m_summary, 1);
  m_toggle = new QToolButton(row);
  m_toggle->setObjectName(QStringLiteral("aiContextToggle"));
  m_toggle->setText(tr("上下文"));
  m_toggle->setCheckable(true);
  m_toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
  m_toggle->setArrowType(Qt::RightArrow);
  rowLayout->addWidget(m_toggle);
  layout->addWidget(row);

  m_details = new QWidget(this);
  m_details->setObjectName(QStringLiteral("aiContextDetails"));
  auto *detailsLayout = new QVBoxLayout(m_details);
  detailsLayout->setContentsMargins(tokens.spacingXs, 0, 0, 0);
  detailsLayout->setSpacing(0);
  m_detailBody = new QLabel(m_details);
  m_detailBody->setObjectName(QStringLiteral("aiContextDetailBody"));
  m_detailBody->setTextFormat(Qt::PlainText);
  m_detailBody->setWordWrap(true);
  m_detailBody->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(
    m_detailBody, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  detailsLayout->addWidget(m_detailBody);
  m_details->hide();
  layout->addWidget(m_details);

  connect(m_toggle, &QToolButton::toggled, this, [this](bool on) {
    m_toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    toggleExpanded(on);
  });
}

void AiContextBar::toggleExpanded(bool expanded) {
  m_details->setVisible(expanded);
  m_detailBody->setText(summaryLine().isEmpty() && m_brief.isEmpty()
                          ? tr("暂无上下文信息")
                          : detailText());
}

QString AiContextBar::summaryLine() const {
  QStringList parts;
  if (!m_brief.isEmpty())
    parts << firstLine(m_brief);
  if (!m_hostKnown)
    parts << tr("操作服务未接入");
  else if (!m_hostOpen)
    parts << tr("工程未打开");
  else if (m_hostReadOnly)
    parts << tr("只读");
  if (m_streaming)
    parts << tr("本轮进行中");
  if (parts.isEmpty())
    return tr("无工程上下文");
  return parts.join(QStringLiteral(" · "));
}

void AiContextBar::presentUnavailable(const QString &projectBrief, bool streaming,
                                      const QString &hostTurnId, const QString &reason) {
  m_hostKnown = false;
  m_brief = projectBrief;
  m_streaming = streaming;
  m_hostTurnId = hostTurnId;
  m_hostUnavailableCode = reason;
  m_summary->setText(summaryLine());
  m_detailBody->setText(detailText());
}

void AiContextBar::present(const QString &projectBrief, bool streaming,
                           const QString &hostUnavailableCode, bool hostOpen,
                           bool hostReadOnly, const QString &horizon,
                           const QString &unitId, const QString &documentId,
                           quint64 epoch, const QString &hostTurnId,
                           const QString &turnContextNote) {
  m_hostKnown = true;
  m_brief = projectBrief;
  m_streaming = streaming;
  m_hostUnavailableCode = hostUnavailableCode;
  m_hostOpen = hostOpen;
  m_hostReadOnly = hostReadOnly;
  m_horizon = horizon;
  m_unitId = unitId;
  m_documentId = documentId;
  m_epoch = epoch;
  m_hostTurnId = hostTurnId;
  m_turnContextNote = turnContextNote;
  m_summary->setText(summaryLine());
  m_detailBody->setText(detailText()); // 详情文本始终随快照更新（未展开也保鲜）
}

QString AiContextBar::detailText() const {
  QStringList lines;
  if (!m_brief.isEmpty())
    lines << tr("工程简报：%1").arg(m_brief);
  if (!m_hostKnown) {
    lines << tr("宿主状态：未知（%1）").arg(m_hostUnavailableCode);
  } else if (!m_hostOpen) {
    lines << tr("工程状态：未打开（%1）")
                 .arg(m_hostUnavailableCode.isEmpty() ? tr("无宿主上下文")
                                                      : m_hostUnavailableCode);
  } else {
    lines << tr("工程状态：已打开 · %1").arg(m_hostReadOnly ? tr("只读") : tr("可写"));
    lines << tr("层位：%1 · 地层单元：%2")
                 .arg(providedOrUnknown(m_horizon), providedOrUnknown(m_unitId));
    lines << tr("文档标识：%1（epoch %2）").arg(providedOrUnknown(m_documentId)).arg(m_epoch);
  }
  // 回合归属：实时回合只读冻结身份；host identity 不含层位等内容时不冒充。
  if (m_streaming) {
    lines << tr("正在进行的回合：%1")
                 .arg(m_hostTurnId.isEmpty() ? tr("归属未记录") : m_hostTurnId);
    if (!m_turnContextNote.isEmpty())
      lines << tr("回合冻结身份：%1").arg(m_turnContextNote);
    lines << tr("历史回合的层位/选区以各自冻结参数为准，不沿用当前上下文");
  } else {
    lines << tr("回合归属：空闲（历史卡各自标注归属）");
  }
  lines << tr("控制与导航动作受理时会重新读取当前宿主上下文");
  return lines.join(QLatin1Char('\n'));
}
