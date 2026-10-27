// 层：QGIS 封装
#pragma once

#include "coordinateformat.h"
#include <QObject>
#include <QPointF>
#include <QPointer>

class QgisProjectService;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;
class PaleoCrsSelector;
class QgsMapCanvas;

class QWidget;

class PaleoCoordinateEditor : public QObject
{
  Q_OBJECT
public:
  explicit PaleoCoordinateEditor(QWidget *parent = nullptr);
  ~PaleoCoordinateEditor() override;

  QLineEdit *xEdit() const { return m_x; }
  QLineEdit *yEdit() const { return m_y; }
  QToolButton *mapPickButton() const { return m_mapPick; }
  PaleoCrsSelector *sourceSelector() const { return m_source; }
  void setSourceCrs(const QgsCoordinateReferenceSystem &crs);
  void setTargetCrs(const QgsCoordinateReferenceSystem &crs,
                    const QgsCoordinateTransformContext &context = {});
  void setProjectStorage(QgisProjectService *project);
  QString targetCrsDefinition() const;
  void setMapCanvas(QWidget *canvas);
  void setPoint(const QgsPointXY &point);
  void setPoint(const QPointF &point);
  bool sourcePoint(QgsPointXY *point, QString *error = nullptr) const;
  bool targetPoint(QgsPointXY *point, QString *error = nullptr) const;
  bool targetPoint(QPointF *point, QString *error = nullptr) const;
  bool showMapPoint(const QgsPointXY &point);
  bool showMapPoint(double x, double y);
  bool validatePoint();
  void showError(const QString &error);
  paleo::coordinates::OrdinateFormat format() const;
  QWidget *widget() const;

signals:
  void pointChanged();
  void mapPickRequested(PaleoCoordinateEditor *editor);

private:
  void updateFormat();
  QPointer<QWidget> m_widget;
  QLineEdit *m_x = nullptr;
  QLineEdit *m_y = nullptr;
  QLabel *m_xCaption = nullptr;
  QLabel *m_yCaption = nullptr;
  QLabel *m_error = nullptr;
  QComboBox *m_format = nullptr;
  QToolButton *m_mapPick = nullptr;
  PaleoCrsSelector *m_source = nullptr;
  QgsMapCanvas *m_canvas = nullptr;
  QgsCoordinateReferenceSystem m_targetCrs;
  QgsCoordinateTransformContext m_context;
};
