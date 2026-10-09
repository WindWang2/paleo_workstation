// 层：功能
#include "wellsectionworkflow.h"
#include "../qgis/wellattributestore.h"
#include "algorithms/faultsurface/faultsurface.h"
#include "catalog/datacatalog.h"
#include "domain/faultset.h"
#include "io/cuttingsdoc.h"
#include "io/lasalias.h"
#include "io/lascache.h"
#include "io/lasdoc.h"
#include "io/wellfileparsers.h"
#include "metadata/faultsetstore.h"
#include "sectionworkbench.h"
#include "services/imagelod.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include "services/welllogset.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
  // 图片道清单（GUI 步收集，任务线程装载）：井 id → 待装图片。装载按
  // imagelod 缩略级（LOD 口径：原图驻磁盘按需全载）。
  struct PendingImage {
    QString path;
    double depthMd = 0.0;
    QString caption;
    QString assetId;
  };
  QHash<QString, QVector<PendingImage>> pendingImages;
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
    // 井斜轨迹（TVD 域换算源）：无链接 = 无测斜——几何恒等按 MD 绘制，
    // TVD 域如实标注不可用；不告警（非数据损坏）。资产在但不可解析 →
    // surveyError 如实记 + 告警（TVD 域该井不出几何）。
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
  collectCoreImages(*shared);
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

  // 图片道装载（任务线程）：imagelod 缩略级（解码期降采样——装载内存峰值
  // = 缩略字节而非全图字节；EXIF Orientation 在装载路径统一应用）。QImage
  // 是非 GUI 类型，QPixmap 转换归视图线程。单张失败跳过（坏图不让整段
  // 剖面失败——warnings 如实记）。
  for (wellsection::Well &w : shared.wells) {
    QVector<wellsection::ImageAnchor> loaded;
    for (const auto &pending : shared.pendingImages.value(w.id)) {
      const paleo::imagelod::TrackImage ti =
          paleo::imagelod::loadThumbnail(pending.path);
      if (ti.isNull()) {
        shared.warnings << tr("图片道装载失败：%1").arg(pending.path);
        continue;
      }
      wellsection::ImageAnchor a;
      a.md = pending.depthMd;
      a.caption = pending.caption;
      a.image = ti.thumbnail;
      a.assetId = pending.assetId;
      a.path = pending.path;
      a.fullSize = ti.fullSize;
      loaded.append(a);
    }
    if (!loaded.isEmpty()) {
      std::sort(loaded.begin(), loaded.end(),
                [](const wellsection::ImageAnchor &a, const wellsection::ImageAnchor &b) {
                  return a.md < b.md;
                });
      w.images = loaded;
    }
  }
}

// 交会分类井层段（catalog 派生资产 well_facies_intervals 的最新版本）挂到
// 各井 facies；文件缺/坏 → 静默跳过（相代码道显示空，不出告警）。
// 图片道清单（GUI 计划步）：经 facade imagesFor 拿 core/lab_analysis 角色
// 深度锚图片的绝对路径——只记清单，QImage 装载在任务线程（loadCurveBodies）。
void WellSectionWorkflow::collectCoreImages(Shared &shared) const
{
  for (const wellsection::Well &w : shared.wells) {
    for (const WellImageAnchor &a : m_data.imagesFor(w.id)) {
      Shared::PendingImage p;
      p.path = a.path;
      p.depthMd = a.depthMd;
      p.caption = a.caption;
      p.assetId = a.assetId;
      shared.pendingImages[w.id].append(p);
    }
  }
}

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

