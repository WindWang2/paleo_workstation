// 层：视图
#pragma once
#include <QHash>
#include <QWidget>

class PaleoProjectStore;
class PaleoTaskService;
class QHideEvent;
class QShowEvent;
class QTimer;
class QTreeWidgetItem;

// ui/ — TaskPanel: bottom-dock 任务页（autoplan pass-2 D2）。两块内容：
// 1) store busy 注册表的忙图层镜像（§35 门工具读的那份，含编辑等无进度任务）；
// 2) PaleoTaskService 的异步任务行——1s 粒度进度条 + 10s 字节线性 ETA +
//    协作式取消。面板轮询随可见性启停（showEvent/hideEvent），无信号的
//    store 不需要改。
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

  protected:
    // 500ms 轮询随可见性启停（底栏默认隐藏，不可见不轮询）。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    PaleoProjectStore *m_store;
    PaleoTaskService *m_tasks;
    QTimer *m_pollTimer = nullptr;
    bool m_refreshPending = false;
    // #164：子控件构造时缓存——refresh 每行一次 findChild 是 O(子对象数)，
    // 行数 × 子控件数（每行两个 itemWidget）即 O(N²)。
    class QTreeWidget *m_list = nullptr;
    class QLabel *m_empty = nullptr;
    class QLabel *m_count = nullptr;
    QHash<qint64, QTreeWidgetItem *> m_taskRows;  // taskId -> row
    QHash<QString, QTreeWidgetItem *> m_busyRows; // layerId -> row
};
