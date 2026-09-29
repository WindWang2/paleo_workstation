// 层：数据
#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "Engine/Sdk.h"
#include "Engine/Types.h"
#include "domain/seismic/sgyindex.h"
#include "domain/seismic/sgyvolume.h"
#include "domain/seismic/sgydatacache.h"
#include "domain/seismic/sgysectionbuilder.h"

class PaleoTask;
class PaleoTaskService;

namespace seismic {

struct SeismicDatasetEntry;     // cpp 内定义：Dataset + 使用锁（engine 契约：单线程独占使用）
struct SeismicDatasetRegistry;  // cpp 内定义：条目注册表（shared_ptr 共享给 worker，析构安全）

// 秒级首屏快照（QuickOpenSegyPreview 的主线程可消费副本）：
// 网格范围/角点验证结论/中央测线真振幅缩略（未读列 NaN）。
struct SeismicQuickPreview
{
  bool ok = false;
  QString error;
  QString summary;          // 验证摘要或回落原因（fallback 时无缩略）
  bool ruleVerified = false;
  qint64 traceCount = 0;
  int sampleCount = 0;
  int sampleIntervalUs = 0;
  int inlineMin = 0;
  int inlineMax = 0;
  int xlineMin = 0;
  int xlineMax = 0;
  int previewInline = 0;
  int previewXline = 0;
  int columnsRead = 0;
  int columnsTotal = 0;
  double totalMs = 0.0;
  std::shared_ptr<const SgySliceImage> preview;
};

// 渐进瓦片：x/y 为全网格列/行偏移（行 0 = 最大 inline 的显示向，与引擎一致）。
// sampleIndex 供 UI 丢弃陈旧请求的瓦片。
struct SeismicTimeTile
{
  int sampleIndex = 0;
  int x = 0;
  int y = 0;
  int completed = 0;
  int total = 0;
  std::shared_ptr<const SgySliceImage> image;
};

// 后端状态快照（热切换提示 / LOD 标签）。
struct SeismicBackendStatus
{
  bool ok = false;
  QString error;
  QString backendName;   // direct | workspace | paged-workspace
  bool fellBackToDirect = false;
  QString quality;       // L0 full / L2 8x8x1 / direct (SEG-Y)
  QString geometry;      // traces=.. samples=.. inline=.. xline=..
  int lodLevels = 0;     // 较粗层数（不含 L0 基座）
  int activeLod = 0;
};

// 道头信息（D2.11 道头查询卡）：240B 道头关键字解码 + 定位信息。
struct SeismicTraceHeaderInfo
{
  bool ok = false;
  QString error;
  qint64 traceIndex = -1;
  qint64 fileOffset = 0;     // 道头字节偏移（3600 + n*traceBytes）
  int inlineNo = 0;
  int xlineNo = 0;
  int fieldRecord = 0;       // 字节 9-12
  int cdpEnsemble = 0;       // 字节 21-24
  double cdpX = 0.0;         // 字节 73-76（含 71-72 比例因子）
  double cdpY = 0.0;         // 字节 77-80
  int sampleCount = 0;       // 道头字 115-116
  int sampleIntervalUs = 0;  // 道头字 117-118
};

// 转码质量报告（D1.4/D1.5/D1.10）：道数/覆盖率/丢弃率/值域 + 结构化日志行。
struct SeismicTranscodeReport
{
  QString kind;                    // "sf3c" | "sf3p"
  QString output;                  // 工作区 base / .sf3p 路径
  bool ok = false;
  bool cancelled = false;
  bool resumed = false;            // 续跑命中（有跳过块或 reused）
  qint64 chunksWritten = 0;
  qint64 chunksSkipped = 0;        // 续跑时已存在的块
  qint64 chunksTotal = 0;
  qint64 tracesRead = 0;
  qint64 tracesTotal = 0;          // ROI 内 (inline,xline) 格点数
  qint64 missingTraces = 0;        // 源文件缺席（NaN 填充）
  qint64 damagedTraces = 0;        // 源读取失败（跳过 + NaN 填充）
  QStringList damagedSample;       // 前 32 个坏道 "inline/xline"
  double valueMin = 0.0;           // 无数据时 validValues=false
  double valueMax = 0.0;
  bool validValues = false;
  qint64 bytesWritten = 0;
  double elapsedSeconds = 0.0;
  QStringList lodLevels;           // sf3p：实际构建的层级（"L1"/"L2"/"L3"）
  double coverage() const          // 已写块 / 总块
  {
    return chunksTotal > 0 ? double(chunksWritten + chunksSkipped) / chunksTotal : 0.0;
  }
  double droppedRatio() const      // (缺席+损坏) / ROI 格点
  {
    return tracesTotal > 0 ? double(missingTraces + damagedTraces) / tracesTotal : 0.0;
  }
  QString summaryLine() const;     // 任务页一行摘要
  QString toJsonLine() const;      // PALEO-SEISMIC-TRANSCODE 结构化日志行
};

// 工作区断点探测（D1.2/D1.6/D1.8）：半成品识别与「继续转码」入口的数据源。
struct SeismicWorkspaceProbe
{
  bool exists = false;             // meta/.sf3p 文件存在
  bool complete = false;           // 全部块就绪（Auto 可升级）
  bool resumable = false;          // 存在但未完成 → 可续跑
  bool readable = false;           // 头解析成功（false = 版本不支持/损坏）
  int formatVersion = 0;
  int algorithmVersion = 0;
  qint64 chunksDone = 0;
  qint64 chunksTotal = 0;
  qint64 samples = 0;
  qint64 inlines = 0;
  qint64 xlines = 0;
  QString error;                   // 不可读原因（版本/损坏）
  QString stateText() const;       // UI 文案：「未开始/已完成 N 块/续跑 N/M」
};

// services/ — SeismicTaskService: 地震数据异步任务协调服务
//
// 1. 将全卷扫描与几何构建接入 PaleoTaskService 线程池，UI 线程永不阻塞；
// 2. 自动协同 SgyIndexCache 实现 .sgyidx 磁盘缓存自动加载与落盘；
// 3. 内存层维护有界 LRU 缓存（SgyDataCache），切片与剖面提取自动去重复用；
// 4. 支持秒级协作取消（CancelToken），取消后不发布不完整索引与切片；
// 5. 引擎新通道：QuickOpen 秒开、.sf3p 分页工作区（显式路径/Backend::Paged，
//    Auto 永不自动升级）、瓦片渐进时间片、ReadVoxelWindow、渐进 LOD 切换。
class SeismicTaskService : public QObject
{
  Q_OBJECT

public:
  explicit SeismicTaskService(PaleoTaskService *taskService = nullptr,
                              std::size_t dataCacheBudgetMb = 256,
                              QObject *parent = nullptr);
  ~SeismicTaskService() override;

