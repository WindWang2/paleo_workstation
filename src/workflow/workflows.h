// 层：功能
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
class DataCatalog;
struct ValidationIssue;

// workflow/ — thin orchestrators binding UI actions to services/algorithms.
// They never touch Qgs* directly beyond type names; heavy work runs via
// QgisProcessingService (temp-then-merge) and layer declaration via
// QgisLayerService.

// T26（wave3/derived-publish）：派生产物登记通道。三个写出产物的 workflow
// （预测/约束IDW/融合+相多边形）共享这一个绑定语义：catalog 必须是工程唯一
// 写实例，产物落 artifacts/derived/{asset}/{version}/ 并登记 DERIVED 版本。
// 未绑定 catalog 时这些写出路径拒绝执行（不再退回 QDir::temp()）。
void PaleoWorkflowBindDerivedCatalog(QObject *workflow, DataCatalog *catalog,
                                     const QString &projectDir);
DataCatalog *PaleoWorkflowDerivedCatalog(const QObject *workflow);
QString PaleoWorkflowDerivedProjectDir(const QObject *workflow);

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

    // T26：预测产物的登记通道（见 PaleoWorkflowBindDerivedCatalog）。
    void setCatalog(DataCatalog *catalog, const QString &projectDir);

    bool runPrediction(const QString &horizon, const QString &algorithmId, const QVariantMap &params, QString *error = nullptr);

  signals:
    void predictionDone(const QString &horizon, const QString &resultLayerId);
    void predictionFailed(const QString &horizon, const QString &error);

  private:
    // m2/mapping-pages(A)：结果层 id 的稳定段（algorithmId 净化）——同一
    // horizon+algorithmId 重跑复用同一 layerId，manifest upsert 不新增重复行。
    static QString stableResultSuffix(const QString &algorithmId);
    // m2(A) 3b 置信度调查（2026-09-27）：PaleoOnnxService 只取模型首个输出
    // tensor（无第二输出/方差通道），paleo:* 五算法均为确定性单输出栅格——
    // 当前没有任何算法产出真实置信度，恒 false；此时不声明伴生层
    // confidence.<horizon>，不用常量假栅格冒充。真实置信度通道接入后在此
    // 按算法能力判定，runPrediction 会据此声明（group "02_Prediction"）。
    static bool confidenceCompanionAvailable(const QString &algorithmId);
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

    // T26：IDW 单因素栅格的登记通道（见 PaleoWorkflowBindDerivedCatalog）。
    void setCatalog(DataCatalog *catalog, const QString &projectDir);

    bool addConstraint(const QString &horizon, const QString &wkt, const QString &type, int faciesCode,
                       QString *error = nullptr, QString *constraintIdOut = nullptr);
    QVector<QVariantMap> loadConstraints(const QString &horizon = QString());
    bool runConstraintIDW(const QString &horizon, const QString &pointsLayerId, const QString &field,
                          double cellSize, QString *error = nullptr);

    // ---- m2(B) 单因素图页（§10 词表驱动）----
    // 注册表项生成链：解析井点图层（原 runIdwRequested 壳里的 wells 查找逻辑
    // 挪到 workflow 侧，params 可携 pointsLayerId 覆盖）→ def.processingAlgId
    // 插值（约束线随行，同 runConstraintIDW）→ 色带样式落盘 styles/ →
    // declare "factor.<horizon>.<factorId>" 进 04_SingleFactor（同 id 幂等
    // upsert）。params：field/cellSize（缺省取注册表 defaultParams）+
    // pointsLayerId（可选）。
    bool generateFactor(const QString &horizon, const QString &factorId,
                        const QVariantMap &params, QString *error = nullptr);

    // 等值线（§12：GIS LineString 图层）。gdal:contour 在 C++ 嵌入运行时未注册
    // （Python provider），按 §32/§33 降级决议走 GDAL C API（FactorContourService，
    // gdal:contour 同一底层）→ 真 QgsVectorLayer 进 "04_SingleFactor/Contours"
    // 子组，layerId "contours.<horizon>.<factorId>"，幂等。
    bool generateContours(const QString &horizon, const QString &factorLayerId,
                          double interval, QString *error = nullptr);
  signals:
    void constraintAdded(const QString &constraintId);
    void factorDone(const QString &horizon, const QString &resultLayerId);

    // ---- m2(B)：单因素页消费的新信号（factorDone 原语义不动）----
    void factorGenerated(const QString &horizon, const QString &factorId, const QString &layerId);
    void contoursGenerated(const QString &horizon, const QString &factorLayerId,
                           const QString &contourLayerId);
};

// ③综合编图 — fuse declared single-factor rasters into composite facies layer.
class CompositionWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit CompositionWorkflow(QgisProcessingService *proc, QgisLayerService *layers, QObject *parent = nullptr);

    // T26：融合栅格与相多边形的登记通道（见 PaleoWorkflowBindDerivedCatalog）。
    void setCatalog(DataCatalog *catalog, const QString &projectDir);

    bool fuseFactors(const QString &horizon, const QStringList &factorLayerIds, QString *error = nullptr);

    // §36 — classified/fused raster → editable facies polygons.
    // Declares vector layer "facies.<horizon>" in group 05_PaleoMap.
    // params: MIN_AREA, SIMPLIFY, SNAP_TOLERANCE, ANGLE_TOLERANCE, CONSTRAINT_LAYER (layer id).
    // The raster declaration's horizon wins when it is set.
    bool deriveFaciesPolygons(const QString &horizon, const QString &rasterLayerId,
                              const QVariantMap &params = QVariantMap(), QString *error = nullptr);

    // ---- m2(C)：相属性回写（编辑态页「相属性」区的落库侧）----
    // 页面拿不到选中要素 id——这里按图层当前选中集解析（无选中 → 拒绝）。
    // attrs 携 facies_code(int)/facies_type/comment；写入走图层 edit buffer
    // （beginEditCommand/changeAttributeValue/endEditCommand，QGIS 4.2 命令
    // 组在 QgsMapLayer 上）；facies_type/comment 字段缺则先补建。变更留在
    // 编辑会话里（随编辑条「保存」提交 / 取消回滚）——不在这里绕过单写者
    // 纪律直接 commit。
    bool saveFaciesAttributes(const QString &layerId, const QVariantMap &attrs,
                              QString *error = nullptr);

    // m2(C)：把派生只读的相界 gpkg 备成可编辑工作副本并重指声明。
    // deriveFaciesPolygons 的产物按 T26 纪律 chmod 只读（catalog DERIVED
    // 版本不可变），而相界编辑需要可写面：源文件只读时复制到
    // <projectDir>/artifacts/layers/facies/<layerId>.gpkg（可写），manifest
    // 同 id 重指——只读原件留在 artifacts/derived 作 catalog provenance，
    // sha 不动。源本就可写或非文件源 → 原样返回 layerId；失败 → 空串 +
    // *error。重跑 derive 后再进编辑会用新派生文件重新铺工作副本。
    QString prepareFaciesForEditing(const QString &layerId, QString *error = nullptr);
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
