// 层：QGIS 封装
#pragma once
#include <QString>
#include <QVariantMap>

class QgsColorRamp;
class QgsRasterLayer;

// qgis/factorstylewriter.h — 单因素色带样式落盘（m2/mapping-pages 任务 B）。
// 每个因素一份默认 QgsColorRamp 预设（PALEO_QGIS_PLAN §10 词表对应，地图域
// 数据符号——DESIGN.md：相色标由 QGIS 样式系统管理，不属 UI token）：
// 构建 QgsSingleBandPseudoColorRenderer 配到临时 raster layer 后
// saveNamedStyle 落盘 <styleDir>/factor_<factorId>.qml；图层声明里的
// styleRef="factor_<factorId>" 由 QgisStyleService::applyStyle 消费。
// styleDir 为空 → 不落盘，仅返回内存样式描述（调用方决定是否降级）。
// 层：QGIS 封装
namespace FactorStyleWriter
{

bool applyTo(QgsRasterLayer *layer, const QString &factorId);

// factorId 的色带预设（caller 拥有；未知 id → nullptr）。
QgsColorRamp *rampFor(const QString &factorId);

// 预设描述（color1/color2/单位语义），供注册表展示与测试断言；未知 id → 空表。
QVariantMap presetDescription(const QString &factorId);

// 为 rasterPath 的栅格构建伪彩色渲染样式并落盘
// <styleDir>/factor_<factorId>.qml（目录不存在则建）。返回 .qml 绝对路径；
// styleDir 为空、色带缺失或栅格不可读 → 空串 + error。
QString writeStyleQml(const QString &factorId, const QString &rasterPath,
                      const QString &styleDir, QString *error = nullptr);

// P4 逐版本色带：factor_<id>.qml 为同因素所有层位/版本共用（后写覆盖前写，色带
// 范围只对最新一版成立）。版本化引用 factor_<id>_v<路径摘要12位>，与成果栅格路径
// 一一对应；不改 manifest/catalog 结构（styleRef 本就是自由字符串）。
QString versionedStyleRef(const QString &factorId, const QString &rasterPath);
// 写共用 factor_<id>.qml（兼容既有引用）并另存该版本的 <versionedStyleRef>.qml；
// 返回版本化 .qml 路径；任一步失败 → 空串 + error。
QString writeVersionedStyleQml(const QString &factorId, const QString &rasterPath,
                               const QString &styleDir, QString *error = nullptr);

} // namespace FactorStyleWriter

// 地图域数字与线面协调规范（DESIGN.md「单因素地图符号」）；UI 主题不改数值语义色。
class QgsVectorLayer;
namespace FactorStyleWriter {
inline constexpr double contourWidthMm = 0.25;
inline constexpr double contourCasingWidthMm = 0.55;
inline constexpr double contourLabelSizePt = 9.0;
inline constexpr double contourLabelBufferMm = 0.6;
inline constexpr double contourLabelRepeatMm = 60.0;
inline constexpr double contourMinimumLengthMm = 10.0;
bool applyContours(QgsVectorLayer *layer);
}
