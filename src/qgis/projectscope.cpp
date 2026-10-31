// 层：QGIS 封装
#include "projectscope.h"

#include <QCryptographicHash>
#include <qgsproject.h>

QString paleoProjectScopeKey(QgsProject *project)
{
  if (!project)
    return {};
  const QString file = project->fileName();
  if (file.isEmpty())
    return {};
  return QString::fromLatin1(
      QCryptographicHash::hash(file.toUtf8(), QCryptographicHash::Sha1).toHex());
}
