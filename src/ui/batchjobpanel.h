// 层：视图
#pragma once
#include <QHash>
#include <QWidget>

class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
namespace PaleoBatchQueue
{
class BatchQueue;
}

// ui/ — BatchJobPanel: 批次进度面板（方向 33）。
//
// 纪律：视图**只观察信号，不干活**。本面板不发计算指令、不碰 catalog、不调
// 任何工作流；它读 BatchQueue 的 itemChanged / batchProgress / batchFinished，
// 把逐作业状态（状态/进度/耗时/失败原因）摊成一张表，批后出汇总行。
// 轮询随可见性启停（showEvent/hideEvent）——批次状态本身不发的低频变化
// （如暂停）靠它兜底，与 TaskPanel 同一口径。
class BatchJobPanel : public QWidget
{
  Q_OBJECT
  public:
    // queue 可为 nullptr（面板仍构造，refresh 退化为空表提示）。
    explicit BatchJobPanel(PaleoBatchQueue::BatchQueue *queue,
                           QWidget *parent = nullptr);
    // 全量重建行（队列换代 / 首次挂载）。信号到达时只做增量。
    void refresh();

  private:
    QTreeWidgetItem *rowFor(const QString &itemId);
    void updateRow(QTreeWidgetItem *row, const QString &itemId);
    void updateSummary();
    void onItemChanged(const QString &itemId);
    void onProgress(int done, int total);
    void onFinished();

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    PaleoBatchQueue::BatchQueue *m_queue = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_summary = nullptr;
    QHash<QString, QTreeWidgetItem *> m_rows; // itemId -> 行
};
