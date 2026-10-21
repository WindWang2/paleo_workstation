// 层：视图
#pragma once
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

class ConstraintWorkflow;
class QComboBox;
class QLabel;
class QShowEvent;
class QTableWidget;
class QVBoxLayout;

// ui/pages/constraintpage.h — ②约束与单因素页（自 pagepanels 拆出，公共
// 形状不变：objectName/信号签名/ctor 签名全保留）。
// 层：视图
// m2(B) 升级为「单因素清单 + 生成」双区（§10 词表）：
//   · 因素表 factorTable（勾选互斥单选/名称/输入/状态「未生成|已生成·layerId」）
//   · 参数行 factorFieldEdit/factorCellSizeSpin + generateFactorButton →
//     generateFactorRequested(factorId, horizon, params)
//   · 等值线行 contourIntervalSpin + factorContourAlgorithmCombo +
//     contourButton → contourRequested(factorLayerId, interval, contourMethod)
//   · 勾选态变化 → factorVisibilityRequested(layerId, visible)（互斥上图意图）
//   · 类型化约束线五入口 directionButton/breakLineButton/softBoundaryButton/
//     contourStopButton/cartographicDetourButton →
//     drawTypedConstraintRequested(horizon, shape, constraintType, faciesCode)
//     （五种 Semantic 枚举各一个绘制入口，type 落 ConstraintStore 词表）
//   · 已绘约束线编辑面：顶点编辑入口 editConstraintVerticesRequested(horizon)
//     （壳侧接编辑会话 + PaleoVertexTool + 撤销栈）、删除
//     constraintDeleteRequested(horizon, id)、列表右键语义切换
//     constraintSemanticChangeRequested(horizon, id, semantic)（五种词表）
// 旧链保留不动：drawConstraintRequested/runIdwRequested（idw* objectName
// 全保留）；厚度样本表挪进 CollapsibleSection（objectName 不变，默认收起），
// 样本行仍由 MappingWorkflow typed 镜像到 ConstraintWorkflow::
// setThicknessSamples（ARCH-06，原动态属性暗道已撤）。
class ConstraintPage : public QWidget
{
  Q_OBJECT
  public:
    ConstraintPage(ConstraintWorkflow *wf, QWidget *parent = nullptr);
    // 重读厚度样本属性并刷新逐井表（井名/D61 TVD/D62 TVD/层间速度或原因）。
    void refreshThicknessSamples();
    // 填充层位下拉（工程打开后由壳按 mappingHorizons() 喂入；保留仍在
    // 词表内的当前选择，变更链照常触发——约束列表/井因子随新层位刷新）。
    void setHorizons(const QStringList &horizons);

