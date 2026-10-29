// 层：数据
#include "projectdata.h"
#include "stratigraphymodel.h"

#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include "../io/wellcompositexml.h"
#include "../io/wellfileparsers.h"
#include "../io/nativemodel.h"
#include "../metadata/layermanifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cmath>

#include <gdal.h>

// ---------------------------------------------------------------------------
// ProjectDataFacade — DataCatalog/解析器/LayerManifest 之上的读侧适配层。
// 纯 Qt/GDAL：无 Qgs* 类型（§25），编图链可从域层代码直接消费。
// ---------------------------------------------------------------------------

ProjectDataFacade::ProjectDataFacade(QObject *parent)
  : QObject(parent)
{
}

ProjectDataFacade::~ProjectDataFacade()
{
  // m_catalog 为自有子对象时随 QObject 父子关系销毁，这里无需手工释放。
}

bool ProjectDataFacade::setProjectDir(const QString &projectDir)
{
  m_lastError.clear();
  if (m_ownsCatalog && m_catalog)
    m_catalog->deleteLater();
  m_catalog = nullptr;
  m_ownsCatalog = false;
  m_projectDirOverride.clear();

  auto *catalog = new DataCatalog(this);
  QString err;
  if (!catalog->open(projectDir, &err))
  {
    m_lastError = tr("无法打开工区目录的 catalog：%1（%2）").arg(projectDir, err);
    delete catalog;
    return false;
  }
  m_catalog = catalog;
  m_ownsCatalog = true;
  return true;
}

void ProjectDataFacade::setCatalog(DataCatalog *catalog, const QString &projectDir)
{
  if (m_ownsCatalog && m_catalog)
    m_catalog->deleteLater();
  m_catalog = catalog;
  m_ownsCatalog = false;
  m_projectDirOverride = projectDir;
}

void ProjectDataFacade::setManifest(LayerManifest *manifest)
{
  m_manifest = manifest;
}

QString ProjectDataFacade::projectDir() const
{
  if (!m_projectDirOverride.isEmpty())
    return m_projectDirOverride;
  if (!m_catalog)
    return QString();
  // catalogPath = <projectDir>/artifacts/metadata/catalog.json
  const QString path = m_catalog->catalogPath();
  QDir dir(QFileInfo(path).absolutePath()); // .../artifacts/metadata
  dir.cdUp();                               // .../artifacts
  dir.cdUp();                               // .../<projectDir>
  return dir.absolutePath();
}

QVector<ProjectWell> ProjectDataFacade::wells() const
{
  QVector<ProjectWell> out;
  if (!m_catalog)
    return out;
  for (const CatalogEntity &e : m_catalog->entities(QStringLiteral("well")))
  {
    ProjectWell w;
    w.id = e.id;
    w.name = e.name;
    w.surfaceX = e.surfaceX;
    w.surfaceY = e.surfaceY;
    w.kb = e.kb;
    w.coordinateStatus = e.coordinateStatus;
    w.coordinateSystem = e.extra.value("wellCoordinatesCrs").toString();
    w.coordinateUnit = e.extra.value("wellCoordinateUnit").toString();
    if (!w.id.isEmpty())
      out.append(w);
  }
  return out;
}

QString ProjectDataFacade::assetFilePathFor(const QString &wellId, const QString &role,
                                            const QString &assetId) const
{
  if (!m_catalog)
    return QString();
  // audit row 35：linksForEntity 空 id 只回空集（未决集合归 unresolvedLinks()）
  // ——空 wellId 提前返回，不发出歧义查询。
  if (wellId.isEmpty())
    return QString();

  QString found;
  for (const EntityAssetLink &l : m_catalog->linksForEntity(wellId))
  {
    // 显式 assetId：只要该井挂着此资产（非 unresolved），primary 与否都行；
    // 空 assetId 沿用主链接语义。
    if (l.role != role || l.unresolved)
      continue;
    if (!assetId.isEmpty() ? l.assetId == assetId : l.isPrimary)
    {
      found = l.assetId;
      break;
    }
  }
  if (found.isEmpty())
    return QString();

  const CatalogVersion v = m_catalog->currentVersion(found);
  if (v.id.isEmpty())
    return QString();
  return DataCatalog::resolvedVersionPath(projectDir(), v);
}

