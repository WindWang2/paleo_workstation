// 层：功能
#include "wellpredictionservice.h"
#include "../ai/wellfacieskeystore.h"
#include "../domain/faciescatalog.h"
#include "../domain/faciestaxonomy.h"
#include "../domain/welllogfacies.h"
#include "../domain/wellnumeric.h"
#include "../io/welllogread.h"
#include "../qgis/wellattributestore.h"
#include "../services/futuretask.h"
#include "../services/projectdata.h"
#include "../services/welllogset.h"
#include "wellsectionworkflow.h"
#include <QFutureWatcher>
#include <QFileInfo>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <limits>

WellPredictionService::WellPredictionService(QObject *parent)
    : RemotePredictionService(parent) {
  connect(&m_service, &WellFaciesService::modelsReady, this, &WellPredictionService::plan);
  connect(&m_service, &WellFaciesService::failed, this, [this](const QString &reason) {
    if (m_request.id.isEmpty()) return;
    // 获取模型/配置失败属于整批错误；已进入推理后，错误只影响当前井。
    if (m_index < 0 || m_index >= m_runs.size()) return fail(reason);
    skip(m_runs[m_index].well, reason);
    m_service.cancel();
    next();
  });
  connect(&m_service, &WellFaciesService::progress, this, [this](int p, const QString &message) {
    if (m_request.id.isEmpty()) return;
    const int total = std::max(1, int(m_runs.size()));
    emit progress(m_request.id, 10 + (std::max(0, m_index) * 80 + p * 80 / 100) / total);
    emit detailChanged(m_request.id, message);
  });
  connect(&m_service, &WellFaciesService::completed, this, [this](const WellFaciesResult &part) {
    if (m_request.id.isEmpty() || m_index < 0 || m_index >= m_runs.size()) return;
    auto result = part;
    const auto &run = m_runs[m_index];
    result.group = run.prediction.group;
    m_results[run.well].append(result);
    next();
  });
}
WellPredictionService::~WellPredictionService() { cancel(); }
void WellPredictionService::bindCatalog(DataCatalog *catalog, const QString &dir) {
  cancel();
  m_catalog = catalog;
  m_dir = dir;
}
QString WellPredictionService::statusHint() const {
  const auto config = WellFaciesConfig::load();
  const auto error = WellFaciesConfig::validateUrl(config.baseUrl, config.allowInsecureHttp);
  return error.isEmpty() ? tr("测井远端预测：%1（与测井面板共用服务）").arg(config.baseUrl.toString())
                         : error + tr("；请在测井面板的「预测服务」配置");
}
void WellPredictionService::cancel() {
  ++m_generation;
  m_dryRun = false;
  if (m_cancelled) m_cancelled->store(true);
  m_cancelled.reset();
  m_request = {};
  m_service.cancel();
  m_data.clear(); m_runs.clear(); m_results.clear(); m_schema.clear(); m_skipped.clear(); m_index = -1;
}
void WellPredictionService::fail(const QString &reason) {
  if (m_request.id.isEmpty()) return;
  const QString id = m_request.id;
  QString detail = reason;
  if (m_index >= 0 && m_index < m_runs.size())
    detail = tr("%1 · %2：%3").arg(m_data[m_runs[m_index].well].wellName,
                                  m_runs[m_index].prediction.group, reason);
  const bool dryRun = m_dryRun;
  cancel();
  if (dryRun)
    emit readinessReady(id, {}, detail);
  else
    emit failed(id, detail);
}
void WellPredictionService::skip(int well, const QString &reason) {
  if (m_skipped.contains(well)) return;
  const QString detail = reason.trimmed().isEmpty() ? tr("该井无法预测") : reason;
  m_skipped.insert(well, detail);
  if (well < m_results.size()) m_results[well].clear();
  emit detailChanged(m_request.id, tr("跳过 %1：%2；继续下一口井")
      .arg(m_request.wells[well].toMap().value("name").toString(), detail));
}
void WellPredictionService::configure(const WellFaciesConfig &config) {
  // 同一配置不重置服务的已受理任务：取消后再运行相同输入，沿用服务端
  // jobId 续查，不重复提交尚未终态的推理。
  if (!m_configured || config.baseUrl != m_config.baseUrl ||
      config.apiKey != m_config.apiKey ||
      config.allowInsecureHttp != m_config.allowInsecureHttp) {
    m_config = config;
    m_configured = true;
    m_service.configure(config);
  }
}
void WellPredictionService::start(const RemotePredictionRequest &request) {
  if (m_dryRun && !m_request.id.isEmpty()) {
    // 正式运行（或新检查）取代进行中的准备检查：如实收口，不让界面停在“检查中”。
    const QString previous = m_request.id;
    cancel();
    emit readinessReady(previous, {}, tr("检查已中止：已开始新的预测或检查"));
  }
  cancel();
  m_request = request;
  m_schema = request.facies;
  const auto generation = m_generation;
  auto config = WellFaciesConfig::load();
  // 下一轮事件循环起跑，保留 start/cancel 的一致异步语义。
  QTimer::singleShot(0, this, [this, generation, config]() mutable {
    if (generation != m_generation) return;
    if (!m_catalog || !m_catalog->isOpen() || m_request.kind != "wells")
      return fail(tr("测井预测未绑定有效工区"));
    emit detailChanged(m_request.id, tr("检查测井服务配置及密钥"));
    const auto error = WellFaciesConfig::validateUrl(config.baseUrl, config.allowInsecureHttp);
    if (!error.isEmpty()) return fail(error);
    if (config.apiKey.isEmpty() && WellFaciesKeyStore::available()) {
      WellFaciesKeyStore::read(this, [this, generation, config](bool found, const QByteArray &key,
                                                               const QString &error) mutable {
        if (generation != m_generation) return;
        if (!found || key.isEmpty()) return fail(error.isEmpty() ? tr("请在测井面板配置预测服务 API 密钥") : error);
        config.apiKey = key;
        configure(config);
        prepare();
      });
    } else {
      configure(config);
      prepare();
    }
  });
}
void WellPredictionService::check(const RemotePredictionRequest &request) {
  start(request);   // start() 先 cancel()（清 m_dryRun），再异步起跑
  m_dryRun = true;
}
void WellPredictionService::reportReadiness() {
  QVariantList wells;
  QSet<int> planned;
  QHash<int, QStringList> groups;
  for (const auto &run : m_runs) {
    planned.insert(run.well);
    groups[run.well] << run.prediction.group;
  }
  for (int i = 0; i < m_request.wells.size(); ++i) {
    const auto well = m_request.wells[i].toMap();
    const bool ready = !m_skipped.contains(i) && planned.contains(i);
    QString reason = m_skipped.value(i);
    if (!ready && reason.isEmpty())
      reason = tr("没有可调用的测井相模型");
    wells << QVariantMap{{"id", well.value("id")}, {"name", well.value("name")},
                         {"ready", ready}, {"reason", ready ? QString() : reason},
                         {"groups", groups.value(i)}};
  }
  const QString id = m_request.id;
  cancel();
  emit readinessReady(id, wells, {});
}
void WellPredictionService::prepare() {
  struct Source {
    QVector<WellCurveRef> curves;
    QVector<WellTop> tops;
    WellComposite::ComprehensiveWellData data;
  };
  QVector<Source> sources;
  ProjectDataFacade project;
  project.setCatalog(m_catalog, m_dir);
  WellSectionWorkflow lithology(m_catalog);
  QStringList ids;
  for (const auto &v : m_request.wells) ids << v.toMap().value("id").toString();
  const auto lithologies = lithology.lithologiesFor(ids);
  for (const auto &v : m_request.wells) {
    const auto well = v.toMap();
    const QString id = well.value("id").toString();
    Source source;
    source.data.wellName = well.value("name").toString();
    source.curves = WellLogSet::wellCurveIndex(m_catalog, m_dir, id);
    // 编图与砂地比、厚度、残差同一套层序，不跟 20kou 这类主分层。
    source.tops = project.topsForMapping(id);
    QString topsVersion;
    const QString topsAsset = project.mappingTopsAssetId(id);
    if (!topsAsset.isEmpty())
      topsVersion = m_catalog->currentVersion(topsAsset).id;
    // 工区维护表中的组/段及显式底深随本次输入冻结，优先于原始 tops。
    for (const auto &value : well.value("vector_formations").toList()) {
      const auto row = value.toMap();
      double top = 0, base = 0;
      const QString name = row.value("formation").toString().trimmed();
      FaciesCatalog::formationRange(row.value("top_md"), row.value("base_md"), &top, &base);
      if (name.isEmpty() || !std::isfinite(top) || !std::isfinite(base)) continue;
      WellComposite::FormationInterval formation;
      formation.name = name;
      formation.topDepth = float(top); formation.bottomDepth = float(base);
      formation.unitType = row.value("unit_type", QStringLiteral("段")).toString();
      if (formation.unitType.isEmpty())
        formation.unitType = name.endsWith(QStringLiteral("组")) ? QStringLiteral("组") : QStringLiteral("段");
      source.data.formationIntervals.append(formation);
    }
    source.tops.erase(std::remove_if(source.tops.begin(), source.tops.end(),
      [](const WellTop &t) { return !std::isfinite(t.md); }), source.tops.end());
    std::stable_sort(source.tops.begin(), source.tops.end(),
      [](const WellTop &a, const WellTop &b) { return a.md < b.md; });
    bool current = false;
    for (const auto &top : source.tops)
      current |= top.horizon.compare(m_request.horizon, Qt::CaseInsensitive) == 0;
    for (const auto &formation : source.data.formationIntervals)
      current |= formation.name.compare(m_request.horizon, Qt::CaseInsensitive) == 0;
    if (!current) skip(int(sources.size()), tr("缺少当前地层 %1 的分层").arg(m_request.horizon));
    if (!topsVersion.isEmpty()) m_request.sourceVersionIds << topsVersion;
    if (source.curves.isEmpty()) skip(int(sources.size()), tr("没有可读测井曲线"));
    for (const auto &segment : lithologies.value(id)) {
      WellComposite::LithologyInterval interval;
      interval.topDepth = float(segment.topMd); interval.bottomDepth = float(segment.baseMd);
      interval.lithoName = segment.litho;
      source.data.lithologyIntervals.append(interval);
    }
    sources.append(source);
  }
  const auto generation = m_generation;
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  auto readErrors = std::make_shared<QHash<int, QString>>();
  m_cancelled = cancelled;
  auto *watcher = new QFutureWatcher<QVector<WellComposite::ComprehensiveWellData>>(this);
  emit progress(m_request.id, 1);
  emit detailChanged(m_request.id, tr("后台读取实际测井曲线、地层和岩性"));
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this,
          [this, generation, total = sources.size()](int done) {
    if (generation != m_generation) return;
    emit progress(m_request.id, 1 + done * 9 / std::max(1, int(total)));
    emit detailChanged(m_request.id, tr("读取测井曲线 %1/%2").arg(done).arg(total));
  });
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, readErrors] {
    const auto data = watcher->result();
    watcher->deleteLater();
    if (generation != m_generation) return;
    m_cancelled.reset(); m_data = data;
    for (int i = 0; i < m_data.size(); ++i) {
      WellAttributeStore::apply(m_request.wells[i].toMap().value("vector_intervals").toList(), &m_data[i]);
      if (readErrors->contains(i)) skip(i, readErrors->value(i));
      else if (m_data[i].continuousCurves.isEmpty()) skip(i, tr("测井曲线解析失败或没有有效 MD 数据"));
    }
    m_results.resize(m_data.size());
    if (m_skipped.size() == m_data.size()) return m_dryRun ? reportReadiness() : finish();
    emit detailChanged(m_request.id, tr("获取远端测井模型及输入要求"));
    emit progress(m_request.id, 10);
    m_service.fetchModels();
  });
  watcher->setFuture(QtConcurrent::run(
      paleo::tasks::pool(m_tasks),
      [sources, cancelled, readErrors](
          QPromise<QVector<WellComposite::ComprehensiveWellData>> &promise) {
        promise.setProgressRange(0, int(sources.size()));
        QVector<WellComposite::ComprehensiveWellData> data;
        for (const auto &source : sources) {
          if (cancelled->load())
            break;
          auto well = source.data;
          QHash<QString, QPair<LasHeaderInfo, LasDoc>> docs;
          double top = std::numeric_limits<double>::infinity(), base = -top;
          for (const auto &ref : source.curves) {
            if (cancelled->load())
              break;
            if (!docs.contains(ref.path)) {
              LasHeaderInfo header;
              LasDoc parsed;
              parsed.ok = WellLogRead::parse(ref.path, header, parsed.curves, &parsed.error);
              parsed.curveNames = header.curveNames;
              docs.insert(ref.path, {header, parsed});
            }
            const auto &header = docs[ref.path].first;
            const auto &doc = docs[ref.path].second;
            const bool explicitAxis = WellLogRead::detect(ref.path) != WellLogFormat::Las;
            const auto unitScale = doc.curves.isEmpty() ? std::optional<double>() :
                paleo::wellnumeric::depthScaleToMetres(doc.curves.first().unit);
            if (explicitAxis && (!doc.ok || header.indexBasis != QLatin1String("MD") || !unitScale)) {
              readErrors->insert(int(data.size()), QCoreApplication::translate("WellPredictionService", "%1：预测需要已声明米或英尺单位的 MD 测深，当前基准为 %2；请检查索引与单位。%3")
                  .arg(QFileInfo(ref.path).fileName(), header.indexBasis.isEmpty() ? QCoreApplication::translate("WellPredictionService", "未声明") : header.indexBasis, doc.error));
              well.continuousCurves.clear();
              break;
            }
            if (doc.curves.isEmpty() || ref.column < 1 ||
                ref.column >= doc.curves.size())
              continue;
            // 预测输入使用测深（MD）；其他深度域不能直接当成 MD。
            if (header.indexBasis != "MD")
              continue;
            WellComposite::CurveData curve;
            curve.name = ref.mnemonic;
            curve.unit = doc.curves[ref.column].unit;
            const auto &depth = doc.curves.first();
            if (!unitScale && !depth.unit.isEmpty())
              continue;
            for (double d : depth.values) {
              d *= unitScale.value_or(1.0);
              curve.depths << float(d);
              if (std::isfinite(d)) {
                top = std::min(top, double(curve.depths.last()));
                base = std::max(base, double(curve.depths.last()));
              }
            }
            for (double value : doc.curves[ref.column].values)
              curve.values << float(value);
            well.continuousCurves.append(curve);
          }
          well.minDepth = top;
          well.maxDepth = base;
          QSet<QString> names;
          for (const auto &formation : well.formationIntervals)
            names.insert(formation.name);
          for (int i = 0; i < source.tops.size(); ++i) {
            const auto &t = source.tops[i];
            const double bottom =
                i + 1 < source.tops.size() ? source.tops[i + 1].md : base;
            if (t.horizon.isEmpty() || bottom <= t.md ||
                names.contains(t.horizon))
              continue;
            names.insert(t.horizon);
            WellComposite::FormationInterval formation;
            formation.name = t.horizon;
            formation.topDepth = float(t.md);
            formation.bottomDepth = float(bottom);
            formation.unitType = t.horizon.endsWith(QStringLiteral("组"))
                                     ? QStringLiteral("组")
                                     : QStringLiteral("段");
            well.formationIntervals.append(formation);
          }
          std::stable_sort(well.formationIntervals.begin(),
                           well.formationIntervals.end(),
                           [](const auto &a, const auto &b) {
                             return a.topDepth < b.topDepth;
                           });
          data.append(well);
          promise.setProgressValue(int(data.size()));
        }
        promise.addResult(data);
      }));
}
void WellPredictionService::plan(const QVector<WellFaciesModel> &models) {
  if (m_request.id.isEmpty()) return;
  m_runs.clear(); m_results.resize(m_data.size());
  for (int i = 0; i < m_data.size(); ++i) {
    if (m_skipped.contains(i)) continue;
    const auto planned = planWellFaciesFormationRun(m_data[i], models, m_request.horizon);
    if (!planned.ready()) { skip(i, planned.reason); continue; }
    for (const auto &run : planned.runs) m_runs.append({i, run});
  }
  if (m_dryRun) return reportReadiness(); // 干跑：止于计划，不发 POST /predict
  m_index = -1;
  next();
}
void WellPredictionService::next() {
  if (m_request.id.isEmpty()) return;
  do { ++m_index; }
  while (m_index < m_runs.size() && m_skipped.contains(m_runs[m_index].well));
  if (m_index >= m_runs.size()) return finish();
  const auto &run = m_runs[m_index];
  emit progress(m_request.id, 10 + m_index * 80 / std::max(1, int(m_runs.size())));
  emit detailChanged(m_request.id, tr("远端预测 %1 · %2（%3/%4）").arg(m_data[run.well].wellName,
      run.prediction.group).arg(m_index + 1).arg(m_runs.size()));
  auto input = run.prediction.input;
  input.facies = m_schema;
  m_service.predict(run.prediction.model, m_data[run.well].wellName, input);
}
void WellPredictionService::finish() {
  RemotePredictionResult result;
  m_request.sourceVersionIds.removeDuplicates();
  result.request = m_request; result.mock = false; result.partialWellCoverage = true;
  result.currentFormationOnly = true;
  result.method = QStringLiteral("remote-well-facies-api-v2");
  QStringList labels;
  for (const auto &parts : m_results)
    for (const auto &iv : mergeWellFaciesResults(parts).intervals) labels << iv.text;
  QString hierarchyError;
  QVariantList expanded;
  if (!FaciesTaxonomy::extend(m_schema, labels, &expanded, &hierarchyError)) {
    fail(hierarchyError); return;
  }
  m_schema = expanded;
  for (int i = 0; i < m_data.size(); ++i) {
    const auto merged = mergeWellFaciesResults(m_results[i]);
    QVariantList intervals, formations;
    QStringList jobs, modelNames;
    for (const auto &part : m_results[i]) {
      jobs << part.jobId; modelNames << part.modelName + " " + part.modelVersion;
    }
    const auto currentFormation = std::find_if(m_data[i].formationIntervals.cbegin(), m_data[i].formationIntervals.cend(),
        [this](const auto &f) { return f.name.compare(m_request.horizon, Qt::CaseInsensitive) == 0; });
    const double top = currentFormation == m_data[i].formationIntervals.cend()
        ? std::numeric_limits<double>::quiet_NaN() : currentFormation->topDepth;
    const double base = currentFormation == m_data[i].formationIntervals.cend()
        ? std::numeric_limits<double>::quiet_NaN() : currentFormation->bottomDepth;
    double previousBottom = top;
    for (const auto &iv : merged.intervals) {
      if (!std::isfinite(iv.topDepth) || !std::isfinite(iv.bottomDepth) ||
          iv.bottomDepth <= iv.topDepth || iv.topDepth < previousBottom - 1e-6 ||
          iv.bottomDepth > base + 1e-6) {
        skip(i, tr("远端结果井段深度无效或相互重叠"));
        break;
      }
      const int code = faciesCodeForLabel(m_schema, iv.text);
      if (!code) { // 不可达（上方预扩展已覆盖全部在场标签）；防御性拒收
        skip(i, tr("远端返回了相词表外的标签「%1」，该井结果无法映射").arg(iv.text));
        break;
      }
      intervals << QVariantMap{{"top", iv.topDepth}, {"bottom", iv.bottomDepth}, {"code", code}};
      previousBottom = iv.bottomDepth;
    }
    bool current = false;
    int currentCode = 0;
    for (const auto &f : m_data[i].formationIntervals) {
      if (f.name.compare(m_request.horizon, Qt::CaseInsensitive) != 0) continue;
      formations << QVariantMap{{"name", f.name}, {"top", f.topDepth}, {"bottom", f.bottomDepth}, {"unit_type", f.unitType}};
      if (f.name.compare(m_request.horizon, Qt::CaseInsensitive) == 0) {
        current = true;
        currentCode = FaciesCatalog::dominantCode(intervals, f.topDepth, f.bottomDepth);
      }
    }
    if (!current || !currentCode)
      skip(i, tr("远端结果未覆盖当前地层 %1").arg(m_request.horizon));
    const bool skipped = m_skipped.contains(i);
    if (skipped) intervals.clear();
    auto point = m_request.wells[i].toMap();
    point.remove("vector_intervals"); point.remove("vector_factors"); point.remove("vector_formations");
    point.insert("facies_code", skipped ? QVariant() : QVariant(currentCode));
    point.insert("prediction_status", skipped ? QStringLiteral("未预测") : QStringLiteral("已预测"));
    point.insert("prediction_reason", m_skipped.value(i));
    point.insert("facies_intervals", QString::fromUtf8(QJsonDocument::fromVariant(intervals).toJson(QJsonDocument::Compact)));
    point.insert("formation_intervals", QString::fromUtf8(QJsonDocument::fromVariant(formations).toJson(QJsonDocument::Compact)));
    point.insert("prediction_model", modelNames.join(QStringLiteral("；")));
    point.insert("prediction_job_id", jobs.join(QStringLiteral("；")));
    point.insert("prediction_depth_top", std::isfinite(top) ? QVariant(top) : QVariant());
    point.insert("prediction_depth_bottom", std::isfinite(base) ? QVariant(base) : QVariant());
    point.insert("result_scope", QStringLiteral("current_formation"));
    result.points << point;
  }
  result.facies = m_schema;
  const QString id = m_request.id;
  m_request = {};
  emit progress(id, 95);
  emit completed(result);
}
