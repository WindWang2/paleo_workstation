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

    // ---- C2（wave/deepen-perf）：相界地质语义符号 --------------------------
    // 相多边形图层按 boundary_kind 分类描边：断层切割（fault_cut）= 断层红
    // 粗描边（Q/HS 1011—2016 断层线用色，同 resources/geology/faults 调性），
    // 其余/未分类 = 常规细灰边。字段缺失 → 无操作（保持现状渲染器）。
    // 单类型首发：仅 fault_cut 类目；其余类型按 BoundarySemantics 词表扩展。
    static void applyFaciesBoundaryStyle(QgsVectorLayer *layer);

    // ---- C3（wave/deepen-perf）：井类别符号（Q/HS 1011—2016 表 K.1）------
    // 数据字段驱动的探井符号全集映射：categoryField 有值域时按 12 类探井
    // 分/renderer 分类渲染（wellCategoryDefinitions() 给词面/符号描述）；
    // 字段缺失或空 → 回落通用「探井」符号（外细环+实心盘，盘≈0.73 外径，
    // 与测区全景 styleSurveyWellLayer 同图式）。类别词面归一化见
    // normalizeWellCategory()（中文词/英文 id 同收）。
    static void applyWellCategoryStyle(QgsVectorLayer *layer, const QString &categoryField);
    // 类别词表（存储 id + 显示名 + 符号构成说明），固定顺序 = 表 K.1 序。
    static QVariantList wellCategoryDefinitions();
    // 常见类别写法 → 规范存储 id（未知 → 原样返回；空 → 空）。
    static QString normalizeWellCategory(const QString &raw);

  private:
    QString m_stylesRoot;
};
