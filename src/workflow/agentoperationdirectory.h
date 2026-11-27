// 层：功能
// 工作台能力、算法、工具与命令的只读搜索结果。
#pragma once

#include "../domain/operationsearch.h"
#include <QJsonObject>
#include <QVector>

namespace paleo::agent {
struct DirectoryEntry {
  int kindRank;
  QString id;
  int version;
  QJsonObject card;
  double score;
};

QVector<DirectoryEntry> buildDirectory(const QString &filter, const OperationSearch &search);
} // namespace paleo::agent
