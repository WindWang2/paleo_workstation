// 层：视图
#pragma once
#include <QWidget>
#include <QString>
#include <QVariant>
#include <QVector>
#include <functional>
#include <tuple>

class QgsAttributeTableFilterModel;
class QgsAttributeTableModel;
class QgsMapCanvas;
class QgsVectorLayer;
class QgsVectorLayerCache;
class QgisEditingService;

// ui/ — AttributeTablePanel: QGIS-native attribute table (QgsAttributeTableView
// over QgsVectorLayerCache + QgsAttributeTableModel + filter model) for any
// instantiated project layer. Layer resolution is injected as a provider so
// the panel never reaches into the layer service itself.
//
// 编辑管线（mapping 主线4）：面板自带显式会话入口（编辑/保存/放弃按钮 +
// beginEditing/saveEditing/cancelEditing API）。会话优先经注入的
// QgisEditingService 走 beginEdit/commitEdit/rollbackEdit——busy 标记
//（markLayerBusy）与释放（markLayerFree）时机由服务统一落，与
// MapVersionController::saveVersion 的提交语义一致（saveVersion 也走
// commitEdit）；无服务时直连 startEditing/commitChanges/rollBack（无 busy
// 标记，与编辑工具条的无服务降级一致）。单元格值改动经原生
// QgsAttributeTableModel 落进图层 edit command/undo 栈；面板从不绕过
// edit buffer 直写 provider。
class AttributeTablePanel : public QWidget
{
  Q_OBJECT
  public:
    // layerProvider: layerId -> instantiated QgsVectorLayer (or nullptr).
    // canvas is required by the filter model for extent filtering.
    AttributeTablePanel(QgsMapCanvas *canvas,
                        std::function<QgsVectorLayer *(const QString &)> layerProvider,
                        QWidget *parent = nullptr);

    void setLayerIds(const QStringList &ids);  // populate the layer picker
    void showLayer(const QString &layerId);    // select + load its table
    QString currentLayerId() const;

    // 会话注入（可选）：null → 直连降级。接线是壳面动作（见
    // docs/progress/mapping.md seam 表）。
    void setEditingService(QgisEditingService *service);

    bool isEditing() const;

    // 方向 52 B 线：增量更新通道（dataChanged 驱动，保持选区与滚动，零 reset）
    bool updateCell(int row, int column, const QVariant &value);
    int updateCells(const QVector<std::tuple<int, int, QVariant>> &cellUpdates);
    QgsAttributeTableModel *attributeModel() const { return m_model; }
    QgsAttributeTableFilterModel *filterModel() const { return m_filter; }

  public slots:
    // 显式会话入口（与按钮同路径）。当前层未解析/不可写 → 拒绝并 editRefused。
    bool beginEditing();
    bool saveEditing();
    bool cancelEditing();

  signals:
    void editingStarted(const QString &layerId);
    void editingStopped(const QString &layerId, bool saved);
    void editRefused(const QString &reason);

  private:
    QgsVectorLayer *currentLayer() const;
    void clearTable();
    void buildEditRow();
    void updateEditRowStates();

    QgsMapCanvas *m_canvas;
    std::function<QgsVectorLayer *(const QString &)> m_layerProvider;
    QgisEditingService *m_editingService = nullptr; // not owned, optional
    QgsVectorLayerCache *m_cache = nullptr;
    QgsAttributeTableModel *m_model = nullptr;
    QgsAttributeTableFilterModel *m_filter = nullptr;
};
