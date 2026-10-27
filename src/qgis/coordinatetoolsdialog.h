// 层：QGIS 封装
#pragma once

#include "coordinateformat.h"
#include <QObject>
#include <QPointer>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class PaleoCoordinateEditor;
class PaleoCrsSelector;
class QgsCoordinateOperationWidget;
class QgsMapCanvas;

class QDialog;

class PaleoCoordinateToolsDialog : public QObject
{
  Q_OBJECT
public:
  explicit PaleoCoordinateToolsDialog(QgsMapCanvas *canvas, QWidget *parent = nullptr);
  ~PaleoCoordinateToolsDialog() override;

  PaleoCoordinateEditor *editor() const { return m_editor; }
  PaleoCrsSelector *targetSelector() const { return m_target; }
  QPushButton *convertButton() const { return m_convert; }
  void setSourcePoint(const QgsPointXY &point);
  bool convertedPoint(QgsPointXY *point) const;
  QString resultText() const;
  QDialog *dialog() const;

public slots:
  void convert();
  bool locateResult();

signals:
  void mapCenterChanged();

private:
  void updateOperation();
  void swapCrs();
  void copyResult();

  QPointer<QDialog> m_dialog;
  QgsMapCanvas *m_canvas = nullptr;
  PaleoCoordinateEditor *m_editor = nullptr;
  PaleoCrsSelector *m_target = nullptr;
  QgsCoordinateOperationWidget *m_operation = nullptr;
  QToolButton *m_operationToggle = nullptr;
  QPushButton *m_convert = nullptr;
  QPushButton *m_swap = nullptr;
  QPushButton *m_copy = nullptr;
  QPushButton *m_locate = nullptr;
  QLabel *m_error = nullptr;
  QLabel *m_resultCaption = nullptr;
  QLineEdit *m_result = nullptr;
  QgsPointXY m_resultPoint;
  bool m_hasResult = false;
};