// 解释岩性段的按井链接消费 provider（方向 69）：经 linksForEntity(wellId)
// 找 interpretation 角色且类型为 well_litho_intervals 的链接（解释链接还
// 有交会相分类等其它类型——按类型过滤，见 faciesclassify 登记面），多份
// 资产取 currentVersion 版本号最新者 → resolvedVersionPath → 解析 schema 1
// JSON → Interpreted 段（provenance 带上资产来源标注）。版本解析结果按
// version id 缓存：共享资产的多口井不重复读盘/重复告警。
// 第二解释源（69 扩展）：解释资产无该井段命中 → role=="cuttings" 链接
// （岩屑录井表，readCuttingsFile）→ Interpreted 段（provenance=「岩屑录井」）。
// 优先级（按井）：有该井段的 well_litho_intervals 解释资产 > cuttings >
// 无（GR 回落）。
class CatalogWellLithologyProvider
    : public wellsection::WellLithologyProvider {
public:
  CatalogWellLithologyProvider(DataCatalog *catalog,
                               const QString &projectDir, QStringList *warnings)
      : m_catalog(catalog), m_projectDir(projectDir),
        m_warnings(warnings) {}

  QVector<wellsection::LithoSegment>
  lithologyFor(const QString &wellId) const override {
    if (!m_catalog)
      return {};
    const auto maintained = WellAttributeStore::rows(m_projectDir, nullptr, false, wellId);
    if (!maintained.isEmpty()) {
      QVector<wellsection::LithoSegment> segments;
      bool hasMaintainedLithology = false;
      for (const auto &v : maintained) {
        const auto row = v.toMap();
        hasMaintainedLithology = hasMaintainedLithology || !row.value("lithology").isNull();
        if (row.value("lithology").toString().isEmpty()) continue;
        wellsection::LithoSegment segment;
        segment.topMd = row.value("top_md").toDouble(); segment.baseMd = row.value("base_md").toDouble();
        segment.litho = row.value("lithology").toString(); segment.source = wellsection::LithoSource::Interpreted;
        segment.provenance = WellSectionWorkflow::tr("测井矢量属性表"); segments << segment;
      }
      // 预测相不是岩性；仅有相而无岩性时仍允许读取已有解释源。
      if (hasMaintainedLithology) return segments;
    }
    QString bestAssetId;
    CatalogVersion best;
    for (const EntityAssetLink &link : m_catalog->linksForEntity(wellId)) {
      if (link.role != QLatin1String("interpretation"))
        continue;
      const CatalogAsset a = m_catalog->assetById(link.assetId);
      if (a.type != QLatin1String("well_litho_intervals"))
        continue;
      const CatalogVersion v = m_catalog->currentVersion(link.assetId);
      if (v.id.isEmpty())
        continue;
      if (bestAssetId.isEmpty() || v.versionNumber > best.versionNumber) {
        best = v;
        bestAssetId = link.assetId;
      }
    }
    // 第二解释源（方向 69 扩展）按井兜底：解释资产未链接、读失败（parse
    // 已告警）或不含该井段 → role=="cuttings" 链接（岩屑录井，一井一份）→
    // 无 → 静默留空（GR 回落是正常态，不告警）。cuttings 多份取
    // currentVersion 版本号最新者，与解释资产同确定性口径。
    if (!bestAssetId.isEmpty()) {
      const QVector<wellsection::LithoSegment> segs =
          parse(best).byWell.value(wellId);
      if (!segs.isEmpty())
        return segs;
      // 资产读失败（已告警）或无该井段：落 cuttings 兜底（不重复告警）。
    }
    return cuttingsLithologyFor(wellId);
  }

  QString sourceLabel() const override { return QStringLiteral("catalog"); }

private:
  struct Parsed {
    bool readable = false;
    int dropped = 0;
    QHash<QString, QVector<wellsection::LithoSegment>> byWell;
  };
  Parsed parse(const CatalogVersion &v) const {
    const auto it = m_parsed.constFind(v.id);
    if (it != m_parsed.constEnd())
      return it.value();
    Parsed p;
    const auto warn = [this](const QString &msg) {
      if (m_warnings)
        *m_warnings << msg;
    };
    QFile file(DataCatalog::resolvedVersionPath(m_projectDir, v));
    if (!file.open(QIODevice::ReadOnly)) {
      warn(WellSectionWorkflow::tr("解释岩性资产读取失败：%1").arg(v.fileName));
      m_parsed.insert(v.id, p);
      return p;
    }
    const QJsonObject root =
        QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QJsonValue schema = root.value(QLatin1String("schema"));
    if (!schema.isNull() && schema.toInt(-1) != 1) {
      warn(WellSectionWorkflow::tr(
               "解释岩性资产 schema 版本不支持（%1），已忽略：%2")
               .arg(QString::number(schema.toDouble()), v.fileName));
      m_parsed.insert(v.id, p);
      return p;
    }
    // 来源标注：JSON provenance（生产者 welllogfacies 落）拼为题注词面；
    // 无 provenance 的老资产 → 空（题注回落「解释」，不伪造来源）。
    const QJsonObject prov =
        root.value(QLatin1String("provenance")).toObject();
    QStringList provParts;
    for (const char *key :
         {"source", "modelName", "modelVersion"})
      if (!prov.value(QLatin1String(key)).toString().trimmed().isEmpty())
        provParts << prov.value(QLatin1String(key)).toString().trimmed();
    const QString provenance = provParts.join(QLatin1Char(' '));
    for (const QJsonValue &iv :
         root.value(QLatin1String("intervals")).toArray()) {
      const QJsonObject o = iv.toObject();
      wellsection::LithoSegment seg;
      seg.topMd = o.value(QLatin1String("top")).toDouble();
      seg.baseMd = o.value(QLatin1String("base")).toDouble();
      seg.litho = o.value(QLatin1String("litho")).toString().trimmed();
      seg.source = wellsection::LithoSource::Interpreted;
      seg.provenance = provenance;
      if (seg.litho.isEmpty() || !(seg.baseMd > seg.topMd)) {
        ++p.dropped;
        continue;
      }
      p.byWell[o.value(QLatin1String("wellId")).toString()].push_back(seg);
    }
    if (p.byWell.isEmpty() && p.dropped == 0)
      warn(WellSectionWorkflow::tr("解释岩性资产无有效数据段：%1")
               .arg(v.fileName));
    if (p.dropped > 0)
      warn(WellSectionWorkflow::tr(
               "解释岩性资产有 %1 个无效段（逆序/空词面）已跳过：%2")
               .arg(p.dropped)
               .arg(v.fileName));
    p.readable = true;
    m_parsed.insert(v.id, p);
    return p;
  }

  DataCatalog *m_catalog;
  QString m_projectDir;
  QStringList *m_warnings;
  mutable QHash<QString, Parsed> m_parsed;

  // ---- 第二解释源：岩屑录井（cuttings）文件（一井一份，readCuttingsFile） ----

  QVector<wellsection::LithoSegment>
  cuttingsLithologyFor(const QString &wellId) const {
    struct Candidate {
      QString assetId;
      CatalogVersion version;
    };
    QVector<Candidate> candidates;
    for (const EntityAssetLink &link : m_catalog->linksForEntity(wellId)) {
      if (link.role != QLatin1String("cuttings"))
        continue;
      // currentVersion 会选到版本号更高的 PDF 派生件。岩性只读表文件。
      CatalogVersion table;
      for (const CatalogVersion &v : m_catalog->versionsForAsset(link.assetId)) {
        const QString name = v.fileName.isEmpty() ? v.path : v.fileName;
        const QString ext = QFileInfo(name).suffix().toLower();
        if (ext != QLatin1String("xlsx") && ext != QLatin1String("xls")
            && ext != QLatin1String("xml") && ext != QLatin1String("csv")
            && ext != QLatin1String("txt") && ext != QLatin1String("tsv"))
          continue;
        if (table.id.isEmpty() || v.versionNumber > table.versionNumber
            || (v.versionNumber == table.versionNumber && v.id > table.id))
          table = v;
      }
      if (table.id.isEmpty())
        continue;
      candidates.push_back({link.assetId, table});
    }
    if (candidates.isEmpty())
      return {}; // 无 cuttings 链接井静默留空（GR 回落是正常态，不告警）

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                if (a.version.versionNumber != b.version.versionNumber)
                  return a.version.versionNumber > b.version.versionNumber;
                return a.version.id > b.version.id;
              });

    const Candidate &best = candidates.first();
    if (candidates.size() > 1) {
      for (int i = 1; i < candidates.size(); ++i) {
        const Candidate &unselected = candidates[i];
        if (m_warnings) {
          *m_warnings << WellSectionWorkflow::tr(
              "井 %1 存在多份岩屑录井数据，已优先选用最新版本 %2（版本号 %3）；未选用：%4（版本号 %5）")
                             .arg(wellId, best.version.fileName)
                             .arg(best.version.versionNumber)
                             .arg(unselected.version.fileName)
                             .arg(unselected.version.versionNumber);
        }
        if (m_catalog) {
          QVariantMap extraPatch;
          extraPatch.insert(QStringLiteral("cuttings_selection"),
                            QStringLiteral("unselected"));
          extraPatch.insert(QStringLiteral("unselected_reason"),
                            QStringLiteral("superseded_by_newer_version"));
          extraPatch.insert(QStringLiteral("unselected_superseded_by"),
                            best.version.fileName);
          m_catalog->updateAssetExtra(unselected.assetId, extraPatch);
        }
      }
    }
    return parseCuttings(best.version);
  }

  // 解析结果按 version id 缓存（与解释资产 m_parsed 同款）：共享文件的多
  // 次渲染不重复读盘/重复告警。诚实面与解释资产同款：文件读不了/解析不出
  // 有效段 → 告警带 fileName 后返回空；跳过行进一条汇总告警（计数+fileName）。
  QVector<wellsection::LithoSegment>
  parseCuttings(const CatalogVersion &v) const {
    const auto it = m_cuttingsParsed.constFind(v.id);
    if (it != m_cuttingsParsed.constEnd())
      return it.value();
    const auto warn = [this](const QString &msg) {
      if (m_warnings)
        *m_warnings << msg;
    };
    QVector<wellsection::LithoSegment> segments;
    const paleo::io::CuttingsTable table =
        paleo::io::readCuttingsFile(DataCatalog::resolvedVersionPath(m_projectDir, v));
    if (!table.ok) {
      warn(WellSectionWorkflow::tr("岩屑录井文件读取失败：%1（%2）")
               .arg(v.fileName, table.error));
      m_cuttingsParsed.insert(v.id, segments);
      return segments;
    }
    if (table.intervals.isEmpty())
      warn(WellSectionWorkflow::tr("岩屑录井文件无有效岩屑段：%1").arg(v.fileName));
    if (!table.issues.isEmpty())
      warn(WellSectionWorkflow::tr("岩屑录井文件有 %1 条数据问题已跳过：%2")
               .arg(table.issues.size())
               .arg(v.fileName));
    for (const paleo::io::CuttingsInterval &interval : table.intervals) {
      wellsection::LithoSegment seg;
      seg.topMd = interval.topMd;
      seg.baseMd = interval.baseMd;
      seg.litho = interval.litho;
      seg.source = wellsection::LithoSource::Interpreted;
      seg.provenance = v.fileName.isEmpty()
                           ? WellSectionWorkflow::tr("岩屑录井")
                           : WellSectionWorkflow::tr("岩屑录井（%1）").arg(v.fileName);
      segments.push_back(seg);
    }
    m_cuttingsParsed.insert(v.id, segments);
    return segments;
  }

  mutable QHash<QString, QVector<wellsection::LithoSegment>> m_cuttingsParsed;
};

