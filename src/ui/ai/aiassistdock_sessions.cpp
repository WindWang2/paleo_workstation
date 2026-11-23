// 层：视图
#include "aiassistdock.h"

#include "../../workflow/aichatcontroller.h"

#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStandardItemModel>

void AiAssistDock::refreshSessions() {
  // H2（W7）：下拉改吃轻量元信息——refresh 只发查询意图，不再触发全库
  // 会话正文载入。结果（热路径同步完成；冷修复后补发）经
  // recentSessionSummariesReady 回到 rebuildSessionCombo。
  if (!m_controller) {
    m_sessions->clear();
    m_sessionIds.clear();
    return;
  }
  m_controller->requestRecentSessionSummaries(20);
}

void AiAssistDock::rebuildSessionCombo(
    const QVector<paleo::chat::ChatSessionSummary> &summaries, bool complete) {
  if (!m_controller)
    return;
  QSignalBlocker block(m_sessions); // 重建不触发 activated/currentIndexChanged
  m_sessions->clear();
  m_sessionIds.clear();
  // 当前会话永远以 controller 当前身份固定首位（元信息不含未落盘的当前态）。
  const ChatSession current = m_controller->session();
  m_sessions->addItem(current.displayTitle().isEmpty()
                        ? tr("（未命名会话）")
                        : current.displayTitle());
  m_sessionIds.append(current.id);
  for (const paleo::chat::ChatSessionSummary &recent : summaries) {
    if (recent.id == current.id)
      continue;
    QString label;
    if (recent.state == paleo::chat::ChatSessionSummary::State::Ok) {
      label = recent.displayTitle.isEmpty() ? tr("（未命名会话）")
                                            : recent.displayTitle;
    } else if (recent.state ==
               paleo::chat::ChatSessionSummary::State::OverLimit) {
      // 有界诊断（纯文本）：选中后由 loadSession 的权威上限拒绝，不伪称可载入。
      label = tr("超限会话：%1").arg(displayNameOf(recent));
    } else {
      label = tr("档案异常（%2）：%1")
                .arg(displayNameOf(recent),
                     recent.diagnostic.isEmpty() ? QStringLiteral("invalid")
                                                 : recent.diagnostic);
    }
    m_sessions->addItem(label);
    m_sessionIds.append(recent.id);
  }
  m_sessions->setCurrentIndex(0);
  // complete=false：有限扫描/修复未完成——如实标注，不把这一页说成完整最近
  // 集合。禁用行不可选、不对应任何会话；修复完成后下一次重建即消失。
  if (!complete) {
    const int row = m_sessions->count();
    m_sessions->addItem(tr("（列表可能不完整：部分会话尚未校验）"));
    m_sessionIds.append(QString());
    // DESIGN.md：禁用控件必须带 reason tooltip。
    if (auto *model = qobject_cast<QStandardItemModel *>(m_sessions->model())) {
      if (auto *item = model->item(row)) {
        item->setEnabled(false);
        item->setToolTip(
          tr("部分会话尚未校验（索引修复中或不可用），当前列表可能不完整"));
      }
    }
  }
  updateActionStates();
}

QString AiAssistDock::displayNameOf(const paleo::chat::ChatSessionSummary &recent) {
  // 诊断行用的辨认名：有标题用标题，否则用 id 前缀（元信息缺失/损坏时）。
  return recent.displayTitle.isEmpty() ? tr("%1…").arg(recent.id.left(8))
                                       : recent.displayTitle;
}

void AiAssistDock::renameComboSession() {
  const int index = m_sessions->currentIndex();
  if (index < 0 || index >= m_sessionIds.size())
    return;
  bool ok = false;
  const QString title = QInputDialog::getText(
    this, tr("重命名会话"), tr("会话标题："), QLineEdit::Normal,
    m_sessions->itemText(index), &ok);
  if (!ok || title.trimmed().isEmpty())
    return;
  emit renameSessionRequested(m_sessionIds.at(index), title.trimmed());
}