QVector<WellTop> ProjectDataFacade::topsFor(const QString &wellId) const
{
  return topsFor(wellId, nullptr);
}

QVector<WellTop> ProjectDataFacade::topsFor(const QString &wellId, WellParseReport *report) const
{
  if (m_catalog) {
    const auto entity = m_catalog->entityById(wellId);
    const auto tops = stratigraphyModelTops(projectDir(), entity.extra.value("areaModelWellId").toLongLong());
    if (!tops.isEmpty()) { if (report) *report = {}; return tops; }
  }
  return topsForAsset(wellId, QString(), report);
}

namespace {
struct MappingTopsPick {
  QString assetId;
  QVector<WellTop> tops;
  int score = 0;
  bool primary = false;
};

MappingTopsPick pickMappingTops(const ProjectDataFacade *facade, DataCatalog *catalog,
                               const QString &wellId) {
  MappingTopsPick best;
  if (!facade || !catalog || wellId.isEmpty())
    return best;
  const QStringList sequence = mappingHorizons();
  QSet<QString> seen;
  for (const EntityAssetLink &link : catalog->linksForEntity(wellId)) {
    if (link.role != QLatin1String("tops") || link.unresolved ||
        seen.contains(link.assetId))
      continue;
    seen.insert(link.assetId);
    const QVector<WellTop> tops = facade->topsForAsset(wellId, link.assetId);
    int score = 0;
    QSet<QString> names;
    for (const WellTop &top : tops) {
      if (!sequence.contains(top.horizon) || !std::isfinite(top.md) ||
          names.contains(top.horizon))
        continue;
      names.insert(top.horizon);
      ++score;
    }
    if (score == 0)
      continue;
    if (score > best.score || (score == best.score && link.isPrimary && !best.primary)) {
      best.assetId = link.assetId;
      best.tops = tops;
      best.score = score;
      best.primary = link.isPrimary;
    }
  }
  return best;
}
} // namespace

QVector<WellTop> ProjectDataFacade::topsForMapping(const QString &wellId) const {
  if (m_catalog) {
    const auto tops = stratigraphyModelTops(projectDir(), m_catalog->entityById(wellId).extra.value("areaModelWellId").toLongLong());
    if (!tops.isEmpty()) return tops;
  }
  const MappingTopsPick pick = pickMappingTops(this, m_catalog, wellId);
  if (pick.score > 0)
    return pick.tops;
  return topsFor(wellId);
}

QString ProjectDataFacade::mappingTopsAssetId(const QString &wellId) const {
  const MappingTopsPick pick = pickMappingTops(this, m_catalog, wellId);
  if (pick.score > 0)
    return pick.assetId;
  if (!m_catalog)
    return {};
  for (const EntityAssetLink &link : m_catalog->linksForEntity(wellId))
    if (link.role == QLatin1String("tops") && link.isPrimary && !link.unresolved)
      return link.assetId;
  return {};
}

QVector<WellTop> ProjectDataFacade::topsForAsset(const QString &wellId,
                                                 const QString &assetId,
                                                 WellParseReport *report) const
{
  if (report)
    *report = {};
  const QString path = assetFilePathFor(wellId, QStringLiteral("tops"), assetId);
  if (path.isEmpty() || !QFile::exists(path))
    return {};
  return topsFromFile(path, wellId, report);
}

QVector<WellTop> ProjectDataFacade::topsFromFile(const QString &path,
                                                 const QString &wellId,
                                                 WellParseReport *report) const
{
  QVector<WellTop> out;
  QVector<WellTopRecord> rows;
  if (!readWellTops(path, rows, nullptr, report)) return out;

  // 井名按 catalog 的规范化规则匹配（连字符/空格/大小写不敏感）。
  QString wellName;
  if (m_catalog)
  {
    const CatalogEntity e = m_catalog->entityById(wellId);
    wellName = e.name;
  }
  const QString normalized = m_catalog ? DataCatalog::normalizeWellName(wellName) : wellName;

  if (report)
    for (QString &issue : report->issues)
      issue = QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), issue);
  for (const WellTopRecord &r : rows)
  {
    if (DataCatalog::normalizeWellName(r.wellName) != normalized)
      continue;
    WellTop top;
    top.horizon = r.topName;
    if (top.horizon.isEmpty())
      continue;
    top.md = r.hasMd ? r.md : qQNaN();
    top.tvd = r.hasTvd ? r.tvd : qQNaN();
    // 坐标只有 X、Y 两列都有效时才采用；(0,0) 是合法的局部网格坐标。
    if (r.hasX && r.hasY)
    {
      top.x = r.x;
      top.y = r.y;
    }
    out.append(top);
  }
  return out;
}

