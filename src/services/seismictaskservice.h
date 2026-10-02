// 层：数据
#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSemaphore>
#include <QSet>
#include <atomic>
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
#include "domain/seismic/timedepthmodel.h"
#include "domain/seismic/sgysectionbuilder.h"
#include "metadata/layermanifest.h"

class PaleoTask;
class PaleoTaskService;
class DataCatalog;

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

// ---- Phase 4 解释工具（D4.1–D4.10）数据模型 --------------------------------
// 解释模型放服务层的理由：视图层 io/* include 白名单仅 lasdoc.h，解释模型
// 必须从视图可达（拾取面板/画布叠加），services 属数据层且为本包独占文件。

// 单个层位拾取点（D4.1）
struct SeismicPick
{
  int id = 0;
  int inlineNo = 0;
  int xlineNo = 0;
  double twtMs = 0.0;
  int sampleIndex = 0;
  float confidence = 1.0f;   // D4.10：追踪置信度 0..1（手动拾取 = 1）
  QString interpreter;       // D4.9：解释者
  QString horizonName;       // 所属层位名

  bool operator==(const SeismicPick &o) const { return id == o.id; }
};

// 断层标记（D4.4）：剖面上的折线（traceFrac 0..1 剖面横向，twtMs 纵向）
struct SeismicFaultSegment
{
  int id = 0;
  SgySliceType sectionType = SgySliceType::Inline;
  int sectionIndex = 0;
  QVector<QPair<double, double>> points; // (traceFrac, twtMs)
  QString interpreter;
  QString name;
};

// 解释会话（D4.8）：拾取集 + 断层集 + 解释者名册，伴生文件持久化
struct SeismicInterpretationSession
{
  QString name;
  QString sourceSgyPath;               // 会话归属的 SEG-Y（伴生文件锚）
  QStringList interpreters;            // D4.9 名册
  QList<SeismicPick> picks;
  QList<SeismicFaultSegment> faults;
  int nextId = 1;

  QStringList horizonNames() const;
  const SeismicPick *pickById(int id) const;
};

// D4.2 局部互相关追踪参数
struct SeismicTrackOptions
{
  int windowSamples = 24;        // 相关窗（种子波形长度）
  int maxSearchSamples = 12;     // 逐道最大搜索半径
  double correlationThreshold = 0.6; // 低于阈值的道不拾取
};

// goal/horizon-autotrack — 追踪执行报告（QC：覆盖率/均值置信度/双侧停因）
struct SeismicTrackReport
{
  int coveredTraces = 0;     // 有拾取的剖面道数
  int totalTraces = 0;       // 剖面总道数
  float meanConfidence = 0.0f;
  QString stopSummary;       // 人话停因（如「左：相关丢失@XL2031；右：到达边界」）
};

// D4.7 网格化结果（规则 il×xl 栅格 + 掩码）
struct SeismicHorizonGrid
{
  int inlineMin = 0, inlineCount = 0, inlineStep = 1;
  int xlineMin = 0, xlineCount = 0, xlineStep = 1;
  std::vector<double> twtMs;     // inlineCount×xlineCount，NaN=无控制点
  std::vector<float> confidence;
  bool isValid() const { return inlineCount > 0 && xlineCount > 0; }
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

  // 15b. 体窗平面平移（wave/deepen-perf A1）：ReadVoxelWindow 结果的一个
  //     采样平面 → 时间片方向约定的 SgySliceImage（width=XL 数、height=IL 数、
  //     行 0=最大 inline 的显示向，与引擎 ReadTimeSlice 逐位同构）。
  //     values-only：rgba 由调用方 SgyVolume::Recolorize 预烘焙（与切片通道
  //     同一色彩语义）。sampleIndex 为窗口内相对采样号；越界返回 false。
  //     3D 体渲染堆叠层消费（16 层合并为单次体窗请求）。
  static bool slicePlaneFromWindow(const engine::VoxelWindow &window,
                                   int sampleIndex, SgySliceImage &out);

  // ---- Phase 4 解释工具（同步 CPU 操作，量级 ≤ 单切片）----

