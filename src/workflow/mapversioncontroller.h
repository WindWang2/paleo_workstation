// 层：功能
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QVariantMap>
#include <QPointer>
#include <memory>

#include "../metadata/mapversionstore.h"

class QgisLayerService;
class QgisEditingService;
class PaleoProjectStore;
class ProjectDataFacade;
class DataCatalog;
class PaleoTaskService;
struct MapVersionSaveCapture;

// workflow/mapversioncontroller — 「保存版本 / 发布 / Published」的编排
// （wave/mapping-pipeline 阶段E；PALEO_QGIS_PLAN §41.7 + §1223 最小语义 +
// PROJECT_AREA_PLAN §177/§260 发布门）。
//
//   保存版本 = 先校验并冻结当前编辑视图（含未提交缓冲），再做文件准备与
//   耐久登记。只有版本行已经存在，才提交工作副本。QGIS commitChanges 会
//   清空原生撤销栈，因此提交不能提前到准备阶段。准备失败保留缓冲和撤销。
//   耐久版本形成后工作副本提交失败时，版本仍然有效，结果标明后续未完成。
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

    void setEditingService( QgisEditingService *editSvc );
    void setProjectStore( PaleoProjectStore *store );
    void setCatalog(DataCatalog *catalog);
    ~MapVersionController() override;
    void setTaskService(PaleoTaskService *tasks);
    bool startSaveVersion(const QString &horizon, const QVariantMap &provenance, QString *error = nullptr);
    bool startPublish(const QString &horizon, const QString &residualSummary, QString *error = nullptr);
    bool snapshotBusy() const;
    void cancelSnapshot();
    // 工程关闭：取消在途快照并作废其代次，避免关闭后仍提交到旧工程。
    void abandonForProjectClose();

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

    // B 包 staleness-lite — 发布门 advisory（DATA_FABRIC_ADOPTION）：只读评估，
    // 目标工程 catalog 内带 extra["stale"] 的 DERIVED 版本计数与提示文案。
    // 不阻断发布、不自动修复——advisory 只要求可见（发布面板/确认文案）。
    // catalog 为空（未开工程）→ 0 / 空文案，不假装有评估。
    static int staleDerivedCount( const DataCatalog *catalog );
    // 0 → 空串（确认文案不加行）；N>0 → 「存在过时下游产物（N 个）——
    // 不阻断本次发布」。
    static QString stalePublishAdvisory( const DataCatalog *catalog );

    struct SaveOutcome {
      enum class Kind { None, Rejected, Cancelled, Saved, SavedFollowUpPending };
      Kind kind = Kind::None;
      int version = 0;
      QString horizon;
      QString message;
      QString stage;
      QStringList committedLayers;
      QStringList pendingLayers;
    };
    struct PhaseMetrics {
      qint64 evidenceScanMs = 0;
      qint64 providerExportMs = 0;
      qint64 sqlRegisterMs = 0;
      qint64 uiMaxGapMs = 0;
      qint64 bytesCopied = 0;
      int providerExports = 0;
      int identityNormalizations = 0;
      int sourceHashes = 0;
      int destinationHashes = 0;
      int files = 0;
      int reusedImmutableAssets = 0;
      int events = 0;
    };
    SaveOutcome lastSaveOutcome() const { return m_lastOutcome; }
    PhaseMetrics lastSaveMetrics() const { return m_lastMetrics; }
    enum class SaveFault { None, Export, Verify, Sql, WorkCommit };
    void setSaveFaultForTest(SaveFault fault);

  signals:
    void snapshotBusyChanged(bool busy);
    // Query batches yield to the event loop before the native commit boundary.
    void snapshotQueryProgress(const QString &horizon, qint64 checked, qint64 total);
    void snapshotFinished(const QString &horizon, const QString &operation,
                          bool success, bool cancelled, const QString &error);
    void versionSaved( const QString &horizon, int version );
    void published( const QString &horizon, int version, const QString &snapshotDir );

  private:
    bool captureVersion(const QString &horizon, const QVariantMap &provenance,
                        QVector<LayerDeclaration> *declarations, QVariantMap *frozen, QString *error,
                        bool featurePrevalidated = false);
    bool commitCapturedEdits(QString *error);
    bool resumePendingWorkCommit(const QString &horizon, MapVersion *out, QString *error);
    bool tryReuseImmutable(const QString &layerId, LayerDeclaration *declaration);
    void rememberImmutableAssets(const MapVersion &version, const QVector<LayerDeclaration> &declared);
    void noteOutcome(SaveOutcome::Kind kind, int version, const QString &horizon, const QString &message,
                     const QString &stage);
    struct SavePreparation;
    void continueSavePreparation(const std::shared_ptr<SavePreparation> &preparation);
    bool launchSaveSnapshot(const QString &horizon, const QVariantMap &provenance, QString *error);
    void releaseSnapshotTokens(const QString &horizon);
    struct AsyncState;
    std::shared_ptr<AsyncState> m_async;
    std::shared_ptr<MapVersionSaveCapture> m_saveCapture;
    quint64 m_snapshotEpoch = 0;
    QPointer<PaleoTaskService> m_tasks;
    QPointer<DataCatalog> m_catalog;
    MapVersionStore *m_store = nullptr;
    QPointer<QgisLayerService> m_layers;
    QPointer<QgisEditingService> m_editSvc;
    QPointer<PaleoProjectStore> m_projectStore;
    SaveOutcome m_lastOutcome;
    PhaseMetrics m_lastMetrics;
    SaveFault m_saveFault = SaveFault::None;
    struct ImmutableAssetReuse {
      QString projectDirectory;
      QString sourceSha256;
      QString snapshotPath;
      QString snapshotSha256;
    };
    QHash<QString, ImmutableAssetReuse> m_reuseBySource;
};
