// 层：组装根
#pragma once
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <qgsrectangle.h>

// app/ — AppContext wires the service graph together (composition root).
// Construct once in main(); owns all services; PaleoMainWindow receives them.
#include "metadata/faultsetstore.h"
#include "metadata/wellsitingstore.h"
class ProjectDirLock;
class QgisRuntime;
class QgisCanvasController;
class QgisProjectService;
class QgisLayerService;
class QgisLayerOrganizer;
class QgisProcessingService;
class QgisEditingService;
class QgisStyleService;
class ToolAvailabilityService;
class ErrorHub;
class AiChatController;
class PaleoOnnxService;
class RemotePredictionRouter;
class AiAssistWorkflow;
class SelectionContext;
class DepthConversionWorkflow;
class PropertyModelWorkflow;
class SeismicMapLink;
class WellMapLink;
class PaleoProjectStore;
class LayerManifest;
class DataImportService;
class QgisLayoutService;
class PredictionWorkflow;
class ConstraintWorkflow;
class FaciesMappingWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
namespace paleo::fault {
class FaultInterpretationController;
}
class ProjectDataFacade;
class MappingWorkflow;
class MappingWorkbench;
class MapVersionStore;
class MapVersionController;
class PaleoTaskService;

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
    // 方向64：统一错误通道（构造即 installGlobal，析构自动摘除）。
    ErrorHub *errorHub() const { return m_errorHub; }
    // ONNX 推理服务（PALEO_HAVE_ORT 构建下恒实例化并绑到 predictionWf；
    // 运行库/模型缺失由服务自身如实报告，!ORT 构建恒为 nullptr）。
    PaleoOnnxService *onnxSvc() const { return m_onnxSvc; }
    // 方向51：远端预测路由（装配根注入 MappingWorkbench 的那个实例）。
    // 恒非空——端点未配置时它不带传输，收到请求如实失败；UI 显示
    // predictionStatusHint()（「远端预测未配置，走本地引擎」）。
    RemotePredictionRouter *remotePredictRouter() const {
      return m_remotePredict;
    }
    QString remotePredictStatusHint() const { return m_remotePredictHint; }
    // 方向51：AI 地质对话助手编排（会话/流式/工具分发）；未配置密钥时呈禁用态。
    AiChatController *aiChat() const { return m_aiChat; }
    // 方向68：Python 脚本面编排（脚本运行/协议/历史/REPL 会话转发）。
    class PythonConsoleController *pythonConsole() const { return m_pythonConsole; }
    // AI 辅助编排（tile 分类产品 + 追踪建议裁决）；无 ORT 构建下为 null。
    AiAssistWorkflow *aiAssistWorkflow() const { return m_aiAssistWf; }
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
    // 方向34：井网辅助（覆盖诊断/候选/方案/planned 计划井）。
    class WellSitingWorkflow *wellsitingWf() const { return m_wellsitingWf; }

    // wave/mapping-pipeline 阶段C+E：读侧门面 / D61 编图链 / 版本状态机。
    ProjectDataFacade *projectData() const { return m_projectData; }
    MappingWorkbench *mappingWorkbench() const { return m_mappingWorkbench; }
    MappingWorkflow *mappingWf() const { return m_mappingWf; }
    DepthConversionWorkflow *depthConversionWf() const { return m_depthWf; }
    PropertyModelWorkflow *propertyModelWf() const { return m_propModelWf; }
    // goal/facies-automapping：沉积相自动编图辅助链（优势相→相界→合成→QA→草稿）。
    FaciesMappingWorkflow *faciesMappingWf() const { return m_faciesMappingWf; }
    MapVersionStore *versionStore() const { return m_versionStore; }
    MapVersionController *versionCtl() const { return m_versionCtl; }

    // goal/fault-interpretation：断层解释编排器（FaultSet 模型 + 撤销栈 +
    // SelectionContext 联动 + 切割镜像层）。存储随工程打开重绑（值成员，
    // 控制器持有稳定地址）。
    paleo::fault::FaultInterpretationController *faultCtl() const { return m_faultCtl; }

    // pass-2 D1/D2：异步任务注册中心（QThreadPool 执行 + 字节进度 + 10s 窗口
    // ETA + 协作取消），任务页轮询它渲染进度条；长 IO 经它上 worker。
    PaleoTaskService *taskSvc() const { return m_taskSvc; }
    ProjectDirLock *projectLock() const { return m_projectLock.get(); }

    // 单写实例降级面（T4）：projectOpened 时 tryLock 失败 → 本实例只读。
    // catalog/manifest/版本库/工程存储的写路径已全部如实拒绝（各自
    // setReadOnly/setLockedReadOnly 接线在本组装根）；UI 态反映经信号
    // projectReadOnlyChanged（壳侧可绑标题「（只读）」/禁用保存动作）。
    bool isProjectReadOnly() const;
    // 关闭当前工程（经 QgisProjectService::closeProject：aboutToClose → 清
    // 工程 → projectClosed → 释放锁/句柄）。
    void closeProject();

    // #152 锁冲突决策钩子（测试/嵌入方用）：返回 true = 只读打开，false =
    // 取消打开（当前工程保持不变）。未设置时 GUI 弹框询问、无头自动只读。
    void setLockConflictResolver(std::function<bool(const QString &lockError)> resolver)
    {
      m_lockConflictResolver = std::move(resolver);
    }

    // #163：排空任务池（取消在途任务并等待 worker 退出，等待期间泵事件）。
    // main() 在 QApplication::exec() 返回后、主窗口析构前调用——worker 闭包
    // 可能引用主窗口持有的对象。幂等；析构函数也会兜底调用。
    bool drainTasks(int timeoutMs = 10000);

  signals:
    void projectReadOnlyChanged(bool readOnly);

  private:
    // §4 井位图层：catalog 井点 → artifacts/layers/wells.geojson → manifest
    // 声明「wells」→ 实例化 → 绑给 m_wellLink。工程打开与 catalog 每次
    // 变更时调用；没有可定位井时不声明空图层。
    // D6：zoomOnGrowth 为真（catalog 变更路径）且井点范围实际变大时
    // zoom-to-content——无关联导入只重写同一份 geojson，范围不变不抢视野；
    // 工程打开路径传 false，让位于 .qgz 里恢复的视野。
    void refreshWellsLayer(bool zoomOnGrowth);
    // goal/well-trajectory：井底位移轨迹线层（surface→TD 投影）。无任何
    // 已决测斜时不写文件不声明层（诚实空，同 wells 的约定）。
    void refreshWellTrajectoriesLayer();
    // #152 打开闸门：在读新工程前为 projectDir 取锁（或复用/降级只读/取消）。
    bool acquireProjectLock(const QString &projectDir, bool creating, QString *error,
                            bool *cancelled);
    void releaseProjectSession();

    bool m_ready = false;
    QgisProjectService *m_projectSvc = nullptr;
    QgisLayerService *m_layerSvc = nullptr;
    QgisCanvasController *m_canvasCtl = nullptr;
    QgisProcessingService *m_procSvc = nullptr;
    QgisEditingService *m_editSvc = nullptr;
    QgisStyleService *m_styleSvc = nullptr;
    ToolAvailabilityService *m_toolSvc = nullptr;
    ErrorHub *m_errorHub = nullptr;
    PaleoOnnxService *m_onnxSvc = nullptr;
    // 方向51：远端推理装配产物（router 的生命周期挂在 this 上）。
    RemotePredictionRouter *m_remotePredict = nullptr;
    QString m_remotePredictHint;
    AiChatController *m_aiChat = nullptr;
    // 方向68：Python 脚本面三件（运行服务/REPL 会话/控制台编排）。
    class ScriptRunnerService *m_scriptRunner = nullptr;
    class PythonReplSession *m_pythonRepl = nullptr;
    class PythonConsoleController *m_pythonConsole = nullptr;
    AiAssistWorkflow *m_aiAssistWf = nullptr;
    SelectionContext *m_selection = nullptr;
    SeismicMapLink *m_seismicLink = nullptr;
    WellMapLink *m_wellLink = nullptr;
    QString m_projectDir; // wells.geojson 输出根（projectOpened 时设置）
    QString m_metaPath;   // 当前会话的 project.sqlite（切换/关闭时关连接，#80）
    QgsRectangle m_lastWellsExtent; // D6 zoom：上次井点范围（空 = 尚无井点）
    PaleoProjectStore *m_store = nullptr;
    LayerManifest *m_manifest = nullptr;
    QgisLayerOrganizer *m_layerOrganizer = nullptr; // 图层树布局器（置顶共享+层位组）
    DataImportService *m_import = nullptr;
    QgisLayoutService *m_layoutSvc = nullptr;
    PredictionWorkflow *m_predictionWf = nullptr;
    ConstraintWorkflow *m_constraintWf = nullptr;
    FaciesMappingWorkflow *m_faciesMappingWf = nullptr;
    CompositionWorkflow *m_compositionWf = nullptr;
    ValidationWorkflow *m_validationWf = nullptr;
    class WellSitingWorkflow *m_wellsitingWf = nullptr;
    WellSitingStore m_wellsitingStore{QString(), nullptr}; // projectOpened 值重绑（FaultSetStore 同式）
    ProjectDataFacade *m_projectData = nullptr;
    MappingWorkbench *m_mappingWorkbench = nullptr;
    MappingWorkflow *m_mappingWf = nullptr;
    DepthConversionWorkflow *m_depthWf = nullptr;
    PropertyModelWorkflow *m_propModelWf = nullptr;
    MapVersionStore *m_versionStore = nullptr;
    MapVersionController *m_versionCtl = nullptr;
    FaultSetStore m_faultStore{QString(), nullptr}; // projectOpened 值重绑
    paleo::fault::FaultInterpretationController *m_faultCtl = nullptr;
    PaleoTaskService *m_taskSvc = nullptr;
    std::unique_ptr<ProjectDirLock> m_projectLock;
    std::unique_ptr<ProjectDirLock> m_pendingLock; // 闸门已取、projectOpened 接管
    bool m_pendingLockReuse = false;               // 闸门判定同目录复用现锁
    std::function<bool(const QString &)> m_lockConflictResolver;
    bool m_lastReadOnlyNotified = false; // 上次广播的只读态（去重）
};
