// 层：功能
#pragma once
#include "services/frozenrasterinput.h"
#include "services/algorithmspec.h"
#include <QJsonObject>
#include <QJsonArray>
class PaleoOnnxService;
namespace paleo {
struct FrozenTilePlan {
  FrozenRasterInput raster;
  QString model, modelSha256, featureSchemaHash, inputSignature, outputSemantics;
  int classCount = 0;
  int featureContractVersion = 0;
  QJsonArray modelFeatures;
  QVariantMap parameters;
  QJsonObject toJson() const;
  static bool fromJson(const QJsonObject &, FrozenTilePlan &, QString *error);
};
// Disk/model work runs only on an existing task worker.
bool prepareTilePlan(PaleoOnnxService *,const QString &model,const FrozenRasterInput &source,
                     const QVariantMap &parameters,FrozenTilePlan &out,
                     const std::function<bool()> &cancelled,QString *error);
AlgorithmRunResult computeTilePlan(PaleoOnnxService *,const FrozenTilePlan &,const QString &privateDir,
                                   QJsonObject &summary,const std::function<bool()> &cancelled);
}