  SgyDataCache &dataCache() { return dataCache_; }
  const SgyDataCache &dataCache() const { return dataCache_; }

  PaleoTaskService *taskService() const { return taskService_; }
  void setTaskService(PaleoTaskService *taskService) { taskService_ = taskService; }

  // 1. 异步索引 SEG-Y：优先读取磁盘缓存；未命中或强制刷新时提交至 PaleoTaskService 异步扫描
  PaleoTask *startIndexing(
      const QString &sgyPath,
      bool forceReindex,
      std::function<void(bool success, SgyIndexPtr index, const QString &error)> onFinished,
      const QString &layerId = QString());

  // 2. 异步切片提取：优先查询内存 LRU 缓存；未命中则提交后台任务提取。
  //    pagedPath 非空时改走显式 .sf3p 分页工作区（LOD 映射后端），
  //    失败如实上报（显式通道不做静默回落）。
  PaleoTask *startSliceExtraction(
      std::shared_ptr<SgyVolume> volume,
      SgySliceType type,
      int sliceIndex,
      std::function<void(bool success, std::shared_ptr<const SgySliceImage> image, const QString &error)> onFinished,
      const QString &pagedPath = QString());

  // 3. 异步任意剖面提取：任意折线路径剖面提取
  PaleoTask *startSectionExtraction(
      std::shared_ptr<SgyVolume> volume,
      const std::vector<glm::ivec2> &pathPoints,
      const SgySectionOptions &options,
      std::function<void(bool success, std::shared_ptr<const SgySliceImage> image, const SgySectionStats &stats, const QString &error)> onFinished);