QVector<WellImageAnchor> ProjectDataFacade::imagesFor(const QString &wellId) const
{
  QVector<WellImageAnchor> out;
  if (!m_catalog || wellId.isEmpty())
    return out;
  for (const EntityAssetLink &l : m_catalog->linksForEntity(wellId))
  {
    if (l.unresolved ||
        (l.role != QStringLiteral("core") &&
         l.role != QStringLiteral("lab_analysis")))
      continue;
    const CatalogVersion v = m_catalog->currentVersion(l.assetId);
    if (v.id.isEmpty())
      continue;
    // 深度锚是图片道的存在前提——extra 无 depthMd（薄片照片深度在文件名
    // 无单位）不收，不猜。
    const QVariant depth = v.extra.value(QStringLiteral("depthMd"));
    if (!depth.isValid() || !std::isfinite(depth.toDouble()) ||
        depth.toDouble() <= 0.0)
      continue;
    const QString path = DataCatalog::resolvedVersionPath(projectDir(), v);
    if (path.isEmpty() || !QFile::exists(path))
      continue;
    WellImageAnchor a;
    a.assetId = l.assetId;
    a.path = path;
    a.depthMd = depth.toDouble();
    a.caption = v.fileName.isEmpty() ? l.assetId : v.fileName;
    out.append(a);
  }
  std::sort(out.begin(), out.end(),
            [](const WellImageAnchor &a, const WellImageAnchor &b) {
              return a.depthMd < b.depthMd;
            });
  return out;
}

std::optional<paleo::WellDeviationSurvey>
ProjectDataFacade::trajectoryFor(const QString &wellId, QString *error) const
{
  if (error)
    error->clear();
  const QString path = assetFilePathFor(wellId, QStringLiteral("trajectory"));
  if (path.isEmpty() || !QFile::exists(path))
    return std::nullopt; // 无链接 = 直井（显式语义，不告警；与 topsFor/tdTableFor 同口径）

  QVector<paleo::DeviationStation> stations;
  DeviationTable native;
  const auto state=paleo::native_model::deviation(path,native,error);
  if(state==paleo::native_model::Read::Failed) {
    m_lastError=error?*error:tr("测斜模型无法读取：%1").arg(path);
    return std::nullopt;
  }
  if(state==paleo::native_model::Read::Ready) {
    for(const auto &r:native.stations) stations.append({r.md,r.inclinationDeg,r.azimuthDeg});
  } else {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
      m_lastError = tr("测斜文件无法读取：%1").arg(path);
      if (error)
        *error = m_lastError;
      return std::nullopt;
    }
    const QByteArray text = f.readAll();
    f.close();

    if (QFileInfo(path).suffix().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0)
    {
      QVector<WellComposite::XmlDeviationStation> parsed;
      QString perr;
      if (!WellComposite::parseDeviationSurvey(path, parsed, &perr))
      {
        m_lastError = tr("测斜 XML 解析失败：%1（%2）").arg(path, perr);
        if (error)
          *error = m_lastError;
        return std::nullopt;
      }
      stations.reserve(parsed.size());
      for (const auto &st : parsed)
        stations.append({st.md, st.inclinationDeg, st.azimuthDeg});
    }
    else
    {
      const DeviationTable table = parseDeviationText(text);
      stations.reserve(table.stations.size());
      for (const DeviationStationRecord &r : table.stations)
        stations.append({r.md, r.inclinationDeg, r.azimuthDeg});
    }

  }

  QString serr;
  auto survey = paleo::WellDeviationSurvey::fromStations(stations, &serr);
  if (!survey)
  {
    m_lastError = tr("测斜站表无效：%1（%2）").arg(path, serr);
    if (error)
      *error = m_lastError;
    return std::nullopt;
  }
  return survey;
}

