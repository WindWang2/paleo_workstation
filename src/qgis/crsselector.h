// 层：QGIS 封装
#pragma once

#include <QObject>
#include <QPointer>
#include <qgscoordinatereferencesystem.h>

class QgsProjectionSelectionWidget;

namespace paleo::coordinates
{
QgsCoordinateReferenceSystem crsFromDefinition(const QString &definition);
QString crsDefinition(const QgsCoordinateReferenceSystem &crs);
}

class QWidget;

class PaleoCrsSelector : public QObject
{
  Q_OBJECT
public:
  explicit PaleoCrsSelector(bool allowEmpty = false, QWidget *parent = nullptr);
  ~PaleoCrsSelector() override;

  QgsCoordinateReferenceSystem crs() const;
  QString definition() const;
  bool setDefinition(const QString &definition);
  QString text() const { return definition(); }
  void setText(const QString &definition) { setDefinition(definition); }
  void clear();
  void setNotSetText(const QString &text);
  bool validate(QString *error, bool metricProject = false) const;
  QgsProjectionSelectionWidget *nativeWidget() const { return m_native; }
  QWidget *widget() const;

signals:
  void definitionChanged(const QString &definition);

private:
  QPointer<QWidget> m_widget;
  QgsProjectionSelectionWidget *m_native = nullptr;
  bool m_allowEmpty = false;
  bool m_restoring = false;
  QString m_invalidDefinition;
};
