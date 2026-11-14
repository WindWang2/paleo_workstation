// 层：数据
#pragma once
#include "domain/wellfaciesrequest.h"
#include <functional>
namespace paleo::wellfacies {
// Paths are host-created, relative to the bound project. No paths are accepted from tool arguments.
QJsonObject storeInput(const QString &project, const PreparedInput &input, QString &code,
                      const std::function<bool()> &cancelled = {});
PreparedInput loadInput(const QString &project,const QJsonObject &reference,
                        const std::function<bool()> &cancelled = {});
bool storeResultCache(const QString &project,const QJsonObject &inputRef,int segment,
                      const QByteArray &bytes,QString &code);
QByteArray loadResultCache(const QString &project,const QJsonObject &inputRef,int segment,
                          const QString &expectedHash,QString &code);
} // namespace paleo::wellfacies
