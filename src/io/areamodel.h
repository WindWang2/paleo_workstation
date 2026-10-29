// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <functional>
#include "lasdoc.h"
namespace paleo::data { class DataService; struct ImportReport; struct SeismicCoordinateRange;
                        struct WellHeadRange; struct ProjectPlanarRange; }
class DataCatalog;
namespace paleo::area_model {
bool ensureProject(const QString &project, QString *error);
QString directory(const QString &project);
QString confirmedProjectCrs(const QString &project);
bool importDirectory(const QString &project, const QString &source,
                     paleo::data::ImportReport &report, QString *error,
                     const std::function<bool()> &cancelled = {}, const QStringList &excluded = {},
                     const std::function<void(int, int, const QString &)> &progress = {});
bool importDirectoryWithCrs(const QString &project, const QString &source,
                     paleo::data::ImportReport &report, QString *error,
                     const std::function<bool()> &cancelled, const QStringList &excluded,
                     const std::function<void(int, int, const QString &)> &progress,
                     const QString &crs, const QString &crsUnit);
void registerProject(const QString &project, const void *owner);
void registerAliases(const DataCatalog &catalog, const void *owner);
void unregisterProject(const void *owner);
enum class Read { Unbound, Ready, Failed };
QString cacheToken(const QString &path);
Read readLog(const QString &path, LasHeaderInfo &header, QList<LasCurve> &curves, QString *error);
Read openSource(const QString &path, paleo::data::DataService &service,
                long long &sourceId, QString *error);
// ---- 三个平面范围读口（数据服务统一计算，宿主不再自行重算角点）----
// surveyRange：单个地震资产（当前版本路径）的测区范围。来源未登记进工区模型时
// 返回 Unbound（旧工程/外部文件），由调用方决定兼容路径；不在本函数内猜几何。
Read surveyRange(const QString &path, paleo::data::SeismicCoordinateRange &range,
                 QString *error, const std::function<bool()> &cancelled = {});
// wellHeadRange / projectRange：按工区模型目录（directory(project)）读取井点矩形
// 与工区范围（井点 ∪ 地震）。root 为空或 data.sqlite 不存在时返回 Unbound
// （未建工区模型，无从计算）；不跨工区猜。范围是否可用另读 status / extent.valid。
Read wellHeadRange(const QString &root, paleo::data::WellHeadRange &range, QString *error);
Read projectRange(const QString &root, paleo::data::ProjectPlanarRange &range, QString *error);
}
