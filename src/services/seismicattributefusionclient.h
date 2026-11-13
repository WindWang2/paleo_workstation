// 层：数据
#pragma once

#include <functional>
#include <QCoreApplication>
#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

namespace paleo {

struct SeismicFusionRequest {
  QString seismicPath;            ///< 本地 SEG-Y 文件路径
  QString topHorizonPath;         ///< 顶层位路径 (SMI/Petrel ASCII 或 GeoTIFF)
  QString bottomHorizonPath;      ///< 底层位路径
  QString horizonPath;            ///< 单层位路径 (选填)
  QString horizonWindowPath;      ///< 预对齐层位时间窗 (选填，如 .hwbin/.npz)
  QString unitName;               ///< 编图单元/小层名，例如 "T70-T72/S2"
  int subIntervalIndex = 0;       ///< 0=全层段，1..4 = S1..S4
  double fractionTop = -1.0;      ///< 顶比例 [0, 1]，-1 自动由 subIntervalIndex 决定
  double fractionBottom = -1.0;   ///< 底比例 [0, 1]
  QString wellsPath;              ///< 井位/含砂率文件 (如 wells2.txt)
  QString outputDir;              ///< 成果输出目录；空时 HTTP 由服务决定，CLI 创建保留的临时目录
  int clusters = 3;               ///< 聚类数 (2..32)
  double gridSpacingX = 50.0;     ///< 网格 X 间距 (米)
  double gridSpacingY = 50.0;     ///< 网格 Y 间距 (米)
  QRectF bbox;                    ///< 目标范围矩形 [xmin, ymin, width, height] (米制，选填)
  QString targetCrs;              ///< 目标坐标系，例如 "EPSG:32650"
  QString strategy = QStringLiteral("auto"); ///< auto | pretrained | train
  QString serverUrl = QStringLiteral("http://127.0.0.1:3210");
  QString pythonExe;              ///< CLI 降级模式使用的 Python 解释器
  QString scriptPath;             ///< CLI 降级模式使用的 service_server.py 路径
  int timeoutMs = 600000;         ///< 总超时毫秒数 (默认 10 分钟)
  qint64 maxResultCells = 16 * 1024 * 1024; ///< 内存栅格预算，超预算拒绝分配
};

struct SeismicFusionResult {
  bool ok = false;
  QString error;
  QString unitName;
  QString outputDir;

  // 输出成果文件清单
  QString geoTiffPath;            ///< 北向上、外沿像元对齐的 GeoTIFF 文件路径
  QString npzPath;                ///< 类别与有效掩码 NPZ 文件
  QString cps3Path;               ///< CPS-3 格式网格文件
  QString binPath;                ///< int32 原始二进制栅格
  QString manifestPath;           ///< result_manifest.json 路径

  // 严格地理坐标与网格元数据
  int columns = 0;                ///< 网格列数 (X 方向像元数)
  int rows = 0;                   ///< 网格行数 (Y 方向像元数)
  double dx = 0.0;                ///< X 像元大小 (米)
  double dy = 0.0;                ///< Y 像元大小 (米)
  QRectF extent;                  ///< 外沿地理包围盒 [extent_left, extent_bottom, width, height]
  QVector<double> gdalGeoTransform; ///< 6 参数 GeoTransform [c, a, b, f, d, e]
  QString crsAuth;                ///< 坐标系代码，如 "EPSG:32650"
  QString crsWkt;                 ///< OGC WKT 投影串

  // 分类数据与统计
  int validCells = 0;             ///< 有效非空像元数
  QVector<int> cellsNorthUp;      ///< 行主序北向上像元数组 (第 0 行=最北)，无效值为 -9999
  QStringList selectedAttributes; ///< 优选的地震属性清单 (如 A4, A1, A7)
  QVariantList clusterSummary;    ///< 聚类统计 (1..K)，不是地质相编码
  QVariantList wellPredictions;   ///< 井点预测结果
  double elapsedSeconds = 0.0;    ///< 耗时秒数
};

class SeismicAttributeFusionClient {
  Q_DECLARE_TR_FUNCTIONS(SeismicAttributeFusionClient)
public:
  /// 测试本地 HTTP 服务是否可达 (GET /health)
  static bool isServerAlive(const QString &serverUrl = QStringLiteral("http://127.0.0.1:3210"),
                            int timeoutMs = 1500);

  /// 同步调用，须在任务 worker 中执行；仅在显式配置脚本且 HTTP 不可达时使用 CLI。
  /// timeoutMs 包含健康检查、提交、轮询/进程与结果读取；停止后的清理最多额外 1 秒。
  static SeismicFusionResult execute(
      const SeismicFusionRequest &request,
      const std::function<void(int percent, const QString &detail)> &progress = {},
      const std::function<bool()> &cancel = {});

  /// 构造发送给服务的标准 JSON 请求
  static QJsonObject buildRequestJson(const SeismicFusionRequest &request);

  /// 解析服务返回的标准 JSON 结果
  static SeismicFusionResult parseResultJson(const QJsonObject &root,
                                             const QString &fallbackOutputDir = QString(),
                                             qint64 maxResultCells = 16 * 1024 * 1024);

  /// 显式 PALEO_FUSION_PYTHON、已配置服务的 .venv、PATH；不含开发机路径。
  static QString defaultPythonExecutable();

  /// 仅使用 PALEO_FUSION_SCRIPT；空表示未启用可选 CLI。
  static QString defaultServiceScript();
};

} // namespace paleo
