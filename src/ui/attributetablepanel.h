// 层：视图
#pragma once
#include <QWidget>
#include <QString>
#include <functional>

class QgsAttributeTableFilterModel;
class QgsAttributeTableModel;
class QgsMapCanvas;
class QgsVectorLayer;
class QgsVectorLayerCache;

// ui/ — AttributeTablePanel: QGIS-native attribute table (QgsAttributeTableView
// over QgsVectorLayerCache + QgsAttributeTableModel + filter model) for any
// instantiated project layer. Layer resolution is injected as a provider so
// the panel never reaches into the layer service itself.
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

  private:
    void clearTable();

    QgsMapCanvas *m_canvas;
    std::function<QgsVectorLayer *(const QString &)> m_layerProvider;
    QgsVectorLayerCache *m_cache = nullptr;
    QgsAttributeTableModel *m_model = nullptr;
    QgsAttributeTableFilterModel *m_filter = nullptr;
};
