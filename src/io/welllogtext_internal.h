// 层：数据
#pragma once
#include "welllogtext.h"
#include "outsourceworkbook.h"

namespace WellLogText::Detail
{
constexpr qint64 kMaxBytes = 64LL * 1024 * 1024;
constexpr qint64 kMaxCells = 4'000'000;
constexpr int kMaxColumns = 256;
struct Column { QString name; QString unit; };
struct Table
{
  QString label;
  QStringList headers;
  QVector<QStringList> rows;
  QVector<int> lines;
};
Column column(const QString &text);
bool isIndex(const QString &name);
bool fail(const QString &path, int line, const QString &message, QString *error,
          QList<LasIssue> *issues, LasIssue::Category category = LasIssue::Category::Format);
bool split(const QString &line, QChar delimiter, QStringList &cells);
bool readText(const QByteArray &bytes, Table &table, QString &wellName, QString &error, int &errorLine);
bool convert(const QString &path, const QVector<Table> &tables, LasHeaderInfo &header,
             QList<LasCurve> &curves, QString *error, QList<LasIssue> *issues);
}
