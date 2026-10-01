// 层：功能
#pragma once
#include <QString>
#include <QVector>
#include <functional>

class PaleoOnnxService;

// ai/ — tile 批量推理（goal/ai-geological-assist 范围2）。
// 纯函数（plan/softmax/stitch）无 io 依赖；runTileInference 是同步执行核心
//（进度回调/协作取消；异步任务编排由 workflow 层经 PaleoTaskService 包）。
// 数据源经 fetch 回调解耦——ai/ 不读 io/，体窗/栅格取数由 workflow 或测试注入。

// 单 tile 的读域（含 halo）与内区（缝合写入域）。内区互不重叠且恰好覆盖
// 全图——缝合接缝消隐靠 halo 读域 + 内区裁剪写回。
struct InferenceTile
{
  int row0 = 0, col0 = 0;            // 读域起点（网格坐标，含 halo，已夹边）
  int rows = 0, cols = 0;            // 读域尺寸
  int innerRow0 = 0, innerCol0 = 0;  // 内区起点（整图坐标）
  int innerRows = 0, innerCols = 0;  // 内区尺寸
  bool isValid() const { return rows > 0 && cols > 0 && innerRows > 0 && innerCols > 0; }
};

// 网格按内区 tileRows×tileCols 切片，halo 为读域四周冗余（边界夹边）。
QVector<InferenceTile> planInferenceTiles( int gridRows, int gridCols,
                                           int tileRows, int tileCols, int halo );

// 逐像素分类统计。argmax=255 → 无数据（fetch 给 NaN 的像素）。
// confidence = 1 − H/lnC（归一化熵；1=确定，0=均匀）；C<2 时恒 1。
struct TileClassGrid
{
  int rows = 0, cols = 0, classes = 0;
  QVector<quint8> argmax;
  QVector<float> confidence;
  QVector<float> probMax;
  bool isEmpty() const { return argmax.isEmpty(); }
};

// logits 布局 [C][pixels]（模型 NCHW 输出去 batch 维）。valid 缺省全真。
void softmaxGrid( const QVector<float> &logits, int classes, int rows, int cols,
                  const QVector<bool> &valid, TileClassGrid *out );

// 把 tile 结果的内区写进全图（整图坐标）。内区互不重叠 → 每格恰好写一次。
void stitchTileIntoGrid( const InferenceTile &tile, const TileClassGrid &tileGrid,
                         TileClassGrid *grid );

struct TileInferenceRequest
{
  QString model;                     // 模型名（PaleoOnnxService 命名）
  int gridRows = 0, gridCols = 0;    // 输出网格（如 411×641 工区）
  int tileRows = 64, tileCols = 64;  // tile 内区尺寸
  int halo = 0;                      // 读域 halo（缝合消隐）
  // 数据源回调：给定读域（含 halo），填 rows*cols 个 float（NaN=缺）。
  // 返回 false + err → 任务诚实失败。由 workflow（VoxelWindow/栅格）注入。
  std::function<bool( int row0, int col0, int rows, int cols,
                      QVector<float> &out, QString &err )> fetch;
};

struct TileInferenceResult
{
  TileClassGrid grid;
  int tilesDone = 0, tilesTotal = 0;
  qint64 inferenceMs = 0;            // 纯推理累计（比率门性能档案用）
  qint64 elapsedMs = 0;              // 全程（含取数/缝合）
};

// 同步执行 tile 批量推理（调用线程即执行线程——异步编排由 workflow 经
// PaleoTaskService 池包一层，本函数不做线程假设）。流程：
//   plan → 逐 tile：fetch 读域（含 halo）→ 模型输入 [1,1,rows,cols] →
//   ORT logits [1,C,rows,cols] → softmax → 内区裁剪缝进全图。
// 契约（失败诚实，不部分外发）：
//   · 模型输入签名须为 4 维 NCHW 兼容（N=1、C=1）；输出须 [1,C,rows,cols]
//     同空间维——不符 → false + 错误说明维度。
//   · fetch 失败 → false + 原始错误；NaN 像素 → argmax=255（无数据）。
//   · onProgress(tileDone,total) 每 tile 完成后回调（进度单调）。
//   · cancelRequested() 每 tile 前检查——真则中止，*error = 「已取消」，
//     result 保留已完成计数；调用方（任务框架）据此判 Cancelled 态。
bool runTileInference( PaleoOnnxService *onnx, const TileInferenceRequest &req,
                       TileInferenceResult *result,
                       const std::function<void( int, int )> &onProgress,
                       const std::function<bool()> &cancelRequested, QString *error );
