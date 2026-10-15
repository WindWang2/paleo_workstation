// 层：QGIS 封装
#pragma once
#include "../domain/wellcompositemodel.h"
#include <QHash>
#include <QVariantList>
class DataCatalog;
class QgisLayerService;
class QgsVectorLayer;

// 可变地质工作表独立于可再生成的井位和不可变预测版本。
// 井段：每井不重叠 MD 段，岩性与相分列；因子：每井/层位唯一行；
// 地层优势相：每井/地层唯一行（组、段各一行），以属性列维护优势相、厚度、
// 占比与来源——数据管理的逐组预测与智能预测的测井预测都写这一张表。
namespace WellAttributeStore {
enum class Table { Intervals, Factors, Formations };
// facies-ux 3：工程内各层位的当前相分类（facies_schema 资产，displayName
// = facies-schema-<层位>）；测井相点着色与图例用。
QHash<QString, QVariantList> faciesSchemas(DataCatalog *catalog);
QString intervalLayerId();
QString factorLayerId();
QString formationLayerId();
QString layerId(Table table);
QgsVectorLayer *open(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, Table table, bool create,
                     QString *error = nullptr);
QgsVectorLayer *open(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, bool factors, bool create,
                     QString *error = nullptr);
// key：井段/因子表按 horizon 过滤，地层优势相表按 formation 过滤；空 = 不过滤。
QVariantList rows(const QString &dir, QgisLayerService *layers, Table table,
                  const QString &wellId = {}, const QString &key = {});
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
// updates: well_id, formation, top_md, base_md，以及要维护的属性。按
// well_id + formation 更新或新增，只覆盖给出的字段，保留未给出的字段和自定义
// 属性；整批在一个 edit buffer 提交，任一行无效则整批不写。
bool mergeFormations(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, const QVariantList &updates,
                     QString *error = nullptr, bool onlyMissing = false);
// 井段 + 地层相两批的事务化包装：井段批先提交，地层批失败时按批前快照
// 恢复井段行（半更新域状态会让重试重复合并且读数自相矛盾）。
bool mergeIntervalsAndFormations(DataCatalog *catalog, const QString &dir,
                                 QgisLayerService *layers,
                                 const QVariantList &intervalUpdates,
                                 const QVariantList &formationUpdates,
                                 QString *error = nullptr,
                                 bool onlyMissing = false);
bool seedFactors(DataCatalog *catalog, const QString &dir,
                 QgisLayerService *layers, const QVariantList &updates,
                 QString *error = nullptr);
void apply(const QVariantList &intervals,
           WellComposite::ComprehensiveWellData *data);
bool validate(QgsVectorLayer *layer, DataCatalog *catalog, Table table,
              QString *error = nullptr);
bool validate(QgsVectorLayer *layer, DataCatalog *catalog, bool factors,
              QString *error = nullptr);
} // namespace WellAttributeStore
