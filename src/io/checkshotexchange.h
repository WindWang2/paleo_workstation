// 层：数据
#pragma once
#include "../domain/wellrecords.h"
#include <QVariantMap>
#include <functional>
#include <memory>

namespace paleo::data { struct ParsedFile; struct TimeDepthTable; }
namespace paleo::io {
inline constexpr char checkshotProfile[] = "paleo_checkshots_v1";
struct CheckshotExchange {
    QVariantMap source;
    QString well;
    WellParseReport report;
    std::shared_ptr<data::ParsedFile> model;
};
bool isCheckshotExchange(const QString &path);
bool isPetrelCheckshotSource(const QString &path);
bool writeCheckshotExchange(const QString &frozenPath, const QString &path, const QVariantMap &source,
                           QString *error, const std::function<bool()> &cancelled);
bool readCheckshotExchange(const QString &path, const QString &project, const QString &fileSha,
                          CheckshotExchange *out, QString *error, const std::function<bool()> &cancelled);
bool checkshotSamples(const data::TimeDepthTable &relation, TimeDepthTable &normalized, QString *error);
}
