// 层：功能
#include "wellsectionworkflow.h"
#include "algorithms/faultsurface/faultsurface.h"
#include "catalog/datacatalog.h"
#include "domain/faultset.h"
#include "io/lasalias.h"
#include "io/lascache.h"
#include "io/lasdoc.h"
#include "io/wellfileparsers.h"
#include "metadata/faultsetstore.h"
#include "sectionworkbench.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include "services/welllogset.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <optional>

// 一次 request 的共享产物：计划步填井身/分层/时深 + 曲线计划，worker 步
// 只补曲线数据体（catalog 只在其所属线程触）。
struct WellSectionWorkflow::Shared {
  QVector<wellsection::Well> wells;
  QStringList warnings;
  struct Entry {
    int wellIndex = -1;
    QString requested;
    WellCurveRef ref;
  };
  QVector<Entry> entries;
  QVector<std::optional<wellsection::Curve>> curves; // 与 entries 同序
};

namespace {
bool usableCoords(const ProjectWell &w) {
  return (w.coordinateStatus == QLatin1String("ok") ||
          w.coordinateStatus == QLatin1String("untransformed")) &&
         std::isfinite(w.surfaceX) && std::isfinite(w.surfaceY);
}

// 曲线选择：1) ref.mnemonic 大小写不敏感精确匹配，canonical 优先；
// 2) 规范名归一后同 canonical（别名命中，索引序取先）。
const WellCurveRef *pickCurveRef(const QVector<WellCurveRef> &index,
                                 const QString &requested) {
  const WellCurveRef *exact = nullptr;
  for (const WellCurveRef &ref : index) {
    if (ref.mnemonic.compare(requested, Qt::CaseInsensitive) != 0)
      continue;
    if (ref.canonical)
      return &ref;
    if (!exact)
      exact = &ref;
  }
  if (exact)
    return exact;
  const QString want = LasAliasMap::shared().canonicalCurve(requested);
  for (const WellCurveRef &ref : index)
    if (LasAliasMap::shared().canonicalCurve(ref.mnemonic) == want)
      return &ref;
  return nullptr;
}

struct GapBatch {
  int pending = 0;
  bool armed = false;
};
} // namespace

WellSectionWorkflow::WellSectionWorkflow(DataCatalog *catalog,
                                         QObject *parent)
    : QObject(parent), m_catalog(catalog) {
  qRegisterMetaType<QVector<wellsection::Well>>();
  qRegisterMetaType<wellsection::SeismicStrip>();
}

WellSectionWorkflow::~WellSectionWorkflow() { cancel(); }

void WellSectionWorkflow::setTaskService(PaleoTaskService *svc) {
  m_tasks = svc;
}
void WellSectionWorkflow::setSeismicTaskService(
    seismic::SeismicTaskService *svc) {
  m_seismicTasks = svc;
}
void WellSectionWorkflow::setSectionWorkbench(SectionWorkbench *wb) {
  m_workbench = wb;
}

void WellSectionWorkflow::setFaultSetStore(FaultSetStore *store) {
  m_faultStore = store;
}

WellSectionWorkflow::FaultProjection WellSectionWorkflow::faultProjection(
    const QVector<wellsection::Well> &wells) const {
  FaultProjection out;
  if (!m_faultStore) {
    out.status = tr("工程未打开，断层解释不可用");
    return out;
  }
  if (wells.size() < 2) {
    out.status = tr("两口以上的井才能投绘断层");
    return out;
  }
  paleo::fault::FaultSet set;
  QString err;
  if (!m_faultStore->load(set, &err)) {
    out.status = err.isEmpty() ? tr("断层解释读取失败") : err;
    return out;
  }
  if (set.faultCount() == 0) {
    out.status = tr("工程内无断层解释");
    return out;
  }
  QVector<QPair<double, double>> path;
  for (const wellsection::Well &w : wells) {
    if (!w.hasCoordinates()) {
      out.status = tr("井位坐标不全，断层未投绘");
      return out;
    }
    path.push_back({w.x, w.y});
  }
  for (const paleo::fault::Fault &f : set.faults()) {
    if (!f.visible || f.surface.isEmpty())
      continue;
    const paleo::faultsurf::SectionCut cut =
        paleo::faultsurf::intersectSurfaceWithPolyline(f.surface, path);
    if (!cut.ok())
      continue;
    wellsection::FaultTrace trace;
    trace.faultName = f.name;
    for (const paleo::faultsurf::SectionHit &h : cut.hits)
      trace.points.push_back({h.traceFrac, h.z});
    out.traces.push_back(trace);
  }
  if (out.traces.isEmpty())
    out.status = tr("断面不穿过剖面井径");
  return out;
}

