// 层：QGIS 封装
#pragma once
#include "../domain/wellcompositemodel.h"
#include <QVariantList>
class DataCatalog;
class QgisLayerService;
class QgsVectorLayer;

// 可变地质工作表独立于可再生成的井位和不可变预测版本。
// 井段：每井不重叠 MD 段，岩性与相分列；因子：每井/层位唯一行。
namespace WellAttributeStore {
QString intervalLayerId();
QString factorLayerId();
QgsVectorLayer *open(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, bool factors, bool create,
                     QString *error = nullptr);
QVariantList rows(const QString &dir, QgisLayerService *layers, bool factors,
                  const QString &wellId = {}, const QString &horizon = {});
bool seed(DataCatalog *catalog, const QString &dir, QgisLayerService *layers,
          const QString &wellId,
          const WellComposite::ComprehensiveWellData &data,
          QString *error = nullptr);
// updates: well_id, top_md, base_md，以及要维护的属性；仅覆盖给出的字段，
// 边界变化时切分旧段，保留岩性和自定义属性。整批在一个 edit buffer 提交。
bool mergeIntervals(DataCatalog *catalog, const QString &dir,
                    QgisLayerService *layers, const QVariantList &updates,
                    QString *error = nullptr, bool onlyMissing = false);
bool seedFactors(DataCatalog *catalog, const QString &dir,
                 QgisLayerService *layers, const QVariantList &updates,
                 QString *error = nullptr);
void apply(const QVariantList &intervals,
           WellComposite::ComprehensiveWellData *data);
bool validate(QgsVectorLayer *layer, DataCatalog *catalog, bool factors,
              QString *error = nullptr);
} // namespace WellAttributeStore
