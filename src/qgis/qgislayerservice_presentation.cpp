// 层：QGIS 封装
#include "qgislayerservice.h"
#include "qgisprojectservice.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <qgsmaplayer.h>
#include <qgsmaplayerstyle.h>
#include <qgsproject.h>

namespace {
QString digest(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString markerKey(const QString &id) {return QStringLiteral("paleo/native-presentation/")+digest(id.toUtf8());}
QJsonObject marker(QgsProject *project,const QString &id) {
  const auto value=project->readEntry("Paleo",markerKey(id));
  if(value.size()>32768)return {};
  return QJsonDocument::fromJson(value.toUtf8()).object();
}
}
QString QgisLayerService::nativePresentationToken(const QString &id) const {
  auto *native=layer(id);if(!native)return {};
  if(native->name().size()>4096 || native->source().size()>8192)return QStringLiteral("!unavailable");
  QgsMapLayerStyle style;style.readFromLayer(native);const auto xml=style.xmlData();
  const auto crs=native->crs().toWkt();
  if(xml.size()>65536 || crs.size()>16384)return QStringLiteral("!unavailable");
  return digest(QJsonDocument(QJsonObject{{"name",native->name()},{"source",native->source()},
    {"crs",crs},{"style",xml}}).toJson(QJsonDocument::Compact));
}
bool QgisLayerService::nativePresentationMatches(const QString &id,const QString &expected,const QString &ownSource) const {
  const auto current=nativePresentationToken(id);
  if(current==QLatin1String("!unavailable"))return false;
  if(current.isEmpty() || current==expected)return true;
  auto *project=m_projectSvc?m_projectSvc->project():nullptr;
  if(!project || ownSource.isEmpty())return false;
  const auto baseline=marker(project,id);
  return baseline.value("source").toString()==ownSource && baseline.value("token").toString()==current;
}
bool QgisLayerService::rememberPresentationState(const QString &id,const QString &source) {
  auto *project=m_projectSvc?m_projectSvc->project():nullptr;if(!project)return false;
  auto baseline=marker(project,id);
  if(baseline.value("source").toString()==source && !baseline.value("token").toString().isEmpty())return true;
  const auto token=nativePresentationToken(id);if(token==QLatin1String("!unavailable"))return false;
  // Keep the baseline outside layer QML so it cannot hash itself. QgsProject
  // persists it together with the native layer; human style changes leave it intact.
  baseline.insert("source",source);baseline.insert("token",token);
  return project->writeEntry("Paleo",markerKey(id),QString::fromUtf8(QJsonDocument(baseline).toJson(QJsonDocument::Compact)));
}
bool QgisLayerService::beginPresentationState(const QString &id,const QString &source,bool needsInstance) {
  auto *project=m_projectSvc?m_projectSvc->project():nullptr;if(!project)return false;
  const auto baseline=marker(project,id);if(baseline.value("source").toString()==source)return true;
  return project->writeEntry("Paleo",markerKey(id),QString::fromUtf8(QJsonDocument(QJsonObject{
    {"source",source},{"token",QString()},{"needsInstance",needsInstance}}).toJson(QJsonDocument::Compact)));
}
bool QgisLayerService::presentationNeedsInstance(const QString &id,const QString &source) const {
  auto *project=m_projectSvc?m_projectSvc->project():nullptr;if(!project)return false;
  const auto baseline=marker(project,id);
  return baseline.value("source").toString()==source && baseline.value("needsInstance").toBool();
}
bool QgisLayerService::nativePresentationSourceMatches(const QString &id,const QString &source) const {
  auto *native=layer(id);return native?native->source()==source:!presentationNeedsInstance(id,source);
}
void QgisLayerService::initializePresentationState(const QString &id) {
  auto *project=m_projectSvc?m_projectSvc->project():nullptr;auto *native=layer(id);
  if(!project || !native)return;
  auto baseline=marker(project,id);
  if(baseline.value("source").toString()!=native->source())return;
  if(baseline.value("token").toString().isEmpty()) {
    rememberPresentationState(id,native->source());baseline=marker(project,id);
  }
  // This flag records an unfinished replacement, not a requirement to keep a
  // successfully presented layer instantiated after the active horizon changes.
  if(project->mapLayer(native->id())==native && baseline.value("needsInstance").toBool()) {
    baseline.insert("needsInstance",false);
    project->writeEntry("Paleo",markerKey(id),QString::fromUtf8(QJsonDocument(baseline).toJson(QJsonDocument::Compact)));
  }
}
