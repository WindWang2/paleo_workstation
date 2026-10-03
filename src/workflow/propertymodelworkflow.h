// 层：功能
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <vector>

#include "../algorithms/stratgrid/propfill.h"
#include "../algorithms/stratgrid/upscale.h"
#include "../domain/faultset.h"

class DataCatalog;
class PaleoTask;
namespace paleo::jobs {
template <class JobT>
class JobRunner;
} // namespace paleo::jobs

// workflow/ — 属性建模编排（goal/property-modeling）。
// 视图只发意图；本层读层位栅格、建地层格架、粗化井曲线、按断层阻断做
// IDW 充填，并把属性体登记为 catalog DERIVED（类型 property_volume）。
// 不持有部件、不画像素。深度域与层位 z 必须同号（bot > top）；对不上时
// 粗化得到无值，不把时间面假装成深度。

struct PropertyModelRequest
{
  QString propertyName = QStringLiteral("PROP");
  QString topName;
  QString botName;
  QString topPath;
  QString botPath;
  bool useEmbeddedSurfaces = false;
  paleo::stratgrid::SurfaceGrid top;
  paleo::stratgrid::SurfaceGrid bot;
  int nLayers = 10;
  paleo::stratgrid::Aggregator aggregator = paleo::stratgrid::Aggregator::ThicknessWeightedMean;
  double idwPower = 2.0;
  std::vector<paleo::stratgrid::WellCurve> wells;
  std::vector<paleo::stratgrid::FaultSegment> faults;
  int trajectoryWellCount = 0; // 井筒站点来自测斜轨迹的井数（provenance 面）
};

struct PropertyModelOutput
{
  bool ok = false;
  QString error;
  QString path;
  QString assetId;
  QString versionId;
  QString paramHash;
  int liveColumns = 0;
  int filledCells = 0;
  int unfilledLiveCells = 0;
  paleo::stratgrid::PropertyVolume volume;
};

// extractSlice 的值类型。失败时 width 为 0，values 空。
struct PropertyGridSlice
{
  int width = 0;
  int height = 0;
  float valueMin = 0.f;
  float valueMax = 0.f;
  std::vector<float> values;
};

class PropertyModelWorkflow : public QObject
{
  Q_OBJECT
public:
  explicit PropertyModelWorkflow(DataCatalog *catalog, const QString &projectDir,
                                 QObject *parent = nullptr);
  void rebind(DataCatalog *catalog, const QString &projectDir);

  // progress(fraction, stage)：fraction < 1 时返回 false 表示取消，不登记版本。
  // fraction 1.0 是成功 commit 之后的完成通知，不是取消点（返回值忽略）。
  PropertyModelOutput run(const PropertyModelRequest &request,
                          const std::function<bool(double, const QString &)> &progress = {});

  // #85 两段式拆分：计算段纯数据（读层位文件/建格架/粗化/IDW/序列化，
  // worker 线程可跑，不碰 catalog）；登记段是 DerivedAssetRegistrar 的
  // stage+commit——#106 owner-thread 写守卫下必须在 catalog 所属线程调。
  // run() 就是两段直连（老调用点/测试不变），异步路径：worker runCompute →
  // GUI finished 回调里 commitComputed。
  struct PropertyModelComputed
  {
    bool ok = false;
    QString error;
    QByteArray blob;             // 已序列化属性体（含 provenance）
    QString fileName;            // 受管落位文件名（PROP_<name>.pprop）
    QString display;             // catalog 展示名
    QStringList parentPaths;     // 层位栅格源路径（嵌入面时为空 → 无父版本）
    QVariantMap extra;           // catalog 版本 extra（param_hash/参数/网格计数）
    PropertyModelOutput out;     // volume/paramHash/计数已填；path/assetId/versionId 由 commit 回填
  };
  PropertyModelComputed runCompute(const PropertyModelRequest &request,
                                   const std::function<bool(double, const QString &)> &progress = {});
  // runCompute 的纯函数形态：不读任何成员、不发信号——worker 线程用（#153）。
  // projectDir/catalogOpen 由 owner 线程快照。
  static PropertyModelComputed
  computeSnapshot(const PropertyModelRequest &request, const QString &projectDir,
                  bool catalogOpen,
                  const std::function<bool(double, const QString &)> &progress = {});
  // catalog owner 线程调用。成功 → computed.out 回填 path/assetId/versionId
  // 并 emit modelStored；失败 → out.ok=false/out.error + emit modelFailed。
  bool commitComputed(PropertyModelComputed *computed);

