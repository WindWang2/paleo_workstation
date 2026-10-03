// 层：功能
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <memory>
#include <variant>
#include "../algorithms/singlefactor/types.h"
#include "../domain/types.h"
#include "../metadata/paleoprojectstore.h"
#include "../services/singlefactordef.h"

class QgisProcessingService;
class QgisLayerService;
class SelectionContext;
class PaleoOnnxService;
class ConstraintStore;
class ProjectDataFacade;
class DataCatalog;
class PaleoTask;
struct ValidationIssue;
namespace paleo::jobs {
template <class JobT>
class JobRunner;
} // namespace paleo::jobs

struct WorkflowExecutionContext
{
  QPointer<QgisProcessingService> proc;
  QPointer<QgisLayerService> layers;
  QPointer<PaleoProjectStore> store;
  QPointer<DataCatalog> catalog;
  QString projectDir;
#if PALEO_HAVE_ORT
  QPointer<PaleoOnnxService> onnx;
#endif
};

class ConstraintWorkflow;
class PredictionWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
using SingleFactorWorkflow = ConstraintWorkflow;
using TrendWorkflow = ConstraintWorkflow;
using SeismicWorkflow = PredictionWorkflow;

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

    DataCatalog *catalog() const;
    QString projectDir() const { return m_projectDir; }
    QgisProcessingService *processingService() const;
    QgisLayerService *layerService() const;
#if PALEO_HAVE_ORT
    PaleoOnnxService *onnxService() const;
#endif

  signals:
    void predictionDone(const QString &horizon, const QString &resultLayerId);
    void predictionFailed(const QString &horizon, const QString &error);

  private:
    QPointer<QgisProcessingService> m_proc;
    QPointer<QgisLayerService> m_layers;
#if PALEO_HAVE_ORT
    QPointer<PaleoOnnxService> m_onnx;
