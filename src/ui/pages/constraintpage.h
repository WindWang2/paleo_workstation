#pragma once
#include <QString>
#include <QVariantMap>
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
//   · 三入口 provenanceButton/distributionButton/controlPointButton →
//     drawTypedConstraintRequested(horizon, shape, constraintType, faciesCode)
// 旧链保留不动：drawConstraintRequested/runIdwRequested（idw* objectName
// 全保留）；厚度样本表挪进 CollapsibleSection（objectName 不变，默认展开），
// 样本行仍由 MappingWorkflow 镜像到 ConstraintWorkflow 的
// "paleo.thickness.samples"/"paleo.thickness.message" 动态属性。
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
  signals:
    void drawConstraintRequested(const QString &horizon, const QString &shape, int faciesCode);
    void runIdwRequested(const QString &horizon);
    // ---- m2(B) ----
    void generateFactorRequested(const QString &factorId, const QString &horizon,
                                 const QVariantMap &params);
    void contourRequested(const QString &factorLayerId, double interval);
    void factorVisibilityRequested(const QString &layerId, bool visible);
    void drawTypedConstraintRequested(const QString &horizon, const QString &shape,
                                      const QString &constraintType, int faciesCode);
  protected:
    void showEvent(QShowEvent *event) override;
  private:
    // 勾选/生成态 → 生成与等值线按钮可用性 + 禁用 reason tooltip（DESIGN.md）。
    void updateFactorActionStates();
};
