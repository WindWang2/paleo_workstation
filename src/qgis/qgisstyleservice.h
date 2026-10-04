// 层：QGIS 封装
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QgsMapLayer;
class QgsVectorLayer;
class QgsMarkerSymbol;
class QgsProject;

// qgis/ — QgisStyleService applies named styles to layers.
// Style refs resolve to .qml files under vendor share or project styles/ dir.
class QgisStyleService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisStyleService(QObject *parent = nullptr);

    void setStylesRoot(const QString &dir);            // styles/<ref>.qml
    bool applyStyle(QgsMapLayer *layer, const QString &styleRef, QString *error = nullptr);
    QStringList availableStyles() const;               // basenames under stylesRoot

    static void applyWellLayerStyle(class QgsVectorLayer *layer);
    // goal/well-trajectory：井底位移轨迹线（DESIGN.md text 墨色实线——
    // 实测轨迹不是不确定面，不用虚线）。
    static void applyTrajectoryLayerStyle(class QgsVectorLayer *layer);

    // 测区范围/成图边界面：空心填 + text 墨色描边——置顶共享层上不遮
    // 盖因素图与井点（地图域样式，DESIGN.md 例外条款）。
    static void applyBoundaryLayerStyle(class QgsVectorLayer *layer);

    // ---- 方向 34：井网辅助图层符号 ----------------------------------------
    // 计划井点：空心橙方框（非实井——与实井实心圆点在图上可分），name 标注。
    static void applyPlannedWellLayerStyle(class QgsVectorLayer *layer);
    // 井网覆盖空洞面：警示橙半透明填 + 橙虚线描边（registration 临时层
    // 同调性；不遮挡下层井位/因素图）。
    static void applyHoleLayerStyle(class QgsVectorLayer *layer);

    // 约束线图层按 type 字段语义分类渲染：方向线红实线、打断线墨实线、
    // 解释软边界橙虚线、等值线停线灰虚线、制图绕行蓝点划线；未标/旧
    // type=line 走常规细墨线。字段缺失 → 无操作。
    static void applyConstraintLayerStyle(class QgsVectorLayer *layer);

    // 等值线图层：细灰线 + ELEV 小字号沿线标注（标注随图层 z 序补丁下
    // 标注只压在同层等值线上）。字段缺失 → 只换线型不标注。
    static void applyContourLayerStyle(class QgsVectorLayer *layer);

    // ---- C2（wave/deepen-perf）：相界地质语义符号 --------------------------
    // 相多边形图层按 boundary_kind 分类描边（四类全开——方向 39）：断层切割
    //（fault_cut）= 断层红粗描边（Q/HS 1011—2016 断层线用色，同
    // resources/geology/faults 调性）；整合接触 = 细实线、尖灭 = 虚线、
    // 相变 = 点线 + 可选渐变带（带宽 data-defined 绑 transition_width，
    // 图层地图单位）；其余/未分类/表外值 = 常规细灰边（中性，不猜类）。
    // 字段缺失 → 无操作（保持现状渲染器）。
    static void applyFaciesBoundaryStyle(QgsVectorLayer *layer);

    // ---- C3（wave/deepen-perf）：井类别符号（Q/HS 1011—2016 表 K.1）------
    // 数据字段驱动的井别符号全集映射：categoryField 有值域时按 16 类井别
    //（表 K.1 十二类探井 + 方向 31 开发区块四类：生产油/气井、注水井、
    // 采水井）分/renderer 分类渲染（wellCategoryDefinitions() 给词面/符号
    // 描述）；字段缺失或空 → 回落通用「探井」符号（外细环+实心盘，盘≈
    // 0.73 外径，与测区全景 styleSurveyWellLayer 同图式）。类别词面归一化
    // 见 normalizeWellCategory()（中文词/英文 id 同收）。符号统一挂比例尺
    // 缩放（@map_scale 数据定义尺寸：1:25万 6mm → 1:250万 3mm）。
    static void applyWellCategoryStyle(QgsVectorLayer *layer, const QString &categoryField);

    // ---- 方向35：演化迁移矢量符号 ------------------------------------------
    // 迁移矢量图层（evolution.vectors.<层位>）的箭头场渲染：进积蓝、退积红
    //（地图域数据符号——物源/展布线同调性的既有红蓝惯例，不属 UI token），
    // 前缘采样矢量细、质心汇总矢量粗。字段 vector_kind（front/centroid）与
    // advance（1 进 / 0 退）缺失 → 单一灰箭头（不接管语义分色）。
    static void applyMigrationVectorStyle(QgsVectorLayer *layer);
    // 类别词表（存储 id + 显示名 + 符号构成说明），固定顺序 = 表 K.1 序。
    static QVariantList wellCategoryDefinitions();
    // 常见类别写法 → 规范存储 id（未知 → 原样返回；空 → 空）。
    static QString normalizeWellCategory(const QString &raw);

    // ---- 方向 31（geological-symbols）：花纹/断层线型语义入口 --------------
    // 词表单点在 GeoPatterns（geopatterns.h）；样式入口统一收本类。资源
    // 走 qrc（/geology 前缀），不落机器绝对路径；字段缺失一律不接管渲染器。

    // 岩性花纹：lithologyField 值归一化（normalizeLithology）→ SVG 平铺分类
    // 渲染（未命中值落纯色兜底桶）；Polygon 专属；缺字段保持现状渲染器。
    static void applyLithologyPatternStyle(QgsVectorLayer *layer,
                                           const QString &lithologyField);
    // 相单元花纹：字段值 → 相花纹（河流/三角洲/湖泊/滨海/洪积/沙漠/冰川）
    // 分类渲染；同岩性入口口径（尺寸基准/兜底另立）。
    static void applyFaciesPatternStyle(QgsVectorLayer *layer,
                                        const QString &faciesField);
    // 断层线型：kindField 值（normal/reverse/strike/inferred 及中文词面）
    // → 断层红实线挂齿/推测虚线分类渲染；Line 专属；缺字段不接管。
    static void applyFaultLineLayerStyle(QgsVectorLayer *layer,
                                         const QString &kindField);

    // ---- 方向 31 批 5：样式版本语义（符号覆盖随工程持久化）--------------
    // 图层级覆盖记 customProperty paleo/symbolSemantics = JSON
    // {family: lithology|facies|line, id, version}；family/id 锚 GeoPatterns
    // 词表（跨版本稳定 id），version = 词表版本锚（资源语义大改时递增，
    // 老工程读老版本不静默换图式）。SVG 资源走 qrc（不落机器绝对路径），
    // renderer 由 .qgs/.qgz 原生保存，customProperty 供语义重放/审计。

    // 挂单符号语义覆盖（选择器 patternPicked 的落点）并写版本锚；几何
    // 类型不匹配/词表未命中 → 不动渲染器、不写属性（返回 false）。
    static bool applySymbolOverride(QgsVectorLayer *layer, const QString &family,
                                    const QString &patternId);
    // 工程重开后按 customProperty 重放覆盖（幂等；无属性/词表已失配 →
    // 返回 false 并保持 .qgs 原样 renderer）。
    static bool restoreSymbolOverride(QgsVectorLayer *layer);
    // 全工程重放（打开工程后调一次）。
    static void restoreAllSymbolOverrides(QgsProject *project);
    // 词表版本锚（resources/geology 语义表大改时递增）。
    static int symbolTableVersion();

  private:
    QString m_stylesRoot;
};
