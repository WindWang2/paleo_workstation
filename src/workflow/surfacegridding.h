// 层：功能
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>

#include "../algorithms/gridsolver.h"

class ConstraintStore;
class DataCatalog;
class QgisLayerService;

// workflow/ — 层位散点网格化 + 面运算编排（goal/gridding-surface-ops）。
// 编排不画像素：参数从视图收集，数值核在 algorithms/gridsolver，
// 落盘/登记走 io（horizonbinner GeoTIFF + DerivedAssetRegistrar 受管版本），
// 上图声明经 rasterReady 信号排队回壳（manifest 写有主线程纪律）。
class SurfaceGriddingWorkflow : public QObject
{
  Q_OBJECT
  public:
  struct Options
  {
    double cellSize = 25.0;      // 输出网格像元尺寸（平面单位）
    double tension = 0.25;       // 连续曲率张力 [0,1)
    int maxSweeps = 500;         // 每级迭代上限（多级级联累计）
    bool useBarriers = true;     // ConstraintStore break_line → 硬屏障
    bool runCrossValidation = false;
    int cvPoints = 16;           // 留一法折数上限（QC 快档）
  };

  struct Outcome
  {
    QString tifPath, assetId, versionId, layerId, title;
    int rows = 0, cols = 0;
    int sweeps = 0, constrainedNodes = 0, collisions = 0, rejected = 0, barrierCells = 0;
    bool converged = false;
    double finalDelta = 0;
    double distToDataMax = 0, distToDataMean = 0; // 距数据距离质量面统计
    double cvRms = 0;
    int cvFolds = 0;
  };

  // 面运算报告（等厚/基准面上方体积）；体积 = z 单位 × 面积单位。
  struct VolumeReport
  {
    double volume = 0, absVolume = 0, area = 0, cellArea = 0;
    double minThickness = 0, maxThickness = 0, meanThickness = 0;
    qlonglong cells = 0, nullCells = 0, positiveCells = 0, negativeCells = 0;
    QString csv() const;
  };

  // 层位散点文本勘察（参数表的上下文）：散点范围/头 Grid_size 建议像元/
  // 约束库有无 break_line。io 解析下沉到本层（视图层 io include 受限）。
  struct Inspect
  {
    bool ok = false;
    QString error;
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    bool hasHeaderCell = false;
    double headerCellSize = 0;
    bool hasConstraints = false;
    int pointCount = 0;
  };
  static Inspect inspectHorizonText(const QByteArray &text, const QString &constraintGpkgPath);

  explicit SurfaceGriddingWorkflow(QgisLayerService *layers, QObject *parent = nullptr);

  // catalog 必须是工程里唯一的写实例（app 里由 DataImportService 持有）；
  // 未绑定时 gridHorizonText 拒绝运行——不落临时目录死链。
  void setCatalog(DataCatalog *catalog, const QString &projectDir);
  // 屏障源 = 工程 GPKG（ConstraintStore 词面）路径；空串 = 无屏障。
  // 只读 load，worker 线程内栈上开 store（写面 enqueue 不参与）。
  void setBarrierSource(const QString &constraintGpkgPath);

  // 网格化层位散点文本 → 受管派生 GeoTIFF。任务线程可调：stage(key,percent)
  // 节流上报、cancel 协作取消；成功返回空串并经 rasterReady（排队回 GUI
  // 线程）请壳上图。网格规模守卫（1 亿像元预算）在 io/geometryForExtent。
  QString gridHorizonText(const QString &horizon, const QByteArray &text,
                          const Options &opt, const std::function<bool()> &cancel,
                          const std::function<void(const QString &, int)> &stage,
                          Outcome *outcome);

  // 面运算：两结构面栅格（同网格）→ 等厚 = top − base + 体积/面积报告。
  // writeManaged=true 时另写受管等厚栅格（Float32 GeoTIFF，nodata=-9999，
  // 同 top 的几何；label 进资产名，登记 DERIVED 并经 rasterReady 请壳上图）。
  // 网格不一致/打开失败/writeManaged 而未绑 catalog → 错误串。
  QString isopachBetweenRasters(const QString &topTif, const QString &baseTif,
                                const QString &label, VolumeReport *report,
                                bool writeManaged, Outcome *isopachOutcome = nullptr);

  signals:
  // 声明请求排队回壳：壳在 GUI 线程 upsert LayerManifest + 实例化上图。
  void rasterReady(const QString &layerId, const QString &source, const QString &title,
                   const QString &horizon);
  void griddingFailed(const QString &horizon, const QString &error);

  private:
  QString buildBarrierMask(const QString &horizon, const paleo::gridsolver::GridGeometry &geom,
                           std::vector<std::uint8_t> *mask) const;

  QgisLayerService *m_layers = nullptr;
  DataCatalog *m_catalog = nullptr;
  QString m_projectDir;
  QString m_barrierGpkgPath;
};
