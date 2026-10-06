// 层：数据
#pragma once
#include <QString>
#include <QVariantMap>
#include <QJsonArray>

// io/geojsonaffine — D11 临时配准（手工仿射）。
// 未配准 GeoJSON（经纬度/异空间）经用户给定的二维仿射参数变换到工程
// 局部测网米。产出是 DERIVED 版本的 GeoJSON——坐标已变换，属性原样；
// 「临时配准」状态记录在 catalog 版本 extra 与图层命名/水印上，不伪造
// 正式配准。
struct GeoAffineParams
{
  double tx = 0.0;      // 平移 X（米）
  double ty = 0.0;      // 平移 Y（米）
  double sx = 1.0;      // X 缩放
  double sy = 1.0;      // Y 缩放
  double rotDeg = 0.0;  // 绕原点逆时针旋转（度）
};

// BIZ-07（方向58）参数闸：五个分量必须有限；|sx|/|sy| ∈ [1e-9, 1e9]
// （零/近零缩放把几何塌成点，负值镜像合法）；|tx|/|ty| ≤ 1e10 米（远超任何
// 投影坐标量程，挡 1e300 之类溢出量级）；rotDeg 任意有限值（周期量）。
// 不合法 → false，*why 收逐字段原因（含字段名，供错误通道上报）。
bool geoAffineParamsValid(const GeoAffineParams &p, QString *why = nullptr);

// 单点变换：先缩放、再旋转、后平移。
void geoAffineApply(const GeoAffineParams &p, double inX, double inY,
                    double *outX, double *outY);

// 就地变换 GeoJSON coordinates 数组（Point=[x,y]/LineString/多边形/Multi*
// 递归共享同一形状）。只动每个 position 的前两分量，高程等后续分量原样。
void geoAffineTransformCoords(const GeoAffineParams &p, QJsonValue *coords);

// 变换整份 GeoJSON 文件并写出（紧凑 JSON；properties/结构原样）。
// *outFeatures 收要素数；*outBounds 收变换后 [minX,minY,maxX,maxY]。
// 非 FeatureCollection / 无 geometry / 写盘失败 → false + error。
// 参数不过 geoAffineParamsValid → 读源之前即 false + error，不产出文件。
bool geoAffineTransformFile(const QString &inPath, const QString &outPath,
                            const GeoAffineParams &p, QString *error,
                            int *outFeatures = nullptr,
                            double outBounds[4] = nullptr);

// 源文件坐标范围（对话框实时预览用）；解析失败回 false。
bool geoJsonBounds(const QString &inPath, double outBounds[4], QString *error);

// 参数序列化/复原（catalog 版本 extra）。
QVariantMap geoAffineToMap(const GeoAffineParams &p);
