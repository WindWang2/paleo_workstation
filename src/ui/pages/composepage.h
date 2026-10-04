// 层：视图
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QWidget>

class QListWidget;
class CompositionWorkflow;
class QgisLayerService;

// ui/pages/composepage.h — ③综合编图页（自 pagepanels 拆出，公共形状不变：
// objectName/信号签名/ctor 签名全保留）。
// 层：视图
// ③综合编图 — factor list (checkbox rows) + fuse button.
// m2(C)：融合清单只列 04_SingleFactor 组内的分析栅格（等值线子组与
// cartographic.* 制图工作场只上图，不进融合）；新增相属性区
// （faciesAttrArea）、参考图区（referenceArea）与布局设计器入口；矢量化成功
// 后的自动编辑态由壳接线（setFaciesEditTarget 记录目标层）。
class ComposePage : public QWidget
{
  Q_OBJECT
  public:
    ComposePage(CompositionWorkflow *wf, QgisLayerService *layers, QWidget *parent = nullptr);
    void refreshFactors();                       // re-list declared factor.* layers and facies rasters

    // wave/mapping-pipeline 阶段E — 发布门：层位 PDF 能导出之前「发布」保持
    // 禁用（shell 在导出成功后调 setPublishEnabled(true)）。
    void setPublishEnabled(bool enabled);
    // 阶段E 完整发布门（§177/§260）：PDF 资产已落到版本行 + 每口井都有残差
    // 或原因，两条都满足才放闸；缺哪条写进 tooltip（covered<0 = 调用方未
    // 评估残差，退化为旧 setPublishEnabled 语义）。
    void setPublishState(bool hasPdf, int covered, int total);
    // 版本状态标注（还没有版本 / 编辑中·vN / 已发布·vN）；已发布后「保存版本」
    // 按钮改叫「保存新版本」——下一次保存产生新版本，不回写已发布快照。
    void setVersionState(int version, bool published);
    // D8 厚度触发控件：未选层位时禁用并写原因；选中后命名「生成 <层位>
    // 等厚图」。壳在 activeHorizonChanged 时喂当前层位（空串 = 未选）。
    void setThicknessHorizon(const QString &horizon);

    // m2(C)：相属性编辑目标层（壳在 faciesPolygonsReady 后指名——同一信号
    // 驱动画布自动编辑态）。空串清除目标；refreshFactors 兜底采纳唯一声明
    // 的 facies.* 矢量层。
    void setFaciesEditTarget(const QString &layerId);

    // 方向 25 M6：版面库——壳喂当前工程的版面名单（layoutSvc->layoutNames()），
    // 页面只列不查；选中行的打开/删除意图走信号，批量出图走整页动作。
    void setLayoutNames(const QStringList &names);

  signals:
    void fuseRequested(const QStringList &factorLayerIds);
    // rasterLayerId is a declared raster; minArea/simplifyTolerance are map units.
    void polygonizeRequested(const QString &rasterLayerId, double minArea, double simplifyTolerance);
    // wave/mapping-pipeline 阶段C — D61 编图链（厚度→IDW→转相面）；层位由
    // shell 从 activeHorizon 解析（chip 选择）。链路状态文案走 statusLabel。
    void thicknessChainRequested();
    // 层位图 PDF 导出（井位 + 相多边形）；成功后 shell 登记布局产物。
    void exportPdfRequested();
    // m2(C)：在布局设计器中打开本页版面（壳调 layoutdesignershell 入口）。
    void layoutDesignerRequested();
    // 阶段E — 保存版本 / 发布（发布受 setPublishEnabled 门控）。
    void saveVersionRequested();
    void publishRequested();
    // m2(C)：相属性保存意图——页面拿不到选中要素 id，壳/工作流侧按图层当前
    // 选中集解析；attrs 携 facies_code(int)/facies_type/comment。
    void faciesAttributesSaveRequested(const QString &layerId, const QVariantMap &attrs);
    // 方向 39：相界边界核查意图——壳调 CompositionWorkflow::runFaciesBoundaryQa
    //（faciesqa 引擎按 boundary_kind 出核查项），报告回 statusLabel。
    void boundaryQaRequested(const QString &layerId);
    // m2(C)：参考图叠加意图（06_Reference 组声明图层的勾选/取消）。
    void referenceVisibilityRequested(const QString &layerId, bool visible);

    // 方向 25 M6：版面库意图（选中行的名字随信号走；页面不持布局指针）。
    void layoutOpenRequested(const QString &layoutName);
    void layoutDeleteRequested(const QString &layoutName);
    // 批量出图（按层位组一键每层一幅）——骨架版面由壳解析（活动层位版面优先）。
    void batchFigureExportRequested();

  private:
    // 相属性区目标层 UI 同步（标签 + 保存按钮使能/reason tooltip）。
    void updateFaciesTargetUi(const QString &layerId);

    QListWidget *m_layoutList = nullptr;
};
