// 层：视图
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include "../../services/derivationgraph.h"

class DerivationPanel;

#include "dataops/dataopscommands.h"
#include "dataops/dataopsmodel.h"
#include "../../services/previewdoc.h" // F3：GeoJsonSummary（异步统计挂起）

class PreviewDocService;
namespace paleo::dataops
{
class DataOpsUndoStack;
class VersionTimeline;
class TopologyGraph;
}

// ui/pages/entitypanel — 数据管理页的「实体数据视图」侧（W5：自 DataPage
// 分家；P3 D4：内联编辑/版本时间线/拓扑图/实体 CRUD/多选态）。
// 本面板自身即 entityViewSection（objectName 保留——壳把它重新
// 挂到右侧「数据属性」dock）。
//
// p5a：当前选中实体的角色槽数据视图——entityDataView()（B 包纯查询门面）
// 按角色词表枚举 (实体,角色) 槽、下游 DERIVED 产物与悬空血缘诊断。
// 上下文由壳/列表侧经 setContext 下发（D6 地图点选 → entityId；表选中 →
// assetId 反推实体）。catalog.changed() 由主窗口接线调 refresh() 重取。
class EntityPanel : public QWidget
{
  Q_OBJECT

  public:
    explicit EntityPanel(QWidget *parent = nullptr);

    void setDocService(PreviewDocService *doc);
    void setContext(const QString &entityId, const QString &assetId);
    void setVersionContext(const QString &assetId, const QString &versionId);
    // D4.9 多选态：批量概要（类型/状态/标签分布 + 共同实体）。
    void setMultiContext(const QStringList &entityIds, const QStringList &assetIds);
    // P3 共享命令面（DataPage 接线：栈与 stores 与列表侧同源）。
    void setSharedOps(const paleo::dataops::DataOpsContext &ctx,
                      paleo::dataops::DataOpsUndoStack *stack,
                      paleo::dataops::OperationsHistory *history);
    // D4.1：树内 F2 / 菜单「重命名实体」意图落点（壳转发到本方法）。
    void beginRenameEntity(const QString &entityId);
    void beginDeleteEntity(const QString &entityId);
    void beginCreateEntity();

  public slots:
    void refresh(); // 重取并重渲染角色槽/派生/属性段

  signals:
    void statusMessage(const QString &msg);          // D5.4 反馈（壳接状态栏）
    void versionActivated(const QString &versionId); // 只发版本预览意图
    void entityRefreshRequested();                   // CRUD 后请列表/实体视图重取
    // 井 well_log 非主文件「设为主文件」。面板不写 catalog，由创建方接 setLinkPrimary。
    void wellLogSetPrimaryRequested(const QString &entityId, const QString &assetId);

  private:
    void refreshDerivation();
    void buildD4Ui();          // CRUD 条/版本时间线/拓扑/统计段装配
    void refreshMultiSummary(); // D4.9 批量概要
    // F3（goal/perf-systematize 簇2）：GeoJSON 统计单次请求——缓存命中直装；
    // 无任务服务同步执行（测试：selectAsset 后立即断言，不经事件循环）。
    // 返回 true = apply 已同步执行完（调用方跳过占位覆盖）；false = 异步
    // 在途（到达时世代号防陈旧，装缓存后回调）。
    bool requestGeoJsonSummary(
        const QString &absPath,
        const std::function<void(const PreviewDocService::GeoJsonSummary &)> &apply);
    PreviewDocService *m_doc = nullptr;
    QString m_entityId;
    QString m_assetId;
    QString m_versionId, m_graphSelected;
    bool m_preserveGraphOnce = false;
    DerivationPanel *m_derivation = nullptr;
    paleo::derivation::Graph m_derivationData;
    QStringList m_multiEntityIds;  // D4.9
    QStringList m_multiAssetIds;   // D4.9
    int m_geoSeq = 0;              // F3：统计世代号（新选择作废在途）
    QHash<QString, PreviewDocService::GeoJsonSummary> m_geoSummaryCache; // 路径→统计
    paleo::dataops::DataOpsContext m_ctx;
    paleo::dataops::DataOpsUndoStack *m_stack = nullptr;
    paleo::dataops::OperationsHistory *m_history = nullptr;
    paleo::dataops::VersionTimeline *m_timeline = nullptr;
    paleo::dataops::TopologyGraph *m_topology = nullptr;
};
