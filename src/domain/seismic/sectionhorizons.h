// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <vector>

namespace seismic {
struct SectionHorizonCurve {
  QString id, name;
  int internalEdge=0;
  bool provisional=false;
  std::vector<double> timesMs; // displayed column order; non-finite values break the line
};
struct SectionHorizonBand {
  QString id, name, topCurveId, bottomCurveId;
  int subdivision=0;
};
struct SectionHorizons {
  std::vector<SectionHorizonCurve> curves;
  QString schemeId, revision;
  QStringList warnings;
  std::vector<SectionHorizonBand> bands;
};
}
