// 层：数据
#pragma once
#include <QMap>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

struct ProjectSetupSources
{
  QString root, points, wells, seismic, horizons, references, boundary, paleomaps;
  QString projectCrs; // Explicit computation CRS; empty chooses the detected metric CRS.
  bool normalizeMaps = true;
  QVariantMap mapChoices; // path -> { included, horizon }
  QVariantMap pointMapping; // sheet / well / x / y / kb / td / crs
  QMap<QString, QString> wellTargets; // path -> name:<well>, empty keeps file identity
};
struct ProjectSetupMap
{
  QString path, horizon, problem, crs;
  QStringList members;
  bool included = true;
};
struct ProjectSetupPreview
{
  QStringList paths, notes, errors;
  QMap<QString, QString> types, hashes;
  QMap<QString, QString> wellTargets, logWellNames;
  QStringList knownWells;
  QMap<QString, QVariantMap> mappings;
  QStringList sheets, columns;
  QVector<QStringList> pointRows;
  QVariantMap pointMapping;
  QString sourceCrs, projectCrs;
  QVector<ProjectSetupMap> maps;
  QMap<QString, QString> mapHashes;
  int wells = 0, tops = 0, logs = 0, seismic = 0, horizons = 0, references = 0;
};
