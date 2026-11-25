// 层：数据
#pragma once
#include <QStringList>
#include <functional>

class DataCatalog;
struct FolderRowResult;

namespace paleo::exchange {
struct ReceiveResult {
    QStringList files;  // Durable originals, including any committed before cancellation or failure.
    QStringList reused; // Subset of files which already had a verified receipt.
    QString error;
    bool cancelled = false;
    bool needsSenderId = false;
};

// Worker-only. The caller supplies its active writable project and invokes normal import separately.
// An older native export without project_id requires an explicit stable senderOverride.
ReceiveResult receiveOriginalPackage(const QString &projectDir, const QString &packageDir,
    const QString &senderOverride = {}, const std::function<bool()> &cancelled = {},
    const std::function<void(qint64, qint64, const QString &)> &progress = {});

// Persist the local import outcome separately from the immutable transfer receipt.
// Ordinary imports outside the managed inbox do not have a transfer receipt.
QString recordReceivedImport(const DataCatalog &catalog, const FolderRowResult &row,
                            const QString &outcome);
}
