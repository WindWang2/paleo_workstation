// 层：数据
#include "checkshotexchange.h"
#include "metadata/managedpath.h"
#include <paleo_data/parser.h>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

namespace paleo::io {
namespace {
const QByteArray prefix("# paleo_checkshots_v1 ");
constexpr qint64 maxDeclaration = 4096;
constexpr qint64 maxBytes = 64ll * 1024 * 1024;
bool isPetrelHeader(QFile &file) { return file.peek(128).split('\n').first().trimmed() == "# Petrel checkshots format"; }
bool reject(QString *error, const QString &message) { if (error) *error = message; return false; }
bool valid(const QVariantMap &info) {
    for (const auto &key : {"project_id", "asset_id", "version_id", "source_name"})
        if (info.value(key).typeId() != QMetaType::QString || info.value(key).toString().trimmed().isEmpty()) return false;
    static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
    return hash.match(info.value("payload_sha256").toString()).hasMatch();
}
bool copyPayload(QFile &input, QIODevice &output, QCryptographicHash &whole, const QString &expected,
                 QString *error, const std::function<bool()> &cancelled) {
    if (input.size() - input.pos() > maxBytes || !isPetrelHeader(input))
        return reject(error, QStringLiteral("当前时深交换支持声明 Petrel checkshots 格式且不超过 64 MiB 的单井表"));
    QCryptographicHash payload(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    while (!input.atEnd()) {
        if (cancelled && cancelled()) return reject(error, QStringLiteral("已取消时深交换"));
        const auto part = input.read(1024 * 1024);
        bytes += part.size();
        if (part.isEmpty() || bytes > maxBytes || output.write(part) != part.size())
            return reject(error, QStringLiteral("时深交换文件读取或写入不完整"));
        whole.addData(part); payload.addData(part);
    }
    if (input.error() != QFile::NoError || QString::fromLatin1(payload.result().toHex()) != expected)
        return reject(error, QStringLiteral("时深表原始内容摘要不符"));
    return true;
}
bool singleWell(const data::ParsedFile &model, QString *error) {
    if (model.kind != "time_depth" || model.wells.size() != 1 || !model.unassignedTimeDepth.empty() ||
        model.wells.front().timeDepthRelations.size() != 1 ||
        model.wells.front().timeDepthRelations.front().depthTimePairs.empty())
        return reject(error, QStringLiteral("时深交换需要一口具名井的一张有效表：%1").arg(QString::fromUtf8(model.message)));
    return true;
}
}
bool isCheckshotExchange(const QString &path) {
    QFile file(path);
    if (QFileInfo(path).suffix().compare(QStringLiteral("checkshots"), Qt::CaseInsensitive) != 0) return false;
    return file.open(QIODevice::ReadOnly) && file.read(prefix.size()) == prefix;
}
bool isPetrelCheckshotSource(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && isPetrelHeader(file);
}
bool writeCheckshotExchange(const QString &frozenPath, const QString &path, const QVariantMap &source,
                           QString *error, const std::function<bool()> &cancelled) {
    QFile input(frozenPath);
    QSaveFile file(path);
    const auto declaration = prefix + QJsonDocument::fromVariant(source).toJson(QJsonDocument::Compact) + '\n';
    if (!valid(source) || declaration.size() >= maxDeclaration) return reject(error, QStringLiteral("时深交换缺少可核对的来源工程和版本"));
    if (!input.open(QIODevice::ReadOnly) || !file.open(QIODevice::WriteOnly) || file.write(declaration) != declaration.size())
        return reject(error, QStringLiteral("无法准备时深交换文件"));
    QCryptographicHash whole(QCryptographicHash::Sha256);
    if (!copyPayload(input, file, whole, source.value("payload_sha256").toString(), error, cancelled)) return false;
    if (!file.commit()) return reject(error, file.errorString());
    return singleWell(data::parseFile(path.toUtf8().toStdString(), {}, cancelled), error);
}
bool readCheckshotExchange(const QString &path, const QString &project, const QString &fileSha,
                          CheckshotExchange *out, QString *error, const std::function<bool()> &cancelled) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return reject(error, input.errorString());
    const auto declaration = input.readLine(maxDeclaration);
    const auto info = QJsonDocument::fromJson(declaration.mid(prefix.size())).object().toVariantMap();
    if (!declaration.startsWith(prefix) || !declaration.endsWith('\n') || !valid(info))
        return reject(error, QStringLiteral("时深交换来源清单无效"));
    auto temp = managedpath::temporaryDirectory(project, QStringLiteral("checkshot-import"));
    if (!temp) return reject(error, QStringLiteral("无法在工程中建立时深验证目录"));
    QFile snapshot(temp->filePath("source.checkshots"));
    if (!snapshot.open(QIODevice::WriteOnly)) return reject(error, snapshot.errorString());
    QCryptographicHash whole(QCryptographicHash::Sha256); whole.addData(declaration);
    if (!copyPayload(input, snapshot, whole, info.value("payload_sha256").toString(), error, cancelled)) return false;
    if (!snapshot.flush() || QString::fromLatin1(whole.result().toHex()) != fileSha)
        return reject(error, QStringLiteral("时深交换文件与已验证摘要不符"));
    snapshot.close();
    auto model = std::make_shared<data::ParsedFile>(data::parseFile(snapshot.fileName().toUtf8().toStdString(), {}, cancelled));
    if (!singleWell(*model, error)) return false;
    auto &well = model->wells.front();
    auto &relation = well.timeDepthRelations.front();
    relation.relationName = info.value("source_name").toString().toUtf8().toStdString();
    relation.sourceFile = path.toUtf8().toStdString(); well.sourceFiles = {relation.sourceFile};
    out->source = info; out->well = QString::fromUtf8(well.wellInfo.wellName);
    out->report.acceptedRows = static_cast<int>(relation.depthTimePairs.size());
    for (const auto &issue : model->issues) out->report.issues << QString::fromUtf8(issue.message);
    out->model = std::move(model);
    return true;
}
bool checkshotSamples(const data::TimeDepthTable &relation, TimeDepthTable &normalized, QString *error) {
    if (relation.depthInfo.depthType != "MD" || (relation.depthInfo.depthUnit != "m" && relation.depthInfo.depthUnit != "ft") ||
        relation.depthInfo.mdReference.empty() || (relation.timeUnit != "ms" && relation.timeUnit != "s") ||
        (relation.timeType != "TWT" && relation.timeType != "OWT") || relation.timeReference.empty())
        return reject(error, QStringLiteral("时深表已保存，请复核深度单位、MD 起算面、时间单位、单/双程和时间零面后使用"));
    const double depth = relation.depthInfo.depthUnit == "ft" ? .3048 : 1.;
    const double time = (relation.timeUnit == "s" ? 1000. : 1.) * (relation.timeType == "OWT" ? 2. : 1.);
    normalized.rows.clear();
    for (const auto &p : relation.depthTimePairs) {
        TdRow row; row.timeMs = p.timeValue * time; row.md = p.depthValue * depth;
        row.hasMd = std::isfinite(row.md); row.tvd = row.tvdss = qQNaN();
        normalized.rows.append(row);
    }
    return true;
}
}