QString WellSectionWorkflow::projectDir() const {
  return m_catalog ? QDir::cleanPath(m_catalog->catalogPath() + "/../../..")
                   : QString();
}

void WellSectionWorkflow::syncData() const {
  m_data.setCatalog(m_catalog, projectDir());
}

QVector<WellSectionWorkflow::WellChoice>
WellSectionWorkflow::wellChoices() const {
  QVector<WellChoice> out;
  syncData();
  for (const ProjectWell &w : m_data.wells())
    out.push_back({w.id, w.name, usableCoords(w),
                   usableCoords(w) ? w.surfaceX : qQNaN(),
                   usableCoords(w) ? w.surfaceY : qQNaN()});
  return out;
}

QStringList
WellSectionWorkflow::availableMnemonics(const QStringList &wellIds) const {
  syncData();
  QStringList out;
  QSet<QString> seen; // 大小写不敏感去重（留首个拼写）
  for (const QString &id : wellIds)
    for (const WellCurveRef &ref :
         WellLogSet::wellCurveIndex(m_catalog, projectDir(), id)) {
      const QString key = ref.mnemonic.toUpper();
      if (seen.contains(key))
        continue;
      seen.insert(key);
      out << ref.mnemonic;
    }
  out.sort(Qt::CaseInsensitive);
  return out;
}