#endif
    QPointer<DataCatalog> m_catalog;
    QString m_projectDir;

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
    ~ConstraintWorkflow() override;
    void setConstraintStore(ConstraintStore *store);
    void setStore(PaleoProjectStore *store);
    ConstraintStore *constraintStore() const;

    // T26：IDW 单因素栅格的登记通道（见 PaleoWorkflowBindDerivedCatalog）。
    void setCatalog(DataCatalog *catalog, const QString &projectDir);

    bool addConstraint(const QString &horizon, const QString &wkt, const QString &type, int faciesCode,
                       QString *error = nullptr, QString *constraintIdOut = nullptr,
                       const QVariantMap &lineParams = {});
    // 把逐线语义和半径写进 params_json。重开后 loadConstraints 读回同一份。
    bool updateConstraintLine(const QString &id, const QVariantMap &lineParams, QString *error = nullptr);
    bool updateConstraintLines(const QStringList &ids, const QVariantMap &patch, QString *error = nullptr);
    // 语义切换（五种 Semantic 词表）：改写 type 列 + params_json.semantic，
    // 其余逐线参数原样保留。走 updateConstraintLine 同一持久化通道。
    bool switchConstraintSemantic(const QString &constraintId, const QString &semantic,
                                  QString *error = nullptr);
    // 删除约束行（store 落盘删除，不静默清几何）；页面与图层经
    // constraintRemoved 刷新。已实例化的 constraints.<horizon> 图层由壳侧重载。
    bool removeConstraint(const QString &constraintId, QString *error = nullptr);
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

  private:
    // 主线6：strathick 的等厚引擎分派（paleo:paleo_isopach，INPUT_TOP/
    // INPUT_BASE 双构造面栅格）。params：topLayerId/baseLayerId（声明图层
    // id，必填）+ negativeToNodata（默认 true，倒置层序折 nodata）。
    bool generateIsopachFactor(const QString &horizon, const QString &factorId,
                               const SingleFactorDefinition &def, const QVariantMap &params,
                               QString *error = nullptr);

    // C5（wave/deepen-perf）：welldist 的距离变换引擎分派
    //（paleo:paleo_distance_transform，SingleFactorContracts 冻结契约）。无
    // FIELD（距离不需属性值）；约束图层随行（仅 break_line 参与绕障，算法侧
    // 按 type 分拣）；引擎未注册 → 显式拒绝（不静默降级 IDW）。
    bool generateDistanceFactor(const QString &horizon, const QString &factorId,
                                const SingleFactorDefinition &def, const QVariantMap &params,
                                QString *error = nullptr);

    // method=local_direction_idw。缺省 method 仍走旧 IDW，不进这里。
    bool generateLocalDirectionFactor(const QString &horizon, const QString &factorId,
                                      const SingleFactorDefinition &def, const QVariantMap &params,
                                      QString *error);

    // method=kriging / sgs（方向18 地质统计学方法包）。样本不足阈值时计算段
    // 自动降级 IDW 并在产物 extra 记 method_actual="idw"（诚实标注条款）。
    bool generateGeostatFactor(const QString &horizon, const QString &factorId,
                               const QString &method, const SingleFactorDefinition &def,
                               const QVariantMap &params, QString *error = nullptr);

    // method=surfer_idw：Surfer 式全局 IDW，断层绕行测地距离（faultpath 内核）。
    bool generateSurferIdwFactor(const QString &horizon, const QString &factorId,
                                 const SingleFactorDefinition &def, const QVariantMap &params,
                                 QString *error);

    // WS-C：method=structural_idw（上游 Drawing structural_idw 移植，
    // paleo:paleo_structural_idw）。boundaryLayerId 必填，cellSize 不需要；
    // 发布同 local_direction_idw 旁路约定，另复制 .structural.json 并把参与
    // 井点落成 samples.<层位>.<因素> 点图层。
    bool generateStructuralFactor(const QString &horizon, const QString &factorId,
                                  const SingleFactorDefinition &def, const QVariantMap &params,
                                  QString *error);

    // 三个单因素引擎共用的收尾：样式 best-effort 落盘 + factor 栅格声明 +
    // C4 资产关联补盖 + factorGenerated（声明失败不发成功信号）。
    bool declareFactorResult(QgisLayerService *layers, const QString &horizon,
                             const QString &factorId,
                             const SingleFactorDefinition &def, const QString &outPath,
                             const QString &projectDir, const QString &assetId,
                             QString *error);

  public:

    // 等值线（§12：GIS LineString 图层）。gdal:contour 在 C++ 嵌入运行时未注册
    // （Python provider），按 §32/§33 降级决议走 GDAL C API（FactorContourService，
    // gdal:contour 同一底层）→ 真 QgsVectorLayer 进 "04_SingleFactor/Contours"
    // 子组，layerId "contours.<horizon>.<factorId>"，幂等。
    bool generateContours(const QString &horizon, const QString &factorLayerId,
                          double interval, QString *error = nullptr);
    // 显式级别的分析场等值线。layerId 仍是 contours.<层位>.<因素>。
    bool generateContoursAtLevels(const QString &horizon, const QString &factorLayerId,
                                  const QVector<double> &levels, QString *error = nullptr);

    // 读已提交的分析栅格，另写制图工作场并声明 cartographic.<层位>.<因素>。
    // 不覆盖 factor.<层位>.<因素>，也不改分析场字节。
    // refuseUnresolved 为真且穿线数大于 0 时不提交、不声明。
    bool generateCartographicWork(const QString &horizon, const QString &factorLayerId,
                                  const QVector<double> &levels, QString *error = nullptr,
                                  bool refuseUnresolved = false, int *unresolvedOut = nullptr);

    // 从制图工作场提线。图层 cartographic.<层位>.<因素>.contours，名称「解释性等值线」。
    // 严格模式有未解决穿线时不发布工作场，也不发布线。
    bool generateInterpretiveContours(const QString &horizon, const QString &factorLayerId,
                                      const QVector<double> &levels, QString *error = nullptr,
                                      bool strict = true);

    // 本地方向：准备在界面线程，计算可在任务线程，发布回到 catalog 所属线程。
    // engineId 区分共享三段式的单因素引擎（paleo_local_direction_idw /
    // paleo_surfer_idw）。
    struct LocalDirectionJob
    {
        bool prepared = false;
        bool ok = false;
        quint64 generation = 0;
        QString error;
        QString horizon;
        QString factorId;
        QString field;
        double cellSize = 1.0;
        QString engineId = QStringLiteral( "paleo:paleo_local_direction_idw" );
        QString wellUri;
        QString constraintUri;
        bool hasConstraints = false;
        QStringList parentPaths;
        QVariantMap params;
        QString outputPath;
        QString supportPath;
        QString qcPath;
    };
    bool prepareLocalDirectionJob(const QString &horizon, const QString &factorId,
                                  const QVariantMap &params, LocalDirectionJob *job, QString *error = nullptr,
                                  const QString &engineId = QStringLiteral( "paleo:paleo_local_direction_idw" ));
    bool computeLocalDirectionJob(LocalDirectionJob *job, const std::function<bool()> &cancelled = {},
                                  const std::function<void(double)> &progress = {});
    bool publishLocalDirectionJob(const LocalDirectionJob &job, QString *error = nullptr);

    // 方向18 克里金/SGS：准备在界面线程（只解析 URI 与参数），计算可在任务
    // 线程（纯数值核 + GDAL 写临时目录），发布回到 catalog 所属线程。
    // method = "kriging" | "sgs"。params 词表：
    //   field/cellSize（同其他方法）
    //   variogramModel: spherical|exponential|gaussian（缺省 spherical）
    //   nugget/sill/range: 数值，range>0 且 sill>0 视为显式模型；否则自动拟合
    //   azimuth: 走向方位（度，从北顺时针）；<0 = 自动（全向拟合，各向同性）
    //   maxPoints: 克里金/SGS 邻域上限（缺省 16）
    //   realizations/seed: SGS 实现数（缺省 4）与种子（缺省 42）
    struct GeostatJob
    {
        bool prepared = false;
        bool ok = false;
        bool degraded = false; // 样本不足 → 计算段降级 IDW（method_actual="idw"）
        quint64 generation = 0;
        QString error;
        QString method; // "kriging" | "sgs"
        QString horizon;
        QString factorId;
        QString field;
        double cellSize = 1.0;
        QString wellUri;
        QStringList parentPaths;
        QVariantMap params;
        QString outputPath;
        QString supportPath; // 克里金：估计方差场；SGS：实现间标准差场；降级：空
        QString qcPath;
    };
    bool prepareGeostatJob(const QString &horizon, const QString &factorId, const QString &method,
                           const QVariantMap &params, GeostatJob *job, QString *error = nullptr);
    bool computeGeostatJob(GeostatJob *job, const std::function<bool()> &cancelled = {},
                           const std::function<void(double)> &progress = {});
    bool publishGeostatJob(const GeostatJob &job, QString *error = nullptr);

    // 分析场等值线：准备在调用线程，GDAL 可在任务线程，发布回到 catalog 所属线程。
    // fixedLevels 为真时用 levels（空级别直接拒绝）；否则要求正间距。
    // structural（algorithm_id=paleo:paleo_structural_idw）走上游 field_contours
    // 提取：interval<=0 = 自动级别（对话框自适应规则），不降级到 GDAL 等值线。
    struct AnalysisContourJob
    {
        bool prepared = false;
        bool ok = false;
        bool fixedLevels = false;
        bool structural = false;
        quint64 generation = 0;
        QString error;
        QString horizon;
        QString factorLayerId;
        QString factorId;
        QString rasterPath;
        QString structuralPath;
        QString analysisSha;
        QStringList parentPaths;
        double interval = 0.0;
        QVector<double> levels;
        QVector<double> resolvedLevels;
        QString outputPath;
    };
    bool prepareAnalysisContourJob(const QString &horizon, const QString &factorLayerId,
                                   double interval, const QVector<double> &levels, bool fixedLevels,
                                   AnalysisContourJob *job, QString *error = nullptr);
    bool computeAnalysisContourJob(AnalysisContourJob *job, const std::function<bool()> &cancelled = {});
    bool publishAnalysisContourJob(const AnalysisContourJob &job, QString *error = nullptr);

    // 解释性等值线：工作场与提线可在任务线程写临时文件，登记仍回调用线程。
    // 约束线在准备阶段解析。计算线程只读 constraintLines，不再打开图层。
    // strict 默认与 generateInterpretiveContours 相同，不改拒绝穿线的缺省。
    struct InterpretiveContourJob
    {
        bool prepared = false;
        bool ok = false;
        bool strict = true;
        bool hasConstraints = false;
        bool frozenConstraints = false;
        quint64 generation = 0;
        int unresolved = 0;
        QString error;
        QString horizon;
        QString factorLayerId;
        QString factorId;
        QString rasterPath;
        QString constraintUri;
        QString analysisSha;
        QStringList parentPaths;
        QVector<double> levels;
        std::vector<paleo::singlefactor::ConstraintLine> constraintLines;
        std::vector<std::string> ignoredConstraints;
        QString workPath;
        QString qcPath;
        QString contourPath;
    };
    bool prepareInterpretiveContourJob(const QString &horizon, const QString &factorLayerId,
                                       const QVector<double> &levels, bool strict,
                                       InterpretiveContourJob *job, QString *error = nullptr);
    bool computeInterpretiveContourJob(InterpretiveContourJob *job,
                                       const std::function<bool()> &cancelled = {});
    bool publishInterpretiveContourJob(const InterpretiveContourJob &job, QString *error = nullptr);

    // ---- 方向20：三组作业的统一异步面 ---------------------------------
    // 上面 9 个 prepare/compute/publish 是同一协议的三次重造（各自实现忙则
    // 互斥、取消接线、进度回包、临时产物清理、失败诚实）。它们一行未改——
    // 下面的统一面是**新增的等价入口**，把这三组接到 JobRunner 上。
    //
    // 三种 job 的字段并不相同（各自有各自的中间产物路径），故用 variant 容纳：
    // variant 索引即 Kind，compute 段据此分派，commit 段同理。
    using ConstraintJobVariant = std::variant<LocalDirectionJob, AnalysisContourJob,
                                              InterpretiveContourJob>;
    enum class ConstraintJobKind { LocalDirection, AnalysisContour, InterpretiveContour };

    struct ConstraintJob
    {
        ConstraintJobKind kind = ConstraintJobKind::LocalDirection;
        ConstraintJobVariant payload;
        // 任务标题（现状各调用点的标题文案原样搬过来，避免改 i18n 面）。
        QString title;
        // 进度阶段词表：现状各调用点自带的 prepare/geometry/encode 映射在此
        // 声明式表达，由框架统一节流后经 reportStage 上任务面板。
        std::function<QString(double)> stageOf;
    };

    // 各自 job 的三段入口（owner 线程 / worker / owner 线程）。非静态：它们要
    // 访问 catalog、layers、projectStore。
    bool prepareConstraintJob(ConstraintJob &job, const QVariantMap &params,
                              QString *error = nullptr);
    bool computeConstraintJob(ConstraintJob &job, const std::function<bool()> &cancelled,
                              const std::function<void(double)> &progress = {});
    bool publishConstraintJob(const ConstraintJob &job, QString *error = nullptr);
    // 未成功发布时的临时产物路径（框架 cleanup 钩子用）。
    static QStringList constraintJobTempPaths(const ConstraintJob &job);

    // owner 线程（catalog 所属线程）调用。runner 与本对象同线程。
    // 返回 nullptr 表示「忙则拒绝」或任务服务缺席——调用方据此走各自老路径。
    PaleoTask *startConstraintJob(paleo::jobs::JobRunner<ConstraintJob> &runner,
                                  ConstraintJob job, QObject *progressSink = nullptr);

    DataCatalog *catalog() const;
    QString projectDir() const { return m_projectDir; }
    QgisProcessingService *processingService() const;
    QgisLayerService *layerService() const;
    PaleoProjectStore *projectStore() const;

  signals:
    void constraintAdded(const QString &constraintId);
    void factorDone(const QString &horizon, const QString &resultLayerId);

    // ---- m2(B)：单因素页消费的新信号（factorDone 原语义不动）----
    void factorGenerated(const QString &horizon, const QString &factorId, const QString &layerId);
    void contoursGenerated(const QString &horizon, const QString &factorLayerId,
                           const QString &contourLayerId);
    void cartographicWorkGenerated(const QString &horizon, const QString &factorLayerId,
                                   const QString &layerId);
    void interpretiveContoursGenerated(const QString &horizon, const QString &factorLayerId,
                                       const QString &layerId);
    void constraintLineUpdated(const QString &constraintId);
    void constraintRemoved(const QString &constraintId);

  private:
    QPointer<QgisProcessingService> m_proc;
    QPointer<QgisLayerService> m_layers;
    QPointer<PaleoProjectStore> m_projectStore;
    mutable std::unique_ptr<ConstraintStore> m_ownedConstraintStore;
    ConstraintStore *m_externalConstraintStore = nullptr;
    QPointer<DataCatalog> m_catalog;
    QString m_projectDir;
    QVector<QVariantMap> m_inMemoryConstraints;
    int m_inMemorySeq = 0;
    // 发布代次。本地方向在准备入口加一；等值线与解释性等值线在校验通过后加一；
    // setStore 也加一。因子图层改指向新文件时，在 declare 之前加一。
    // 提交前对不上就丢掉这次临时文件，不声明图层。
    quint64 m_publishGeneration = 0;
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

    DataCatalog *catalog() const;
    QString projectDir() const { return m_projectDir; }
    QgisProcessingService *processingService() const;
    QgisLayerService *layerService() const;

  signals:
    void compositionDone(const QString &horizon, const QString &resultLayerId);
    void faciesPolygonsReady(const QString &horizon, const QString &layerId);
    void faciesPolygonsFailed(const QString &horizon, const QString &error);

  private:
    QPointer<QgisProcessingService> m_proc;
    QPointer<QgisLayerService> m_layers;
    QPointer<DataCatalog> m_catalog;
    QString m_projectDir;
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

    QgisLayerService *layerService() const;
    PaleoProjectStore *projectStore() const;
    ProjectDataFacade *projectData() const;
    double residualThresholdMs() const { return m_residualThresholdMs; }

  signals:
    void validationDone(int issueCount);

  private:
    QPointer<QgisLayerService> m_layers;
    QPointer<PaleoProjectStore> m_store;
    QPointer<ProjectDataFacade> m_projectData;
    double m_residualThresholdMs = 10.0;
    QVariantList m_residualRows;
};
