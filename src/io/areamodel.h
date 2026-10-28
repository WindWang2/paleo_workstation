// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <functional>
#include "lasdoc.h"
namespace paleo::data { class DataService; struct ImportReport; }
class DataCatalog;
namespace paleo::area_model {
bool ensureProject(const QString &project, QString *error);
QString directory(const QString &project);
bool importDirectory(const QString &project, const QString &source,
                     paleo::data::ImportReport &report, QString *error,
                     const std::function<bool()> &cancelled = {}, const QStringList &excluded = {},
                     const std::function<void(int, int, const QString &)> &progress = {});
void registerProject(const QString &project, const void *owner);
void registerAliases(const DataCatalog &catalog, const void *owner);
void unregisterProject(const void *owner);
enum class Read { Unbound, Ready, Failed };
QString cacheToken(const QString &path);
Read readLog(const QString &path, LasHeaderInfo &header, QList<LasCurve> &curves, QString *error);
Read openSource(const QString &path, paleo::data::DataService &service,
                long long &sourceId, QString *error);
}