int WellSectionWorkflow::request(const QStringList &wellIds,
                                 const QStringList &mnemonics) {
  const int generation = ++m_generation;
  if (m_sectionTask) {
    m_sectionTask->requestCancel();
    m_sectionTask.clear();
  }
  auto shared = std::make_shared<Shared>();
  syncData();
  const QVector<ProjectWell> candidates = m_data.wells();
  const QString dir = projectDir();
  for (const QString &id : wellIds) {
    const ProjectWell *pw = nullptr;
    for (const ProjectWell &w : candidates)
      if (w.id == id) {
        pw = &w;
        break;
      }
    if (!pw) {
      shared->warnings << tr("未找到井 %1").arg(id);
      continue;
    }
    wellsection::Well well;
    well.id = pw->id;
    well.name = pw->name;
    if (usableCoords(*pw)) {
      well.x = pw->surfaceX;
      well.y = pw->surfaceY;
    }
    if (std::isfinite(pw->kb))
      well.kb = pw->kb;
    if (m_catalog) {
      const double td = m_catalog->entityById(pw->id).td;
      if (std::isfinite(td) && td > 0.0)
        well.totalDepth = td;
    }
    int missingMd = 0;
    WellParseReport topsReport;
    const auto tops = m_data.topsFor(pw->id, &topsReport);
    if (!topsReport.issues.isEmpty())
      shared->warnings << tr("井 %1 的分层文件读面：%2")
                              .arg(pw->name, wellParseSummary(topsReport));
    for (const WellTop &t : tops) {
      if (std::isfinite(t.md))
        well.tops.push_back({t.horizon, t.md});
      else
        ++missingMd;
    }
    std::sort(well.tops.begin(), well.tops.end(),
              [](const wellsection::Top &a, const wellsection::Top &b) {
                return a.md < b.md;
              });
    if (missingMd > 0)
      shared->warnings << tr("井 %1 有 %2 个分层缺 MD，未参与连井")
                              .arg(pw->name)
                              .arg(missingMd);
    // 时深：逐井校正优先（workbench），否则主时深表 MD 列严格表。
    if (m_workbench) {
      seismic::TimeDepthModel model;
      double shift = 0.0;
      QString status;
      if (m_workbench->mdTimeDepth(pw->id, &model, &shift, &status))
        well.timeDepth = wellsection::TimeDepth{model, shift, status};
    } else {
      std::vector<seismic::TdPoint> points;
      for (const TdSample &s : m_data.tdTableFor(pw->id))
        if (std::isfinite(s.md) && std::isfinite(s.timeMs))
          points.push_back({s.md, s.timeMs});
      seismic::TimeDepthModel model;
      if (model.setCheckshots(points))
        well.timeDepth = wellsection::TimeDepth{model, 0.0, tr("时深表")};
    }
    // 井斜轨迹（TVD 域换算源）：无链接 = 直井（显式语义，不告警）；资产在
    // 但不可解析 → surveyError 如实记 + 告警（TVD 域该井标不可用）。
    // error 出参按本次调用写明（多井同坏文件也不漏记）。
    QString trajErr;
    const auto trajectory = m_data.trajectoryFor(pw->id, &trajErr);
    if (trajectory)
      well.survey = *trajectory;
    else if (!trajErr.isEmpty()) {
      well.surveyError = trajErr;
      shared->warnings
          << tr("井 %1 的井斜轨迹不可用：%2").arg(pw->name, well.surveyError);
    }
    const QVector<WellCurveRef> index =
        WellLogSet::wellCurveIndex(m_catalog, dir, pw->id);
    const int wellIndex = shared->wells.size();
    for (const QString &mnemonic : mnemonics) {
      const WellCurveRef *ref = pickCurveRef(index, mnemonic);
      if (ref)
        shared->entries.push_back({wellIndex, mnemonic, *ref});
    }
    shared->wells.push_back(well);
  }
  attachFaciesSegments(shared->wells);
  attachLithoSegments(shared->wells, &shared->warnings);
  shared->curves.resize(shared->entries.size());
  if (!m_tasks) {
    loadCurveBodies(*shared, nullptr);
    emit sectionReady(generation, shared->wells, shared->warnings);
    return generation;
  }
  QPointer<WellSectionWorkflow> guard(this);
  PaleoTask *task = m_tasks->start(
      tr("连井剖面取数"),
      [shared](PaleoTask *t) -> QString {
        loadCurveBodies(*shared, t);
        return QString();
      },
      QString(), PaleoTask::Priority::High, /*quiet=*/true);
  m_sectionTask = task;
  connect(task, &PaleoTask::finished, this,
          [guard, generation, shared, task]() {
            if (!guard || guard->m_generation != generation ||
                guard->m_sectionTask != task)
              return;
            if (task->state() == PaleoTask::State::Succeeded)
              emit guard->sectionReady(generation, shared->wells,
                                       shared->warnings);
          });
  return generation;
}