HorizonRasterInfo ProjectDataFacade::horizonRasterDecl(const QString &horizon) const
{
  HorizonRasterInfo info;
  if (!m_manifest && !m_declarationSnapshot)
    return info;

  // Prefer the exact "horizon.<H>" declaration id, then any "horizon."-
  // prefixed id, otherwise the first declared raster bound to this horizon.
  // A manifest read failure is a read failure —
  // surface it via m_lastError instead of letting it masquerade as
  // "no raster declared for this horizon".
  QVector<LayerDeclaration> decls;
  QString readErr;
  if (m_declarationSnapshot)
    decls = *m_declarationSnapshot;
  else if (!m_manifest->readAll(&decls, &readErr)) {
    m_lastError = tr("无法读取图层清单（层位 %1 栅格声明）：%2")
                      .arg(horizon, readErr.isEmpty() ? tr("未知错误") : readErr);
    return info;
  }
  // Deterministic order when several rasters bind one horizon: the exact
  // canonical id ("horizon.<H>" — what DataImportService declares for the
  // derived grid) beats a bare "horizon."-prefixed id, which beats any other
  // raster decl for the horizon. The loose prefix alone would resolve a
  // raw-plus-derived pair in manifest row order.
  const QString exactId = QStringLiteral("horizon.") + horizon;
  QString anyId, anySrc, prefId, prefSrc;
  for (const LayerDeclaration &d : decls)
  {
    if (d.horizon != horizon)
      continue;
    if (d.type.compare(QStringLiteral("raster"), Qt::CaseInsensitive) != 0)
      continue;
    if (d.layerId == exactId)
    {
      anyId = d.layerId;
      anySrc = d.source;
      prefId.clear(); // canonical id is definitive — stop scanning
      break;
    }
    if (anyId.isEmpty())
    {
      anyId = d.layerId;
      anySrc = d.source;
    }
    if (prefId.isEmpty() && d.layerId.startsWith(QLatin1String("horizon.")))
    {
      prefId = d.layerId;
      prefSrc = d.source;
    }
  }
  info.layerId = prefId.isEmpty() ? anyId : prefId;
  const QString source = prefId.isEmpty() ? anySrc : prefSrc;
  if (source.isEmpty())
    return info;

  const QString path = source.section(QLatin1Char('|'), 0, 0);
  if (!QFile::exists(path))
  {
    m_lastError = tr("层位 %1 的时间栅格文件不存在：%2").arg(horizon, path);
    return info;
  }

  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
  if (!ds)
  {
    m_lastError = tr("无法打开层位 %1 的时间栅格：%2").arg(horizon, path);
    return info;
  }
  double gt[6] = {0, 0, 0, 0, 0, 0};
  GDALGetGeoTransform(ds, gt);
  info.cols = GDALGetRasterXSize(ds);
  info.rows = GDALGetRasterYSize(ds);
  info.cellSize = gt[1];
  info.xmin = gt[0];
  info.xmax = gt[0] + gt[1] * info.cols;
  info.ymax = gt[3];
  info.ymin = gt[3] + gt[5] * info.rows;
  const char *inlineMin = GDALGetMetadataItem(ds, "PALEO_INLINE_MIN", nullptr);
  const char *inlineMax = GDALGetMetadataItem(ds, "PALEO_INLINE_MAX", nullptr);
  if (inlineMin)
    info.inlineMin = QByteArray(inlineMin).toInt();
  if (inlineMax)
    info.inlineMax = QByteArray(inlineMax).toInt();
  GDALClose(ds);

  info.path = path;
  info.valid = info.rows > 0 && info.cols > 0 && info.cellSize > 0.0;
  return info;
}

EntityView ProjectDataFacade::entityView(const QString &entityId) const
{
  if (!m_catalog || !m_catalog->isOpen())
    return EntityView();
  return entityDataView(*m_catalog, entityId);
}

QVector<CatalogVersion> ProjectDataFacade::downstreamClosureOf(
    const QString &versionId) const
{
  if (!m_catalog || !m_catalog->isOpen())
    return {};
  return m_catalog->downstreamClosure(versionId);
}

std::shared_ptr<DataCatalog> ProjectDataFacade::catalogSnapshot() const {
  return m_catalog ? std::shared_ptr<DataCatalog>(
                         m_catalog->createStagingCopy(QString()))
                   : nullptr;
}
void ProjectDataFacade::setDeclarationSnapshot(
    const QVector<LayerDeclaration> &declarations) {
  m_declarationSnapshot = declarations;
}
