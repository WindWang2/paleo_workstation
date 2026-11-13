// 层：功能
#pragma once
#include "capabilityrecovery.h"
#include <QSet>
namespace paleo::capability::publication_detail {
QJsonObject specJson(const AlgorithmSpec &spec);
bool specFromJson(const QJsonObject &json, AlgorithmSpec &spec);
QJsonObject targetsJson(const QVector<AlgorithmOutputTarget> &targets);
bool targetsFromJson(const QString &root, const QJsonObject &json, QVector<AlgorithmOutputTarget> &targets);
QJsonObject preparedJson(const DerivedPreparedSet &prepared);
bool preparedFromJson(const QString &root, const OperationLog::Record &record, DerivedPreparedSet &prepared,
                      QString *error, const std::function<bool()> &cancelled);
QJsonObject artifactManifest(const QString &root, const QString &operationId, const QString &privateDir,
                             const AlgorithmRunResult &outputs, QString *error,
                             const std::function<bool()> &cancelled, const QSet<QString> &allowedManaged = {});
bool outputsFromManifest(const QString &root, const QString &operationId, const QJsonObject &manifest,
                         AlgorithmRunResult &outputs, QString *error,
                         const std::function<bool()> &cancelled, bool verifyBytes = true,
                         const QSet<QString> &allowedManaged = {});
AlgorithmRunResult preparedOutputs(const DerivedPreparedSet &prepared);
}