  // 4. SEG-Y → .sf3c 工作区转码（engine TranscodeJob，可续跑/可取消）。
  //    workspaceBase 为空时按 Auto 约定取 <sgyPath> 本身（产出 <sgy>.sf3c.meta +
  //    <sgy>.sf3.sNNN 分片）；成功后 sdk::Dataset 的 Auto 后端自动升级随机访问。
  PaleoTask *startWorkspaceTranscode(
      const QString &sgyPath,
      const QString &workspaceBase,
      std::function<void(bool success, const QString &workspaceDir, const QString &error)> onFinished);

  // 4b. 转码 + 质量报告（D1.1 分阶段聚合进度 + D1.4 报告 + D1.10 结构化日志）。
  //     onReport 在任务结束时回调一次（成功/取消/失败都回，ok 标志区分）。
  PaleoTask *startWorkspaceTranscodeDetailed(
      const QString &sgyPath,
      const QString &workspaceBase,
      std::function<void(bool success, const QString &workspaceDir, const QString &error)> onFinished,
      std::function<void(const SeismicTranscodeReport &report)> onReport);

  // 5. QuickOpen 秒级预览：有限道头探测 + 中央测线真振幅缩略（主线 1 第一段）。
  //    全卷索引（第二段）另行走 startIndexing/startVolumeLoad。
  PaleoTask *startQuickOpen(
      const QString &sgyPath,
      int maxColumns,
      std::function<void(bool success, const SeismicQuickPreview &preview)> onFinished);

  // 6. 后台体加载（两段式第二段）：.sgyidx 命中时秒级完成，供 3D/时间片面板换装。
  PaleoTask *startVolumeLoad(
      const QString &sgyPath,
      std::function<void(bool success, std::shared_ptr<SgyVolume> volume, const QString &error)> onFinished);

  // 7. SEG-Y → .sf3p 分页工作区金字塔转码（L0+LOD，可续跑/可取消）。
  //    产出 sf3pPath 本体与 <stem>.lN.sf3p 兄弟层级；Auto 后端不变，
  //    需显式传 .sf3p 或 Backend::Paged（见 registry 条目表）。
  PaleoTask *startPagedTranscode(
      const QString &sgyPath,
      const QString &sf3pPath,
      bool buildLod,
      std::function<void(bool success, const QString &sf3pPath, const QString &error)> onFinished);

  // 7b. 分页转码 + 质量报告 + 自适应金字塔层数（D1.9：按体量规划 L1/L2/L3，
  //     buildLod=false 关闭全部 LOD；true = 自动规划）。onReport 终态回调一次。
  PaleoTask *startPagedTranscodeDetailed(
      const QString &sgyPath,
      const QString &sf3pPath,
      bool buildLod,
      std::function<void(bool success, const QString &sf3pPath, const QString &error)> onFinished,
      std::function<void(const SeismicTranscodeReport &report)> onReport);

  // 8. 瓦片渐进时间片（仅 paged L0）：tile 经 timeSliceTileReady 信号分块上屏，
  //    完成图经 onFinished 发布；焦点优先由引擎保证。
  PaleoTask *startTimeSliceTiled(
      const QString &sf3pPath,
      int sampleIndex,
      int tileSize,
      int focusInline,
      int focusXline,
      std::function<void(bool success, std::shared_ptr<const SgySliceImage> image, const QString &error)> onFinished);

