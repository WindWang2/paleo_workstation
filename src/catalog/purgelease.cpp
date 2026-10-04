// 层：数据
#include "purgelease.h"
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
namespace {
QMutex mutex;
QSet<QString> active;
QString key(const QString &path) { return QDir::cleanPath(QDir::current().absoluteFilePath(path)); }
}
std::shared_ptr<CatalogPurgeLease> CatalogPurgeLease::acquire(const QString &path) {
  QMutexLocker lock(&mutex);
  const QString canonical = key(path);
  if (path.isEmpty() || active.contains(canonical)) return {};
  active.insert(canonical);
  return std::shared_ptr<CatalogPurgeLease>(new CatalogPurgeLease(canonical));
}
bool CatalogPurgeLease::isHeld(const QString &path) {
  QMutexLocker lock(&mutex); return active.contains(key(path));
}
CatalogPurgeLease::~CatalogPurgeLease() { QMutexLocker lock(&mutex); active.remove(m_key); }
