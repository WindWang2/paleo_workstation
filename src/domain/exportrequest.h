// 层：数据
#pragma once
#include <QStringList>

namespace paleo::exporting {
enum class Scope { Current, Original, Sources };
enum class DataFormat { Native, Exchange, GeoPackage, GeoJson, Shapefile, Csv, GeoTiff };
struct DataRequest {
  QStringList versionIds;
  QString destination; // new batch directory; existing directories are never replaced
  Scope scope = Scope::Current;
  DataFormat format = DataFormat::Native;
  bool includeReferences = false;
  bool previewOnly = false;
};
struct Result {
  QString directory, error;
  QStringList files, failures, warnings;
  int succeeded = 0, total = 0;
  qint64 plannedBytes = 0;
  bool cancelled = false;
  bool complete() const { return !cancelled && error.isEmpty() && failures.isEmpty() && total > 0; }
};
}
