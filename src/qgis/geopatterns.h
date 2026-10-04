// 层：QGIS 封装
#pragma once
#include <QColor>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QgsFillSymbol;
class QgsLineSymbol;

// qgis/geopatterns.h — 地质符号语义注册表（方向 31 单点）。
// 岩性/相花纹与断层/相界/测区线型的语义 id ↔ qrc 资源映射只在此维护；
// QgisStyleService 的样式入口消费本表，UI 只读词表做选择器/图例（不堆
// Qgs 符号代码）。资源锚定 resources/geology（qrc /geology 前缀，随二进制
// 版本走，不落机器绝对路径）：岩性纹理 = textures/*.svg（Q/HS 1011—2016
// 附录 F，可平铺），相单元 = strata/*_fill.svg（附录 O）；线型规范
//（颜色/线宽/虚线式/端头）随定义携带。词面出处 catalog.json。
namespace GeoPatterns
{

// ---- 词表 ------------------------------------------------------------------
// QVariantMap 字段：id/title/texture(qrc 相对名，线型为空)/widthMm(平铺
// 尺寸)/style(solid|dash|dot)/group(lithology|facies|fault|facies_boundary|
// survey_boundary)。id 全局唯一且稳定（入库存 customProperty 用）。

// 岩性花纹常用集（附录 F.1/F.2/F.3 调性；砂岩三粒级+含砾、粉砂岩、泥岩、
// 页岩、灰岩、白云岩、砾岩、煤、石膏、岩盐等）。
QVariantList lithologyDefinitions();
// 相单元花纹（附录 O.1/O.2 的 *_fill 可平铺变体：河流/三角洲/湖泊/滨海/
// 洪积扇/沙漠/冰川/三角洲朵体）。
QVariantList faciesDefinitions();
// 线型规范：断层（normal/reverse/strike/inferred，断层红实/虚线 + 齿
// glyph）、相界（definite/inferred/transitional）、测区边界
//（survey_boundary，墨色实线 + 圆端头）。
QVariantList lineStyleDefinitions();

// id → 显示名（词面走翻译面；未知 → 原 id 返回，不吞数据）。
QString titleFor(const QString &id);
// 花纹 id → qrc 资源全路径（:/geology/...；线型/未知 → 空串）。
QString textureResourcePath(const QString &patternId);
// 岩性词面归一化：中文词面/粒级写法/英文 id → 规范 id；未知原样；空→空。
QString normalizeLithology(const QString &raw);
// 断层词面归一化：正/逆/走滑/推测断层及英文 id → 规范 id；未知原样；空→空。
QString normalizeFaultKind(const QString &raw);
// 花纹 id 的全部字段值命中写法（规范 id + 词面 + 岩性同义词；去重）——
// 分类渲染桶登记与选择器语义过滤共用。
QStringList valueBuckets(const QString &patternId);

// ---- 符号构造（QgisStyleService 入口内部消费；所有权归调用方）--------------
// 岩性花纹填充：SVG 平铺层 + 细灰描边；未知 id → 纯色兜底层。
QgsFillSymbol *lithologyFillSymbol(const QString &patternId);
// 相单元花纹填充：同口径（相花纹平铺尺寸基准与岩性分立，见词表 widthMm）。
QgsFillSymbol *faciesFillSymbol(const QString &patternId);
// line pattern 填充（渐变相带等无 SVG 素材场景的线纹实现）。
QgsFillSymbol *linePatternFillSymbol(const QColor &color, double distanceMm,
                                     double angleDeg);
// 断层线型四类：实测实线挂齿（normal 齿朝一侧/reverse 反侧/strike 双向
// cross），推测虚线不挂齿；未知 → 推测式（不确定地质体虚线惯例）。
QgsLineSymbol *faultLineSymbol(const QString &kind);
// 线型词表 id（fault_normal/…/survey_boundary）→ 符号：选择器/图例统一
// 构造口（词表 id 与构造参数的映射单点）。未知 → 空指针。
QgsLineSymbol *lineSymbolFor(const QString &lineId);
// 相界线型三类：确定实线/推测虚线/渐变相带点线。
QgsLineSymbol *faciesBoundaryLineSymbol(const QString &kind);
// 测区边界线型：墨色 0.6mm 实线 + 圆端头（DESIGN.md 地图域例外条款）。
QgsLineSymbol *surveyBoundaryLineSymbol();

} // namespace GeoPatterns
