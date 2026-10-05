// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <memory>
class QgsMapCanvas;
class QgsRubberBand;
class QgsPointXY;
// GUI-thread canvas coordination: extent/CRS and cursor, with reentry guards.
class MapCanvasLink : public QObject {
  Q_OBJECT
public:
  MapCanvasLink(QgsMapCanvas *main, QgsMapCanvas *reference,
                QObject *parent = nullptr);
  ~MapCanvasLink() override;
  void setEnabled(bool enabled);
  bool isEnabled() const { return m_enabled; }

private:
  void sync(QgsMapCanvas *source, QgsMapCanvas *target);
  void cursor(QgsMapCanvas *source, QgsMapCanvas *target,
              const QgsPointXY &point);
  QPointer<QgsMapCanvas> m_main, m_reference;
  QPointer<QgsRubberBand> m_mainMarker, m_referenceMarker;
  bool m_enabled = true, m_syncing = false;
};
