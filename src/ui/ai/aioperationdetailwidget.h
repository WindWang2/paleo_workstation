// 层：视图
#pragma once
#include <QByteArray>
#include <QString>
#include <QWidget>

class QJsonObject;
class QLabel;
class QPushButton;
class QTableWidget;

// ui/ai — G5 操作详情区：从选中 operation 卡显式打开的原生检视面。
//
// 内容（W6）：三轴状态 + 事实、事件分页表（afterRevision/nextRevision 十进制
// 游标，历史过期显式呈现缺口，只有用户显式重读才复位）、结果端口表、
// 端口内容有界检视（文本仅限已知文本 encoding，跨页 UTF-8 边界正确；其余
// 二进制按 hex 有界呈现）。控制按钮（刷新/取消/恢复）只发意图给 adapter。
//
// 纪律：不做第二套状态机——所有状态/错误码原样呈现（可解释、不压扁）；
// 切换 operation/port 后迟到回调按目标不匹配丢弃；本组件不触文件、不把
// resultRef 解析为路径、不自动遍历全部端口/分页。
class AiOperationDetailWidget : public QWidget {
  Q_OBJECT
public:
  explicit AiOperationDetailWidget(QWidget *parent = nullptr);

  // 显式打开（用户点卡上「操作详情」）：清空旧目标、请求刷新。
  void openOperation(const QString &operationId);
  void closeEvent(QCloseEvent *event) override;
  QString operationId() const { return m_operationId; }

public slots:
  // adapter 信号入口（dock 转接）；目标不匹配的迟到帧按代次丢弃。
  void onOperationState(const QString &operationId, const QJsonObject &state);
  void onOperationEvents(const QString &operationId, const QJsonObject &events);
  void onOperationResults(const QString &operationId, const QJsonObject &results);
  void onPortPage(const QString &operationId, const QString &portId,
                  const QJsonObject &page);
  void onActionFinished(const QString &operationId, const QString &action,
                        const QJsonObject &response);
  void onBusyChanged(const QString &operationId, const QString &action, bool busy);

signals:
  // 意图（adapter 裁决；detail 只转发用户显式动作）。
  void refreshRequested(const QString &operationId);
  void eventsRequested(const QString &operationId, quint64 afterRevision);
  void resultsRequested(const QString &operationId);
  void portPageRequested(const QString &operationId, const QString &portId,
                         qint64 offset, int maxBytes);
  void cancelRequested(const QString &operationId);
  void resumeRequested(const QString &operationId);
  void closed();

private:
  void rebuildStateRows(const QJsonObject &state);
  void showEventPage(const QJsonObject &events);
  void showPorts(const QJsonObject &results);
  void showPortPage(const QString &portId, const QJsonObject &page);
  void resetPortView();
  void setControlBusy(const QString &action, bool busy);

  QString m_operationId;
  QString m_eventsCursor;   // 十进制 revision（空 = 尚未起读）
  QString m_selectedPort;
  qint64 m_portOffset = 0;  // 下一页读取游标
  QByteArray m_portBytes;   // 已取回字节（有界）
  QString m_portEncoding;
  QLabel *m_title = nullptr;
  QLabel *m_axes = nullptr;
  QTableWidget *m_stateTable = nullptr;
  QTableWidget *m_eventsTable = nullptr;
  QPushButton *m_eventsNext = nullptr;
  QPushButton *m_eventsRestart = nullptr;
  QLabel *m_eventsNote = nullptr;
  QTableWidget *m_portsTable = nullptr;
  QPushButton *m_portRead = nullptr;
  QLabel *m_portPreview = nullptr;
  QPushButton *m_portNext = nullptr;
  QPushButton *m_refresh = nullptr;
  QPushButton *m_cancel = nullptr;
  QPushButton *m_resume = nullptr;
  QPushButton *m_close = nullptr;
};