    // ---- m2(B) ----
    // 绑定图层服务（只读门面：layerDeclared → 状态列刷新；再绑时重读清单）。
    // 传 null 解绑；重复绑定同一服务幂等。
    void bindLayerService(QObject *layers);
    // 记录因素的已生成 layerId（状态列写「已生成·<layerId>」；若该行正被
    // 勾选，同时发 factorVisibilityRequested(layerId, true) 上图意图）。
    void noteFactorLayer(const QString &factorId, const QString &layerId);
    // 当前勾选行因素的已生成 layerId（未勾选或未生成 → 空串）。
    QString checkedFactorLayerId() const;
    // 勾选因素是 structural_idw 产物（其栅格带 .structural.json 侧卡）。
    bool checkedFactorIsStructural() const;
    // 长计算进行时禁用生成/等值线，并说明原因。取消按钮在忙时可用。
    void setRunBusy(bool busy);
    void noteRunStage(const QString &stage, int percent);
    void refreshConstraintList();
    void setWellFactorFields(const QVariantList &fields);
    QVariantMap wellFactorParams() const;
    QVariantMap newConstraintLineParams(const QString &type) const;
    void refreshWellFactorFields();
    void refreshWellFactorResults();
    // 等值线间距默认约为当前单因素有效井点值极差的 1/6。用户改过之后，
    // 同一因素不再覆盖；换因素或换层位再按新数据重算。
    void applySuggestedContourInterval();
    // 口径切换把逐井表失效清空后，若当前口径恰有仍新鲜的已保存提取
    //（含刚提取完切走再切回），就地恢复显示，不落回「尚未提取」。
    void restoreWellFactorDisplay();
    void refreshFactorResults();
    void refreshFactorLayers();
  signals:
    void drawConstraintRequested(const QString &horizon, const QString &shape, int faciesCode);
    void runIdwRequested(const QString &horizon);
    // ---- m2(B) ----
    void generateFactorRequested(const QString &factorId, const QString &horizon,
                                 const QVariantMap &params);
    // WS-C5：测区边界面图层导入入口（结构 IDW 边界行的「导入…」按钮）。
    // 壳侧接文件对话框 → MappingWorkbench::importBoundaryLayer。
    void boundaryImportRequested();
    void constraintImportRequested(const QString &horizon, const QString &role);
    void factorResultRequested(const QString &versionId, const QString &action);
    void taskCenterRequested();
    void maintainWellFactorsRequested(const QString &horizon);
    void extractWellFactorsRequested(const QString &factorId, const QString &horizon, const QVariantMap &params);
    void contourRequested(const QString &factorLayerId, double interval,
                          const QString &contourMethod);
    void interpretiveContourRequested(const QString &factorLayerId, const QVector<double> &levels);
    void runCancelRequested();
    void factorVisibilityRequested(const QString &layerId, bool visible);
    void drawTypedConstraintRequested(const QString &horizon, const QString &shape,
                                      const QString &constraintType, int faciesCode);
    // ---- 方向23：已绘约束线编辑面 ----
    void constraintParametersRequested(const QString &horizon, const QStringList &ids, const QVariantMap &patch);
    void constraintSelectionChanged(const QString &horizon, const QStringList &ids);
    // 双击约束行：在地图上选中并缩放到该线（定位问题约束）。
    void constraintLocateRequested(const QString &horizon, const QString &constraintId);
    void editConstraintVerticesRequested(const QString &horizon);
    void constraintDeleteRequested(const QString &horizon, const QString &constraintId);
    void constraintSemanticChangeRequested(const QString &horizon, const QString &constraintId,
                                           const QString &semantic);
  protected:
    void showEvent(QShowEvent *event) override;
  private:
    // 方向 96 拆 TU：ctor 按原文行序分解为下列构建段（创建序/连接序逐行
    // 保持；实现分布 constraintpage_wellfactors/_interpolation/_generate/
    // _constraints/_sections.cpp）。跨段指针约定：objectName 命中控件一律
    // findChild 就地解析（事件期/ctor 期与原捕获等价）；无 objectName 的
    // cellCaption 经构建段返回值 → 参数传递。
    void buildWellFactorSection(QWidget *content, QVBoxLayout *lay, QComboBox *horizons,
                                QTableWidget *factors);
    QLabel *buildFieldAndCellRows(QVBoxLayout *lay); // 返回 cellCaption（约束族段移动用）
    void buildMethodSection(QWidget *content, QVBoxLayout *lay);
    void buildBoundaryRows(QWidget *content, QVBoxLayout *lay);
    void buildAdvancedSection(QWidget *content, QVBoxLayout *lay);
    void buildSurfaceRow(QVBoxLayout *lay);
    void buildGenerateSection(QWidget *content, QVBoxLayout *lay);
    void buildContourSection(QWidget *content, QVBoxLayout *lay);
    void wireFactorTableSelection(QTableWidget *factors);
    void buildConstraintArea(QWidget *content, QVBoxLayout *lay, QComboBox *horizons,
                             QLabel *cellCaption);
    void buildFactorResultsSection(QWidget *content, QVBoxLayout *lay);
    void buildTailSections(QWidget *content, QVBoxLayout *lay);
    void wireWorkflowFeedback(ConstraintWorkflow *wf);
    void wireInputStaleness();
    void wireWellFactorTriggers(ConstraintWorkflow *wf, QComboBox *horizons,
                                QTableWidget *factors);
    // 勾选/生成态 → 生成与等值线按钮可用性 + 禁用 reason tooltip（DESIGN.md）。
    void updateFactorActionStates();
    // 复用动作的可用性，显示当前图件、阻断原因和可定位的下一步。
    void updateOperationGuidance();
    // 主线6：等厚引擎行（顶/底构造面选择）的可见性与清单填充。
    void updateEngineRows();
    // E.2：updateEngineRows 拆段（原函数体 230 行 > #502 防增长闸 200 行阈值）——
    // 方法→参数组/协变量/方向参数可见性、结构 IDW 边界/格网行、等厚顶底面
    // 清单填充各自成函数；行为与原单体实现逐行等价。
    void updateInterpolationVisibility(bool interpolant, const QString &selectedMethod,
                                       bool isopach, bool confidence);
    void updateStructuralIdwSection(bool interpolant, const QString &selectedMethod);
    void fillIsopachSurfaceCombos(const QString &horizon);
    // updateEngineRows 析出的两个下拉重建段（file_budget 防增长闸）。
    void rebuildCovariateCombo();
    void rebuildBoundaryCombo( QComboBox *boundaryCombo );
    void invalidateWellFactors();
    void updateWellFactorActionState();
    void refreshWellFactorGeoHint();
    void markInputsStale();
    void loadSelectedConstraintLine();
    // 按当前成图方法重算逐条约束「可用/参与」状态、提示与就绪门（不重读 store）。
    void applyConstraintUsePlan();
    QVariantMap selectedLineParams() const;
};
