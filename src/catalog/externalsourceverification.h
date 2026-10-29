// 层：数据
#pragma once
#include <QString>
#include <functional>
struct CatalogVersion;

// Worker-safe: project and version are value snapshots, never a live catalog.
// Model SEG-Y fingerprints are verified by Data_service, never as full SHA-256.
bool verifyExternalSourceFile(const QString &project, const CatalogVersion &version,
                             QString *error = nullptr,
                             const std::function<bool()> &cancelled = {},
                             bool *mismatched = nullptr,
                             const std::function<void(qint64, qint64)> &progress = {});
