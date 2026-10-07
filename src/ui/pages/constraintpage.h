// 层：视图
#pragma once
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

class ConstraintWorkflow;
class QShowEvent;

// ui/pages/constraintpage.h — ②约束与单因素页（自 pagepanels 拆出，公共
// 形状不变：objectName/信号签名/ctor 签名全保留）。
// 层：视图
// m2(B) 升级为「单因素清单 + 生成」双区（§10 词表）：
//   · 因素表 factorTable（勾选互斥单选/名称/输入/状态「未生成|已生成·layerId」）
//   · 参数行 factorFieldEdit/factorCellSizeSpin + generateFactorButton →
//     generateFactorRequested(factorId, horizon, params)
//   · 等值线行 contourIntervalSpin + contourButton → contourRequested(
//     factorLayerId, interval)（§12：GIS LineString，不是画布临时线）
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
// 全保留）；厚度样本表挪进 CollapsibleSection（objectName 不变，默认展开），
// 样本行仍由 MappingWorkflow typed 镜像到 ConstraintWorkflow::
// setThicknessSamples（ARCH-06，原动态属性暗道已撤）。
class ConstraintPage : public QWidget
{
  Q_OBJECT
  public:
    ConstraintPage(ConstraintWorkflow *wf, QWidget *parent = nullptr);
    // 重读厚度样本属性并刷新逐井表（井名/D61 TVD/D62 TVD/层间速度或原因）。
    void refreshThicknessSamples();

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
  signals:
    void drawConstraintRequested(const QString &horizon, const QString &shape, int faciesCode);
    void runIdwRequested(const QString &horizon);
    // ---- m2(B) ----
    void generateFactorRequested(const QString &factorId, const QString &horizon,
                                 const QVariantMap &params);
    // WS-C5：测区边界面图层导入入口（结构 IDW 边界行的「导入…」按钮）。
    // 壳侧接文件对话框 → MappingWorkbench::importBoundaryLayer。
    void boundaryImportRequested();
    void extractWellFactorsRequested(const QString &factorId, const QString &horizon, const QVariantMap &params);
    void contourRequested(const QString &factorLayerId, double interval);
    void interpretiveContourRequested(const QString &factorLayerId, const QVector<double> &levels);
    void runCancelRequested();
    void factorVisibilityRequested(const QString &layerId, bool visible);
    void drawTypedConstraintRequested(const QString &horizon, const QString &shape,
                                      const QString &constraintType, int faciesCode);
    // ---- 方向23：已绘约束线编辑面 ----
    void constraintParametersRequested(const QString &horizon, const QStringList &ids, const QVariantMap &patch);
    void constraintSelectionChanged(const QString &horizon, const QStringList &ids);
    void editConstraintVerticesRequested(const QString &horizon);
    void constraintDeleteRequested(const QString &horizon, const QString &constraintId);
    void constraintSemanticChangeRequested(const QString &horizon, const QString &constraintId,
                                           const QString &semantic);
  protected:
    void showEvent(QShowEvent *event) override;
  private:
    // 勾选/生成态 → 生成与等值线按钮可用性 + 禁用 reason tooltip（DESIGN.md）。
    void updateFactorActionStates();
    // 主线6：等厚引擎行（顶/底构造面选择）的可见性与清单填充。
    void updateEngineRows();
    void invalidateWellFactors();
    void updateWellFactorActionState();
    void markInputsStale();
    void loadSelectedConstraintLine();
    QVariantMap selectedLineParams() const;
};
