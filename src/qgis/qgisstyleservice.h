// 层：QGIS 封装
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QgsMapLayer;
class QgsVectorLayer;
class QgsMarkerSymbol;

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

    // 约束线图层按 type 字段语义分类渲染：方向线红实线、打断线墨实线、
    // 解释软边界橙虚线、等值线停线灰虚线、制图绕行蓝点划线；未标/旧
    // type=line 走常规细墨线。字段缺失 → 无操作。
    static void applyConstraintLayerStyle(class QgsVectorLayer *layer);

    // 等值线图层：细灰线 + ELEV 小字号沿线标注（标注随图层 z 序补丁下
    // 标注只压在同层等值线上）。字段缺失 → 只换线型不标注。
    static void applyContourLayerStyle(class QgsVectorLayer *layer);

    // ---- C2（wave/deepen-perf）：相界地质语义符号 --------------------------
    // 相多边形图层按 boundary_kind 分类描边：断层切割（fault_cut）= 断层红
    // 粗描边（Q/HS 1011—2016 断层线用色，同 resources/geology/faults 调性），
    // 其余/未分类 = 常规细灰边。字段缺失 → 无操作（保持现状渲染器）。
    // 单类型首发：仅 fault_cut 类目；其余类型按 BoundarySemantics 词表扩展。
    static void applyFaciesBoundaryStyle(QgsVectorLayer *layer);

    // ---- C3（wave/deepen-perf）：井类别符号（Q/HS 1011—2016 表 K.1）------
    // 数据字段驱动的探井符号全集映射：categoryField 有值域时按 12 类探井分
    ///renderer 分类渲染（wellCategoryDefinitions() 给词面/符号描述）；
    // 字段缺失或空 → 回落通用「探井」符号（外细环+实心盘，盘≈0.73 外径，
    // 与测区全景 styleSurveyWellLayer 同图式）。类别词面归一化见
    // normalizeWellCategory()（中文词/英文 id 同收）。
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

  private:
    QString m_stylesRoot;
};
