#pragma once
#include <QHash>
#include <QWidget>

class PaleoProjectStore;
class PaleoTaskService;
class QTreeWidgetItem;

// ui/ — TaskPanel: bottom-dock 任务页（autoplan pass-2 D2）。两块内容：
// 1) store busy 注册表的忙图层镜像（§35 门工具读的那份，含编辑等无进度任务）；
// 2) PaleoTaskService 的异步任务行——1s 粒度进度条 + 10s 字节线性 ETA +
//    协作式取消。面板照旧轮询，无信号的 store 不需要改。
class TaskPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit TaskPanel(PaleoProjectStore *store,
                       PaleoTaskService *tasks = nullptr,
                       QWidget *parent = nullptr);
    void refresh(); // reconcile rows from store->busyLayers() + tasks->tasks()

  private:
    QTreeWidgetItem *rowForTask(qint64 id);
    void rebuildBusyRows();
    void updateTaskRow(QTreeWidgetItem *row, class PaleoTask *task);

    PaleoProjectStore *m_store;
    PaleoTaskService *m_tasks;
    QHash<qint64, QTreeWidgetItem *> m_taskRows;  // taskId -> row
    QHash<QString, QTreeWidgetItem *> m_busyRows; // layerId -> row
};