  // D4.2 局部互相关追踪：从种子道出发双向沿同相轴追踪。返回逐道拾取
  // （confidence = 峰值相关系数；低于阈值的道缺席）。数值核在
  // algorithms/horizontrack（goal/horizon-autotrack 下沉），此处为剖面
  // 布局桥接 + 域映射。
  static QList<SeismicPick> trackHorizon(
      const SgySliceImage &slice,
      SgySliceType sectionType, int sectionIndex,
      int colMin, int colMax,          // 列号范围（IL 剖面列=XL，XL 剖面列=IL）
      int seedTraceCol, int seedSample,
      const SeismicTrackOptions &options,
      const QString &interpreter, const QString &horizonName,
      float sampleIntervalMs);

  // goal/horizon-autotrack — 多种子双向追踪 + 合并（重叠道取高置信，
  // 种子含在返回值中、id=0 由会话分配）。seeds = (剖面列号, 采样) 列表；
  // report 可空。停因人话化（覆盖失败区如实留空的 QC 口径）。
  static QList<SeismicPick> trackHorizonMultiSeeds(
      const SgySliceImage &slice,
      SgySliceType sectionType, int sectionIndex,
      int colMin, int colMax,
      const QList<QPair<int, int>> &seeds,
      const SeismicTrackOptions &options,
      const QString &interpreter, const QString &horizonName,
      float sampleIntervalMs,
      SeismicTrackReport *report = nullptr);

  // goal/horizon-autotrack — 异步可取消追踪（PaleoTask，协作取消逐道生效；
  // 取消不发布拾取——与服务「取消无半成品」纪律一致）。返回任务句柄。
  PaleoTask *startHorizonTracking(
      const SgySliceImage &slice,
      SgySliceType sectionType, int sectionIndex,
      int colMin, int colMax,
      const QList<QPair<int, int>> &seeds,
      const SeismicTrackOptions &options,
      const QString &interpreter, const QString &horizonName,
      float sampleIntervalMs,
      std::function<void(bool ok, const QList<SeismicPick> &picks,
                         const SeismicTrackReport &report,
                         const QString &error)> onFinished);

  // D4.7 拾取网格化：IDW（反距离加权）插值成规则测网栅格。
  // 既有算法层 ConstraintIDW 是 QgsProcessing 形态（需 Processing 上下文与
  // 约束线），拾取网格化无约束语义——自实现纯 IDW（TODOS 登记合并点）。
  static SeismicHorizonGrid gridPicks(const QList<SeismicPick> &picks);

  // D4.3 拾取集 → 层位资产：写 CSV（inline,xline,twt_ms,confidence）+
  // DERIVED 版本登记 catalog（父版本 = 源地震版本）。返回登记后的版本路径。
  // goal/horizon-autotrack：layerOut 非空时额外产层位栅格 GeoTIFF（拾取
  // 网格化 → BinnedHorizon → horizonbinner 既有管线，不开平行格式）并回填
  // LayerDeclaration（"horizon.<名>"，group 00_Data）供调用方声明上图。
  static QString registerHorizonAsset(
      DataCatalog *catalog, const QString &seismicAssetId,
      const QString &seismicVersionId, const QString &horizonName,
      const QList<SeismicPick> &picks, const QString &outputDir,
      QString *error,
      LayerDeclaration *layerOut = nullptr);

  // D4.4 断层段 → 矢量派生资产：CSV 折线（section,traceFrac,twtMs）+ 登记。
  static QString registerFaultAsset(
      DataCatalog *catalog, const QString &seismicAssetId,
      const QString &seismicVersionId, const QString &faultName,
      const QList<SeismicFaultSegment> &faults, const QString &outputDir,
      QString *error);

  // D4.8 会话持久化：伴生文件 <sgy>.seispicks.json（项目无关可携带）
  static bool saveSession(const SeismicInterpretationSession &session, QString *error);
  static bool loadSession(const QString &sgyPath, SeismicInterpretationSession &out, QString *error);

  // D4.5 CSV 导出
  static bool exportPicksCsv(const QList<SeismicPick> &picks, const QString &filePath, QString *error);

  // ---- 地震属性（goal/seismic-attributes）--------------------------------
  // 属性核在 src/algorithms/seismicattr.h（纯数值）；此处只做任务编排：
  // 切片/邻线读取 → 核计算 → SgySliceImage 属性图（与源切片逐位同几何，
  // 可直接叠加显示），进度/取消复用 PaleoTask 语义。