  // ---- 方向 20：JobRunner 迁移面 ----------------------------------------
  // 上面那对 runCompute/commitComputed 是同一条三段式协议的裸写形态：自己接
  // 任务池、自己查忙、自己判取消、自己排 finished 回包。startJob() 把同一契约
  // 接到统一框架上，行为等价点：
  //   * 忙则拒绝（现状 m_propModelRunning 布尔的等价物 → 框架 busy() 门控）；
  //   * 取消（runner.requestCancel / 任务页取消 / 工程切换开新任务会话）在
  //     compute 的下一个进度点生效（#160），commit 不执行（「发布是临界区」）；
  //   * 失败态经既有通道上 UI：失败串落到 computed.error，commit 段（失败态
  //     也执行）emit modelFailed，UI 在 jobCompleted 后 showResult(false, why)；
  //   * 任务服务缺席时退化同步直连（现状无池兜底路径原样保留）。
  //
  // Job 结构体复用 PropertyModelComputed（它已经是「输入快照 + 中间产物 +
  // 失败态」的完整形态），另挂 request/overlayAlpha 供 UI 段取用。
  struct PropertyModelJob
  {
    PropertyModelRequest request;
    PropertyModelComputed computed;
    double overlayAlpha = 1.0;
    // owner 线程 prepare 段快照（#153）：worker 只读这两个字段，不读成员。
    QString projectDir;
    bool catalogOpen = false;
    // 已登记标记：commit 段登记成功时置 true。UI 收尾接
    // JobRunnerBase::jobCompleted（commit/drop 之后发，#159）读它与
    // computed.out.ok/error 判成功、失败。
    bool registered = false;
  };

  // owner 线程（catalog 所属线程）调用。runner 由调用方持有并与本对象同线程。
  // 返回 nullptr 表示「已有计算在进行」或任务服务缺席——两者与现状的拒绝/退化
  // 语义一一对应，调用方据此走各自的老路径。
  // started 非空时回填本代 job（shared_ptr，与 commit 段共享同一份），供 UI 段
  // 读登记结果。
  PaleoTask *startJob(paleo::jobs::JobRunner<PropertyModelJob> &runner,
                      const PropertyModelRequest &request, double overlayAlpha,
                      std::shared_ptr<PropertyModelJob> *started = nullptr);

  static bool loadSurface(const QString &path, paleo::stratgrid::SurfaceGrid *out,
                          QString *error = nullptr);
  // LINESTRING / POLYGON 外环 → 线段。Z 坐标只取 x,y。解析不出点 → 空，不臆造。
  static std::vector<paleo::stratgrid::FaultSegment> segmentsFromWkt(const QString &wkt);
  static std::vector<paleo::stratgrid::FaultSegment>
  segmentsFromFaultSet(const paleo::fault::FaultSet &faults);

  // 输入面、井曲线与参数的稳定摘要（无时间戳）。同输入同哈希。
  static QString paramHash(const PropertyModelRequest &request,
                           const paleo::stratgrid::SurfaceGrid &top,
                           const paleo::stratgrid::SurfaceGrid &bot);

  // 从已绑定 catalog 收集顶底层位栅格与井曲线。不读断层（壳层另加）。
  // 层位缺失 → *error 并返回空请求；没有可用井曲线不算失败。
  PropertyModelRequest requestFromCatalog(const QString &topHorizon,
                                          const QString &bottomHorizon,
                                          const QString &curveMnemonic,
                                          int nLayers,
                                          paleo::stratgrid::Aggregator aggregator,
                                          double idwPower,
                                          QString *error = nullptr) const;

  // axis 0/1/2，语义同 stratgrid::extractSlice。失败 width = 0。
  static PropertyGridSlice gridSlice(const paleo::stratgrid::PropertyVolume &volume,
                                     int axis, int index, QString *error = nullptr);

signals:
  void modelStored(const QString &path);
  void modelFailed(const QString &reason);

private:
  DataCatalog *m_catalog = nullptr;
  QString m_projectDir;
};
