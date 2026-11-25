// 层：数据
#include "originalpackage.h"
#include "metadata/managedpath.h"
#include "domain/exportrequest.h"
#include "domain/importrows.h"
#include "catalog/datacatalog.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <limits>

namespace paleo::exchange {
namespace {
using managedpath::resolveManagedPath;
using Cancel = std::function<bool()>;
using Progress = std::function<void(qint64, qint64, const QString &)>;
struct Original {
    QString source, name, asset, version, sha, receiptId;
    qint64 size = 0;
};
struct Package {
    QString sender;
    QList<Original> originals;
    qint64 size = 0;
};
QString failText(const QString &message, const QString &name) {
    return message + QStringLiteral("：") + name;
}
bool validSegment(const QString &part) {
    static const QRegularExpression forbidden(QStringLiteral(R"([<>:"|?*])"));
    static const QRegularExpression device(QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$))"),
                                           QRegularExpression::CaseInsensitiveOption);
    return managedpath::isSafePathSegment(part) && !part.endsWith('.') && !part.endsWith(' ') &&
           !part.contains(forbidden) && !device.match(part).hasMatch();
}
QJsonObject readObject(const QString &path, qint64 limit, QString &error) {
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > limit) {
        error = failText(QObject::tr("无法读取清单或清单过大"), path);
        return {};
    }
    const auto bytes = file.read(limit + 1);
    const auto document = QJsonDocument::fromJson(bytes);
    if (file.error() != QFile::NoError || bytes.size() > limit || !document.isObject())
        error = failText(QObject::tr("清单格式无效"), path);
    return document.object();
}
QString readOriginal(const QJsonObject &record, const QString &root, const QString &sender, Original &item) {
    const auto files = record.value("files").toArray();
    item.asset = record.value("asset_id").toString();
    item.version = record.value("version_id").toString();
    if (record.value("stage") != "RAW" || files.size() != 1 || !validSegment(item.asset) || !validSegment(item.version))
        return QObject::tr("仅可接收每个 RAW 版本含一个 LAS 或 SEG-Y 原件的资料包");
    const auto file = files.first().toObject();
    const auto path = file.value("path").toString();
    const auto parts = path.split('/');
    if (parts.size() != 4 || (parts[0] != "data" && parts[0] != "sources") || parts[1] != item.asset ||
        parts[2] != item.version || !validSegment(parts[3]))
        return failText(QObject::tr("原件不在清单声明的资产版本目录"), path);
    item.name = parts[3];
    const auto suffix = QFileInfo(item.name).suffix().toLower();
    if (suffix != "las" && suffix != "sgy" && suffix != "segy")
        return failText(QObject::tr("该文件不是 LAS 或 SEG-Y 原件"), item.name);
    item.sha = file.value("sha256").toString().toLower();
    item.size = file.value("bytes").toInteger(-1);
    static const QRegularExpression fullSha(QStringLiteral("^[0-9a-f]{64}$"));
    if (!fullSha.match(item.sha).hasMatch() || item.sha != record.value("source_sha256").toString().toLower() || item.size < 0)
        return failText(QObject::tr("原件缺少一致的完整摘要或长度"), item.name);
    item.source = resolveManagedPath(root, path);
    if (item.source.isEmpty() || !QFileInfo(item.source).isFile() || QFileInfo(item.source).size() != item.size)
        return failText(QObject::tr("原件位置或长度与清单不符"), item.name);
    const auto identity = QJsonDocument(QJsonArray{sender, item.version}).toJson(QJsonDocument::Compact);
    item.receiptId = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    return {};
}
QString readPackage(const QString &root, const QString &overrideId, Package &package, bool &needsSenderId) {
    QString error;
    const auto doc = readObject(resolveManagedPath(root, "metadata.json"), 16 * 1024 * 1024, error);
    if (!error.isEmpty()) return error;
    if (doc.value("schema") != "paleo_export_v1" || doc.value("format").toInteger(-1) != int(exporting::DataFormat::Native) ||
        !doc.value("complete").toBool() || doc.value("assets").toArray().isEmpty())
        return QObject::tr("请选择已完成的原生格式资料导出包");
    package.sender = doc.value("project_id").toString();
    if (!package.sender.isEmpty() && !overrideId.isEmpty() && overrideId != package.sender)
        return QObject::tr("来源工程身份与清单不一致");
    if (package.sender.isEmpty()) package.sender = overrideId;
    if (package.sender.trimmed().isEmpty()) {
        needsSenderId = true;
        return QObject::tr("旧资料包缺少来源工程身份，请在资料页接收时指定来源工程标识");
    }
    QSet<QString> versions;
    for (const auto &value : doc.value("assets").toArray()) {
        Original item;
        error = readOriginal(value.toObject(), root, package.sender, item);
        if (!error.isEmpty()) return error;
        if (versions.contains(item.version) || item.size > std::numeric_limits<qint64>::max() - package.size)
            return QObject::tr("资料包版本重复或文件总量超出可处理范围");
        versions.insert(item.version);
        package.size += item.size;
        package.originals.append(item);
    }
    return {};
}
QJsonObject receipt(const QString &sender, const Original &item) {
    return {{"schema", "paleo_received_original_v1"}, {"project_id", sender}, {"asset_id", item.asset},
            {"version_id", item.version}, {"file_name", item.name}, {"bytes", item.size}, {"sha256", item.sha},
            {"receipt_id", item.receiptId}};
}
QString ensureReceipt(const QString &directory, const QJsonObject &wanted, bool create, QString &storedName) {
    const auto path = resolveManagedPath(directory, "receipt.json");
    if (path.isEmpty()) return QObject::tr("收件回执位置无效");
    storedName = wanted.value("file_name").toString();
    if (QFileInfo::exists(path) || !create) {
        QString error;
        auto saved = readObject(path, 64 * 1024, error);
        if (!error.isEmpty()) return error;
        const auto previousName = saved.value("file_name").toString();
        saved.insert("file_name", storedName);
        if (!validSegment(previousName) || saved != wanted)
            return QObject::tr("相同来源工程与版本已有不同内容或身份的收件记录");
        storedName = previousName; // A later package may rename the same version's file.
        return {};
    }
    QSaveFile file(path);
    const auto bytes = QJsonDocument(wanted).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return failText(QObject::tr("无法写入收件回执"), path);
    return {};
}
struct Transfer {
    const Cancel &cancel;
    const Progress &progress;
    qint64 completed, total;
    bool stopped() const { return cancel && cancel(); }
};
// Compare every retained prefix byte before resuming a partial transfer. No second payload copy is needed.
QString transfer(const Original &item, const QString &target, bool published, const Transfer &job) {
    QFile input(item.source), output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(published ? QIODevice::ReadOnly : QIODevice::ReadWrite))
        return failText(QObject::tr("无法打开原件或工程收件文件"), item.name);
    const qint64 retained = output.size();
    if (retained > item.size || (published && retained != item.size) || input.size() != item.size)
        return failText(QObject::tr("原件或已收文件长度不符"), item.name);
    if (!published) {
        const QStorageInfo volume(target);
        if (!volume.isValid() || !volume.isReady() || volume.bytesAvailable() < item.size - retained)
            return QObject::tr("工程盘不可用或剩余空间不足");
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 offset = 0;
    while (offset < item.size) {
        if (job.stopped()) return {};
        const bool compare = offset < retained;
        const auto count = qMin<qint64>(1024 * 1024, (compare ? retained : item.size) - offset);
        const auto bytes = input.read(count);
        if (bytes.size() != count)
            return failText(QObject::tr("原件传输中断"), item.name);
        if (compare && output.read(count) != bytes)
            return failText(QObject::tr("工程内已有文件与本次原件不同"), item.name);
        if (!compare && output.write(bytes) != count)
            return failText(QObject::tr("写入收件文件失败"), item.name);
        hash.addData(bytes);
        offset += count;
        if (job.progress) job.progress(job.completed + offset, job.total, item.name);
    }
    if (job.stopped()) return {};
    if (!input.atEnd() || input.error() != QFile::NoError || output.error() != QFile::NoError ||
        (!published && !output.flush()) || QString::fromLatin1(hash.result().toHex()) != item.sha)
        return failText(QObject::tr("原件完整性核验失败"), item.name);
    return {};
}
QString receiveOne(const QString &inbox, const QString &sender, const Original &item,
                   const Transfer &job, ReceiveResult &result) {
    const auto final = resolveManagedPath(inbox, item.receiptId);
    const auto pending = resolveManagedPath(inbox, item.receiptId + ".pending");
    const auto lockPath = resolveManagedPath(inbox, item.receiptId + ".lock");
    if (final.isEmpty() || pending.isEmpty() || lockPath.isEmpty())
        return QObject::tr("收件目录越界或不可用");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return QObject::tr("该版本正在接收或收件目录不可写，请稍后重试");
    const bool published = QFileInfo::exists(final);
    const auto directory = published ? final : pending;
    if (!published && !QDir().mkpath(directory)) return QObject::tr("无法创建工程收件暂存目录");
    QString storedName;
    QString error = ensureReceipt(directory, receipt(sender, item), !published, storedName);
    if (!error.isEmpty()) return error;
    const auto file = resolveManagedPath(directory, storedName);
    if (file.isEmpty()) return QObject::tr("收件文件位置无效");
    error = transfer(item, file, published, job);
    if (job.stopped() || !error.isEmpty()) {
        // Explicit cancellation drops our draft. An interrupted process leaves it for validated resume.
        if (!published) {
            QFile::remove(file);
            QFile::remove(QDir(pending).filePath("receipt.json"));
            QDir().rmdir(pending);
        }
        return error;
    }
    if (!published && !QDir().rename(pending, final)) return QObject::tr("收件发布失败，可重新接收");
    const auto accepted = QDir(final).filePath(storedName);
    result.files.append(accepted);
    if (published) result.reused.append(accepted);
    return {};
}
}

