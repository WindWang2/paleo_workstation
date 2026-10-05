// 层：数据
#pragma once
#include <QFile>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <vector>

// io/ — 地震属性容器读写（goal/attr-volume）。
//
// SATR（剖面属性，v1）：魔数 "SATR" + quint32 版本 + qint32 width/height +
//   quint32 JSON 头长 + JSON 头 + 小端 f32 值块（行主序，row 0 = 最深样——
//   与 SgySliceImage 同约定）。原写端在 SeismicTaskService::
//   registerAttributeSliceAsset；本文件把读/写收敛到一处（crossplot 读端
//   与服务写端共用同一解析，防格式漂移）。读回器是本方向新增件。
//
// SATV（属性体，v1）：魔数 "SATV" + quint32 版本 + quint32 JSON 头长 +
//   JSON 头 + 块表（quint32 块数 + 每块 quint64 偏移/字节数）+ 分块 payload。
//   块 = blockIl 条 inline；payload 内布局 values[((localIl*nXl)+xl)*nS+s]
//   （道连续，与属性核/引擎体布局同构）。头带轴值表（稀疏测网线号域）、
//   参数包、值域与参数摘要（去重缓存/provenance 用）。写端流式逐 inline
//   落块（峰值驻留 = 单块），读端按块寻址抽取任意 IL/XL/时间面。
namespace paleo::sattr
{

// ---- SATR 剖面容器 -----------------------------------------------------------

struct SattrSectionHeader
{
  QString attrId;             // "envelope"|...（服务层键）
  QString section;            // "il" | "xl"
  int sectionIndex = 0;       // 测线号
  int width = 0;              // 图宽（道数）
  int height = 0;             // 图高（样数）
  double valueMin = 0.0;
  double valueMax = 0.0;
  qint64 traceCount = 0;
  qint64 validTraceCount = 0;
  double readMs = 0.0;
  double computeMs = 0.0;
  QString sourceSgyPath;
  QString createdAt;          // ISO UTC
  // 参数包（与 SeismicTaskService::SeismicAttrParams 对齐；coherenceWeighting
  // v1.1 新增——旧文件缺键读 0 = 等权，向后兼容）
  int windowHalfSamples = 8;
  int coherenceIlHalf = 1;
  int coherenceXlHalf = 1;
  int coherenceTimeHalf = 2;
  int coherenceWeighting = 0; // 0=等权 1=道距加权
};

// values 尺寸须为 width*height（行主序）。失败返回 false 并回填 error。
bool writeSattrSection(const QString &path, const SattrSectionHeader &header,
                       const QVector<float> &values, QString *error = nullptr);

// 读回：头逐字段 + 值块。cancelled 非∅时逐样协作检查（crossplot 大图复用）。
// 文件长/魔数/版本/尺寸不自洽 → 如实失败，不静默截断。
bool readSattrSection(const QString &path, SattrSectionHeader *header,
                      QVector<float> *values, QString *error = nullptr,
                      const std::function<bool()> &cancelled = {});

// ---- SATV 体容器 -------------------------------------------------------------

struct SattrVolumeInfo
{
  QString attrId;
  int nIl = 0, nXl = 0, nS = 0;
  QVector<int> ilValues;      // inline 轴值域（升序，稀疏测网如实记录）
  QVector<int> xlValues;      // crossline 轴值域
  double sampleIntervalMs = 0.0;
  double startTimeMs = 0.0;   // 首样 TWT（记录延迟；未知 = 0）
  int blockIl = 0;            // 每块 inline 数（末块可短）
  int blockCount = 0;         // ceil(nIl / blockIl)
  // 值域/有效单元在 EOF footer（24B：f64 valueMin + f64 valueMax + u64
  // validCells，小端）——流式写端扫描完毕才知道统计值，头内不预写假值。
  double valueMin = 0.0;
  double valueMax = 0.0;
  qint64 validCells = 0;
  double readMs = 0.0;
  double computeMs = 0.0;
  QString sourceSgyPath;
  QString createdAt;
  QString paramHash;          // 参数包摘要（hex；空 = 未提供）
  int windowHalfSamples = 8;
  int coherenceIlHalf = 1;
  int coherenceXlHalf = 1;
  int coherenceTimeHalf = 2;
  int coherenceWeighting = 0;
  double ilTraceSpacing = 0.0; // 米（道距加权记录；0 = 未知/等权）
  double xlTraceSpacing = 0.0;
};

// 流式写端：begin 落头 + 全量块表（尺寸前置可算，无需回填）；writeInline
// 逐线追加（内部攒块，满块刷盘）；finish 校验写满 nIl 并把扫描统计值写进
// EOF footer（24B）。
class SattrVolumeWriter
{
public:
  SattrVolumeWriter() = default;
  ~SattrVolumeWriter();
  SattrVolumeWriter(const SattrVolumeWriter &) = delete;
  SattrVolumeWriter &operator=(const SattrVolumeWriter &) = delete;

