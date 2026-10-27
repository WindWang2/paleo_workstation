// 层：QGIS 封装
#pragma once

#include <QObject>
#include <QPointer>
#include <qgspointxy.h>

class QgsMapCanvas;
class QgsMapTool;
class PaleoNativeCoordinateMapTool;

class PaleoCoordinateMapTool : public QObject
{
  Q_OBJECT
public:
  explicit PaleoCoordinateMapTool(QgsMapCanvas *canvas);

  QgsMapTool *nativeTool() const;
  operator QgsMapTool *() const { return nativeTool(); }
  static PaleoCoordinateMapTool *fromMapTool(QgsMapTool *tool);

signals:
  void pointPicked(const QgsPointXY &point);
  void cancelled();

private:
  QPointer<PaleoNativeCoordinateMapTool> m_native;
};
