// 层：视图
#pragma once
#include "../../workflow/aiplanpresentation.h"
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

// ui/ai — H5 计划审阅面板：从计划提案工具卡「审阅此计划」显式打开的原生
// 检视/编辑面。
//
// 呈现（W3/W4）：步骤列表、选中节点参数（提案值/当前值/来源三列——用户
// 修改与算法补默认分开标注）、输入端口与边表、终端成果、首个可定位预检
// 问题；IDW 有限点集的 x/y/z 有界表格检查与输入；points_ref/constraints_ref
// 一律标注「提案值（来源未核验）」，不升格为宿主核验事实。
//
// 纪律：本面板只发意图（重新预检/计算私有成果/查看原操作/打开既有操作
// 详情），编排与宿主链在 workflow/aiplanpresentation；按钮 enabled 与 handler
// 共用 adapter 的同一当前状态判定；失败原因留在面板内，不 toast 后消失。
// 无通用 DAG 拖拽编辑；不读 Catalogue 私有目录、不直接调用执行器。
class AiPlanReviewWidget : public QWidget {
  Q_OBJECT
public:
  explicit AiPlanReviewWidget(QWidget *parent = nullptr);
  // 注入/重挂展示 adapter（幂等；null = 解绑，面板只读）。
  void attachPresenter(paleo::chat::AiPlanPresentation *presenter);

  // 从工具卡打开提案（payload = paleo.plan-preview.v1 结果 JSON）。adapter
  // 拒绝（契约不符/缺完整图）时仍显示可证实的摘要与拒绝理由，不拼凑执行。
  void openProposal(const QJsonObject &payload, const QString &toolCallId);

  QString draftId() const { return m_draftId; }
  bool reviewing() const { return !m_draftId.isEmpty(); }

signals:
  void closed();
  // 打开既有操作详情（dock 的 AiOperationDetailWidget；复用三轴/取消/恢复）。
  void operationDetailRequested(const QString &operationId);

protected:
  void changeEvent(QEvent *event) override;

private:
  void rebuild();                       // 从 adapter DraftView 整体重建（含修订对照）
  void rebuildNodeDetail();             // 选中节点的参数/输入/端口
  void refreshStatusLine(const QString &extra = QString());
  void refreshActionStates();           // 按钮 enabled = adapter 同一判定
  void applyEdits();                    // 有界编辑 → adapter::updateDraftGraph
  QJsonObject buildEditedGraph() const; // 从控件状态组装规范化图 JSON
  void showRefusal(const QString &code, const QString &detail); // 只读拒绝态
  QString paramSourceText(const QString &nodeId, const QString &key) const;
  void handlePresenterSignals();
  void onDraftChanged(const QString &draftId);
  void onPreflightFinished(const QString &draftId, const QJsonObject &payload);
  void onComputeFinished(const QString &draftId, const QJsonObject &facts);
  void onOriginalResolved(const QString &draftId, const QJsonObject &record);
  void onBusyChanged(const QString &draftId, const QString &action, bool busy);

  QPointer<paleo::chat::AiPlanPresentation> m_presenter;
  QString m_draftId;
  bool m_rebuilding = false;   // 重建期间屏蔽 itemChanged 回灌
  QLabel *m_title = nullptr;
  QLabel *m_status = nullptr;  // 预检/执行状态行（失败原因常驻）
  QListWidget *m_steps = nullptr;
  QLabel *m_nodeFacts = nullptr;
  QTableWidget *m_params = nullptr;   // 参数：提案值/当前值/来源
  QTableWidget *m_points = nullptr;   // IDW 点集 x/y/z/well_id（有界编辑）
  QLineEdit *m_subinterval = nullptr; // IDW 必填 subinterval_id（有界编辑）
  QLabel *m_inputNotes = nullptr;     // ref/CRS 诚实性注记（提案值·未核验）
  QTableWidget *m_edges = nullptr;
  QLabel *m_terminal = nullptr;
  QListWidget *m_issues = nullptr;
  QLabel *m_operationNote = nullptr;  // 原操作事实/受理注记
  QPushButton *m_addPoint = nullptr;
  QPushButton *m_removePoint = nullptr;
  QPushButton *m_preflight = nullptr;
  QPushButton *m_compute = nullptr;
  QPushButton *m_original = nullptr;
  QPushButton *m_operationDetail = nullptr;
  QPushButton *m_close = nullptr;
};
