// 层：数据
#pragma once
#include <QString>
#include <QStringList>

class DataCatalog;

// io/ — 性能基准合成夹具（wave/io-perf-cache D8.5）。
// 全部确定性（LCG 种子固定），不依赖本地真实工程数据；selfcheck perf 组、
// 回归门测试、10k 资产 catalog 基准共用这里。
namespace PerfFixtures
{
  // 合成 LAS 2.0：rows 行 × curves 列（首列 DEPT，起始 startDepth 步长
  // stepDepth；其余列确定性伪随机；每 97 行埋一个 NULL 值）。
  // 默认曲线集与工区口径一致：DEPT/GR/DT/RHOB/NPHI。
  bool makeSyntheticLas(const QString &path, int rows,
                        const QStringList &curves = {"DEPT", "GR", "DT", "RHOB", "NPHI"},
                        double startDepth = 1000.0, double stepDepth = 0.125,
                        QString *error = nullptr);

  // 合成 SEG-Y rev1（IEEE fp32）：inlCount×xlCount 测网，samples 采样。
  // inline/xline 写标准 189/193 字节位，坐标按测网仿射，dt 默认 2000us。
  // 返回总道数。
  int makeSyntheticSegy(const QString &path, int inlCount, int xlCount, int samples,
                        qint32 baseInline = 1000, qint32 baseXline = 2000,
                        int dtUs = 2000, QString *error = nullptr);

  // 在已打开的 DataCatalog 上灌 nAssets 个资产（每资产 1 实体 + 1 资产 +
  // 1 受管 RAW 版本 + 1 链接；受管文件落到 artifacts/RAW/ 下，内容确定性）。
  // 用 BatchSave 一次性落盘。
  bool populateSyntheticCatalog(DataCatalog *catalog, int nAssets, QString *error = nullptr);

  // 一步到位：建 <dir> 工程目录 + 打开 catalog + 灌 nAssets + 关闭。
  bool makeSyntheticCatalogDir(const QString &dir, int nAssets, QString *error = nullptr);

  // 合成井文本三件套（井口/分层/时深）——编码基准与 GB18030 解码测试用。
  bool makeSyntheticWellFiles(const QString &dir, int wellCount, QString *error = nullptr);

  // 合成 GeoTIFF（GDAL 写）：w×h Float32，值域确定性正弦面；可选 nodata 洞
  //（矩形区域填 nodata——D3.6 全 nodata 瓦片断言用）。
  bool makeSyntheticGeoTiff(const QString &path, int w, int h, bool withNodataHole,
                            QString *error = nullptr);

  // 确定性伪随机（夹具内部用；导出给测试比对期望值）。
  quint32 lcgNext(quint32 &state);
  double lcgUnit(quint32 &state); // [0,1)
} // namespace PerfFixtures