void WellSectionWorkflow::loadCurveBodies(Shared &shared,
                                          PaleoTask *task) {
  // 按路径分组，一份 LAS 只载一次（LasCache 线程安全）。
  QStringList order;
  QHash<QString, QVector<int>> byPath;
  for (int i = 0; i < shared.entries.size(); ++i) {
    const QString &path = shared.entries[i].ref.path;
    if (!byPath.contains(path))
      order << path;
    byPath[path].append(i);
  }
  for (const QString &path : order) {
    if (task && task->cancelRequested())
      return;
    const LasDoc doc = LasCache::shared().load(path);
    if (!doc.ok || doc.curves.size() < 2)
      continue;
    const QString unit = doc.curves.at(0).unit.trimmed().toUpper();
    double scale = 1.0;
    if (unit == QLatin1String("FT") || unit == QLatin1String("F"))
      scale = 0.3048;
    else if (unit != QLatin1String("M")) {
      // 单位未知跳过这一份文件的全部曲线，同井其它文件照常。
      const QString name =
          shared.wells.value(
              shared.entries.value(byPath.value(path).first()).wellIndex)
              .name;
      shared.warnings << tr("井 %1 的测井文件深度单位未知，曲线未加载")
                             .arg(name);
      continue;
    }
    const auto &depths = doc.curves.at(0);
    for (const int ei : byPath.value(path)) {
      const Shared::Entry &e = shared.entries.at(ei);
      if (e.ref.column < 1 || e.ref.column >= doc.curves.size())
        continue;
      const auto &values = doc.curves.at(e.ref.column);
      wellsection::Curve curve;
      curve.mnemonic = e.requested;
      curve.sourceMnemonic = e.ref.mnemonic;
      curve.unit = values.unit;
      const int size = qMin(depths.values.size(), values.values.size());
      curve.depths.reserve(size);
      curve.values.reserve(size);
      for (int i = 0; i < size; ++i) {
        curve.depths.push_back(static_cast<float>(depths.values[i] * scale));
        curve.values.push_back(static_cast<float>(values.values[i]));
      }
      shared.curves[ei] = curve;
    }
  }
  for (int i = 0; i < shared.entries.size(); ++i)
    if (shared.curves[i])
      shared.wells[shared.entries[i].wellIndex].curves.push_back(
          *shared.curves[i]);
}

// 交会分类井层段（catalog 派生资产 well_facies_intervals 的最新版本）挂到
// 各井 facies；文件缺/坏 → 静默跳过（相代码道显示空，不出告警）。
void WellSectionWorkflow::attachFaciesSegments(
    QVector<wellsection::Well> &wells) const {
  if (!m_catalog || wells.isEmpty())
    return;
  // versionsForAsset 按值返回临时容器——迭代器/指针不能跨循环存活，
  // 拷贝持有再比较（UAF 防线）。
  CatalogVersion best;
  bool has = false;
  for (const CatalogAsset &a : m_catalog->assets()) {
    if (a.type != QLatin1String("well_facies_intervals"))
      continue;
    for (const CatalogVersion &v : m_catalog->versionsForAsset(a.id))
      if (!has || v.versionNumber > best.versionNumber) {
        best = v;
        has = true;
      }
  }
  if (!has)
    return;
  QFile file(DataCatalog::resolvedVersionPath(projectDir(), best));
  if (!file.open(QIODevice::ReadOnly))
    return;
  const QJsonObject root =
      QJsonDocument::fromJson(file.readAll()).object();
  file.close();
  QHash<QString, QVector<wellsection::FaciesSegment>> byWell;
  for (const QJsonValue &iv : root.value(QLatin1String("intervals")).toArray()) {
    const QJsonObject o = iv.toObject();
    wellsection::FaciesSegment seg;
    seg.topMd = o.value(QLatin1String("top")).toDouble();
    seg.baseMd = o.value(QLatin1String("base")).toDouble();
    seg.classId = o.value(QLatin1String("classId")).toInt(-1);
    if (seg.classId < 0 || !(seg.baseMd > seg.topMd))
      continue;
    byWell[o.value(QLatin1String("wellId")).toString()].push_back(seg);
  }
  for (wellsection::Well &w : wells) {
    const auto it = byWell.constFind(w.id);
    if (it == byWell.constEnd())
      continue;
    w.facies = it.value();
    std::sort(w.facies.begin(), w.facies.end(),
              [](const wellsection::FaciesSegment &a,
                 const wellsection::FaciesSegment &b) {
                return a.topMd < b.topMd;
              });
  }
}