// 解释岩性段（方向 69 按井链接消费：有该井段的解释资产
// well_litho_intervals 优先，无该井段回落 role=="cuttings" 的岩屑录井
// 文件；再空走 GR 回落）挂到各井
// litho；两源皆无链接的井静默留空（GR 回落是正常态）。资产在但读不了/
// 解析不出有效段 → 如实告警（回落不是静默伪装解释缺失）；schema 未知 →
// 拒读。cuttings 段 source=Interpreted、provenance=「岩屑录井」，题注自然
// 呈「解释·岩屑录井」——不新增 LithoSource 枚举。
// 资产契约：{"schema":1,"provenance":{...},"intervals":[{"wellId","top",
// "base","litho"}]}，深度 MD 米；生产者 = wellfaciesworkflow（welllogfacies
// 预测结果落 DERIVED）。岩屑表契约：顶深/底深/岩性必需列 + 描述可选列
// （方言表头见 cuttingsdoc.h），一井一份。
QHash<QString, QVector<wellsection::LithoSegment>> WellSectionWorkflow::lithologiesFor(
    const QStringList &wellIds, QStringList *warnings) const {
  CatalogWellLithologyProvider provider(m_catalog, projectDir(), warnings);
  QHash<QString, QVector<wellsection::LithoSegment>> result;
  for (const QString &id : wellIds) result.insert(id, provider.lithologyFor(id));
  return result;
}

void WellSectionWorkflow::attachLithoSegments(
    QVector<wellsection::Well> &wells, QStringList *warnings) const {
  if (!m_catalog || wells.isEmpty())
    return;
  CatalogWellLithologyProvider provider(m_catalog, projectDir(), warnings);
  for (wellsection::Well &w : wells) {
    QVector<wellsection::LithoSegment> segs = provider.lithologyFor(w.id);
    if (segs.isEmpty())
      continue;
    std::sort(segs.begin(), segs.end(),
              [](const wellsection::LithoSegment &a,
                 const wellsection::LithoSegment &b) {
                return a.topMd < b.topMd;
              });
    w.litho = segs;
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
