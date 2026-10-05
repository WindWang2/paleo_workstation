// 层：数据
#pragma once

#include "propfill.h"
#include "stratgrid.h"

#include <QString>

#include <cstdint>
#include <vector>

// stratgrid/objectmodel — 对象建模最小骨架（goal/prop-model-v2，必做项）。
//
// 河道/点坝两类对象的参数化放置：几何参数（走向/长度/宽度/厚度/曲率/垂向
// 锚定）显式给定，放置位置由种子随机（mt19937_64，跨平台可复现；一对象
// 一次播种，参数不加抖动——放置记录即几何回读面）。播种规则：限制相带内
// 取柱（zoneCode ≥ 0 且给了相带栅格 → 只在该带柱心播种；几何出带不裁剪，
// 河道本来可以穿过相带界线——口径如实）。
//
// 几何口径（钉死）：
//   * 河道中线 = 播种点 + t·L·走向 + 曲率·sin(2πt)·左法向，t∈[0,1]；
//     cell 命中 = 柱心到中线折线（512 段）距离 ≤ width/2；
//   * 点坝 = 地图椭圆：长轴沿走向 length、短轴 width，中心即播种点；
//   * 垂向：层位坐标 s ∈ [verticalFrac − thickness/柱厚, verticalFrac]
//     （verticalFrac=1 底锚定；对象沿层理展布，错位格架上自动跟随断块）；
//   * 对象体可跨断层几何展布（连续地质体两盘各自错开），数值场的分块
//     连通由背景场（IDW/SGS）机制承担；
//   * 多对象重叠：后放覆盖先放（确定性），重叠计数如实。
//
// 优先级口径（钉死）：对象优先——对象 cell 的值硬覆盖背景场
//（applyObjectOverride）；对象指示场本身是独立属性体（value=对象属性值，
// 非对象 NaN）。
namespace paleo::stratgrid
{

enum class ObjectType
{
  Channel = 0,
  PointBar = 1
};

struct ObjectSpec
{
  ObjectType type = ObjectType::Channel;
  double azimuthDeg = 90;   // 走向（从北顺时针；点坝=长轴方向）
  double length = 0;        // 河道中线长 / 点坝长轴长；<=0 → 0.8×格架地图对角线
  double width = 200;       // 地图宽（河道全宽 / 点坝短轴）
  double thickness = 20;    // 米（沿层厚度）
  double curvature = 0;     // 河道中线正弦振幅（米）；点坝忽略
  double verticalFrac = 1;  // 垂向锚定：对象占层段上界（1 = 底锚定）
  double value = 1;         // 对象属性值
  int count = 1;            // 放置数量（每对象独立播种）
  int zoneCode = -1;        // 播种限制相带；-1 = 全域
};

struct ObjectPlacementRecord
{
  ObjectType type = ObjectType::Channel;
  double centerX = 0;
  double centerY = 0;      // 播种点（河道中线起点 / 点坝椭圆心）
  double azimuthDeg = 0;
  double length = 0;
  double width = 0;
  double thickness = 0;
  double curvature = 0;
  double verticalFrac = 0;
  double value = 0;
  int cells = 0;           // 该对象命中的 cell 数（含被后续对象覆盖的部分）
  int zoneCode = -1;
};

struct ObjectModelMeta
{
  std::vector<ObjectPlacementRecord> placements;
  int objectCells = 0;  // 命中 cell 总数（去重后）
  int overlapCells = 0; // 多对象重叠 cell 数
  QString caliber;
};

// 对象指示/属性场：value = 对象属性值，非对象 NaN；columnBlock 照竖帘口径填。
bool placeObjects(const ZoneGrid &grid, const std::vector<int> *zonePerColumn,
                  std::uint64_t seed, const std::vector<ObjectSpec> &specs,
                  PropertyVolume *out, ObjectModelMeta *meta, QString *error = nullptr);

// 口径钉死：对象优先——对象 cell 硬覆盖背景场值；背景计数随之重算。
bool applyObjectOverride(const PropertyVolume &objects, PropertyVolume *background,
                         QString *error = nullptr);

} // namespace paleo::stratgrid
