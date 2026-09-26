#pragma once
#include <QObject>
#include <QString>
#include <QVariantMap>

#include "../metadata/mapversionstore.h"

class QgisLayerService;
class ProjectDataFacade;

// workflow/mapversioncontroller — 「保存版本 / 发布 / Published」的编排
// （wave/mapping-pipeline 阶段E；PALEO_QGIS_PLAN §41.7 + §1223 最小语义 +
// PROJECT_AREA_PLAN §177/§260 发布门）。
//
//   保存版本 = 该层位全部矢量图层的编辑会话 commit（QGIS 原生：commit 清
//   undo 栈；此处再显式 clear 兜底，undo 不跨版本边界）→ MapVersionStore
//   版本号递增 + provenance 记录；新版本行继承最近一次登记的 PDF 产物引用。
//   发布 = result/ 快照（成果 gpkg + 绑定 PDF，只读）→ Published；发布门
//   = 版本行上有 PDF 资产 id+SHA-256（导出登记 OUTPUT 资产后经 saveVersion
//   继承）+ 每口井都有残差或原因（residualSummaryJson 产出的完整快照，
//   由调用方在发布时传入并落表）。
//   Published 快照只读；继续编辑产生下一版本，不回写已发布快照。
class MapVersionController : public QObject
{
  Q_OBJECT
  public:
    MapVersionController( MapVersionStore *store, QgisLayerService *layers,
                          QObject *parent = nullptr );

    MapVersion saveVersion( const QString &horizon, const QVariantMap &provenance,
                            QString *error = nullptr );
    QString publish( const QString &horizon, const QString &residualSummary,
                     QString *error = nullptr );

    // 阶段E — 逐井残差摘要：对每口井按阶段C 的口径给出「残差或原因」——
    // 无分层 / 无时深表 / 超出时深表 / 时深表无序 / 井位不在测网内 /
    // 井位落在空道 之一，或数值残差。产出紧凑 JSON：
    //   {"horizon":h,"wells_total":N,"covered":N,"missing":[井名…],
    //    "rows":[{"well_id","name","kind":"residual"|"reason",
    //             "residual_ms":… | "reason":"…"}]}
    // missing 列出走到采样才发现评不了的井（层位无时间栅格）。pd 为空或
    // 无井 → wells_total=0（不完整）。完整性判定见
    // MapVersionStore::residualSummaryComplete —— 发布门与按钮 tooltip 共用。
    static QString residualSummaryJson( const ProjectDataFacade *projectData,
                                        const QString &horizon );

  signals:
    void versionSaved( const QString &horizon, int version );
    void published( const QString &horizon, int version, const QString &snapshotDir );

  private:
    MapVersionStore *m_store = nullptr;
    QgisLayerService *m_layers = nullptr;
};
