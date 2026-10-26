// 层：数据
#pragma once
#include "welllogtext.h"
#include "outsourceworkbook.h"
#include <QSet>

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
struct Samples
{
  QVector<double> index;
  QList<LasCurve> curves;
  Column axis;
  QString indexBasis;
  int indexColumn = -1;
  int wellColumn = -1;
  QVector<int> valueColumns;
};
Column column(const QString &text);
bool isIndex(const QString &name);
bool fail(const QString &path, int line, const QString &message, QString *error,
          QList<LasIssue> *issues, LasIssue::Category category = LasIssue::Category::Format);
bool split(const QString &line, QChar delimiter, QStringList &cells);
bool readText(const QByteArray &bytes, Table &table, QString &wellName, QString &error, int &errorLine);
bool convert(const QString &path, const QVector<Table> &tables, LasHeaderInfo &header,
             QList<LasCurve> &curves, QString *error, QList<LasIssue> *issues);
bool beginTable(const QString &path, const Table &table, LasHeaderInfo &header,
                Samples &samples, QSet<QString> &names, QString *error, QList<LasIssue> *issues);
bool appendSample(const QString &path, const QStringList &headers, const QStringList &row, int line,
                  LasHeaderInfo &header, Samples &samples, QString *error, QList<LasIssue> *issues);
bool mergeSamples(const QString &path, const QVector<Samples> &sets, qint64 maxCells,
                  LasHeaderInfo &header, QList<LasCurve> &curves, QString *error, QList<LasIssue> *issues);
bool parseXml(const QString &path, LasHeaderInfo &header, QList<LasCurve> &curves,
              QString *error, QList<LasIssue> *issues);
}
