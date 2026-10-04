// 层：视图
#pragma once
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "../../workflow/mapbookqueue.h"

class DataCatalog;
class PaleoMapBookPanel;
class PaleoTask;
class PaleoTaskService;
class QgsMapLayer;
class QgsProject;

// ui/layout — PaleoMapBookController：地图册面板 ↔ 批量导出队列的接线（#148）。
//
// 面板只发信号（batchRequested/cancelRequested），队列只编排导出；本类把两者
// 接起来：按面板参数拼 PaleoMapBookQueue::Request，每册新建一个 Exporter（一次
// 只跑一册），把进度/结果回显到面板。工程上下文全部经 provider 现取——工程
// 切换后不残留旧工程的 QgsProject/图层指针。
//
// 工程生命周期（#152 中枢）：宿主在 projectAboutToClose 调 resetProject()——
// 在途一册请求取消（Exporter 在下一版边界停下，不再碰旧图层），代际 +1，
// 迟到的 finished 回包按代际丢弃，面板回到待命并清空日志/进度。
class PaleoMapBookController : public QObject
{
    Q_OBJECT

  public:
    PaleoMapBookController( PaleoMapBookPanel *panel, PaleoTaskService *tasks,
                            QObject *parent = nullptr );

    void setProjectProvider( const std::function<QgsProject *()> &provider );
    //! 每版地图项的图层（缺省口径：画布当前图层集）。
    void setLayersProvider( const std::function<QList<QgsMapLayer *>()> &provider );
    //! 产物登记：catalog 非空且 projectDir 非空时逐版登记。
    void setCatalog( DataCatalog *catalog );
    void setProjectDirProvider( const std::function<QString()> &provider );
    void setCrsTextProvider( const std::function<QString()> &provider );
    void setHorizonProvider( const std::function<QString()> &provider );

    bool busy() const { return static_cast<bool>( m_exporter ); }
    PaleoTask *currentTask() const { return m_task.data(); }
    quint64 generation() const { return m_generation; }

    //! 工程即将关闭/切换：取消在途、丢弃迟到结果、面板回待命。
    void resetProject();

    //! 等同面板点「开始批量导出」（测试/编程入口）。返回是否已起跑。
    bool startBatch();
    void cancelBatch();

  signals:
    void statusMessage( const QString &message );
    //! 本代一册导出结束（被 resetProject 作废的不发）。
    void batchFinished( const PaleoMapBookQueue::Result &result );

  private:
    void onExporterFinished( quint64 generation, PaleoMapBookQueue::Exporter *exporter,
                             const PaleoMapBookQueue::Result &result );

    QPointer<PaleoMapBookPanel> m_panel;
    PaleoTaskService *m_tasks = nullptr;
    DataCatalog *m_catalog = nullptr; // AppContext 所有，生命周期长于主窗
    std::function<QgsProject *()> m_projectProvider;
    std::function<QList<QgsMapLayer *>()> m_layersProvider;
    std::function<QString()> m_projectDirProvider;
    std::function<QString()> m_crsProvider;
    std::function<QString()> m_horizonProvider;

    QPointer<PaleoMapBookQueue::Exporter> m_exporter;
    QPointer<PaleoTask> m_task;
    quint64 m_generation = 0;
};
