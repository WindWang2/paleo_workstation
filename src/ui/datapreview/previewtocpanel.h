// 层：视图
#pragma once

#include <QWidget>

class QLabel;

#include "../../qgis/previewrasteranalysis.h"

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QStackedWidget;
class QgsMapLayer;
class QgsRasterLayer;
class QgsVectorLayer;

// ui/datapreview/previewtocpanel — 预览迷你 TOC 与符号快调（P2 D4.x）。
// 图层列表（可见性开关 + 拖拽排序，预览层序独立）+ 每层透明度/混合模式 +
// 栅格符号快调（色带/拉伸/反转，即时应用）+ 矢量符号快调（分类字段/单色）
// + 图层属性速览（源路径/类型/范围/单元大小）+ 图层移除（不动 catalog）+
// 图例区（D2.4，相图分类图例）。TOC 状态经 PreviewStateMemory 随资产记忆
//（D4.7）。
class PreviewTocPanel : public QWidget
{
    Q_OBJECT
  public:
    explicit PreviewTocPanel( QWidget *parent = nullptr );

    // assetKey 非空时启用状态记忆（层名键）。
    void setAssetKey( const QString &assetKey );
    QString assetKey() const { return m_assetKey; }

    // 层注册（name 为显示名；tooltip = 源路径）。重复 add 同一层为幂等。
    // defaultVisible 是首次加入的可见位；同资产重开时按 D4.7 记忆恢复。
    void addLayer( QgsMapLayer *layer, const QString &name, const QString &sourcePath,
                   bool defaultVisible = true );
    void removeLayer( QgsMapLayer *layer );
    void clear();
    int layerCount() const;
    QList<QgsMapLayer *> layersTopToBottom() const;
    QgsMapLayer *currentLayer() const;
    // 宿主（画布侧）可见性变化回写 UI（不加发信号，防回环）。
    void setLayerVisibleInUi( QgsMapLayer *layer, bool visible );

    // D2.4 图例（分类渲染的 category 列表）。
    struct LegendEntry
    {
      QString name;
      QColor color;
    };
    void setLegendEntries( const QVector<LegendEntry> &entries );
    int legendEntryCount() const { return m_legendEntries.size(); }

    // D4.4 分类样式回调：分支持有相色逻辑（faciesColor），面板只收集参数。
    // categorized=false → 单色（color 有效）；true → 分类（field 有效）。
    std::function<void( QgsVectorLayer *, const QString &field, bool categorized,
                        const QColor &color )>
        vectorStyleApplier;

    // 状态记忆落盘时机：显式 flush（页关闭/切换时宿主调用）+ 每次变化即存。
    void saveMemory() const;

  signals:
    void layerVisibilityChanged( QgsMapLayer *layer, bool visible );
    void layerOrderChanged( const QList<QgsMapLayer *> &layersTopToBottom );
    void layerRemoveRequested( QgsMapLayer *layer );
    void rasterStyleChanged( QgsRasterLayer *layer );
    void layerOpacityChanged( QgsMapLayer *layer, double opacity );
    void layerBlendChanged( QgsMapLayer *layer, int blendMode );
    void attributeTableRequested( QgsVectorLayer *layer ); // D7.3 全表入口

  private:
    struct RowInfo
    {
      QgsMapLayer *layer = nullptr;
      QString name;
      QString sourcePath;
      bool visible = true;
    };
    void rebuildList();
    void rebuildQuickPanel();
    void applyRasterStyle( QgsRasterLayer *rl );
    void applyVectorStyle( QgsVectorLayer *vl );

    QString m_assetKey;
    QList<RowInfo> m_rows; // index 0 = 顶
    QVector<LegendEntry> m_legendEntries;

    QListWidget *m_list = nullptr;
    QLabel *m_listEmptyLabel = nullptr; // goal/ui-experience-polish：TOC 空态指引（PaleoEmptyStateLabel）
    QStackedWidget *m_quickPanel = nullptr;
    QLabel *m_propsLabel = nullptr;
    QWidget *m_legendBox = nullptr;
    QLabel *m_legendContent = nullptr;

    // 栅格快调控件（当前层切换时回填）
    QComboBox *m_rampCombo = nullptr;
    QComboBox *m_stretchCombo = nullptr;
    QCheckBox *m_invertCheck = nullptr;
    QDoubleSpinBox *m_minSpin = nullptr;
    QDoubleSpinBox *m_maxSpin = nullptr;
    QSlider *m_opacitySlider = nullptr;
    QComboBox *m_blendCombo = nullptr;
    // 矢量快调控件
    QComboBox *m_fieldCombo = nullptr;
    QCheckBox *m_categorizedCheck = nullptr;
    QPushButton *m_colorBtn = nullptr;
    QPushButton *m_attrTableBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    bool m_suppressSignals = false;
};
