// 层：视图
#pragma once
#include <QWidget>
#include <QString>
#include <QVariant>
#include <QVector>
#include <QPointer>
#include <functional>
#include <tuple>

class QgsAttributeTableFilterModel;
class QgsAttributeTableModel;
class QgsMapCanvas;
class QgsVectorLayer;
class QgsAttributeTableDialog;
class PaleoAttributeTableHost;
class QgisEditingService;

// Layer selection and application edit-service injection around the original
// QGIS attribute table dialog. QGIS owns the dual view, filters, field tools,
// selection, form validation and edit-buffer updates.
class AttributeTablePanel : public QWidget
{
  Q_OBJECT
  public:
    // layerProvider: layerId -> instantiated QgsVectorLayer (or nullptr).
    // canvas is required by the filter model for extent filtering.
    AttributeTablePanel(QgsMapCanvas *canvas,
                        std::function<QgsVectorLayer *(const QString &)> layerProvider,
                        QWidget *parent = nullptr);
    ~AttributeTablePanel() override;

    // populate the layer picker；names 与 ids 同序，显示图层列表里的图层名
    //（缺省/空名退回 id）。
    void setLayerIds(const QStringList &ids, const QStringList &names = {});
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
    // 兼容调用入口；界面编辑动作由 QGIS 原版工具栏提供。
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
    void updateEditRowStates();

    QgsMapCanvas *m_canvas;
    std::function<QgsVectorLayer *(const QString &)> m_layerProvider;
    QgisEditingService *m_editingService = nullptr; // not owned, optional
    QPointer<QgsVectorLayer> m_currentLayer;
    QString m_currentLayerId;
    PaleoAttributeTableHost *m_host = nullptr;
    QWidget *m_tableHost = nullptr;
    QgsAttributeTableDialog *m_nativeDialog = nullptr;
    QMetaObject::Connection m_layerDestroyed;
    QgsAttributeTableModel *m_model = nullptr;
    QgsAttributeTableFilterModel *m_filter = nullptr;
};
