#pragma once
#include <QObject>
#include <QString>

// app/ — AppContext wires the service graph together (composition root).
// Construct once in main(); owns all services; PaleoMainWindow receives them.
class QgisRuntime;
class QgisCanvasController;
class QgisProjectService;
class QgisLayerService;
class QgisProcessingService;
class QgisEditingService;
class QgisStyleService;
class ToolAvailabilityService;
class PaleoOnnxService;
class SelectionContext;
class SeismicMapLink;
class WellMapLink;
class PaleoProjectStore;
class LayerManifest;
class DataImportService;
class QgisLayoutService;
class PredictionWorkflow;
class ConstraintWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
class ProjectDataFacade;
class MappingWorkflow;
class MapVersionStore;
class MapVersionController;

class AppContext : public QObject
{
  Q_OBJECT
  public:
    explicit AppContext(const QString &qgisPrefix, QObject *parent = nullptr);
    ~AppContext() override;
    bool ready() const { return m_ready; }

    QgisProjectService *projectSvc() const { return m_projectSvc; }
    QgisLayerService *layerSvc() const { return m_layerSvc; }
    QgisCanvasController *canvasCtl() const { return m_canvasCtl; }
    QgisProcessingService *processingSvc() const { return m_procSvc; }
    QgisEditingService *editingSvc() const { return m_editSvc; }
    QgisStyleService *styleSvc() const { return m_styleSvc; }
    ToolAvailabilityService *toolSvc() const { return m_toolSvc; }
    // ONNX 推理服务（PALEO_HAVE_ORT 构建下恒实例化并绑到 predictionWf；
    // 运行库/模型缺失由服务自身如实报告，!ORT 构建恒为 nullptr）。
    PaleoOnnxService *onnxSvc() const { return m_onnxSvc; }
    SelectionContext *selection() const { return m_selection; }
    SeismicMapLink *seismicLink() const { return m_seismicLink; }
    // 井—地图联动（§31，预览壳重排接入）：画布拾取 ⇄ SelectionContext；
    // wells 图层在项目打开后由 catalog 井点生成。
    WellMapLink *wellLink() const { return m_wellLink; }
    PaleoProjectStore *store() const { return m_store; }
    LayerManifest *manifest() const { return m_manifest; }
    DataImportService *importSvc() const { return m_import; }
    QgisLayoutService *layoutSvc() const { return m_layoutSvc; }
    PredictionWorkflow *predictionWf() const { return m_predictionWf; }
    ConstraintWorkflow *constraintWf() const { return m_constraintWf; }
    CompositionWorkflow *compositionWf() const { return m_compositionWf; }
    ValidationWorkflow *validationWf() const { return m_validationWf; }

    // wave/mapping-pipeline 阶段C+E：读侧门面 / D61 编图链 / 版本状态机。
    ProjectDataFacade *projectData() const { return m_projectData; }
    MappingWorkflow *mappingWf() const { return m_mappingWf; }
    MapVersionStore *versionStore() const { return m_versionStore; }
    MapVersionController *versionCtl() const { return m_versionCtl; }

  private:
    // §4 井位图层：catalog 井点 → artifacts/layers/wells.geojson → manifest
    // 声明「wells」→ 实例化 → 绑给 m_wellLink。工程打开与 catalog 每次
    // 变更时调用；没有可定位井时不声明空图层。
    void refreshWellsLayer();

    bool m_ready = false;
    QgisProjectService *m_projectSvc = nullptr;
    QgisLayerService *m_layerSvc = nullptr;
    QgisCanvasController *m_canvasCtl = nullptr;
    QgisProcessingService *m_procSvc = nullptr;
    QgisEditingService *m_editSvc = nullptr;
    QgisStyleService *m_styleSvc = nullptr;
    ToolAvailabilityService *m_toolSvc = nullptr;
    PaleoOnnxService *m_onnxSvc = nullptr;
    SelectionContext *m_selection = nullptr;
    SeismicMapLink *m_seismicLink = nullptr;
    WellMapLink *m_wellLink = nullptr;
    QString m_projectDir; // wells.geojson 输出根（projectOpened 时设置）
    PaleoProjectStore *m_store = nullptr;
    LayerManifest *m_manifest = nullptr;
    DataImportService *m_import = nullptr;
    QgisLayoutService *m_layoutSvc = nullptr;
    PredictionWorkflow *m_predictionWf = nullptr;
    ConstraintWorkflow *m_constraintWf = nullptr;
    CompositionWorkflow *m_compositionWf = nullptr;
    ValidationWorkflow *m_validationWf = nullptr;
    ProjectDataFacade *m_projectData = nullptr;
    MappingWorkflow *m_mappingWf = nullptr;
    MapVersionStore *m_versionStore = nullptr;
    MapVersionController *m_versionCtl = nullptr;
};
