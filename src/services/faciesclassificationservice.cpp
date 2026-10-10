// 层：数据
#include "faciesclassificationservice.h"
#include "algorithms/cluster/som.h"
#include "crossplotsamples.h"
#include "faciestraining.h"
#include "metadata/paleoprojectstore.h"
#include <QCryptographicHash>
#include <QFile>
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
// 写栅格失败回收残件：GDALCreate 已在最终路径建文件，后续失败若不删，
// 半本 GTiff 会被按「文件存在」的消费方当有效产物。先关数据集再删
// （Windows 下打开中的句柄不可删）。
void removePartialGtiff(std::unique_ptr<void, decltype(&GDALClose)> &ds,
                        const QString &path) {
  ds.reset();
  QFile::remove(path);
}
// 样本行 → 参考网格像元；越界/重复即拒绝（分类栅格与掩膜件同一映射口径）。
bool pixelOf(const SampleSet &s, std::size_t row, const std::vector<bool> &used,
             int &pixel, QString *e) {
  const auto size = std::size_t(s.grid.rows) * std::size_t(s.grid.cols);
  const int p = s.locations[qsizetype(row)].pixel;
  if (p < 0 || std::size_t(p) >= size || used[std::size_t(p)])
    return error(e, QStringLiteral("样本像元映射无效或重复"));
  pixel = p;
  return true;
}
// 分类（调色板）栅格共用几何/色表/provenance 落盘——writeRaster 与掩膜件
// 必须逐位同口径，否则同层位两图对不上。
bool setupClassificationRaster(GDALDatasetH ds, const SampleSet &s,
                               const Classification &r, QString *e) {
  auto gt = s.grid.transform;
  if (GDALSetGeoTransform(ds, gt.data()) != CE_None ||
      (!s.grid.crs.isEmpty() &&
       GDALSetProjection(ds, s.grid.crs.toUtf8().constData()) != CE_None))
    return error(e, QStringLiteral("无法写栅格几何"));
  auto band = GDALGetRasterBand(ds, 1);
  if (GDALSetRasterNoDataValue(band, 255) != CE_None ||
      GDALSetRasterColorInterpretation(band, GCI_PaletteIndex) != CE_None)
    return error(e, QStringLiteral("分类 nodata 或 palette 类型写入失败"));
  auto table = GDALCreateColorTable(GPI_RGB);
  for (int i = 0; i < r.counts.size(); ++i) {
    const auto c = classColor(i);
    GDALColorEntry entry{short(c.red), short(c.green), short(c.blue), 255};
    GDALSetColorEntry(table, i, &entry);
  }
  GDALColorEntry transparent{0, 0, 0, 0};
  GDALSetColorEntry(table, 255, &transparent);
  const auto paletteResult = GDALSetRasterColorTable(band, table);
  GDALDestroyColorTable(table);
  if (paletteResult != CE_None ||
      GDALSetMetadataItem(ds, "PALEO_CLASSIFICATION",
                          QJsonDocument::fromVariant(r.provenance)
                              .toJson(QJsonDocument::Compact)
                              .constData(),
                          nullptr) != CE_None)
    return error(e, QStringLiteral("分类色表或 provenance 写入失败"));
  return true;
}
bool validClassification(const SampleSet &s, const Classification &r) {
  if (!r.ok || r.labels.size() != s.rows() || r.confidence.size() != s.rows() ||
      r.squaredDistance.size() != s.rows())
    return false;
  QVector<qint64> counts;
  for (std::size_t i = 0; i < s.rows(); ++i) {
    if (r.labels[i] < -1 || r.labels[i] > 254 ||
        !std::isfinite(r.confidence[i]) || r.confidence[i] < 0 ||
        r.confidence[i] > 1 || !std::isfinite(r.squaredDistance[i]) ||
        r.squaredDistance[i] < 0)
      return false;
    if (r.labels[i] >= 0) {
      if (counts.size() <= r.labels[i])
        counts.resize(r.labels[i] + 1);
      ++counts[r.labels[i]];
    }
  }
  return counts == r.counts;
}
} // namespace
bool FaciesClassificationService::standardizeMatrix(
    cluster::Matrix &m, const cluster::Control &ctl, std::vector<double> &mean,
    std::vector<double> &sd, bool *cancelled) {
  const auto n = m.rows(), d = m.dimensions;
  mean.assign(d, 0);
  sd.assign(d, 1);
  std::vector<double> m2(d, 0);
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 1023U) == 0 && ctl.cancelled && ctl.cancelled()) {
      if (cancelled)
        *cancelled = true;
      return false;
    }
    for (std::size_t j = 0; j < d; ++j) {
      const double delta = m.values[i * d + j] - mean[j];
      mean[j] += delta / double(i + 1);
      m2[j] += delta * (m.values[i * d + j] - mean[j]);
    }
  }
  for (std::size_t j = 0; j < d; ++j)
    sd[j] = m2[j] > 0 ? std::sqrt(m2[j] / double(n)) : 1;
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = 0; j < d; ++j)
      m.values[i * d + j] = (m.values[i * d + j] - mean[j]) / sd[j];
  return true;
}
Classification FaciesClassificationService::classify(
    const SampleSet &s, const ClassificationOptions &o,
    const cluster::Control &ctl, const Classification &previous) {
  QString validation;
  if (!CrossplotSamples::validate(s, &validation))
    return failed(validation);
  if (!s.rows())
    return failed(QStringLiteral("没有有效样本；未生成分类"));
  if (o.k < 1 || o.k > 255 || o.maxIterations < 1 || o.manualClass < 0 ||
      o.manualClass > 254)
    return failed(QStringLiteral("分类参数超出有效范围"));
  if (o.method == Classifier::Som &&
      (o.somWidth < 1 || o.somHeight < 1 ||
       qint64(o.somWidth) * qint64(o.somHeight) > 255))
    return failed(QStringLiteral("SOM 原型网格尺寸无效或总簇数超出分类栅格编码范围"));
  if (ctl.cancelled && ctl.cancelled())
    return failed(QStringLiteral("已取消"), true);
  const auto n = s.rows(), d = std::size_t(s.names.size());
  Classification r;
  QString origin = QStringLiteral("crossplot_unsupervised");
  QVariantMap params{{"method", int(o.method)},
                     {"k", o.k},
                     {"maxIterations", o.maxIterations},
                     {"seed", QString::number(o.seed)},
                     {"standardize", o.standardize},
                     {"varianceFloor", 1e-6},
                     {"tolerance", 1e-6},
                     {"dimensions", s.names},
                     {"units", s.units},
                     {"confidenceMaskThreshold", o.confidenceMaskThreshold}};
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
      bool cancelled = false;
      if (!standardizeMatrix(matrix, ctl, mean, sd, &cancelled))
        return failed(QStringLiteral("已取消"), cancelled);
    }
    cluster::Options options;
    options.k = o.k;
    options.maxIterations = o.maxIterations;
    options.seed = o.seed;
    // EM 分块预算：0 = 全量责任度缓冲路径（tst_gmm_chunked 同口径）。
    options.emChunkBudgetBytes = o.emChunkBudgetBytes;
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
    if (o.method == Classifier::Gmm) {
      params.insert("BIC", result.bic);
      params.insert("emChunkBudgetBytes", qint64(o.emChunkBudgetBytes));
      params.insert("chunkedEm", result.chunkedEm);
    }
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
    const bool inherit = previous.ok;
    if (inherit && !validClassification(s, previous))
      return failed(QStringLiteral("已有分类与当前样本不一致"));
    r.labels = inherit ? previous.labels : std::vector<int>(n, -1);
    r.confidence = inherit ? previous.confidence : std::vector<double>(n, 0);
    r.squaredDistance =
        inherit ? previous.squaredDistance : std::vector<double>(n, 0);
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
      if (inside) {
        r.labels[i] = o.manualClass;
        r.confidence[i] = 1;
        r.squaredDistance[i] = 0;
      }
    }
    params.insert("manualClass", o.manualClass);
    params.insert("lasso", polygon);
    params.insert("axes", QVariantList{o.axes.x, o.axes.y, o.axes.z, o.axes.yaw,
                                       o.axes.pitch});
    params.insert("boxLo", list(lo));
    params.insert("boxHi", list(hi));
    params.insert("confidence",
                  inherit ? "rule_membership_with_inherited_confidence"
                          : "rule_membership_not_probability");
    if (inherit) {
      params.insert("previousClassification", previous.provenance);
      params.insert(
          "previousLabelsHash",
          QString::fromLatin1(
              QCryptographicHash::hash(
                  QByteArrayView(
                      reinterpret_cast<const char *>(previous.labels.data()),
                      qsizetype(previous.labels.size() * sizeof(int))),
                  QCryptographicHash::Sha256)
                  .toHex()));
    }
  } else if (o.method == Classifier::Som) {
    // 第二无监督族：Kohonen SOM。标准化口径同 KMeans/GMM（全样本 mean/sd，
    // standardize 标志同 classify）；maxIterations → epochs 映射（同为迭代
    // 轮数），学习率/半径走 cluster::SomOptions 默认指数衰减调度。
    // labels = 原型下标（y * width + x），无地质含义，类名由上层适配。
    cluster::Matrix matrix{d, s.values};
    std::vector<double> mean(d, 0), sd(d, 1);
    if (o.standardize) {
      bool cancelled = false;
      if (!standardizeMatrix(matrix, ctl, mean, sd, &cancelled))
        return failed(QStringLiteral("已取消"), cancelled);
    }
    cluster::SomOptions som;
    som.width = o.somWidth;
    som.height = o.somHeight;
    som.epochs = o.maxIterations; // maxIterations → epochs 映射
    som.seed = o.seed;
    auto result = cluster::som(matrix, som, ctl);
    if (!result.ok)
      return failed(QString::fromStdString(result.error), result.cancelled);
    r.labels = std::move(result.labels);
    r.confidence = std::move(result.confidence);
    r.squaredDistance = std::move(result.squaredDistance);
    params.insert("normalizationMean", list(mean));
    params.insert("normalizationSd", list(sd));
    params.insert("somWidth", som.width);
    params.insert("somHeight", som.height);
    params.insert("somGrid", som.width * som.height);
    params.insert("epochs", som.epochs);
    params.insert("initialLearningRate", som.initialLearningRate);
    params.insert("finalLearningRate", som.finalLearningRate);
    params.insert("initialRadius", som.initialRadius);
    params.insert("finalRadius", som.finalRadius);
    params.insert("quantizationErrorHistory",
                  list(result.quantizationErrorHistory));
    params.insert("confidence", "one_minus_best_over_second");
    origin = QStringLiteral("crossplot_som");
  } else if (isSupervisedClassifier(o.method)) {
    // 监督三族需要 TrainedModel（FaciesTrainingService::train 产物）——
    // 无模型调用如实拒答，防误调无模型推理。
    return failed(QStringLiteral("监督方法需先训练，走 FaciesTrainingService"));
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
  r.provenance.insert("origin", origin);
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
  if (!CrossplotSamples::validate(s) || !validClassification(s, r))
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
  if (!CrossplotSamples::validate(s) || !validClassification(s, r) ||
      !s.grid.spatial || s.grid.cols <= 0 || s.grid.rows <= 0)
    return error(e, QStringLiteral("分类或地图栅格几何无效"));
  const auto size = std::size_t(s.grid.rows) * std::size_t(s.grid.cols);
  std::vector<unsigned char> labels(size, 255);

  std::vector<bool> used(size, false);
  for (std::size_t i = 0; i < s.rows(); ++i) {
    int pixel = -1;
    if (!pixelOf(s, i, used, pixel, e))
      return false;
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
  if (!setupClassificationRaster(ds.get(), s, r, e)) {
    removePartialGtiff(ds, path);
    return false;
  }
  auto band = GDALGetRasterBand(ds.get(), 1);
  if (GDALRasterIO(band, GF_Write, 0, 0, s.grid.cols, s.grid.rows,
                   labels.data(), s.grid.cols, s.grid.rows, GDT_Byte, 0,
                   0) != CE_None) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("分类像元写入失败"));
  }
  if (GDALFlushCache(ds.get()) != CE_None) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("分类栅格落盘失败"));
  }
  return true;
}
bool FaciesClassificationService::writeConfidenceRaster(const QString &path,
                                                        const SampleSet &s,
                                                        const Classification &r,
                                                        QString *e) {
  if (!PaleoProjectStore::isWriteQueueActive())
    return error(e, QStringLiteral("置信度写入必须经过项目写队列"));
  if (!CrossplotSamples::validate(s) || !validClassification(s, r) ||
      !s.grid.spatial || s.grid.cols <= 0 || s.grid.rows <= 0)
    return error(e, QStringLiteral("分类或地图栅格几何无效"));
  const auto size = std::size_t(s.grid.rows) * std::size_t(s.grid.cols);
  // 未采样像素留 -9999 nodata；validClassification 已保证 sampled 置信度在
  // [0,1]，落 Float32 无损可回读逐点比照。
  std::vector<float> values(size, -9999.0f);
  std::vector<bool> used(size, false);
  for (std::size_t i = 0; i < s.rows(); ++i) {
    int pixel = -1;
    if (!pixelOf(s, i, used, pixel, e))
      return false;
    used[std::size_t(pixel)] = true;
    values[std::size_t(pixel)] = static_cast<float>(r.confidence[i]);
  }
  GDALAllRegister();
  std::unique_ptr<void, decltype(&GDALClose)> ds(
      GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(),
                 s.grid.cols, s.grid.rows, 1, GDT_Float32, nullptr),
      GDALClose);
  if (!ds)
    return error(e, QStringLiteral("无法创建置信度栅格"));
  auto gt = s.grid.transform;
  if (GDALSetGeoTransform(ds.get(), gt.data()) != CE_None ||
      (!s.grid.crs.isEmpty() &&
       GDALSetProjection(ds.get(), s.grid.crs.toUtf8().constData()) != CE_None)) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("无法写栅格几何"));
  }
  auto band = GDALGetRasterBand(ds.get(), 1);
  if (GDALSetRasterNoDataValue(band, -9999) != CE_None ||
      GDALSetMetadataItem(ds.get(), "PALEO_PROVENANCE",
                          QJsonDocument::fromVariant(r.provenance)
                              .toJson(QJsonDocument::Compact)
                              .constData(),
                          nullptr) != CE_None) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("置信度 nodata 或 provenance 写入失败"));
  }
  if (GDALRasterIO(band, GF_Write, 0, 0, s.grid.cols, s.grid.rows,
                   values.data(), s.grid.cols, s.grid.rows, GDT_Float32, 0,
                   0) != CE_None) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("置信度像元写入失败"));
  }
  if (GDALFlushCache(ds.get()) != CE_None) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("置信度栅格落盘失败"));
  }
  return true;
}
bool FaciesClassificationService::writeMaskedRaster(const QString &path,
                                                    const SampleSet &s,
                                                    const Classification &r,
                                                    double threshold,
                                                    QString *e) {
  if (!PaleoProjectStore::isWriteQueueActive())
    return error(e, QStringLiteral("掩膜写入必须经过项目写队列"));
  if (!std::isfinite(threshold) || threshold < 0 || threshold > 1)
    return error(e, QStringLiteral("低置信掩膜阈值须在 [0,1]"));
  if (!CrossplotSamples::validate(s) || !validClassification(s, r) ||
      !s.grid.spatial || s.grid.cols <= 0 || s.grid.rows <= 0)
    return error(e, QStringLiteral("分类或地图栅格几何无效"));
  const auto size = std::size_t(s.grid.rows) * std::size_t(s.grid.cols);
  // 低置信像素写 255（nodata/透明），其余写类码——低置信区在图上可辨且不
  // 掩盖主图（置信度层单独存在）。判定用内存 double；置信度件落 Float32
  // 有 24 位尾数截断，掩膜阈值贴边（confidence≈threshold）时两图可能出现
  // 一位像素的解读差，这是该口径的既有取舍而非漏掩。
  std::vector<unsigned char> masked(size, 255);
  std::vector<bool> used(size, false);
  for (std::size_t i = 0; i < s.rows(); ++i) {
    int pixel = -1;
    if (!pixelOf(s, i, used, pixel, e))
      return false;
    used[std::size_t(pixel)] = true;
    if (r.labels[i] >= 0 && r.confidence[i] >= threshold)
      masked[std::size_t(pixel)] = static_cast<unsigned char>(r.labels[i]);
  }
  GDALAllRegister();
  std::unique_ptr<void, decltype(&GDALClose)> ds(
      GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(),
                 s.grid.cols, s.grid.rows, 1, GDT_Byte, nullptr),
      GDALClose);
  if (!ds)
    return error(e, QStringLiteral("无法创建掩膜栅格"));
  if (!setupClassificationRaster(ds.get(), s, r, e)) {
    removePartialGtiff(ds, path);
    return false;
  }
  auto band = GDALGetRasterBand(ds.get(), 1);
  if (GDALRasterIO(band, GF_Write, 0, 0, s.grid.cols, s.grid.rows,
                   masked.data(), s.grid.cols, s.grid.rows, GDT_Byte, 0,
                   0) != CE_None) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("掩膜像元写入失败"));
  }
  if (GDALFlushCache(ds.get()) != CE_None) {
    removePartialGtiff(ds, path);
    return error(e, QStringLiteral("掩膜栅格落盘失败"));
  }
  return true;
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
