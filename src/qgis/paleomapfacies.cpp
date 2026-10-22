// 层：QGIS 封装
#include "../domain/paleomapfacies.h"
#include "../domain/faciescatalog.h"
#include "paleomapsource.h"
#include <QFile>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <memory>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
namespace
{
int field(const QgsFields &fields, const QStringList &names)
{
  for (const auto &name : names)
    for (int i = 0; i < fields.size(); ++i)
      if (fields.at(i).name().compare(name, Qt::CaseInsensitive) == 0)
        return i;
  return -1;
}
QVariantMap sourceClass(const QgsFeature &f, int micro, int phase, int sub)
{
  return {{"microfacies", f.attribute(micro).toString().trimmed()},
          {"facies", phase < 0 || phase == micro ? QString() : f.attribute(phase).toString().trimmed()},
          {"subfacies", sub < 0 || sub == micro ? QString() : f.attribute(sub).toString().trimmed()}};
}
QString key(const QVariantMap &row)
{
  return QString::fromUtf8(QJsonDocument::fromVariant(row).toJson(QJsonDocument::Compact));
}
} // namespace
namespace PaleoMapSource
{
QVariantMap normalizeFacies(const QString &source, const QString &output, const QVariantList &schema,
                            const QString &microField, const std::function<bool()> &cancelled, QString *error, const QString &targetCrs)
{
  QgsVectorLayer layer(source, "source", "ogr");
  if (!layer.isValid() || layer.geometryType() != Qgis::GeometryType::Polygon)
    return {};
  const auto fields = layer.fields();
  const int micro = field(fields, microField.isEmpty()
                                      ? QStringList{"microfacies", "micro_facies", "microfacie", "micro_faci", "microfac",
                                                    "微相", "微相名", "微相名称", "facies_label",
                                                    "facies_lab", "facies", "相名", "name", "名称", "岩相"}
                                      : QStringList{microField});
  if (micro < 0)
  {
    if (!microField.isEmpty() && error)
      *error = QObject::tr("未找到指定的微相字段：%1").arg(microField);
    return {};
  }
  const int phase = field(fields, {"facies_name", "facies_nam", "相", "大相", "沉积相", "facies"});
  const int sub = field(fields, {"subfacies", "sub_facies", "亚相", "亚相名", "亚相名称"});
  QMap<QString, QVariantMap> classes;
  auto iterator = layer.getFeatures(QgsFeatureRequest().setFlags(Qgis::FeatureRequestFlag::NoGeometry));
  QgsFeature feature;
  while (iterator.nextFeature(feature))
  {
    if (cancelled && cancelled())
      return {};
    const auto row = sourceClass(feature, micro, phase, sub);
    classes.insert(key(row), row);
  }
  if (classes.isEmpty())
  {
    if (error)
      *error = QObject::tr("图件没有可转换的相面要素");
    return {};
  }
  QVariantList rows;
  for (const auto &row : classes)
    rows << row;
  const auto resolved = PaleoMapFacies::resolve(schema, rows, error);
  if (resolved.isEmpty())
  {
    if (microField.isEmpty() &&
        QStringList{"name", "名称", "岩相"}.contains(fields.at(micro).name(), Qt::CaseInsensitive) && error)
      error->clear();
    return {};
  }
  QMap<QString, int> codes;
  int n = 0;
  for (auto it = classes.cbegin(); it != classes.cend(); ++it)
    codes.insert(it.key(), resolved.value("codes").toList().at(n++).toInt());
  const auto normalized = resolved.value("facies").toList();
  QgsFields outputFields;
  QList<int> originalFields;
  const QStringList canonical{"facies_code", "facies_name",  "subfacies",
                              "microfacies", "facies_label", "texture", "color"};
  for (int i = 0; i < fields.size(); ++i)
  {
    const auto name = fields.at(i).name().toLower();
    const bool legacyAppearance = QStringList{"facies_code", "facies_cod", "code", "color"}.contains(name);
    if (canonical.contains(name) && !legacyAppearance)
      continue;
    auto preserved = fields.at(i);
    if (legacyAppearance) {
      const QString base = QStringLiteral("source_") + fields.at(i).name();
      QString preservedName = base;
      int suffix = 2;
      while (field(fields, {preservedName}) >= 0 || outputFields.indexFromName(preservedName) >= 0)
        preservedName = base + QStringLiteral("_%1").arg(suffix++);
      preserved.setName(preservedName);
    }
    outputFields.append(preserved);
    originalFields << i;
  }
  outputFields.append(QgsField("facies_code", QMetaType::Type::Int));
  for (int i = 1; i < canonical.size(); ++i)
    outputFields.append(QgsField(canonical[i], QMetaType::Type::QString));
  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = "GPKG";
  options.layerName = "facies";
  options.fileEncoding = "UTF-8";
  const auto destinationCrs = targetCrs.isEmpty() ? layer.crs() : QgsCoordinateReferenceSystem(targetCrs);
  QgsCoordinateTransform transform(layer.crs(), destinationCrs, QgsCoordinateTransformContext());
  if (!destinationCrs.isValid()) { if (error) *error = QObject::tr("工程基准坐标系无效"); return {}; }
  std::unique_ptr<QgsVectorFileWriter> writer(QgsVectorFileWriter::create(
      output, outputFields, layer.wkbType(), destinationCrs, QgsCoordinateTransformContext(), options));
  if (!writer || writer->hasError() != QgsVectorFileWriter::NoError)
  {
    if (error)
      *error = writer ? writer->errorMessage() : QObject::tr("无法创建三级相副本");
    return {};
  }
  iterator = layer.getFeatures();
  while (iterator.nextFeature(feature))
  {
    if (cancelled && cancelled())
    {
      writer.reset();
      QFile::remove(output);
      return {};
    }
    const int code = codes.value(key(sourceClass(feature, micro, phase, sub)));
    auto attrs = FaciesCatalog::attributes(normalized, code);
    attrs.insert("color", FaciesCatalog::appearance(FaciesCatalog::find(normalized, code)).value("color"));
    QgsFeature converted(outputFields);
    auto geometry = feature.geometry();
    try { if (layer.crs() != destinationCrs) geometry.transform(transform); }
    catch (const QgsCsException &) { if (error) *error = QObject::tr("相图坐标转换失败"); return {}; }
    converted.setGeometry(geometry);
    QgsAttributes attributes;
    for (const auto i : originalFields)
      attributes << feature.attribute(i);
    attributes << QVariant(code);
    for (int i = 1; i < canonical.size(); ++i)
      attributes << attrs.value(canonical[i]);
    converted.setAttributes(attributes);
    if (!writer->addFeature(converted))
    {
      if (error)
        *error = writer->lastError();
      writer.reset();
      QFile::remove(output);
      return {};
    }
  }
  if (!writer->flushBuffer())
  {
    if (error)
      *error = writer->lastError();
    writer.reset();
    QFile::remove(output);
    return {};
  }
  writer.reset();
  return {{"facies", normalized},
          {"source_type", "vector"},
          {"layer_type", "vector"},
          {"source_suffix", "|layername=facies"},
          {"spatial", layer.crs().isValid()},
          {"polygon", true},
          {"crs", destinationCrs.toWkt()},
          {"feature_count", layer.featureCount()},
          {"facies_schema", "paleo_facies_hierarchy_v1"},
          {"prediction_level", "micro_facies"},
          {"source_microfacies_field", fields.at(micro).name()}};
}
} // namespace PaleoMapSource
