// 层：数据
#include "horizongridexchange.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>

namespace paleo::io {
namespace {
const QByteArray prefix("# paleo_horizon_grid_v1 ");
const QByteArray gridHeader("inline,xline,twt_ms,confidence");
constexpr qint64 maxHeader = 64 * 1024;
bool reject(QString *error, const QString &reason) { if (error) *error = reason; return false; }
bool validSource(const QVariantMap &source) {
    for (const auto &key : {"project_id","asset_id","version_id","parent_asset_id","parent_version_id","horizon_name"})
        if (source.value(key).typeId() != QMetaType::QString || source.value(key).toString().trimmed().isEmpty()) return false;
    const auto surveys = source.value("survey_ids");
    if (surveys.typeId() != QMetaType::QStringList && surveys.typeId() != QMetaType::QVariantList) return false;
    for (const auto &entry : surveys.toList()) if (entry.typeId() != QMetaType::QString || entry.toString().isEmpty()) return false;
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    return source.value("time_unit").toString() == "ms" && source.value("time_type").toString() == "TWT" &&
           source.value("time_reference").typeId() == QMetaType::QString && sha.match(source.value("payload_sha256").toString()).hasMatch();
}
bool parsePoint(const QByteArray &line, InterpretationPoint &point) {
    const auto fields = line.trimmed().split(',');
    if (fields.size() != 4) return false;
    bool il, xl, time, quality;
    point.inlineNumber = fields[0].toInt(&il); point.crosslineNumber = fields[1].toInt(&xl);
    point.twtMs = fields[2].toDouble(&time); point.confidence = fields[3].toDouble(&quality);
    return il && xl && time && quality && std::isfinite(point.twtMs) && std::isfinite(point.confidence) &&
           point.confidence >= 0 && point.confidence <= 1;
}
}
bool isHorizonGridExchange(const QString &path) {
    if (QFileInfo(path).suffix().compare("csv", Qt::CaseInsensitive) != 0) return false;
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(prefix.size()) == prefix;
}
bool writeHorizonGridExchange(const QString &verifiedCsv, const QString &destination, const QVariantMap &source,
    QString *error, const std::function<bool()> &cancelled) {
    const auto header = prefix + QJsonDocument(QJsonObject::fromVariantMap(source)).toJson(QJsonDocument::Compact) + '\n';
    if (!validSource(source) || header.size() >= maxHeader)
        return reject(error, QStringLiteral("时间域层位的来源工程、父版本或时间约定不完整"));
    QFile input(verifiedCsv);
    QSaveFile file(destination);
    if (!input.open(QIODevice::ReadOnly) || input.peek(64).split('\n').first().trimmed() != gridHeader ||
        !file.open(QIODevice::WriteOnly) || file.write(header) != header.size())
        return reject(error, QStringLiteral("无法写出指定的时间域格点层位"));
    while (!input.atEnd()) {
        if (cancelled && cancelled()) return reject(error, QStringLiteral("已取消层位交换"));
        const auto block = input.read(1024 * 1024);
        if (block.isEmpty() || file.write(block) != block.size()) return reject(error, QStringLiteral("时间域层位写出失败"));
    }
    if (input.error() != QFile::NoError || !file.commit()) return reject(error, QStringLiteral("时间域层位提交失败"));
    return true;
}
bool readHorizonGridExchange(const QString &path, const QString &expectedFileSha, QVariantMap &source,
    QString *error, const std::function<bool()> &cancelled, const std::function<void(const InterpretationPoint &)> &point) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return reject(error, file.errorString());
    const auto declaration = file.readLine(maxHeader);
    if (!declaration.startsWith(prefix) || !declaration.endsWith('\n'))
        return reject(error, QStringLiteral("时间域层位缺少有效的来源清单"));
    const auto info = QJsonDocument::fromJson(declaration.sliced(prefix.size())).object().toVariantMap();
    if (!validSource(info)) return reject(error, QStringLiteral("时间域层位来源或时间约定无效"));
    QCryptographicHash whole(QCryptographicHash::Sha256), payload(QCryptographicHash::Sha256);
    whole.addData(declaration);
    const auto header = file.readLine(1024);
    if (header.trimmed() != gridHeader) return reject(error, QStringLiteral("时间域层位格点列无效"));
    payload.addData(header); whole.addData(header);
    qint64 rows = 0;
    InterpretationPoint last{};
    while (!file.atEnd()) {
        if (cancelled && cancelled()) return reject(error, QStringLiteral("已取消层位读取"));
        const auto line = file.readLine(1024);
        InterpretationPoint current{};
        if (line.size() >= 1023 || !parsePoint(line, current) ||
            (rows && (current.inlineNumber < last.inlineNumber ||
                      (current.inlineNumber == last.inlineNumber && current.crosslineNumber <= last.crosslineNumber))))
            return reject(error, QStringLiteral("时间域层位格点值、置信度或线号顺序无效（第 %1 点）").arg(rows + 1));
        payload.addData(line); whole.addData(line);
        if (point) point(current);
        last = current;
        ++rows;
    }
    if (!rows || file.error() != QFile::NoError || QString::fromLatin1(payload.result().toHex()) != info.value("payload_sha256").toString() ||
        (!expectedFileSha.isEmpty() && QString::fromLatin1(whole.result().toHex()) != expectedFileSha))
        return reject(error, QStringLiteral("时间域层位格点不完整或文件摘要不符"));
    source = info;
    source.insert("point_count", rows);
    return true;
}
}
