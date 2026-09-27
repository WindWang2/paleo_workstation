#pragma once
#include <QString>
#include <QStringList>
#include <QWidget>

class CompositionWorkflow;
class QgisLayerService;

// ui/pages/composepage.h — ③综合编图页（自 pagepanels 拆出，公共形状不变：
// objectName/信号签名/ctor 签名全保留）。
// 层：视图
// ③综合编图 — factor list (checkbox rows) + fuse button.
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

  signals:
    void fuseRequested(const QStringList &factorLayerIds);
    // rasterLayerId is a declared raster; minArea/simplifyTolerance are map units.
    void polygonizeRequested(const QString &rasterLayerId, double minArea, double simplifyTolerance);
    // wave/mapping-pipeline 阶段C — D61 编图链（厚度→IDW→转相面）；层位由
    // shell 从 activeHorizon 解析（chip 选择）。链路状态文案走 statusLabel。
    void thicknessChainRequested();
    // 层位图 PDF 导出（井位 + 相多边形）；成功后 shell 登记布局产物。
    void exportPdfRequested();
    // 阶段E — 保存版本 / 发布（发布受 setPublishEnabled 门控）。
    void saveVersionRequested();
    void publishRequested();
};