  enum class SeismicAttrKind
  {
    Envelope,   // 包络 |a|（Taner 1979）
    InstPhase,  // 瞬时相位（度）
    InstFreq,   // 瞬时频率（Hz，Barnes 2007 差分法）
    InstQ,      // 瞬时 Q 原型（不稳定处 NaN）
    Rms,        // 时窗 RMS 振幅
    MaxAbs,     // 时窗最大绝对振幅
    MeanEnergy, // 时窗平均能量
    Coherence,  // semblance C2 相干（Marfurt 1998）
    Sweetness   // 甜点 env/sqrt(f)（Radovich & Oliveros 1998）
  };

  static QString seismicAttrId(SeismicAttrKind kind);          // "envelope"|...（catalog/面板键）
  static QString seismicAttrDisplayName(SeismicAttrKind kind); // 中文显示名
  static bool seismicAttrNeedsNeighbors(SeismicAttrKind kind); // 相干需邻线（3 线窗）

  struct SeismicAttrParams
  {
    int windowHalfSamples = 8;   // 时窗族半窗（样，闭窗 [i-h,i+h]）
    int coherenceIlHalf = 1;     // 相干 inline 向半窗（道）
    int coherenceXlHalf = 1;     // 相干 crossline 向半窗（道）
    int coherenceTimeHalf = 2;   // 相干垂直半窗（样）
  };

  struct SeismicAttrResult
  {
    bool ok = false;
    QString error;
    QString attrId;
    SgySliceType sectionType = SgySliceType::Inline;
    int sectionIndex = 0;        // 实际解析出的测线号/采样号
    int traceCount = 0;          // 参与计算的道数（含 NaN 道）
    int validTraceCount = 0;     // 有限值道数
    double readMs = 0.0;         // 切片读取耗时（实测表用）
    double computeMs = 0.0;      // 核计算耗时
    std::shared_ptr<const SgySliceImage> image; // 属性图（values 行主序，NaN=无效）
  };

  // 异步属性切片：volume 为已加载体（startVolumeLoad 产物）；sliceIndex 为
  // inline/xline 号（稀疏测网按精确值解析，缺线如实失败）；Time 切片暂不
  // 支持（瞬时族需整道谱，时窗族需垂向窗，见 TODOS 递延）。onFinished 在
  // 服务所在线程回调。
  PaleoTask *startAttributeSlice(
      std::shared_ptr<const SgyVolume> volume,
      SeismicAttrKind kind,
      const SeismicAttrParams &params,
      SgySliceType sliceType,
      int sliceIndex,
      std::function<void(bool success, const SeismicAttrResult &result)> onFinished);

  // 属性图 → 派生资产：写 <outputDir>/<attr>_<il|xl>_<idx>.sattr（"SATR"
  // 魔数 + 版本 + width/height + JSON 头 + 小端 f32 值块）+ DERIVED 版本登记
  // （父版本 = 源地震 RAW 版本）。返回登记后的文件路径（空 = 失败）。
  static QString registerAttributeSliceAsset(
      DataCatalog *catalog, const QString &seismicAssetId,
      const QString &seismicVersionId, const SeismicAttrResult &result,
      const SeismicAttrParams &params, const QString &sourceSgyPath,
      const QString &outputDir, QString *error);


  // ---- Phase 6 性能与可靠性 ----

  // D6.6 错误分类：文件缺/索引坏/内存超限/GL 不可用分级
  struct SeismicErrorCategory
  {
    enum class Kind { None, FileMissing, IndexCorrupt, MemoryBudget, GlUnavailable, Cancelled, Other };
    Kind kind = Kind::None;
    QString userText; // 面向用户的中文描述
    static SeismicErrorCategory classify(const QString &error, bool glContextFailed = false);
  };

