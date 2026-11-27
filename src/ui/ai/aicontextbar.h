// 层：视图
#pragma once
#include <QWidget>

class QLabel;
class QToolButton;

// ui/ai — G5 上下文条：紧凑展示「当前宿主上下文」与「回合归属」的只读投影。
//
// 纪律：本组件只呈现装配层送来的快照值，不自行读目录/全表、不推导地质
// 选区、不展示密钥/端点/私有路径；显示值不构成权限——控制动作前由
// adapter 现读 live context。工程关闭/只读/未提供层位等事实如实标注。
class AiContextBar : public QWidget {
  Q_OBJECT
public:
  AiContextBar(QWidget *parent = nullptr);
  bool hasOpenProject() const { return m_hostKnown && m_hostOpen; }
  // 操作服务未接入（宿主上下文未知，不冒称未打开）：reason 给出原因。
  void presentUnavailable(const QString &projectBrief, bool streaming,
                          const QString &hostTurnId, const QString &reason);
  // 展示快照（W1）：宿主上下文 + 工程简报 + 流式状态。
  void present(const QString &projectBrief, bool streaming,
               const QString &hostUnavailableCode, bool hostOpen, bool hostReadOnly,
               const QString &horizon, const QString &unitId,
               const QString &documentId, quint64 epoch,
               const QString &hostTurnId, const QString &turnContextNote);

private:
  void toggleExpanded(bool expanded);
  QString summaryLine() const;
  QString detailText() const;

  QString m_brief, m_horizon, m_unitId, m_documentId, m_turnContextNote;
  QString m_hostUnavailableCode, m_hostTurnId;
  bool m_hostKnown = false; // false = 操作服务未接入（工程状态未知）
  bool m_streaming = false, m_hostOpen = false, m_hostReadOnly = true;
  quint64 m_epoch = 0;
  QLabel *m_summary = nullptr;
  QToolButton *m_toggle = nullptr;
  QWidget *m_details = nullptr;
  QLabel *m_detailBody = nullptr;
};
