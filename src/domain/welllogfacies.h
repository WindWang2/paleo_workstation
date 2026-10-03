// 层：数据
#pragma once
#include "wellcompositemodel.h"
#include <QJsonArray>
#include <QJsonObject>

struct WellFaciesModel {
  QString id, name, version, formationGroup;
  QStringList curves, labels;
  int window = 1;
};
struct WellFaciesInput {
  QJsonArray rows;
  QString reason;
  bool ready() const { return reason.isEmpty() && !rows.isEmpty(); }
};
struct WellFaciesResult {
  QString jobId, modelName, modelVersion;
  QVector<WellComposite::TextInterval> intervals;
  WellComposite::CurveData confidence;
  QJsonObject response;
};
Q_DECLARE_METATYPE(WellFaciesResult)

WellFaciesInput
prepareWellFaciesInput(const WellComposite::ComprehensiveWellData &data,
                       const WellFaciesModel &model);
bool parseWellFaciesResult(const QJsonObject &json, const QString &wellName,
                           const WellFaciesInput &input,
                           WellFaciesResult *result, QString *error);
