// 层：数据
#include "faciesclassificationservice.h"
#include "crossplotsamples.h"
#include "metadata/paleoprojectstore.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <memory>
namespace paleo::crossplot {
namespace {
QVariantList list(const std::vector<double> &v) {
  QVariantList out;
  for (double x : v)
    out << x;
  return out;
}
Classification failed(const QString &e, bool cancel = false) {
  Classification r;
  r.error = e;
  r.cancelled = cancel;
  return r;
}
bool error(QString *e, const QString &t) {
  if (e)
    *e = t;
  return false;
}
} // namespace
Classification FaciesClassificationService::classify(
    const SampleSet &s, const ClassificationOptions &o,
    const cluster::Control &ctl, const std::vector<int> &previous) {
  QString validation;
  if (!CrossplotSamples::validate(s, &validation))
    return failed(validation);
  if (!s.rows())
    return failed(QStringLiteral("没有有效样本；未生成分类"));
  if (o.k < 1 || o.k > 255 || o.maxIterations < 1 || o.manualClass < 0 ||
      o.manualClass > 254)
    return failed(QStringLiteral("分类参数超出有效范围"));
  if (ctl.cancelled && ctl.cancelled())
    return failed(QStringLiteral("已取消"), true);
  const auto n = s.rows(), d = std::size_t(s.names.size());
  Classification r;
  QVariantMap params{{"method", int(o.method)},
                     {"k", o.k},
                     {"maxIterations", o.maxIterations},
                     {"seed", QString::number(o.seed)},
                     {"standardize", o.standardize},
                     {"varianceFloor", 1e-6},
                     {"tolerance", 1e-6},
                     {"dimensions", s.names},
                     {"units", s.units}};
  params.insert("sampling", s.samplingMetadata);
  params.insert("referenceRows", s.grid.rows);
  params.insert("referenceCols", s.grid.cols);
  params.insert("referenceCRS", s.grid.crs);
  QVariantList referenceTransform;
  for (double v : s.grid.transform)
    referenceTransform << v;
  params.insert("referenceTransform", referenceTransform);
  if (o.method == Classifier::KMeans || o.method == Classifier::Gmm) {
    cluster::Matrix matrix{d, s.values};
    std::vector<double> mean(d, 0), sd(d, 1);
    if (o.standardize) {
      std::vector<double> m2(d, 0);
      for (std::size_t i = 0; i < n; ++i) {
        if ((i & 1023U) == 0 && ctl.cancelled && ctl.cancelled())
          return failed(QStringLiteral("已取消"), true);
        for (std::size_t j = 0; j < d; ++j) {
          const double delta = matrix.values[i * d + j] - mean[j];
          mean[j] += delta / double(i + 1);
          m2[j] += delta * (matrix.values[i * d + j] - mean[j]);
        }
      }
      for (std::size_t j = 0; j < d; ++j)
        sd[j] = m2[j] > 0 ? std::sqrt(m2[j] / double(n)) : 1;
      for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < d; ++j)
          matrix.values[i * d + j] =
              (matrix.values[i * d + j] - mean[j]) / sd[j];
    }
    cluster::Options options;
    options.k = o.k;
    options.maxIterations = o.maxIterations;
    options.seed = o.seed;
    auto result = o.method == Classifier::KMeans
                      ? cluster::kmeans(matrix, options, ctl)
                      : cluster::gmm(matrix, options, ctl);
    if (!result.ok)
      return failed(QString::fromStdString(result.error), result.cancelled);
    r.labels = std::move(result.labels);
    r.confidence = std::move(result.confidence);
    r.squaredDistance = std::move(result.squaredDistance);
    params.insert("normalizationMean", list(mean));
    params.insert("normalizationSd", list(sd));
    params.insert("centersNormalized", list(result.model.centers.values));
    params.insert("weights", list(result.model.weights));
    params.insert("variancesNormalized", list(result.model.variances));
    params.insert("iterations", result.iterations);
    params.insert("effectiveK", int(result.model.centers.rows()));
    params.insert("objectiveHistory", list(result.objectiveHistory));
    if (o.method == Classifier::Gmm)
      params.insert("BIC", result.bic);
    params.insert("confidence", o.method == Classifier::Gmm
                                    ? "maximum_posterior"
                                    : "relative_squared_distance_margin");
  } else if (o.method == Classifier::Hull || o.method == Classifier::Box) {
    if (o.selection.size() < 3)
      return failed(QStringLiteral("手选分类需要有效套索或框选"));
    auto frame = CrossplotSamples::project(s, o.axes);
    auto selected = CrossplotSamples::select(s, frame, o.selection);
    if (selected.indices.isEmpty())
      return failed(QStringLiteral("选区内没有样本"));
    r.labels = previous.size() == n ? previous : std::vector<int>(n, -1);
    r.confidence.assign(n, 0);
    r.squaredDistance.assign(n, 0);
    std::vector<cluster::Point> vertices;
    QVariantList polygon;
    for (auto p : o.selection) {
      vertices.push_back({p.x(), p.y()});
      polygon << QVariant(QVariantList{p.x(), p.y()});
    }
    const auto hull = cluster::convexHull(vertices);
    std::vector<double> lo(d, std::numeric_limits<double>::infinity()),
        hi(d, -std::numeric_limits<double>::infinity());
    for (int i : selected.indices)
      for (std::size_t j = 0; j < d; ++j) {
        lo[j] = std::min(lo[j], s.values[std::size_t(i) * d + j]);
        hi[j] = std::max(hi[j], s.values[std::size_t(i) * d + j]);
      }
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0) {
        if (ctl.cancelled && ctl.cancelled())
          return failed(QStringLiteral("已取消"), true);
        if (ctl.progress)
          ctl.progress(double(i) / double(n));
      }
      const auto &p = frame.points[qsizetype(i)];
      const bool inside = o.method == Classifier::Hull
                              ? cluster::inPolygon({p.x, p.y}, hull)
                              : cluster::inBox(s.values.data() + i * d, lo, hi);
      if (inside)
        r.labels[i] = o.manualClass;
      r.confidence[i] = r.labels[i] >= 0 ? 1 : 0;
    }
    params.insert("manualClass", o.manualClass);
    params.insert("lasso", polygon);
    params.insert("axes", QVariantList{o.axes.x, o.axes.y, o.axes.z, o.axes.yaw,
                                       o.axes.pitch});
    params.insert("boxLo", list(lo));
    params.insert("boxHi", list(hi));
    params.insert("confidence", "rule_membership_not_probability");
  } else
    return failed(QStringLiteral("未知分类器"));
  if (std::any_of(r.labels.begin(), r.labels.end(),
                  [](int label) { return label < -1 || label > 254; }))
    return failed(QStringLiteral("类别编号超出分类栅格编码范围"));
  int classes = 0;
  for (int l : r.labels)
    classes = std::max(classes, l + 1);
  r.counts.fill(0, classes);
  for (int l : r.labels)
    if (l >= 0)
      ++r.counts[l];
  const QByteArray json =
      QJsonDocument::fromVariant(params).toJson(QJsonDocument::Compact);
  r.provenance = params;
  r.provenance.insert(
      "parameterHash",
      QString::fromLatin1(
          QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex()));
  QCryptographicHash sampleHash(QCryptographicHash::Sha256);
  sampleHash.addData(
      QByteArrayView(reinterpret_cast<const char *>(s.values.data()),
                     qsizetype(s.values.size() * sizeof(double))));
  r.provenance.insert("sampleValuesHash",
                      QString::fromLatin1(sampleHash.result().toHex()));
  r.provenance.insert("sampleCount", qint64(n));
  r.provenance.insert("rejected", s.rejected);
  r.provenance.insert("origin", "crossplot_unsupervised");
  r.provenance.insert("geologicalMeaning", "unassigned_cluster_ids");
  r.ok = true;
  if (ctl.progress)
    ctl.progress(1);
  return r;
}
QVector<WellInterval>
FaciesClassificationService::intervals(const SampleSet &s,
                                       const Classification &r) {
  QVector<WellInterval> out;
  if (!r.ok || r.labels.size() != s.rows() || r.confidence.size() != s.rows())
    return out;
  int lastRow = -2;
  for (std::size_t i = 0; i < s.rows(); ++i) {
    const auto &loc = s.locations[qsizetype(i)];
    const int code = r.labels[i];
    if (loc.wellId.isEmpty() || code < 0) {
      lastRow = -2;
      continue;
    }
    const bool adjacent = !out.isEmpty() && out.last().wellId == loc.wellId &&
                          out.last().classId == code &&
                          loc.depth > out.last().base &&
                          (loc.sourceRow < 0 || loc.sourceRow == lastRow + 1);
    if (adjacent) {
      auto &v = out.last();
      v.base = loc.depth;
      v.meanConfidence = (v.meanConfidence * v.sampleCount + r.confidence[i]) /
                         double(v.sampleCount + 1);
      ++v.sampleCount;
    } else
      out << WellInterval{loc.wellId,      loc.depth, loc.depth,
                          r.confidence[i], code,      1};
    lastRow = loc.sourceRow;
  }
  return out;
}
bool FaciesClassificationService::writeRaster(const QString &path,
                                              const SampleSet &s,
                                              const Classification &r,
                                              QString *e) {
  if (!PaleoProjectStore::isWriteQueueActive())
    return error(e, QStringLiteral("分类写入必须经过项目写队列"));
  if (!CrossplotSamples::validate(s) || !r.ok || r.labels.size() != s.rows() ||
      r.confidence.size() != s.rows() || r.squaredDistance.size() != s.rows() ||
      !s.grid.spatial || s.grid.cols <= 0 || s.grid.rows <= 0)
    return error(e, QStringLiteral("分类或地图栅格几何无效"));
  if (std::any_of(r.labels.begin(), r.labels.end(),
                  [](int label) { return label < -1 || label > 254; }))
    return error(e, QStringLiteral("类别编号超出 Byte palette 编码范围"));
  const auto size = std::size_t(s.grid.rows) * std::size_t(s.grid.cols);
  std::vector<unsigned char> labels(size, 255);

  std::vector<bool> used(size, false);
  for (std::size_t i = 0; i < s.rows(); ++i) {
    const int pixel = s.locations[qsizetype(i)].pixel;
    if (pixel < 0 || std::size_t(pixel) >= size || used[std::size_t(pixel)])
      return error(e, QStringLiteral("样本像元映射无效或重复"));
    used[std::size_t(pixel)] = true;
    if (r.labels[i] >= 0) {
      labels[std::size_t(pixel)] = static_cast<unsigned char>(r.labels[i]);
    }
  }
  GDALAllRegister();
  std::unique_ptr<void, decltype(&GDALClose)> ds(
      GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(),
                 s.grid.cols, s.grid.rows, 1, GDT_Byte, nullptr),
      GDALClose);
  if (!ds)
    return error(e, QStringLiteral("无法创建分类栅格"));
  auto gt = s.grid.transform;
  if (GDALSetGeoTransform(ds.get(), gt.data()) != CE_None ||
      (!s.grid.crs.isEmpty() &&
       GDALSetProjection(ds.get(), s.grid.crs.toUtf8().constData()) != CE_None))
    return error(e, QStringLiteral("无法写栅格几何"));
  auto band = GDALGetRasterBand(ds.get(), 1);
  GDALSetRasterNoDataValue(band, 255);
  GDALSetRasterColorInterpretation(band, GCI_PaletteIndex);
  auto table = GDALCreateColorTable(GPI_RGB);
  for (int i = 0; i < r.counts.size(); ++i) {
    const auto c = classColor(i);
    GDALColorEntry entry{short(c.red), short(c.green), short(c.blue), 255};
    GDALSetColorEntry(table, i, &entry);
  }
  GDALColorEntry transparent{0, 0, 0, 0};
  GDALSetColorEntry(table, 255, &transparent);
  GDALSetRasterColorTable(band, table);
  GDALDestroyColorTable(table);
  GDALSetMetadataItem(ds.get(), "PALEO_CLASSIFICATION",
                      QJsonDocument::fromVariant(r.provenance)
                          .toJson(QJsonDocument::Compact)
                          .constData(),
                      nullptr);
  if (GDALRasterIO(band, GF_Write, 0, 0, s.grid.cols, s.grid.rows,
                   labels.data(), s.grid.cols, s.grid.rows, GDT_Byte, 0,
                   0) != CE_None)
    return error(e, QStringLiteral("分类像元写入失败"));
  return GDALFlushCache(ds.get()) == CE_None
             ? true
             : error(e, QStringLiteral("分类栅格落盘失败"));
}
bool FaciesClassificationService::writeIntervals(const QString &path,
                                                 const SampleSet &s,
                                                 const Classification &r,
                                                 QString *e) {
  if (!PaleoProjectStore::isWriteQueueActive())
    return error(e, QStringLiteral("井层段写入必须经过项目写队列"));
  const auto rows = intervals(s, r);
  if (rows.isEmpty())
    return error(e, QStringLiteral("没有可写井层段"));
  QJsonArray array;
  for (const auto &v : rows)
    array << QJsonObject{{"wellId", v.wellId},
                         {"top", v.top},
                         {"base", v.base},
                         {"classId", v.classId},
                         {"meanConfidence", v.meanConfidence},
                         {"sampleCount", v.sampleCount}};
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return error(e, file.errorString());
  const auto bytes =
      QJsonDocument(
          QJsonObject{
              {"schema", 1},
              {"intervalSupport", "inclusive_observed_depths_no_gap_bridging"},
              {"provenance", QJsonObject::fromVariantMap(r.provenance)},
              {"intervals", array}})
          .toJson();
  if (file.write(bytes) != bytes.size() || !file.commit())
    return error(e, file.errorString());
  return true;
}
} // namespace paleo::crossplot
