#pragma once
#include <QObject>
#include <QString>

class QgsMapLayer;

// qgis/ — QgisStyleService applies named styles to layers.
// Style refs resolve to .qml files under vendor share or project styles/ dir.
class QgisStyleService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisStyleService(QObject *parent = nullptr);

    void setStylesRoot(const QString &dir);            // styles/<ref>.qml
    bool applyStyle(QgsMapLayer *layer, const QString &styleRef, QString *error = nullptr);
    QStringList availableStyles() const;               // basenames under stylesRoot

  private:
    QString m_stylesRoot;
};
