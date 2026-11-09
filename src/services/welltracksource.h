// 层：数据
#pragma once

#include "domain/documentsource.h"
#include "domain/wellcompositemodel.h"
#include <paleo_data/curve_window.h>
#include <functional>

struct AreaWellCurveWindow;

namespace paleo::welltrack {

struct CurveChoice {
  qlonglong sourceId = 0, curveId = 0, logSetId = 0;
  QString fileName, curveName;
  document::SourceRef source;
  QString filePath;
};

// Enumerates registered LAS curves for one model well without reading samples.
bool listCurveChoices(const QString &modelDirectory, const QString &documentId, qlonglong wellId,
                      QVector<CurveChoice> &out, QString *error = nullptr,
                      const std::function<bool()> &cancelled = {});

struct CurveReadResult {
  WellComposite::CurveData curve;
  data::CurveReadStats stats;
  QString error;
  bool ok() const { return error.isEmpty() && !curve.isEmpty(); }
};

// Reads the exact registered curve within a budgeted MD window (metres).
CurveReadResult readCurve(const document::SourceRecord &source, qlonglong wellId, qlonglong logSetId,
                          qlonglong curveId, const AreaWellCurveWindow &window,
                          const std::function<bool()> &cancelled = {});

} // namespace paleo::welltrack
