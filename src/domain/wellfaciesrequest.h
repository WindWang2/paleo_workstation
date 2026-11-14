// 层：数据
#pragma once
#include "wellref.h"
#include "documentrelation.h"
#include "stratigraphicunit.h"
#include "welllogfacies.h"
#include <QHash>

namespace paleo::wellfacies {
using ContextKey = document::InterpretationContext;
struct UnitScope {
  strat::UnitSelection unit;
  QStringList occurrenceKeys; // Empty selects all occurrences separately, never their MIN/MAX hull.
  bool valid() const;
};
struct Request {
  document::WellRef well;
  ContextKey context;
  UnitScope scope;
  QString clientRequestId, endpointId, modelId, modelVersion;
  QHash<QString, qlonglong> curveIds; // Explicit model feature -> SDK curve ID; absent requires a unique source.
  qlonglong lithologyTrackId = 0; // Absent requires exactly one compatible lithology track.
};
struct CurveRef {
  QString feature, unit, sourceFingerprint;
  qlonglong curveId = 0, sourceId = 0, logSetId = 0;
  QString sourceDepthType,sourceDepthUnit,mdReference,sourceName;
};
struct OccurrenceWindow {
  QString key, originalDepthType, originalDepthUnit;
  double topMd = qQNaN(), bottomMd = qQNaN();
  bool derivedDivisionGeometry = false;
};
struct PreparedInput {
  bool ok = false;
  QString code, error, wellName, featureSchemaHash, inputHash;
  WellFaciesModel model;
  QVector<CurveRef> curves;
  QVector<OccurrenceWindow> occurrences;
  QVector<WellFaciesInput> segments;
  QVector<QString> segmentOccurrences;
  QVector<QByteArray> requestBodies;
  QStringList requestHashes;
  QStringList skipped;
  QJsonObject sourceManifest;
};
bool requestFromJson(const QJsonObject &json, Request &request, QString *code = nullptr);
QJsonObject unitScopeJson(const UnitScope &scope);
QJsonObject requestJsonSchema();
QString modelReadiness(const WellFaciesModel &model, const QString &id, const QString &version);
bool declaredSemanticToken(const QString &value);
QJsonObject frozenModelJson(const WellFaciesModel &model);
bool frozenModelFromJson(const QJsonObject &json, WellFaciesModel &model);
} // namespace paleo::wellfacies
