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
  // 工具是否仍是画布当前 mapTool（未完成拾取、未被别的工具顶掉）——
  // 顶替方据此决定是否补发 cancelled；已完成的工具 native 已自清必为假。
  bool isActiveOnCanvas() const;

signals:
  void pointPicked(const QgsPointXY &point);
  void cancelled();

private:
  QPointer<PaleoNativeCoordinateMapTool> m_native;
};
