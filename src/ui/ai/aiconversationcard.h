// 层：视图
#pragma once
#include "../../workflow/aiconversationpresentation.h"
#include <QDateTime>
#include <QString>
#include <QToolButton>
#include <QWidget>

class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

// ui/ai — G5 会话工具卡：实时（分发→执行中→结果）与历史（envelope 留存）
// 共用同一 renderer。只呈现、只发意图；操作控制按钮把意图交给 dock→
// AiOperationPresentation adapter，卡本身不触任何服务。
//
// W8 有界纪律：body 惰性构建（历史卡默认收起，首次展开才建结构化视图），
// 展开数量由 dock 统一限额；主题切换经 changeEvent 重建 token 依赖样式。
class AiConversationCard : public QWidget {
  Q_OBJECT
public:
  // W2 归属标注（只读事实；未知项如实标注，不猜测补对）。
  struct Attribution {
    QString turnId;          // 回合 hostTurnId（空 = 旧记录未记）
    bool turnKnown = false;
    bool bindingKnown = false;
    QString bindingDocumentId;
    bool ambiguousCallId = false;
    bool pairedCall = true;
    bool hasResult = true;   // false = 只有调用没有结果帧
    QDateTime timestamp;
  };

  // 实时卡起点：分发结论文案（与既有两态卡同语义）。
  AiConversationCard(const QString &toolName, const QString &toolCallId,
                     const QString &statusText, QWidget *parent = nullptr);

  void markRunning();
  // controller toolResultReady 的 160 字符摘要（兜底态可见——DESIGN.md
  // 「兜底 = 摘要行 + 折叠块补全文」既有口径；结构化视图态隐藏）。
  void setResultSummary(bool ok, const QString &summary);
  // lazyBody=true：历史卡——首次展开才构建结果视图；false：实时结果即展开。
  void setResult(const paleo::chat::ToolResultFacts &facts, bool lazyBody);
  void setAttribution(const Attribution &attribution);
  void setArgumentsSummary(const QString &argumentsJson);
  // adapter 可用性：false = 无操作服务，控制按钮不出现并给原因（W8.4）。
  void setOperationControlsAllowed(bool allowed);
  // adapter 忙碌反馈：对应控制按钮置忙（action ∈ refresh/cancel/resume）。
  void setActionBusy(const QString &action, bool busy);

  QString toolCallId() const { return m_toolCallId; }
  QString operationId() const { return m_operationId; }
  bool expanded() const { return m_expandToggle && m_expandToggle->isChecked(); }
  void setExpanded(bool expanded);

signals:
  void expandedChanged(AiConversationCard *card);
  // 操作意图（adapter 裁决；卡只转发用户显式动作）。
  void operationDetailRequested(const QString &operationId);
  void operationRefreshRequested(const QString &operationId);
  void operationCancelRequested(const QString &operationId);
  void operationResumeRequested(const QString &operationId);
  // 导航意图透传（与既有 AiToolResultView 同语义）。
  void assetNavigateRequested(const QString &assetId);
  void entityNavigateRequested(const QString &entityId);
  void lineageNavigateRequested(const QString &assetId, const QString &versionId);

protected:
  // 主题切换（ApplicationPaletteChange）刷新既有卡：重建 token 依赖样式。
  void changeEvent(QEvent *event) override;

private:
  void buildBody();
  void refreshTheme();
  void swapCapsule(const QString &text, int kind);
  QString attributionText() const;

  QString m_toolName, m_toolCallId, m_operationId;
  QString m_argumentsJson;
  bool m_operationControlsAllowed = true;
  Attribution m_attribution;
  paleo::chat::ToolResultFacts m_facts;
  bool m_hasResult = false;
  bool m_bodyBuilt = false;
  QFrame *m_frame = nullptr;
  QHBoxLayout *m_headerRow = nullptr;
  QLabel *m_nameLabel = nullptr;
  QLabel *m_attributionLabel = nullptr;
  QLabel *m_timestampLabel = nullptr;
  QLabel *m_capsule = nullptr;
  QToolButton *m_expandToggle = nullptr;
  QWidget *m_body = nullptr;
  QVBoxLayout *m_bodyLayout = nullptr;
  QPushButton *m_refreshButton = nullptr;
  QPushButton *m_cancelButton = nullptr;
  QPushButton *m_resumeButton = nullptr;
  QLabel *m_summaryLabel = nullptr;      // 兜底摘要行（既有 160 字符形态）
  QString m_resultSummary;               // 实时帧摘要（历史卡无此帧则空）
};