ReceiveResult receiveOriginalPackage(const QString &projectDir, const QString &packageDir,
    const QString &senderOverride, const Cancel &cancelled, const Progress &progress) {
    ReceiveResult result;
    Package package;
    if (projectDir.isEmpty() || packageDir.isEmpty()) {
        result.error = QObject::tr("缺少接收工程或资料包目录"); return result;
    }
    result.error = readPackage(packageDir, senderOverride, package, result.needsSenderId);
    if (!result.error.isEmpty()) return result;
    const auto inbox = resolveManagedPath(projectDir, "artifacts/received");
    if (inbox.isEmpty() || !QDir().mkpath(inbox)) {
        result.error = QObject::tr("工程收件区不可写"); return result;
    }
    qint64 completed = 0;
    for (const auto &item : package.originals) {
        const Transfer job{cancelled, progress, completed, package.size};
        if (!job.stopped()) result.error = receiveOne(inbox, package.sender, item, job, result);
        if (job.stopped()) result.cancelled = true;
        if (result.cancelled || !result.error.isEmpty()) break;
        completed += item.size;
    }
    return result;
}

QString recordReceivedImport(const DataCatalog &catalog, const FolderRowResult &row, const QString &outcome) {
    const QDir project(catalog.projectDir());
    const auto relative = project.relativeFilePath(row.path);
    if (!relative.startsWith(QStringLiteral("artifacts/received/"))) return {};
    const auto parts = relative.split('/');
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    if (parts.size() != 4 || !digest.match(parts[2]).hasMatch() || !validSegment(parts[3])) return {};
    const auto directory = resolveManagedPath(project.path(), parts.mid(0, 3).join('/'));
    if (directory.isEmpty() || resolveManagedPath(directory, parts[3]).isEmpty())
        return failText(QObject::tr("收件文件位置无效"), row.path);
    QString error;
    const auto original = readObject(resolveManagedPath(directory, "receipt.json"), 64 * 1024, error);
    if (!error.isEmpty()) return error;
    const auto sha = original.value("sha256").toString();
    if (original.value("schema") != "paleo_received_original_v1" || original.value("receipt_id") != parts[2] ||
        original.value("file_name") != parts[3] || !digest.match(sha).hasMatch())
        return failText(QObject::tr("清单格式无效"), row.path);
    QJsonObject entry{{"schema", "paleo_original_import_v1"}, {"receipt_id", parts[2]},
                      {"source_sha256", sha}, {"outcome", outcome}, {"type", row.classifiedType},
                      {"asset_id", row.assetId}, {"entity", row.entityName}, {"message", row.message}};
    QStringList matchingVersions;
    for (const auto &version : catalog.versionsForAsset(row.assetId)) {
        if (version.sha256 == sha && version.stage == QLatin1String("RAW")) {
            matchingVersions.append(version.id);
        }
    }
    if (!row.assetId.isEmpty() && matchingVersions.isEmpty())
        return failText(QObject::tr("原件缺少一致的完整摘要或长度"), row.path);
    if (matchingVersions.size() == 1) entry.insert("local_raw_version_id", matchingVersions.front());
    if (matchingVersions.size() > 1) entry.insert("matching_versions", QJsonArray::fromStringList(matchingVersions));
    const auto bytes = QJsonDocument(entry).toJson(QJsonDocument::Compact);
    const auto key = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const auto path = resolveManagedPath(directory, QStringLiteral("import-%1.json").arg(key));
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        if (readObject(path, bytes.size(), error) == entry) return {};
        return failText(QObject::tr("清单格式无效"), path);
    }
    QSaveFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return failText(QObject::tr("无法写入收件回执"), path);
    return {};
}
}