  // 9. 三维窗口取数：datasetPath 可为 .sgy（Auto）或显式 .sf3p（paged）。
  PaleoTask *startVoxelWindow(
      const QString &datasetPath,
      const engine::VoxelWindowRequest &request,
      std::function<void(bool success, const engine::VoxelWindow &window, const QString &error)> onFinished);

  // 10. 显式 .sf3p 打开（progressiveLod）：就绪后报告可用 LOD 层级与当前质量。
  PaleoTask *startPagedOpen(
      const QString &sf3pPath,
      std::function<void(bool success, const SeismicBackendStatus &status, const QString &error)> onFinished);

  // 11. LOD 切换（paged 专属）：拖动中切粗层、静止后升细层。
  PaleoTask *startLodSwitch(
      const QString &sf3pPath,
      int lodLevel,
      std::function<void(bool success, const QString &quality, const QString &error)> onFinished);

  // 12. 后端探测：转码完成后的「热切换」状态提示（Auto 语义下当前会用哪个后端）。
  PaleoTask *startBackendProbe(
      const QString &sgyPath,
      std::function<void(bool success, const SeismicBackendStatus &status, const QString &error)> onFinished);

  // 13. .sf3c 工作区断点探测（D1.2/D1.6/D1.8）：头级轻量读，主线程可直调。
  //     halfSgyPath 支持 sgy 本身或 .sf3c.meta 两种路径。
  SeismicWorkspaceProbe probeWorkspace(const QString &sgyOrMetaPath) const;

  // 14. .sf3p 断点探测（含 .partial 半成品识别）。
  SeismicWorkspaceProbe probePagedWorkspace(const QString &sf3pPath) const;

  // 15. 道头查询（D2.11）：240B 道头解码。静态——无服务实例也可用（剖面
  //     dock 直接调用）。索引未命中时按二进制头推算道长（规则文件可靠）。
  static SeismicTraceHeaderInfo readTraceHeader(const QString &sgyPath, int traceIndex);

  // 转码完成后作废缓存条目：下次读取按磁盘现状重开（热切换）。
  void invalidateDataset(const QString &path);

signals:
  void indexingFinished(const QString &sgyPath, bool success);
  // 瓦片在 worker 线程产生，经队列投递到本对象所在线程后发射（主线 2）。
  void timeSliceTileReady(const seismic::SeismicTimeTile &tile);

private:
  // 分页转码 LOD 模式：0=不建；1=经典 L1+L2（旧 API 语义）；2=按体量自适应
  PaleoTask *startPagedTranscodeImpl(
      const QString &sgyPath, const QString &sf3pPath, int lodMode,
      std::function<void(bool success, const QString &sf3pPath, const QString &error)> onFinished,
      std::function<void(const SeismicTranscodeReport &report)> onReport);

  // sdk::Dataset 条目注册表（vendor/sbm Engine facade）：按路径惰性打开并缓存；
  // Backend::Auto 在有 .sf3c/.sf3p 工作区时用随机访问后端，否则 Direct
  // （Auto 只发现 .sf3c.meta 伴生，永不自动升级 .sf3p）。
  // engine 契约要求 Dataset 单线程独占使用，故条目中带互斥锁；
  // paged 后端以 progressiveLod 打开（兄弟层级发现 + 从最粗层起步）。
  // 注册表以 shared_ptr 持有并由 worker 携带——服务先行析构时注册表
  // （含其互斥锁与条目哈希）存活到最后一个在途 worker 退出，杜绝
  // 「锁在等待者手中被销毁」的析构竞态。
  std::shared_ptr<SeismicDatasetRegistry> registry_;

  PaleoTaskService *taskService_ = nullptr;
  SgyDataCache dataCache_;

  // D1.7 转码互斥：同输出路径的在途转码集合（主线程 start/finished 串行访问，
  // worker 不触碰）。start 时占用，任务终态释放。
  QSet<QString> activeTranscodeOutputs_;
};

} // namespace seismic
