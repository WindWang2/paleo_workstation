// 层：数据
#include "areamodel.h"
#include "lasdoc.h"
#include <paleo_data/service.h>
#include <QSet>
#include <QFileInfo>

namespace paleo::area_model {
Read readLog(const QString &path, LasHeaderInfo &header, QList<LasCurve> &curves, QString *error) {
  paleo::data::DataService service;
  long long source = 0;
  const auto state = openSource(path, service, source, error);
  if (state != Read::Ready) return state;
  std::vector<paleo::data::DataEntry> entries;
  std::string detail;
  QSet<qlonglong> sets;
  if (!service.listSourceData("curves", source, entries, detail)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
  }
  long long well = 0;
  for (const auto &entry : entries) if (entry.sourceId == source) { sets.insert(entry.parentId); well = entry.wellId; }
  if (sets.size() != 1) {
    if (error) *error = QStringLiteral("area source requires an explicit curve group selection");
    return Read::Failed;
  }
  paleo::data::LogSet logs;
  if (!service.readLogSet(*sets.begin(), true, logs, detail)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
  }
  header = {}; curves.clear();
  header.indexBasis = QString::fromUtf8(logs.depthInfo.depthType);
  LasCurve axis; axis.name = header.indexBasis.isEmpty() ? QStringLiteral("DEPTH") : header.indexBasis;
  axis.unit = QString::fromUtf8(logs.depthInfo.depthUnit);
  axis.values.assign(logs.samplingAxis.depthValues.begin(), logs.samplingAxis.depthValues.end());
  curves.append(axis); header.curveNames << axis.name;
  const bool xml = QFileInfo(path).suffix().compare("xml", Qt::CaseInsensitive) == 0;
  const auto appendDepth = [&](const QString &name, const std::vector<double> &values) {
    LasCurve axis; axis.name = name; axis.unit = QString::fromUtf8(logs.depthInfo.depthUnit);
    axis.values.assign(values.begin(), values.end());
    if (axis.values.empty()) axis.values.assign(logs.samplingAxis.depthValues.size(), paleo::data::Missing);
    header.curveNames << name; curves.append(axis);
  };
  if (xml || !logs.samplingAxis.tvdValues.empty()) appendDepth("TVD", logs.samplingAxis.tvdValues);
  if (xml || !logs.samplingAxis.tvdssValues.empty()) appendDepth("TVDSS", logs.samplingAxis.tvdssValues);
  for (const auto &curve : logs.logCurves) {
    LasCurve value; value.name = QString::fromUtf8(curve.curveName); value.unit = QString::fromUtf8(curve.curveUnit);
    value.descr = QString::fromUtf8(curve.description);
    value.values.assign(curve.sampleValues.begin(), curve.sampleValues.end());
    header.curveNames << value.name; curves.append(value);
  }
  if (well > 0) {
    paleo::data::WellInfo info;
    if (!service.readWellInfo(well, info, detail)) { if (error) *error = QString::fromUtf8(detail); return Read::Failed; }
    header.wellName = QString::fromUtf8(info.wellName);
  }
  header.sawAscii = true;
  return Read::Ready;
}
}
