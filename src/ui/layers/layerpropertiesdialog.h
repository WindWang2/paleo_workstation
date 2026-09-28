// 层：视图
#pragma once

#include <QObject>
#include <QString>

class QgsMapCanvas;
class QgsMessageBar;
class QgisLayerService;
class QWidget;

// 图层属性入口：openLayerProperties(layerId) 经 LayerResolver
//（QgisLayerService::layer）解析 QgsMapLayer*，按类型分发
// QgsVectorLayerProperties / QgsRasterLayerProperties 原生壳，Paleo 业务页
//（layerId/所属 group/层位 horizon/关联 catalog assetId/资产名/来源
// provenance/创建时间——只读展示）挂进同一对话框。
//
// 属性改动落 QgsMapLayer 对象（QGIS 持久化进 .qgz）；Paleo 侧只读展示
// manifest/catalog 业务字段，不复制路径/CRS/provider。
//
// 样式管理（QgsMapLayerStyleManager）：「保存当前样式为预设」「从预设恢复」
//「导入/导出 .qml」——预设名随 manifest styleRef 约定。
class LayerPropertiesDialog : public QObject
{
  Q_OBJECT

  public:
    struct Deps
    {
        QgsMapCanvas *canvas = nullptr;
        QgsMessageBar *messageBar = nullptr; // 缺省时自造一个实例持有
    };

    LayerPropertiesDialog(QgisLayerService *layerService, const Deps &deps,
                          QObject *parent = nullptr);
    ~LayerPropertiesDialog() override;

    // Paleo 业务页（只读字段 + 「在数据页查看资产」按钮）；offscreen 测试
    // 通道，openLayerProperties 内部复用。未知 layerId → nullptr。
    QWidget *createBusinessPage(const QString &layerId, QWidget *parent = nullptr);

  public slots:
    // 按图层类型分发原生属性对话框；offscreen（platformName=="offscreen"）
    // 下只构造不 exec（QTest 可验证三类图层行为）。
    void openLayerProperties(const QString &layerId);

  signals:
    // 「在数据页查看资产」跳转意图（assetId 为空 = 未关联，按钮禁用）
    void assetInspectionRequested(const QString &assetId);
    // 原生对话框接受（OK）后转发（offscreen 构造路径不发射）
    void layerPropertiesApplied(const QString &layerId);

  private:
    QgisLayerService *m_layerService = nullptr;
    QgsMapCanvas *m_canvas = nullptr;
    QgsMessageBar *m_messageBar = nullptr; // 注入缺省时自造持有
    bool m_ownMessageBar = false;
};