  bool begin(const QString &path, const SattrVolumeInfo &info, QString *error = nullptr);
  // values 尺寸须为 nXl*nS（道连续 [xl*nS+s]）。
  bool writeInline(const float *values, QString *error = nullptr);
  bool finish(double valueMin, double valueMax, qint64 validCells,
              QString *error = nullptr);
  // 放弃写入（取消/失败路径）：关文件弃半成品（调用方可删 .partial 文件）。
  void abort();

  [[nodiscard]] qint64 inlinesWritten() const { return writtenIl_; }
  [[nodiscard]] bool isActive() const { return file_.isOpen(); }

private:
  bool flushBlock(QString *error);

  SattrVolumeInfo info_;
  QFile file_;
  std::vector<float> block_;  // 当前块攒板（≤ blockIl*nXl*nS）
  qint64 writtenIl_ = 0;
};

// 随机访问读端：open 解析头 + 块表；抽取面按块寻址（峰值驻留 = 单块）。
class SattrVolumeReader
{
public:
  SattrVolumeReader() = default;

  bool open(const QString &path, QString *error = nullptr);
  [[nodiscard]] bool isOpen() const { return file_.isOpen(); }
  [[nodiscard]] const SattrVolumeInfo &info() const { return info_; }

  // 抽取面（越界索引如实失败）：
  //   extractInline(ilIdx) → nXl*nS，布局 [xl*nS+s]（道连续）
  //   extractXline(xlIdx)  → nIl*nS，布局 [ilIdx*nS+s]
  //   extractTime(sIdx)    → nIl*nXl，布局 [ilIdx*nXl+xl]
  bool extractInline(int ilIdx, std::vector<float> *out, QString *error = nullptr);
  bool extractXline(int xlIdx, std::vector<float> *out, QString *error = nullptr);
  bool extractTime(int sIdx, std::vector<float> *out, QString *error = nullptr);
  // 多采样面一次扫块（单遍块读——k 个面一次取出；3D 体渲染堆叠层用，
  // 逐面 extractTime 是 k 遍全文件读，禁止在循环里那样用）。
  bool extractTimePlanes(const std::vector<int> &sIdx,
                         std::vector<std::vector<float>> *out,
                         QString *error = nullptr);
  // 单元读（3D 悬停查询等点取样；勿用于全扫——按块粒度取数）。
  bool readCell(int ilIdx, int xlIdx, int sIdx, float *out, QString *error = nullptr);

private:
  bool readBlock(int blockIndex, QString *error); // 整块进 blockScratch_

  SattrVolumeInfo info_;
  QFile file_;
  std::vector<float> blockScratch_;
  struct BlockEntry
  {
    quint64 offset = 0;
    quint64 bytes = 0;
  };
  std::vector<BlockEntry> blocks_;
};

} // namespace paleo::sattr
