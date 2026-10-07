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