  // D6.3/D6.8 内存治理（P4 管理器不存在——自建同形接口；合并点：统一
  // 管理器落地后把 budgetBytes()/onExceeded() 委托给它即可，PR 已注明）
  struct SeismicMemoryReport
  {
    qint64 volumeBytes = 0;
    qint64 totalRamBytes = 0;
    qint64 budgetBytes = 0;   // RAM/2
    bool overBudget = false;
    QString recommendation;   // 分页通道建议文案
  };
  static qint64 totalRamBytes();
  static SeismicMemoryReport assessMemoryBudget(qint64 volumeBytes,
                                                qint64 totalRamOverride = -1);
  // 当前服务占用的内存估计（数据缓存预算 + 任意线缓存 + 数据集条目缓存）
  qint64 estimatedMemoryBytes() const;

  // D6.4 并发纪律：地震后台任务共享 ≤4 并发（信号量槽位；全 start* 走此闸，
  // 排队 = 信号量等待不耗 CPU；入池前已取消的任务直接跳过执行）
  static constexpr int kMaxConcurrentTasks = 4;
  int activeTaskCount() const; // 在途任务数（含信号量排队中）
  // 提交一个受 ≤4 并发闸约束的地震任务（测试/扩展面；常规走各 start*）。
  // quiet=true：交互内嵌取数（切片/剖面/LOD/瓦片/体窗）不拉起任务中心。
  PaleoTask *startBounded(const QString &title,
                          const std::function<QString(PaleoTask *)> &work,
                          const QString &layerId = QString(), bool quiet = false);

  // ---- Phase 5 井震与任意线 ----

  // D5.4 合成记录：AC(声波)+DEN(密度) → 波阻抗 → 反射系数 → Ricker 子波
  // 褶积。缺曲线/时深表 → ok=false + reason（降级为仅轨迹投影）。
  struct SeismicSyntheticResult
  {
    bool ok = false;
    QString reason;
    std::vector<double> twtMs;   // 采样时间
    std::vector<float> amplitude; // 归一化振幅
    int sampleCount = 0;
  };
  static SeismicSyntheticResult computeSyntheticSeismogram(
      const std::vector<double> &acDepthsM, const std::vector<float> &acUsPerM,
      const std::vector<double> &denDepthsM, const std::vector<float> &denValues,
      const TimeDepthModel &tdModel, double rickerHz = 25.0);

  // 转码完成后作废缓存条目：下次读取按磁盘现状重开（热切换）。
  void invalidateDataset(const QString &path);

  // D5.2 任意线提取缓存查询：同（体指纹×路径）重复提取直接命中。
  std::shared_ptr<const SgySliceImage> cachedSection(
      const std::vector<glm::ivec2> &pathPoints, std::shared_ptr<const SgyVolume> volume) const;
  void cacheSection(const std::vector<glm::ivec2> &pathPoints,
                    std::shared_ptr<const SgyVolume> volume,
                    std::shared_ptr<const SgySliceImage> image);

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

  // D6.4 并发闸（类型见下方 SeismicConcurrencyGate——moc 不支持类内嵌套）
  std::shared_ptr<struct SeismicConcurrencyGate> gate_;

  // D5.2 任意线 LRU（≤4；键 = 体积指纹 ^ 路径 FNV）
  struct SectionCacheEntry
  {
    qint64 key = 0;
    std::shared_ptr<const SgySliceImage> image;
    quint64 lastUse = 0;
  };
  mutable std::vector<SectionCacheEntry> sectionCache_;
  mutable quint64 sectionCacheClock_ = 0;
  static qint64 sectionCacheKey(const std::vector<glm::ivec2> &pathPoints,
                                std::shared_ptr<const SgyVolume> volume);

  // A3（wave/deepen-perf）同路径瓦片请求取代：startTimeSliceTiled 对同一
  // .sf3p 的新请求启动即取消旧在途任务（引擎按瓦片粒度协作中止——被顶替
  // 的整图读取不再排队占并发闸）。仅服务所在线程访问（start/finished 均
  // 队列回主线程）。
  QHash<QString, QPointer<PaleoTask>> inFlightTiledTasks_;
};

// D6.4 并发闸：≤4 槽信号量 + 在途计数。shared_ptr 由 worker 携带——
// 服务析构时在途 worker 不悬挂（同 registry 析构竞态模式）。
struct SeismicConcurrencyGate
{
  explicit SeismicConcurrencyGate(int slotCount)
      : slotSemaphore(slotCount) {}
  QSemaphore slotSemaphore;
  std::atomic<int> active{0};
};

} // namespace seismic
