// 层：功能
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <vector>

#include "../algorithms/stratgrid/propfill.h"
#include "../algorithms/stratgrid/upscale.h"
#include "../domain/faultset.h"

class DataCatalog;

// workflow/ — 属性建模编排（goal/property-modeling）。
// 视图只发意图；本层读层位栅格、建地层格架、粗化井曲线、按断层阻断做
// IDW 充填，并把属性体登记为 catalog DERIVED（类型 property_volume）。
// 不持有部件、不画像素。深度域与层位 z 必须同号（bot > top）；对不上时
// 粗化得到无值，不把时间面假装成深度。

struct PropertyModelRequest
{
  QString propertyName = QStringLiteral("PROP");
  QString topName;
  QString botName;
  QString topPath;
  QString botPath;
  bool useEmbeddedSurfaces = false;
  paleo::stratgrid::SurfaceGrid top;
  paleo::stratgrid::SurfaceGrid bot;
  int nLayers = 10;
  paleo::stratgrid::Aggregator aggregator = paleo::stratgrid::Aggregator::ThicknessWeightedMean;
  double idwPower = 2.0;
  std::vector<paleo::stratgrid::WellCurve> wells;
  std::vector<paleo::stratgrid::FaultSegment> faults;
};

struct PropertyModelOutput
{
  bool ok = false;
  QString error;
  QString path;
  QString assetId;
  QString versionId;
  QString paramHash;
  int liveColumns = 0;
  int filledCells = 0;
  int unfilledLiveCells = 0;
  paleo::stratgrid::PropertyVolume volume;
};

class PropertyModelWorkflow : public QObject
{
  Q_OBJECT
public:
  explicit PropertyModelWorkflow(DataCatalog *catalog, const QString &projectDir,
                                 QObject *parent = nullptr);
  void rebind(DataCatalog *catalog, const QString &projectDir);

  // progress(fraction, stage) 返回 false → 取消，不登记版本。
  PropertyModelOutput run(const PropertyModelRequest &request,
                          const std::function<bool(double, const QString &)> &progress = {});

  static bool loadSurface(const QString &path, paleo::stratgrid::SurfaceGrid *out,
                          QString *error = nullptr);
  // LINESTRING / POLYGON 外环 → 线段。解析不出点 → 空，不臆造。
  static std::vector<paleo::stratgrid::FaultSegment> segmentsFromWkt(const QString &wkt);
  static std::vector<paleo::stratgrid::FaultSegment>
  segmentsFromFaultSet(const paleo::fault::FaultSet &faults);

  // 输入面、井曲线与参数的稳定摘要（无时间戳）。同输入同哈希。
  static QString paramHash(const PropertyModelRequest &request,
                           const paleo::stratgrid::SurfaceGrid &top,
                           const paleo::stratgrid::SurfaceGrid &bot);

signals:
  void modelStored(const QString &path);
  void modelFailed(const QString &reason);

private:
  DataCatalog *m_catalog = nullptr;
  QString m_projectDir;
};
