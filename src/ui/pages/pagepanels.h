#pragma once
#include <QWidget>
#include <QString>
#include <QVariantMap>

// ui/pages/ — per-page right-dock panels (§42.2 inventory).
// Each panel is a plain QWidget bound to services/workflows via ctor injection.
// UI never touches Qgs* classes directly (§25) — panels emit intents; the
// shell/workflows do the GIS work. objectName set on every interactive widget
// for tests.

class PredictionWorkflow;
class ConstraintWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
class QgisLayerService;
class PaleoOnnxService;

// 数据管理 — asset import intents + asset table（§4 预览壳重排：页内预览
// 已挪到主区地图下方的竖向分栏，右 dock 只留导入按钮与资产表）。
// 服务经动态属性 "paleo.page.importsvc"（QObject* → DataImportService）绑定，
// 由 mainwindow 接线处设置；catalog 变更后调用 refreshAssetTable() 刷新列表。
class DataPage : public QWidget
{
  Q_OBJECT
  public:
    explicit DataPage(QWidget *parent = nullptr);
  public slots:
    void refreshAssetTable();                     // 从 catalog 资产重建资产表
    // D6 地图→表联动：实体 id 集合（画布点选井）→ 选中这些实体已决关联
    // 对应的资产行（滚动到首个命中行）。行选中照走 assetActivated → 预览
    // 打开，与手点同一通路；未决链接不算命中。无命中不改当前选中。
    void selectAssetsForEntities(const QStringList &entityIds);
    // T31「查看未决」：把资产表过滤到仍有未决链接的行；off 清除过滤。
    void setUnresolvedFilter(bool on);
  signals:
    void importRequested(const QString &kind);  // "wells" | "seismic" | "boundary"
    void assetActivated(const QString &assetId); // 列表选中 → 预览标签打开
};

// ①智能预测 — horizon + algorithm selection, run button, status.
class PredictPage : public QWidget
{
  Q_OBJECT
  public:
    PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent = nullptr);
    void setHorizons(const QStringList &horizons);
    void setAlgorithms(const QStringList &algIds);
  signals:
    void runRequested(const QString &horizon, const QString &algorithmId, const QVariantMap &params);

  private:
    QVariantMap parseInputParams();
};

class QShowEvent;

// ②约束与单因素 — constraint list + run-IDW row + D61→D62 厚度样本表。
// 样本行由 MappingWorkflow 镜像到 ConstraintWorkflow 的
// "paleo.thickness.samples"/"paleo.thickness.message" 动态属性（面板只持有
// ConstraintWorkflow*）；showEvent 时重读渲染，不弹对话框。
class ConstraintPage : public QWidget
{
  Q_OBJECT
  public:
    ConstraintPage(ConstraintWorkflow *wf, QWidget *parent = nullptr);
    // 重读厚度样本属性并刷新逐井表（井名/D61 TVD/D62 TVD/层间速度或原因）。
    void refreshThicknessSamples();
  signals:
    void drawConstraintRequested(const QString &horizon, const QString &shape, int faciesCode);
    void runIdwRequested(const QString &horizon);
  protected:
    void showEvent(QShowEvent *event) override;
};

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

// ④验证 — run button + issues table + locate intent.
class ValidatePage : public QWidget
{
  Q_OBJECT
  public:
    ValidatePage(ValidationWorkflow *wf, QWidget *parent = nullptr);
    void populate();                              // run validate(), fill table
    // 残差表渲染（populate 复用）：行 map 契约 well_name/status/residual_ms/
    // reason/threshold_ms…。独立成静态面供测试直灌（workflow 无注入点时
    // validate() 会清空 residualRows 属性）。
    static void fillResidualTable(class QTableWidget *table, const QVariantList &rows);
  signals:
    // wave/mapping-pipeline：payload 携带三视图联动所需的机器字段（来自
    // ValidationIssue::wellId + details）：wellId、horizon、inline、time_ms
    // 等；非残差问题 payload 为空表。layerId 仍用于地图缩放。
    void locateRequested(const QString &layerId, const QString &wktLocation,
                         const QVariantMap &payload);
    // 「在数据页看这条剖面」（预览壳重排）：payload 同 locateRequested——
    // inline 是目标测线号、time_ms 是目标时间。shell 负责换页+打开剖面标签。
    void seismicSectionRequested(const QVariantMap &payload);
};