// 解释岩性段（catalog 资产 well_litho_intervals 的最新版本）挂到各井
// litho；无资产 → 静默回落（GR 推断是正常态）。资产在但读不了/解析不出
// 有效段 → 如实告警（回落不是静默伪装解释缺失）；schema 未知 → 拒读。
// 资产契约：{"schema":1,"intervals":[{"wellId","top","base","litho"}]}，
// 深度 MD 米；生产者/导入器递延（TODOS）——消费先行，谁落库谁点亮。
void WellSectionWorkflow::attachLithoSegments(
    QVector<wellsection::Well> &wells, QStringList *warnings) const {
  if (!m_catalog || wells.isEmpty())
    return;
  // versionsForAsset 按值返回临时容器——拷贝持有再比较（UAF 防线同上）。
  CatalogVersion best;
  bool has = false;
  for (const CatalogAsset &a : m_catalog->assets()) {
    if (a.type != QLatin1String("well_litho_intervals"))
      continue;
    for (const CatalogVersion &v : m_catalog->versionsForAsset(a.id))
      if (!has || v.versionNumber > best.versionNumber) {
        best = v;
        has = true;
      }
  }
  if (!has)
    return;
  const auto warn = [warnings](const QString &msg) {
    if (warnings)
      *warnings << msg;
  };
  QFile file(DataCatalog::resolvedVersionPath(projectDir(), best));
  if (!file.open(QIODevice::ReadOnly)) {
    warn(tr("解释岩性资产读取失败：%1").arg(best.fileName));
    return;
  }
  const QJsonObject root =
      QJsonDocument::fromJson(file.readAll()).object();
  file.close();
  const QJsonValue schema = root.value(QLatin1String("schema"));
  if (!schema.isNull() && schema.toInt(-1) != 1) {
    warn(tr("解释岩性资产 schema 版本不支持（%1），已忽略")
             .arg(QString::number(schema.toDouble())));
    return;
  }
  QHash<QString, QVector<wellsection::LithoSegment>> byWell;
  int dropped = 0;
  for (const QJsonValue &iv : root.value(QLatin1String("intervals")).toArray()) {
    const QJsonObject o = iv.toObject();
    wellsection::LithoSegment seg;
    seg.topMd = o.value(QLatin1String("top")).toDouble();
    seg.baseMd = o.value(QLatin1String("base")).toDouble();
    seg.litho = o.value(QLatin1String("litho")).toString().trimmed();
    if (seg.litho.isEmpty() || !(seg.baseMd > seg.topMd)) {
      ++dropped;
      continue;
    }
    byWell[o.value(QLatin1String("wellId")).toString()].push_back(seg);
  }
  if (byWell.isEmpty() && dropped == 0) {
    warn(tr("解释岩性资产无有效数据段：%1").arg(best.fileName));
    return;
  }
  if (dropped > 0)
    warn(tr("解释岩性资产有 %1 个无效段（逆序/空词面）已跳过").arg(dropped));
  for (wellsection::Well &w : wells) {
    const auto it = byWell.constFind(w.id);
    if (it == byWell.constEnd())
      continue;
    w.litho = it.value();
    std::sort(w.litho.begin(), w.litho.end(),
              [](const wellsection::LithoSegment &a,
                 const wellsection::LithoSegment &b) {
                return a.topMd < b.topMd;
              });
  }
}

