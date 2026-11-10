// 层：数据
#pragma once

#include <QString>
#include <cpl_string.h>
#include <gdal.h>

namespace ProjectGeoTiff
{
inline constexpr const char *kDriver = "GTiff";
inline constexpr const char *kCreationOptions[] = {
    "TILED=YES", "COMPRESS=DEFLATE", "BIGTIFF=IF_SAFER"};

inline GDALDriverH driver()
{
  return GDALGetDriverByName(kDriver);
}

// The producer owns the data type, georeferencing, nodata and metadata.
inline GDALDatasetH create(const QString &path, int columns, int rows, GDALDataType type,
                           GDALDriverH gtiff = driver())
{
  if (!gtiff)
    return nullptr;
  CPLStringList options;
  for (const char *option : kCreationOptions)
    options.AddString(option);
  return GDALCreate(gtiff, path.toUtf8().constData(), columns, rows, 1, type, options.List());
}
}
