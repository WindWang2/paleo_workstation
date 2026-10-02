// 层：数据
#pragma once
#include "catalog/datacatalog.h"
#include "crossplotsamples.h"
#include "metadata/layermanifest.h"
namespace paleo::crossplot {
struct SourceSpec {
  SourceChoice choice;
  QString path, curve, versionId, layerId, sha256;
  Location well;
  bool managed = true;
  bool timeHorizon = false;
};
class CrossplotSources {
public:
  static QVector<SourceSpec> inventory(DataCatalog *, const QString &projectDir,
                                       const QVector<LayerDeclaration> &);
  static SampleResult load(const QVector<SourceSpec> &,
                           const cluster::Control & = {});
  // Reads existing SATR, validates its version/payload, and uses its source
  // survey.
  static bool attributeSection(const QString &path, AttributeSection *,
                               QString *error, const cluster::Control & = {});
};
} // namespace paleo::crossplot
