#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include "../domain/types.h"

class QgisProcessingService;
class QgisLayerService;
class SelectionContext;
class PaleoProjectStore;
class PaleoOnnxService;
class ConstraintStore;
class ProjectDataFacade;
struct ValidationIssue;

// workflow/ — thin orchestrators binding UI actions to services/algorithms.
// They never touch Qgs* directly beyond type names; heavy work runs via
// QgisProcessingService (temp-then-merge) and layer declaration via
// QgisLayerService.

// ①智能预测 — run a prediction algorithm for a horizon, declare result layer.
class PredictionWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit PredictionWorkflow(QgisProcessingService *proc, QgisLayerService *layers, QObject *parent = nullptr);

    // Binds the ONNX inference service. Each model under its root is exposed
    // as an "onnx:<model>" algorithm id routed through PaleoOnnxService
    // instead of the Processing registry (no-op hook when built without ORT).
    void setOnnxService(PaleoOnnxService *onnx);
    // Runnable algorithm ids: "paleo:*" from the Processing registry plus
    // "onnx:<model>" per PaleoOnnxService::availableModels().
    QStringList availableAlgorithms() const;

    bool runPrediction(const QString &horizon, const QString &algorithmId, const QVariantMap &params, QString *error = nullptr);
  signals:
    void predictionDone(const QString &horizon, const QString &resultLayerId);
    void predictionFailed(const QString &horizon, const QString &error);
};

// ②约束与单因素 — ingest drawn constraint geometries + run ConstraintIDW.
class ConstraintWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit ConstraintWorkflow(QgisProcessingService *proc, QgisLayerService *layers, QObject *parent = nullptr);
    void setConstraintStore(ConstraintStore *store);
    void setStore(PaleoProjectStore *store);
    ConstraintStore *constraintStore() const;

    bool addConstraint(const QString &horizon, const QString &wkt, const QString &type, int faciesCode,
                       QString *error = nullptr, QString *constraintIdOut = nullptr);
    QVector<QVariantMap> loadConstraints(const QString &horizon = QString());
    bool runConstraintIDW(const QString &horizon, const QString &pointsLayerId, const QString &field,
                          double cellSize, QString *error = nullptr);
  signals:
    void constraintAdded(const QString &constraintId);
    void factorDone(const QString &horizon, const QString &resultLayerId);
};

// ③综合编图 — fuse declared single-factor rasters into composite facies layer.
class CompositionWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit CompositionWorkflow(QgisProcessingService *proc, QgisLayerService *layers, QObject *parent = nullptr);
    bool fuseFactors(const QString &horizon, const QStringList &factorLayerIds, QString *error = nullptr);

    // §36 — classified/fused raster → editable facies polygons.
    // Declares vector layer "facies.<horizon>" in group 05_PaleoMap.
    // params: MIN_AREA, SIMPLIFY, SNAP_TOLERANCE, ANGLE_TOLERANCE, CONSTRAINT_LAYER (layer id).
    // The raster declaration's horizon wins when it is set.
    bool deriveFaciesPolygons(const QString &horizon, const QString &rasterLayerId,
                              const QVariantMap &params = QVariantMap(), QString *error = nullptr);
  signals:
    void compositionDone(const QString &horizon, const QString &resultLayerId);
    void faciesPolygonsReady(const QString &horizon, const QString &layerId);
    void faciesPolygonsFailed(const QString &horizon, const QString &error);
};

// ④验证 — run cross-horizon validation rules; instantiate layers on demand.
class ValidationWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit ValidationWorkflow(QgisLayerService *layers, PaleoProjectStore *store, QObject *parent = nullptr);
    // Checks: duplicate horizon names, declared-layer source missing on disk,
    // busy-layer conflicts. Returns issue list (may be empty = clean).
    QList<ValidationIssue> validate();

    // wave/mapping-pipeline — 绑定读侧门面后，validate() 增加 D61 井上时间残差
    // 检查（TD 插值 vs 层位 DERIVED 栅格；见 mappingworkflow.h）。残差只评 D61。
    void setProjectData(ProjectDataFacade *projectData);
    // 残差起评阈值（ms）。默认 10.0（autoplan §5C：|r|<=10 通过，>10 成问题）。
    void setResidualThresholdMs(double thresholdMs);
    // 最近一次 validate() 的 D61 逐井残差行（验证页残差表数据源）。
    // 元素为 QVariantMap：well_id/well_name/status/residual_ms/reason/
    // threshold_ms/x/y/inline/time_ms/raster_ms。未跑过或无门面 → 空表。
    QVariantList lastResidualRows() const;

  signals:
    void validationDone(int issueCount);
};
