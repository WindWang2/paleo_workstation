// 层：QGIS 封装
#include "qgisstyleservice.h"

#include <QDir>
#include <QFileInfo>

#include <qgsmaplayer.h>

namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }
} // namespace

// qgis/ — QgisStyleService applies named styles to layers.
// Style refs resolve to .qml files under vendor share or project styles/ dir.

QgisStyleService::QgisStyleService(QObject *parent)
  : QObject(parent)
{
}

void QgisStyleService::setStylesRoot(const QString &dir)
{
  m_stylesRoot = dir;
}

bool QgisStyleService::applyStyle(QgsMapLayer *layer, const QString &styleRef, QString *error)
{
  if (!layer)
  {
    setError(error, tr("cannot apply a style to a null layer"));
    return false;
  }
  if (m_stylesRoot.isEmpty())
  {
    setError(error, tr("no styles root configured — call setStylesRoot() first"));
    return false;
  }
  if (styleRef.isEmpty())
  {
    setError(error, tr("cannot apply an empty style reference"));
    return false;
  }

  // styles/<ref>.qml — tolerate a ref that already carries the suffix
  const QString fileName = styleRef.endsWith(QStringLiteral(".qml"), Qt::CaseInsensitive)
    ? styleRef : styleRef + QStringLiteral(".qml");
  const QString path = QDir(m_stylesRoot).filePath(fileName);
  if (!QFileInfo::exists(path))
  {
    setError(error, tr("style '%1' not found at %2").arg(styleRef, path));
    return false;
  }

  bool resultFlag = false;
  const QString status = layer->loadNamedStyle(path, resultFlag);
  if (!resultFlag)
  {
    setError(error, tr("loadNamedStyle('%1') failed: %2")
                      .arg(path, status.isEmpty() ? tr("unknown error") : status));
    return false;
  }
  return true;
}

QStringList QgisStyleService::availableStyles() const
{
  QStringList refs;
  if (m_stylesRoot.isEmpty())
    return refs;

  const QDir dir(m_stylesRoot);
  const QStringList files = dir.entryList({QStringLiteral("*.qml")}, QDir::Files, QDir::Name);
  refs.reserve(files.size());
  for (const QString &f : files)
    refs << QFileInfo(f).completeBaseName(); // basename without .qml
  return refs;
}