int WellSectionWorkflow::requestSeismic(
    const QVector<wellsection::Well> &wells, const SeismicSource &source) {
  const int generation = ++m_seismicGeneration;
  for (const QPointer<PaleoTask> &t : m_gapTasks)
    if (t)
      t->requestCancel();
  m_gapTasks.clear();
  auto strip = std::make_shared<wellsection::SeismicStrip>();
  const int gaps = qMax(0, wells.size() - 1);
  strip->gaps.resize(gaps);
  if (wells.size() < 2) {
    emit seismicReady(generation, *strip);
    return generation;
  }
  const auto emitStatus = [&](const QString &status) {
    strip->status = status;
    for (auto &g : strip->gaps)
      g.reason = status;
    emit seismicReady(generation, *strip);
    return generation;
  };
  if (!source.volume || !source.grid.valid)
    return emitStatus(tr("未加载带坐标的地震体"));
  if (!m_seismicTasks)
    return emitStatus(tr("地震任务服务不可用"));

  QPointer<WellSectionWorkflow> guard(this);
  auto batch = std::make_shared<GapBatch>();
  // 全部缝终态后统一收尾：自适应截幅 + 全失效时把首条原因提为总体状态。
  const auto finalize = [guard, generation, strip]() {
    strip->clip = wellsection::adaptiveClip(strip->gaps);
    if (!strip->anyValid())
      for (const auto &g : strip->gaps)
        if (!g.reason.isEmpty()) {
          strip->status = g.reason;
          break;
        }
    if (guard && guard->m_seismicGeneration == generation)
      emit guard->seismicReady(generation, *strip);
  };
  const auto finishOne = [batch, finalize]() {
    if (--batch->pending == 0 && batch->armed)
      finalize();
  };
  const double stepMs = source.volume->SampleIntervalUs() / 1000.0;
  for (int i = 0; i < gaps; ++i) {
    const wellsection::Well &a = wells[i];
    const wellsection::Well &b = wells[i + 1];
    auto &gap = strip->gaps[i];
    int inlA = 0, xlA = 0, inlB = 0, xlB = 0;
    QString reason;
    if (!a.hasCoordinates())
      gap.reason = tr("%1 缺少井口坐标").arg(a.name);
    else if (!b.hasCoordinates())
      gap.reason = tr("%1 缺少井口坐标").arg(b.name);
    else if (!a.timeDepth)
      gap.reason = tr("%1 缺少时深关系").arg(a.name);
    else if (!b.timeDepth)
      gap.reason = tr("%1 缺少时深关系").arg(b.name);
    else if (!source.grid.xyToInlineXline(a.x, a.y, &inlA, &xlA, &reason))
      gap.reason = tr("%1 不在地震测网内").arg(a.name);
    else if (!source.grid.xyToInlineXline(b.x, b.y, &inlB, &xlB, &reason))
      gap.reason = tr("%1 不在地震测网内").arg(b.name);
    else if (inlA == inlB && xlA == xlB)
      gap.reason = tr("两井位于同一地震道");
    if (!gap.reason.isEmpty())
      continue;
    ++batch->pending;
    seismic::SgySectionOptions options;
    options.maxColumns = 512;
    options.interpolate = false;
    const double startMs = source.timeOriginMs;
    PaleoTask *task = m_seismicTasks->startSectionExtraction(
        std::make_shared<seismic::SgyVolume>(*source.volume),
        {glm::ivec2(inlA, xlA), glm::ivec2(inlB, xlB)}, options,
        [strip, batch, finishOne, i, startMs, stepMs](
            bool ok, std::shared_ptr<const seismic::SgySliceImage> image,
            const seismic::SgySectionStats &stats, const QString &error) {
          auto &gap = strip->gaps[i];
          if (ok && image && image->width > 0 && image->height > 0 &&
              image->values.size() ==
                  static_cast<size_t>(image->width) * image->height) {
            gap.columns = image->width;
            gap.samples = image->height;
            gap.startMs = startMs;
            gap.stepMs = stepMs;
            gap.values = image->values;
            // 列 0 必须是左井端：道距序倒挂 → 每行翻转列序。
            if (stats.columnDistances.size() >= 2 &&
                stats.columnDistances.front() > stats.columnDistances.back()) {
              for (int r = 0; r < gap.samples; ++r)
                std::reverse(
                    gap.values.begin() + size_t(r) * gap.columns,
                    gap.values.begin() + size_t(r + 1) * gap.columns);
            }
          } else {
            gap.columns = 0;
            gap.samples = 0;
            gap.values.clear();
            gap.reason =
                error.isEmpty()
                    ? QCoreApplication::translate("WellSectionWorkflow",
                                                  "剖面提取失败")
                    : error;
          }
          finishOne();
        });
    if (task)
      m_gapTasks << task;
  }
  batch->armed = true;
  if (batch->pending == 0)
    finalize();
  return generation;
}

void WellSectionWorkflow::cancel() {
  ++m_generation;
  ++m_seismicGeneration;
  if (m_sectionTask) {
    m_sectionTask->requestCancel();
    m_sectionTask.clear();
  }
  for (const QPointer<PaleoTask> &t : m_gapTasks)
    if (t)
      t->requestCancel();
  m_gapTasks.clear();
}
